#include "core/omni_runtime.h"
#include "core/message_reader.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntimeDispatch"

namespace omnibinder {

namespace {

std::string serviceClientLabel(IMessageConnection* client, int client_id) {
    const char* prefix = (client && client->isFramed()) ? "shm=" : "fd=";
    return std::string(prefix) + std::to_string(client_id);
}

} // namespace

// ============================================================
// 端点事件与入站请求分派
// ============================================================

bool OmniRuntime::Impl::isEntryAlive(const std::string& service_name,
                                     LocalServiceEntry* entry) const {
    std::map<std::string, LocalServiceEntry*>::const_iterator it =
        local_services_.find(service_name);
    // 对比指针地址即可，不解引用悬垂值
    return it != local_services_.end() && it->second == entry;
}

void OmniRuntime::Impl::emitDiagHook(LocalServiceEntry* entry, uint8_t direction,
                                     const Message& msg) {
    if (entry && entry->diag_enabled && entry->diag_topic_id != 0) {
        Buffer diag_buf;
        diag_serialize_event(diag_buf, direction, msg);
        broadcastInternal(entry->diag_topic_id, diag_buf);
    }
}

/*
 * @brief  端点级 fd 事件分发：每个端点自行判断 fd 归属，不认识则忽略
 * @note   端点回调可能执行用户代码并注销服务（删除 entry），每步后按名重查（约束 1）
 */
void OmniRuntime::Impl::onServiceEndpointEvent(const std::string& name, int fd,
                                               uint32_t events) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
    if (it == local_services_.end()) return;
    LocalServiceEntry* entry = it->second;

    for (size_t i = 0; i < entry->endpoints.size(); ++i) {
        entry->endpoints[i]->onPollEvent(fd, events);
        it = local_services_.find(name);
        if (it == local_services_.end() || it->second != entry) return;
    }

    // 接入新客户端后同步其在 EventLoop 中的 fd（liveness / per-client notify）
    syncEndpointFds(name, entry);
}

void OmniRuntime::Impl::onServiceClientAccepted(const std::string& name, int client_id,
                                                IMessageConnection* client) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
    if (it == local_services_.end() || !client) return;
    LocalServiceEntry* entry = it->second;

    entry->clients[client_id] = client;
    entry->client_recv_buffers[client_id] = new Buffer();
    client_id_to_service_[client_id] = name;
    if (entry->service) {
        entry->service->onClientConnected(serviceClientLabel(client, client_id));
    }
}

void OmniRuntime::Impl::onServiceClientReadable(const std::string& name, int client_id) {
    Message msg;
    while (true) {
        // 每轮循环前重新确认 entry/client/buffer 存活：上一轮用户回调可能
        // unregisterService → delete entry（约束 1/2）
        std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
        if (it == local_services_.end()) return;
        LocalServiceEntry* entry = it->second;
        std::map<int, IMessageConnection*>::iterator tit = entry->clients.find(client_id);
        if (tit == entry->clients.end() || !tit->second) return;
        IMessageConnection* transport = tit->second;
        std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_id);
        if (bit == entry->client_recv_buffers.end()) return;
        Buffer* recv_buf = bit->second;

        int ret = readNextMessage(*transport, *recv_buf, msg);
        if (ret == 0) return;
        if (ret < 0) {
            OMNI_LOG_WARN(LOG_TAG, "invalid client stream for %s client[%d], disconnecting",
                          name.c_str(), client_id);
            onServiceClientDisconnected(name, client_id);
            return;
        }

        handleServiceClientMessage(name, client_id, msg);
    }
}

void OmniRuntime::Impl::handleServiceClientMessage(const std::string& name, int client_id,
                                                   const Message& msg) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
    if (it == local_services_.end()) return;
    LocalServiceEntry* entry = it->second;
    std::map<int, IMessageConnection*>::iterator tit = entry->clients.find(client_id);
    if (tit == entry->clients.end() || !tit->second) return;
    IMessageConnection* transport = tit->second;
    const char* transport_label = dataChannelKindName(transport->type());

    switch (msg.getType()) {
    case MessageType::MSG_HEARTBEAT: {
        // 心跳 ACK 可丢弃：必须非阻塞/尽力而为，绝不占用 owner loop 的发送预算
        Message ack(MessageType::MSG_HEARTBEAT_ACK, msg.getSequence());
        tit = entry->clients.find(client_id);
        if (tit != entry->clients.end() && tit->second) {
            if (!sendOnFdBestEffort(tit->second, ack)) {
                OMNI_LOG_DEBUG(LOG_TAG, "heartbeat_ack_send_dropped service=%s client=%d",
                               name.c_str(), client_id);
            }
        }
        break;
    }
    case MessageType::MSG_INVOKE:
        onInvokeRequest(name, client_id, msg, transport_label);
        break;
    case MessageType::MSG_INVOKE_ONEWAY:
        onInvokeOneWayRequest(name, msg, transport_label);
        break;
    case MessageType::MSG_SUBSCRIBE_BROADCAST: {
        uint32_t topic_id = 0;
        std::string topic_name;
        if (!decodeSubscribeBroadcastPayload(msg, topic_id, topic_name)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "malformed_subscribe_broadcast transport=%s seq=%u err=%d",
                          transport_label, msg.getSequence(),
                          static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        const bool is_shm = transport->isFramed();
        emitDiagHook(entry, DIAG_EVENT_SUBSCRIBE, msg);
        // emitDiagHook 可能触发断连/用户回调并注销本服务（约束 1）：注册订阅前重查
        it = local_services_.find(name);
        if (it == local_services_.end() || it->second != entry) break;
        if (entry->clients.find(client_id) == entry->clients.end()) break;
        if (is_shm) {
            topic_runtime_.addShmSubscriberService(topic_id, name,
                                                   static_cast<uint32_t>(client_id));
        } else {
            topic_runtime_.addTcpSubscriber(topic_id, client_id);
        }
        OMNI_LOG_INFO(LOG_TAG, "Broadcast subscriber client[%d] added for topic %s (id=0x%08x)",
                      client_id, topic_name.c_str(), topic_id);
        break;
    }
    case MessageType::MSG_BROADCAST: {
        // 防御分支：正常拓扑下广播由发布者发往订阅者，服务端不会收到；
        // 仍与客户端直连路径共用同一解码+诊断+分发 helper
        if (!dispatchBroadcastMessage(msg)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "malformed_broadcast transport=server service=%s client=%d seq=%u err=%d",
                          name.c_str(), client_id, msg.getSequence(),
                          static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled message for %s client[%d]: %s",
                       name.c_str(), client_id, messageTypeToString(msg.getType()));
        break;
    }
}

bool OmniRuntime::Impl::dispatchBroadcastMessage(const Message& msg) {
    uint32_t topic_id = 0;
    Buffer data;
    if (!decodeBroadcastPayload(msg, topic_id, data)) {
        return false;
    }
    emitDiagEvent(DIAG_EVENT_BROADCAST, msg);
    topic_runtime_.dispatch(topic_id, data);
    return true;
}

void OmniRuntime::Impl::onServiceClientDisconnected(const std::string& name, int client_id) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
    if (it == local_services_.end()) {
        client_id_to_service_.erase(client_id);
        return;
    }
    LocalServiceEntry* entry = it->second;
    std::map<int, IMessageConnection*>::iterator cit = entry->clients.find(client_id);
    if (cit == entry->clients.end()) {
        client_id_to_service_.erase(client_id);
        return;
    }

    IMessageConnection* client = cit->second;
    // 先摘除 EventLoop 注册再释放对象（约束 3）
    if (client && client->fd() >= 0) {
        loop_->removeFd(client->fd());
        entry->endpoint_fds.erase(client->fd());
    }
    entry->clients.erase(cit);

    client_id_to_service_.erase(client_id);
    topic_runtime_.removeTcpSubscriberFd(client_id);
    topic_runtime_.removeShmSubscriberService(name, client_id);

    std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_id);
    if (bit != entry->client_recv_buffers.end()) {
        delete bit->second;
        entry->client_recv_buffers.erase(bit);
    }

    std::string label = serviceClientLabel(client, client_id);
    if (entry->service) {
        entry->service->onClientDisconnected(label);
    }

    // 用户回调可能注销服务 → entry 及其 endpoint 已释放（析构完成资源回收）
    it = local_services_.find(name);
    if (it == local_services_.end() || it->second != entry) return;

    // 最后通知端点释放 per-client transport（removeClient 幂等）
    for (size_t i = 0; i < entry->endpoints.size(); ++i) {
        entry->endpoints[i]->removeClient(client_id);
    }
}

void OmniRuntime::Impl::onInvokeRequest(const std::string& service_name, int client_id,
                                        const Message& msg,
                                        const char* transport_label) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;

    LocalServiceEntry* entry = it->second;
    Service* service = entry->service;
    if (!service) return;

    emitDiagHook(entry, DIAG_EVENT_REQUEST, msg);
    emitDiagEvent(DIAG_EVENT_REQUEST, msg);

    InvokeDispatchResult result = dispatchLocalInvoke(service, msg, transport_label,
                                                      service_name.c_str());

    // dispatchLocalInvoke 内执行用户 onInvoke，回调可能 unregisterService → delete entry。
    // 返回后必须重新判活，避免访问已释放的 entry 字段。
    if (!isEntryAlive(service_name, entry)) return;

    if (result.status == InvokeDispatchStatus::SUCCESS) {
        Message reply = makeInvokeSuccessReply(msg.getSequence(), result.response);
        emitDiagHook(entry, DIAG_EVENT_RESPONSE, reply);
        // emitDiagHook 可能经话题分发执行用户回调并注销服务（约束 1）
        if (!isEntryAlive(service_name, entry)) return;
        std::map<int, IMessageConnection*>::iterator tit = entry->clients.find(client_id);
        IMessageConnection* transport = (tit != entry->clients.end()) ? tit->second : NULL;
        if (transport && !sendOnFd(transport, reply)) {
            OMNI_LOG_WARN(LOG_TAG, "invoke_reply_send_failed service=%s client=%d seq=%u status=0",
                          service_name.c_str(), client_id, msg.getSequence());
        }
    } else {
        Message reply = makeInvokeErrorReply(msg.getSequence(),
                                             static_cast<ErrorCode>(result.error_code));
        emitDiagHook(entry, DIAG_EVENT_RESPONSE, reply);
        if (!isEntryAlive(service_name, entry)) return;
        std::map<int, IMessageConnection*>::iterator tit = entry->clients.find(client_id);
        IMessageConnection* transport = (tit != entry->clients.end()) ? tit->second : NULL;
        if (transport && !sendOnFd(transport, reply)) {
            OMNI_LOG_WARN(LOG_TAG, "invoke_reply_send_failed service=%s client=%d seq=%u status=%d",
                          service_name.c_str(), client_id, msg.getSequence(),
                          result.error_code);
        }
    }
}

void OmniRuntime::Impl::onInvokeOneWayRequest(const std::string& service_name,
                                              const Message& msg,
                                              const char* transport_label) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    Service* service = it->second->service;
    if (!service) return;
    emitDiagHook(it->second, DIAG_EVENT_ONE_WAY, msg);
    emitDiagEvent(DIAG_EVENT_ONE_WAY, msg);

    InvokeDispatchResult result = dispatchLocalInvoke(service, msg, transport_label,
                                                      service_name.c_str());
    if (result.status == InvokeDispatchStatus::IDL_MISMATCH) {
        uint32_t iface_id = 0, idl_hash = 0, method_id = 0;
        Buffer ignored;
        decodeInvokePayload(msg, iface_id, idl_hash, method_id, ignored);
        OMNI_LOG_ERROR(LOG_TAG,
                       "oneway_idl_mismatch service=%s method=0x%08x err=%d message discarded",
                       service_name.c_str(), method_id, result.error_code);
    }
}

InvokeDispatchResult OmniRuntime::Impl::dispatchLocalInvoke(Service* service, const Message& msg,
                                                            const char* transport_label,
                                                            const char* service_name) {
    uint32_t interface_id = 0;
    uint32_t client_idl_hash = 0;
    uint32_t method_id = 0;
    Buffer request;
    if (!decodeInvokePayload(msg, interface_id, client_idl_hash, method_id, request)) {
        OMNI_LOG_WARN(LOG_TAG,
                      "malformed_invoke_payload service=%s transport=%s seq=%u err=%d",
                      service_name, transport_label, msg.getSequence(),
                      static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        InvokeDispatchResult result;
        result.status = InvokeDispatchStatus::DECODE_FAILED;
        result.error_code = static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        return result;
    }

    if (interface_id == OMNI_DIAG_IFACE_ID) {
        return dispatchDiagInvoke(service, service_name, request);
    }

    InvokeDispatchResult result;
    result.error_code = 0;

    if (service->interfaceInfo().interface_id != interface_id) {
        result.status = InvokeDispatchStatus::INTERFACE_MISMATCH;
        result.error_code = static_cast<int>(ErrorCode::ERR_INTERFACE_NOT_FOUND);
        return result;
    }

    // 严格 IDL 校验：客户端声明哈希（非 0）时，服务端注册的方法哈希必须与之相等；
    // 服务端未声明（0）或方法不存在一律视为不匹配，避免 C 服务漏注册哈希时静默放行。
    // client_idl_hash == 0 表示调用方无法提供哈希（如 omni-cli hex 模式），保持兼容放行。
    if (client_idl_hash != 0) {
        uint32_t server_idl_hash = 0;
        const InterfaceInfo& iface = service->interfaceInfo();
        for (size_t i = 0; i < iface.methods.size(); ++i) {
            if (iface.methods[i].method_id == method_id) {
                server_idl_hash = iface.methods[i].idl_hash;
                break;
            }
        }
        if (client_idl_hash != server_idl_hash) {
            OMNI_LOG_WARN(LOG_TAG,
                          "idl_mismatch service=%s transport=%s method=0x%08x "
                          "client_hash=0x%08x server_hash=0x%08x",
                          service_name, transport_label, method_id,
                          client_idl_hash, server_idl_hash);
            result.status = InvokeDispatchStatus::IDL_MISMATCH;
            result.error_code = static_cast<int>(ErrorCode::ERR_IDL_MISMATCH);
            return result;
        }
    }

    int invoke_status = service->onInvoke(method_id, request, result.response);
    if (invoke_status != 0) {
        OMNI_LOG_WARN(LOG_TAG,
                      "invoke_failed service=%s transport=%s seq=%u method=0x%08x err=%d",
                      service_name, transport_label, msg.getSequence(), method_id,
                      invoke_status);
        result.status = InvokeDispatchStatus::INVOKE_FAILED;
        result.error_code = invoke_status;
        return result;
    }

    result.status = InvokeDispatchStatus::SUCCESS;
    return result;
}

InvokeDispatchResult OmniRuntime::Impl::dispatchDiagInvoke(Service* service,
                                                           const char* service_name,
                                                           const Buffer& request) {
    InvokeDispatchResult result;
    result.status = InvokeDispatchStatus::SUCCESS;
    result.error_code = 0;

    LocalServiceEntry* entry = nullptr;
    for (auto& kv : local_services_) {
        if (kv.second->service == service) { entry = kv.second; break; }
    }
    if (!entry) {
        result.status = InvokeDispatchStatus::INVOKE_FAILED;
        result.error_code = static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
        return result;
    }

    bool enable = (request.size() >= 1 && request.data()[0] != 0);
    if (!enable && diag_active_count_ == 0) {
        result.response.writeUint8(0);
        return result;
    }

    if (enable && !entry->diag_enabled) {
        std::string diag_topic = "__diag__" + std::string(service_name);
        uint32_t tid = topic_runtime_.getTopicId(diag_topic);
        if (tid == 0) {
            int pub_ret = publishTopicInternal(diag_topic, 0);
            if (pub_ret != 0) {
                result.status = InvokeDispatchStatus::INVOKE_FAILED;
                result.error_code = pub_ret;
            } else {
                tid = topic_runtime_.getTopicId(diag_topic);
            }
        }
        // publishTopicInternal 会阻塞等待 SM reply，期间可能经 SM 消息触发用户回调
        // 注销本服务（约束 1），返回后须重查 entry 存活
        entry = nullptr;
        for (auto& kv : local_services_) {
            if (kv.second->service == service) { entry = kv.second; break; }
        }
        if (tid != 0 && entry) {
            entry->diag_topic_id = tid;
            entry->diag_enabled = true;
            diag_active_count_++;
            OMNI_LOG_INFO(LOG_TAG, "diag_enabled service=%s topic=%s topic_id=0x%08x",
                          service_name, diag_topic.c_str(), entry->diag_topic_id);
            result.response.writeUint8(0);
        } else {
            result.status = InvokeDispatchStatus::INVOKE_FAILED;
            result.error_code = -1;
        }
    } else if (!enable) {
        if (entry->diag_enabled && diag_active_count_ > 0) {
            diag_active_count_--;
        }
        entry->diag_enabled = false;
        entry->diag_topic_id = 0;
        OMNI_LOG_INFO(LOG_TAG, "diag_disabled service=%s", service_name);
        result.response.writeUint8(0);
    } else {
        result.response.writeUint8(0);
    }
    return result;
}

// ============================================================
// 数据面直连 — ConnectionManager 回调
// ============================================================

void OmniRuntime::Impl::onDirectMessage(const std::string& service_name, Message& msg) {
    MessageType type = msg.getType();
    uint32_t seq = msg.getSequence();

    // 只有 MSG_INVOKE_REPLY 才可能消费为 RPC 回复。其余消息类型（MSG_BROADCAST、
    // MSG_HEARTBEAT_ACK 等主动消息）即使 seq 恰好命中正在等待的槽，也绝不进入
    // pending 槽——否则会被误当成 invoke 回复（seq 碰撞应答错配）。
    if (type == MessageType::MSG_INVOKE_REPLY) {
        if (storeAndConsumeDataReply(seq, msg)) {
            return;
        }
    }

    switch (type) {
    case MessageType::MSG_BROADCAST: {
        if (!dispatchBroadcastMessage(msg)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "malformed_broadcast transport=direct seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        }
        break;
    }
    case MessageType::MSG_HEARTBEAT_ACK: {
        ServiceState* hb_state = findServiceState(service_name);
        if (hb_state && hb_state->has_heartbeat) {
            hb_state->heartbeat.last_ack_time = platform::currentTimeMs();
            hb_state->heartbeat.pending = false;
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled direct message from %s: %s",
                       service_name.c_str(), messageTypeToString(type));
        break;
    }
}

void OmniRuntime::Impl::onDirectDisconnect(const std::string& service_name) {
    OMNI_LOG_WARN(LOG_TAG, "Direct connection to %s lost", service_name.c_str());
    stats_.connection_errors++;

    // 断开前拷贝该连接绑定的话题：handleServiceLost 会移除连接并调度重连，
    // 重连成功后按此重放订阅。notifyError 会执行用户回调且可能修改
    // services_ 中该服务的条目，不能持迭代器/引用（约束 1/2）。
    // is_topic_publisher 旧语义（仅 pub_info.name 为空时成立）导致共享真实
    // 服务名的订阅者连接断开时既不重订阅也不通知错误；现在只要连接绑定过
    // 话题订阅（含共享连接），断开都通知订阅者。
    std::vector<std::string> topics;
    ServiceState* rc_state = findServiceState(service_name);
    if (rc_state && rc_state->has_reconnect) {
        topics = rc_state->reconnect.topic_subscriptions;
    }

    handleServiceLost(service_name);

    // 订阅回调保留：直连重连后会重放 MSG_SUBSCRIBE_BROADCAST，forgetSubscription
    // 会让重放回来的广播因缺少本地回调而静默丢弃
    for (size_t i = 0; i < topics.size(); ++i) {
        OMNI_LOG_WARN(LOG_TAG, "Topic subscription '%s' lost with publisher connection %s",
                      topics[i].c_str(), service_name.c_str());
        topic_runtime_.notifyError(fnv1a_32(topics[i]), ErrorCode::ERR_CONNECTION_CLOSED);
    }
}

} // namespace omnibinder
