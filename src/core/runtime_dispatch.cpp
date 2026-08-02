#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"

#include <cstring>

#define LOG_TAG "OmniRuntimeDispatch"

namespace omnibinder {

// ============================================================
// 本地服务 accept 和请求处理
// ============================================================

void OmniRuntime::Impl::onServiceAccept(const std::string& service_name,
                                          int listen_fd, uint32_t events) {
    (void)listen_fd; (void)events;
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    LocalServiceEntry* entry = it->second;
    if (!entry || !entry->server || !loop_) return;

    ITransport* client = entry->server->accept();
    if (!client) return;

    int cfd = client->fd();
    entry->client_transports[cfd] = client;
    entry->client_recv_buffers[cfd] = new Buffer();
    client_fd_to_service_[cfd] = service_name;

    loop_->addFd(cfd, EventLoop::EVENT_READ | EventLoop::EVENT_ERROR,
        [this, service_name, cfd](int fd, uint32_t ev) {
            (void)fd;
            this->onServiceClientData(service_name, cfd, ev);
        });

    if (entry->service) {
        entry->service->onClientConnected("fd=" + std::to_string(cfd));
    }
}

void OmniRuntime::Impl::onServiceClientData(const std::string& service_name,
                                              int client_fd, uint32_t events) {
    (void)events;
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    LocalServiceEntry* entry = it->second;
    if (!entry) return;

    std::map<int, ITransport*>::iterator tit = entry->client_transports.find(client_fd);
    if (tit == entry->client_transports.end()) return;

    uint8_t buf[4096];
    int ret = tit->second->recv(buf, sizeof(buf));
    if (ret < 0) {
        removeServiceClient(service_name, client_fd);
        return;
    }
    if (ret == 0) return;

    std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_fd);
    if (bit == entry->client_recv_buffers.end()) return;
    static const size_t MAX_CLIENT_RECV_BUFFER = MAX_MESSAGE_SIZE;
    if (bit->second->size() + static_cast<size_t>(ret) > MAX_CLIENT_RECV_BUFFER) {
        OMNI_LOG_ERROR(LOG_TAG, "client_recv_buffer overflow for fd=%d (>%zuMB)",
                       client_fd, MAX_CLIENT_RECV_BUFFER / (1024*1024));
        removeServiceClient(service_name, client_fd);
        return;
    }
    bit->second->writeRaw(buf, static_cast<size_t>(ret));

    processServiceClientMessages(service_name, entry, client_fd);
}
void OmniRuntime::Impl::processServiceClientMessages(const std::string& service_name,
                                                      LocalServiceEntry* entry,
                                                      int client_fd) {
    std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_fd);
    if (bit == entry->client_recv_buffers.end()) return;

    Buffer* recv_buf = bit->second;
    while (true) {
        // 每轮循环前重新确认 entry 存活：上一轮 dispatch（onInvokeRequest /
        // onInvokeOneWayRequest / onSubscribeBroadcast）执行用户回调时可能
        // unregisterService → delete entry，recv_buf 随之悬垂（参照 drainServiceShm
        // 的判活模式）。同时重新定位 recv_buf：回调链中的 removeServiceClient
        // 可能已删除该 client 的 Buffer。
        std::map<std::string, LocalServiceEntry*>::iterator alive = local_services_.find(service_name);
        if (alive == local_services_.end() || alive->second != entry) return;
        bit = entry->client_recv_buffers.find(client_fd);
        if (bit == entry->client_recv_buffers.end()) return;
        recv_buf = bit->second;

        size_t avail = recv_buf->size() - recv_buf->readPosition();
        if (avail < MESSAGE_HEADER_SIZE) break;

        size_t pos = recv_buf->readPosition();
        MessageHeader hdr;
        if (!Message::parseHeader(recv_buf->data() + pos, avail, hdr)) break;
        if (!Message::validateHeader(hdr)) {
            recv_buf->trySetReadPosition(pos + 1);
            size_t remaining = recv_buf->size() - recv_buf->readPosition();
            if (remaining > 0 && recv_buf->readPosition() > 0) {
                memmove(recv_buf->mutableData(),
                        recv_buf->data() + recv_buf->readPosition(), remaining);
            }
            recv_buf->setWritePosition(remaining);
            recv_buf->trySetReadPosition(0);
            return;
        }

        size_t total = MESSAGE_HEADER_SIZE + hdr.length;
        if (avail < total) break;

        Message msg;
        msg.header = hdr;
        if (hdr.length > 0) {
            msg.payload.assign(recv_buf->data() + pos + MESSAGE_HEADER_SIZE, hdr.length);
        }
        if (!recv_buf->trySetReadPosition(pos + total)) return;

        if (msg.getType() == MessageType::MSG_INVOKE) {
            onInvokeRequest(service_name, client_fd, msg, "TCP");
        } else if (msg.getType() == MessageType::MSG_INVOKE_ONEWAY) {
            onInvokeOneWayRequest(service_name, msg, "TCP");
        } else if (msg.getType() == MessageType::MSG_HEARTBEAT) {
            Message ack(MessageType::MSG_HEARTBEAT_ACK, msg.getSequence());
            Buffer buf;
            if (ack.serialize(buf)) {
                ITransport* transport = nullptr;
                std::map<int, ITransport*>::iterator tit = entry->client_transports.find(client_fd);
                if (tit != entry->client_transports.end()) transport = tit->second;
                if (transport) transport->send(buf.data(), buf.size());
            }
        } else if (msg.getType() == MessageType::MSG_SUBSCRIBE_BROADCAST) {
            onSubscribeBroadcast(client_fd, msg, "TCP");
        } else if (msg.getType() == MessageType::MSG_BROADCAST) {
            uint32_t topic_id = 0;
            Buffer data;
            if (!decodeBroadcastPayload(msg, topic_id, data)) continue;
            topic_runtime_.dispatch(topic_id, data);
        }
    }

    size_t remaining = recv_buf->size() - recv_buf->readPosition();
    if (remaining > 0 && recv_buf->readPosition() > 0) {
        memmove(recv_buf->mutableData(), recv_buf->data() + recv_buf->readPosition(), remaining);
    }
    recv_buf->setWritePosition(remaining);
    recv_buf->trySetReadPosition(0);
}


void OmniRuntime::Impl::onInvokeRequest(const std::string& service_name,
                                        int client_fd, const Message& msg,
                                        const char* transport_label) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;

    LocalServiceEntry* entry = it->second;
    Service* service = entry->service;
    if (!service) return;

    if (entry->diag_enabled && entry->diag_topic_id != 0) {
        Buffer diag_buf;
        diag_serialize_event(diag_buf, DIAG_EVENT_REQUEST, msg);
        broadcastInternal(entry->diag_topic_id, diag_buf);
    }
    emitDiagEvent(DIAG_EVENT_REQUEST, msg);

    std::map<int, ITransport*>::iterator transport_it = entry->client_transports.find(client_fd);
    ITransport* transport = (transport_it != entry->client_transports.end()) ? transport_it->second : nullptr;

    InvokeDispatchResult result = dispatchLocalInvoke(service, msg, transport_label, service_name.c_str());

    // dispatchLocalInvoke 内执行用户 onInvoke，回调可能 unregisterService → delete entry。
    // 返回后必须重新判活（对比指针地址即可，不解引用悬垂值），避免访问已释放的 entry 字段。
    std::map<std::string, LocalServiceEntry*>::iterator alive = local_services_.find(service_name);
    if (alive == local_services_.end() || alive->second != entry) return;

    if (result.status == InvokeDispatchStatus::SUCCESS) {
        Message reply = makeInvokeSuccessReply(msg.getSequence(), result.response);
        if (entry->diag_enabled && entry->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, DIAG_EVENT_RESPONSE, reply);
            broadcastInternal(entry->diag_topic_id, diag_buf);
        }
        if (transport && !sendOnFd(transport, reply)) {
            OMNI_LOG_WARN(LOG_TAG, "invoke_reply_send_failed service=%s fd=%d seq=%u status=0",
                          service_name.c_str(), client_fd, msg.getSequence());
        }
    } else {
        Message reply = makeInvokeErrorReply(msg.getSequence(), static_cast<ErrorCode>(result.error_code));
        if (entry->diag_enabled && entry->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, DIAG_EVENT_RESPONSE, reply);
            broadcastInternal(entry->diag_topic_id, diag_buf);
        }
        if (transport && !sendOnFd(transport, reply)) {
            OMNI_LOG_WARN(LOG_TAG, "invoke_reply_send_failed service=%s fd=%d seq=%u status=%d",
                          service_name.c_str(), client_fd, msg.getSequence(), result.error_code);
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
    if (it->second->diag_enabled && it->second->diag_topic_id != 0) {
        Buffer diag_buf;
        diag_serialize_event(diag_buf, DIAG_EVENT_ONE_WAY, msg);
        broadcastInternal(it->second->diag_topic_id, diag_buf);
    }
    emitDiagEvent(DIAG_EVENT_ONE_WAY, msg);

    InvokeDispatchResult result = dispatchLocalInvoke(service, msg, transport_label, service_name.c_str());
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
    // Diagnostic intercept
    {
        uint32_t diag_iface_id = 0;
        uint32_t diag_idl_hash = 0;
        uint32_t diag_method_id = 0;
        uint32_t diag_payload_len = 0;
        BufferView check_buf(msg.payload.data(), msg.payload.size());
        if (check_buf.tryReadUint32(diag_iface_id) && check_buf.tryReadUint32(diag_idl_hash)
            && check_buf.tryReadUint32(diag_method_id) && check_buf.tryReadUint32(diag_payload_len)) {
            if (diag_iface_id == OMNI_DIAG_IFACE_ID) {
                Buffer request;
                if (diag_payload_len > 0) {
                    // 防御：payload_len 来自网络，必须不超过剩余可读字节，否则 writeRaw 越界读
                    if (diag_payload_len > check_buf.remaining()) {
                        InvokeDispatchResult result;
                        result.status = InvokeDispatchStatus::DECODE_FAILED;
                        result.error_code = static_cast<int>(ErrorCode::ERR_DESERIALIZE);
                        return result;
                    }
                    if (!request.writeRaw(check_buf.data() + check_buf.readPosition(), diag_payload_len)) {
                        InvokeDispatchResult result;
                        result.status = InvokeDispatchStatus::DECODE_FAILED;
                        result.error_code = static_cast<int>(ErrorCode::ERR_DESERIALIZE);
                        return result;
                    }
                }
                InvokeDispatchResult result;
                result.status = InvokeDispatchStatus::SUCCESS;
                result.error_code = 0;
                LocalServiceEntry* entry = nullptr;
                for (auto& kv : local_services_) { if (kv.second->service == service) { entry = kv.second; break; } }
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
                        int pub_ret = publishTopicInternal(diag_topic);
                        if (pub_ret != 0) {
                            result.status = InvokeDispatchStatus::INVOKE_FAILED;
                            result.error_code = pub_ret;
                        } else {
                            tid = topic_runtime_.getTopicId(diag_topic);
                        }
                    }
                    if (tid != 0) {
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
        }
    }

    InvokeDispatchResult result;
    result.error_code = 0;

    uint32_t interface_id = 0;
    uint32_t client_idl_hash = 0;
    uint32_t method_id = 0;
    Buffer request;
    if (!decodeInvokePayload(msg, interface_id, client_idl_hash, method_id, request)) {
        OMNI_LOG_WARN(LOG_TAG,
                      "malformed_invoke_payload service=%s transport=%s seq=%u err=%d",
                      service_name, transport_label, msg.getSequence(),
                      static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        result.status = InvokeDispatchStatus::DECODE_FAILED;
        result.error_code = static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        return result;
    }
    if (service->interfaceInfo().interface_id != interface_id) {
        result.status = InvokeDispatchStatus::INTERFACE_MISMATCH;
        result.error_code = static_cast<int>(ErrorCode::ERR_INTERFACE_NOT_FOUND);
        return result;
    }

    if (client_idl_hash != 0) {
        uint32_t server_idl_hash = 0;
        const InterfaceInfo& iface = service->interfaceInfo();
        for (size_t i = 0; i < iface.methods.size(); ++i) {
            if (iface.methods[i].method_id == method_id) {
                server_idl_hash = iface.methods[i].idl_hash;
                break;
            }
        }
        if (server_idl_hash != 0 && client_idl_hash != server_idl_hash) {
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

void OmniRuntime::Impl::onSubscribeBroadcast(int client_fd, const Message& msg,
                                                 const char* transport_label) {
    uint32_t topic_id = 0;
    std::string topic_name;
    if (!decodeSubscribeBroadcastPayload(msg, topic_id, topic_name)) {
        OMNI_LOG_WARN(LOG_TAG,
                      "malformed_subscribe_broadcast transport=%s seq=%u err=%d",
                      transport_label,
                      msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        return;
    }
    {
        std::map<int, std::string>::iterator fd_it = client_fd_to_service_.find(client_fd);
        if (fd_it != client_fd_to_service_.end()) {
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(fd_it->second);
            if (eit != local_services_.end() && eit->second->diag_enabled && eit->second->diag_topic_id != 0) {
                Buffer diag_buf;
                diag_serialize_event(diag_buf, DIAG_EVENT_SUBSCRIBE, msg);
                broadcastInternal(eit->second->diag_topic_id, diag_buf);
            }
        }
    }
    topic_runtime_.addTcpSubscriber(topic_id, client_fd);
    OMNI_LOG_INFO(LOG_TAG, "Broadcast subscriber fd=%d added for topic %s (id=0x%08x)",
                    client_fd, topic_name.c_str(), topic_id);
}

void OmniRuntime::Impl::removeServiceClient(const std::string& service_name, int client_fd) {
    loop_->removeFd(client_fd);
    client_fd_to_service_.erase(client_fd);
    
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it != local_services_.end()) {
        LocalServiceEntry* entry = it->second;
        topic_runtime_.removeTcpSubscriberFd(client_fd);
        std::map<int, ITransport*>::iterator tit = entry->client_transports.find(client_fd);
        if (tit != entry->client_transports.end()) {
            tit->second->close();
            delete tit->second;
            entry->client_transports.erase(tit);
        }
        std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_fd);
        if (bit != entry->client_recv_buffers.end()) {
            delete bit->second;
            entry->client_recv_buffers.erase(bit);
        }
        if (entry->service) {
            entry->service->onClientDisconnected("fd=" + std::to_string(client_fd));
        }
    }
}

// ============================================================
// ConnectionManager 回调
// ============================================================

void OmniRuntime::Impl::onDirectMessage(const std::string& service_name, const Message& msg) {
    MessageType type = msg.getType();
    uint32_t seq = msg.getSequence();

    // 只有 MSG_INVOKE_REPLY 才可能消费为 RPC 回复。其余消息类型（MSG_BROADCAST、
    // MSG_HEARTBEAT_ACK 等主动消息）即使 seq 恰好命中正在等待的槽，也绝不进入
    // pending 槽——否则会被误当成 invoke 回复（seq 碰撞应答错配）。
    if (type == MessageType::MSG_INVOKE_REPLY) {
        if (storeAndConsumeReply(seq, msg)) {
            return;
        }
    }

    switch (type) {
    case MessageType::MSG_BROADCAST: {
        uint32_t topic_id = 0;
        Buffer data;
        if (!decodeBroadcastPayload(msg, topic_id, data)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "malformed_broadcast transport=direct seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        emitDiagEvent(DIAG_EVENT_BROADCAST, msg);
        topic_runtime_.dispatch(topic_id, data);
        break;
    }
    case MessageType::MSG_HEARTBEAT_ACK: {
        std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
        if (it != heartbeat_states_.end()) {
            it->second.last_ack_time = platform::currentTimeMs();
            it->second.pending = false;
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled direct message from %s: %s",
                         service_name.c_str(), messageTypeToString(type));
        break;
    }
}

void OmniRuntime::Impl::onDirectDisconnect(const std::string& service_name,
                                           bool is_topic_publisher,
                                           const std::string& topic_name) {
    OMNI_LOG_WARN(LOG_TAG, "Direct connection to %s lost", service_name.c_str());
    stats_.connection_errors++;
    service_cache_.erase(service_name);
    if (conn_mgr_) {
        conn_mgr_->removeConnection(service_name);
    }
    pauseHeartbeat(service_name);
    
    // Trigger reconnect if auto-reconnect is enabled for this service
    std::map<std::string, ReconnectConfig>::iterator rc_it = reconnect_configs_.find(service_name);
    if (rc_it != reconnect_configs_.end() && rc_it->second.enabled) {
        rc_it->second.current_retry = 0;
        scheduleReconnect(service_name, rc_it->second.interval_ms);
    }
    
    // 话题发布者专属连接断开时，清理订阅状态（fix #12）。
    // 通过连接上传递的结构化标记判定，而非解析 "topic_pub_" 连接名前缀。
    if (is_topic_publisher) {
        topic_runtime_.forgetSubscription(topic_name);
        OMNI_LOG_WARN(LOG_TAG, "Topic publisher for '%s' disconnected, subscription cleared",
                        topic_name.c_str());
        // 连接断开是订阅方最明确的错误事件：通知订阅者的 on_err 回调
        topic_runtime_.notifyError(fnv1a_32(topic_name), ErrorCode::ERR_CONNECTION_CLOSED);
    }
}

// ============================================================
// SHM 服务端请求处理（由 eventfd 回调触发）
// ============================================================

void OmniRuntime::Impl::onShmRequest(const std::string& service_name,
                                            LocalServiceEntry* entry,
                                            uint32_t client_id,
                                            const uint8_t* data, size_t length) {
    if (!entry || !entry->service) return;

    if (length < MESSAGE_HEADER_SIZE) return;
    MessageHeader hdr;
    if (!Message::parseHeader(data, length, hdr) || !Message::validateHeader(hdr)) return;

    Message msg;
    msg.header = hdr;
    if (length < MESSAGE_HEADER_SIZE + hdr.length) {
        OMNI_LOG_WARN(LOG_TAG, "incomplete SHM message for service=%s: need=%zu have=%zu",
                      service_name.c_str(), MESSAGE_HEADER_SIZE + static_cast<size_t>(hdr.length), length);
        return;
    }
    if (hdr.length > 0) {
        msg.payload.assign(data + MESSAGE_HEADER_SIZE, hdr.length);
    }

    switch (msg.getType()) {
    case MessageType::MSG_HEARTBEAT: {
        Message ack(MessageType::MSG_HEARTBEAT_ACK, msg.getSequence());
        Buffer ack_buf;
        if (ack.serialize(ack_buf) && entry->shm_transport) {
            entry->shm_transport->serverSend(client_id, ack_buf.data(), ack_buf.size());
        }
        break;
    }
    case MessageType::MSG_INVOKE: {
        if (entry->diag_enabled && entry->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, DIAG_EVENT_REQUEST, msg);
            broadcastInternal(entry->diag_topic_id, diag_buf);
        }
        emitDiagEvent(DIAG_EVENT_REQUEST, msg);
        InvokeDispatchResult result = dispatchLocalInvoke(entry->service, msg, "SHM",
                                                          entry->service->serviceName());

        // dispatchLocalInvoke 执行用户 onInvoke，回调可能 unregisterService 释放 entry，
        // 返回后重新判活（对比指针地址，不解引用悬垂值），避免访问已释放的 entry 字段
        {
            std::map<std::string, LocalServiceEntry*>::iterator alive = local_services_.find(service_name);
            if (alive == local_services_.end() || alive->second != entry) return;
        }

        Message reply = result.status == InvokeDispatchStatus::SUCCESS
            ? makeInvokeSuccessReply(msg.getSequence(), result.response)
            : makeInvokeErrorReply(msg.getSequence(),
                                   static_cast<ErrorCode>(result.error_code));
        if (entry->diag_enabled && entry->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, DIAG_EVENT_RESPONSE, reply);
            broadcastInternal(entry->diag_topic_id, diag_buf);
        }
        Buffer send_buf;
        if (!reply.serialize(send_buf)) {
            OMNI_LOG_ERROR(LOG_TAG,
                           "shm_reply_serialize_failed service=%s client_id=%u seq=%u",
                           entry->service->serviceName(), client_id, msg.getSequence());
            break;
        }
        if (entry->shm_transport) {
            int ret = entry->shm_transport->serverSend(client_id, send_buf.data(),
                                                       send_buf.size());
            if (ret <= 0) {
                OMNI_LOG_ERROR(LOG_TAG,
                               "shm_reply_send_failed service=%s client_id=%u seq=%u ret=%d",
                               entry->service->serviceName(), client_id,
                               msg.getSequence(), ret);
            }
        }
        break;
    }
    case MessageType::MSG_INVOKE_ONEWAY:
        if (entry->diag_enabled && entry->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, DIAG_EVENT_ONE_WAY, msg);
            broadcastInternal(entry->diag_topic_id, diag_buf);
        }
        emitDiagEvent(DIAG_EVENT_ONE_WAY, msg);
        (void)dispatchLocalInvoke(entry->service, msg, "SHM",
                                  entry->service->serviceName());
        break;
    case MessageType::MSG_SUBSCRIBE_BROADCAST: {
        uint32_t topic_id = 0;
        std::string topic_name;
        if (!decodeSubscribeBroadcastPayload(msg, topic_id, topic_name)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "malformed_subscribe_broadcast transport=SHM seq=%u service=%s err=%d",
                          msg.getSequence(), service_name.c_str(),
                          static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        if (entry->diag_enabled && entry->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, DIAG_EVENT_SUBSCRIBE, msg);
            broadcastInternal(entry->diag_topic_id, diag_buf);
        }
        topic_runtime_.addShmSubscriberService(topic_id, service_name, client_id);
        OMNI_LOG_INFO(LOG_TAG,
                      "SHM broadcast subscriber for topic %s (0x%08x) on %s",
                      topic_name.c_str(), topic_id, service_name.c_str());
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled SHM message for %s: %s",
                       service_name.c_str(), messageTypeToString(msg.getType()));
        break;
    }
}

} // namespace omnibinder
