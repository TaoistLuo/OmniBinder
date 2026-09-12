#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#include <algorithm>
#include <utility>

#define LOG_TAG "OmniRuntimeTopic"

namespace omnibinder {

// ============================================================
// 死亡通知
// ============================================================

int OmniRuntime::Impl::subscribeServiceDeath(const std::string& service_name,
                                               const DeathCallback& callback) {
    return callSerialized([this, &service_name, &callback]() -> int {
        return subscribeServiceDeathInternal(service_name, callback);
    });
}

int OmniRuntime::Impl::subscribeServiceDeathInternal(const std::string& service_name,
                                                    const DeathCallback& callback) {
    Message msg(MessageType::MSG_SUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(service_name);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    ServiceState& svc_state = ensureServiceState(service_name);
    svc_state.death_cb = callback;
    svc_state.has_death = true;
    return 0;
}

int OmniRuntime::Impl::unsubscribeServiceDeath(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
        return unsubscribeServiceDeathInternal(service_name);
    });
}

int OmniRuntime::Impl::unsubscribeServiceDeathInternal(const std::string& service_name) {
    Message msg(MessageType::MSG_UNSUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(service_name);
    sendToSM(msg);
    ServiceState* svc_state = findServiceState(service_name);
    if (svc_state) {
        svc_state->death_cb = DeathCallback();
        svc_state->has_death = false;
        eraseServiceStateIfUnused(service_name);
    }
    return 0;
}

// ============================================================
// 话题
// ============================================================

int OmniRuntime::Impl::publishTopic(const std::string& topic_name, uint32_t idl_hash) {
    return callSerialized([this, &topic_name, idl_hash]() -> int {
        return publishTopicInternal(topic_name, idl_hash);
    });
}

int OmniRuntime::Impl::publishTopicInternal(const std::string& topic_name, uint32_t idl_hash) {
    if (!initialized_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }

    Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    // 构建 publisher ServiceInfo 供 SM 告知订阅者连接地址。
    // 使用第一个已注册本地服务的监听端口；若没有本地服务则返回 ERR_SERVICE_NOT_FOUND
    // （与 initDiagDataService 的差异路径不同：它会在 publish 前先注册一个专用诊断服务，保证存在本地服务）。
    ServiceInfo pub_info;
    pub_info.host_id = host_id_;
    
    if (!local_services_.empty()) {
        LocalServiceEntry* entry = local_services_.begin()->second;
        pub_info.name = local_services_.begin()->first;
        pub_info.host = listenerAdvertiseHost();
        pub_info.port = entry->port;
        if (entry->service) {
            pub_info.shm_config = entry->service->shmConfig();
        }
    } else {
        OMNI_LOG_WARN(LOG_TAG, "publishTopic requires a registered local service to advertise publisher endpoint");
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    if (!serializeServiceInfo(pub_info, msg.payload)) {
        OMNI_LOG_ERROR(LOG_TAG, "serialize_publish_topic_payload_failed topic=%s",
                       topic_name.c_str());
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    if (!msg.payload.writeUint32(idl_hash)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    topic_runtime_.rememberPublishedTopic(topic_name, topic_id, pub_info.name, idl_hash);
    return 0;
}

int OmniRuntime::Impl::broadcast(uint32_t topic_id, const Buffer& data) {
    return callSerialized([this, topic_id, &data]() -> int {
        return broadcastInternal(topic_id, data);
    });
}

int OmniRuntime::Impl::broadcastInternal(uint32_t topic_id, const Buffer& data) {
    Message msg(MessageType::MSG_BROADCAST, 0);
    msg.payload.writeUint32(topic_id);
    msg.payload.writeUint32(static_cast<uint32_t>(data.size()));
    if (data.size() > 0) {
        msg.payload.writeRaw(data.data(), data.size());
    }

    // 诊断 BROADCAST 钩子（diag topic 跳过，防止递归）
    if (diag_active_count_ > 0 && !isDiagDataTopic(topic_id)) {
        // 先拷贝服务名再遍历：emitDiagHook 内部会广播，广播的 TCP 部分写失败路径
        // 会执行断连用户回调并可能注销服务，持 local_services_ 迭代器会失效（约束 2）
        std::vector<std::string> diag_service_names;
        bool is_diag_topic = false;
        for (std::map<std::string, LocalServiceEntry*>::iterator kv = local_services_.begin();
             kv != local_services_.end(); ++kv) {
            if (kv->second->diag_topic_id == topic_id) {
                is_diag_topic = true;
                break;
            }
            diag_service_names.push_back(kv->first);
        }
        if (!is_diag_topic) {
            for (size_t i = 0; i < diag_service_names.size(); ++i) {
                std::map<std::string, LocalServiceEntry*>::iterator eit =
                    local_services_.find(diag_service_names[i]);
                if (eit == local_services_.end()) continue;
                emitDiagHook(eit->second, DIAG_EVENT_BROADCAST, msg);
            }
        }
    }
    if (!isDiagDataTopic(topic_id)) {
        emitDiagEvent(DIAG_EVENT_BROADCAST, msg);
    }

    Buffer send_buf;
    if (!msg.serialize(send_buf)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

    // 遍历订阅列表期间不得执行用户代码：onServiceClientDisconnected 会移除订阅
    // 条目并可能经 onClientDisconnected 注销服务（约束 1/2）。TCP 部分写/错误
    // 先收集，两个订阅列表遍历完再统一走正常断开路径。
    std::vector<std::pair<std::string, int> > broken_tcp_clients;

    const std::vector<int>& tcp_subscribers = topic_runtime_.tcpSubscribers(topic_id);
    if (!tcp_subscribers.empty()) {
        OMNI_LOG_DEBUG(LOG_TAG, "Broadcast topic 0x%08x over TCP to %zu subscribers",
                        topic_id, tcp_subscribers.size());
        // 逆序遍历，便于安全删除陈旧条目
        for (int i = static_cast<int>(tcp_subscribers.size()) - 1; i >= 0; --i) {
            int client_id = tcp_subscribers[i];
            std::map<int, std::string>::iterator sit = client_id_to_service_.find(client_id);
            if (sit == client_id_to_service_.end()) {
                topic_runtime_.removeTcpSubscriber(topic_id, client_id);
                continue;
            }
            const std::string service_name = sit->second;
            std::map<std::string, LocalServiceEntry*>::iterator eit =
                local_services_.find(service_name);
            if (eit == local_services_.end()) {
                topic_runtime_.removeTcpSubscriber(topic_id, client_id);
                continue;
            }
            std::map<int, IClientTransport*>::iterator tit =
                eit->second->clients.find(client_id);
            if (tit == eit->second->clients.end() || !tit->second) {
                topic_runtime_.removeTcpSubscriber(topic_id, client_id);
                continue;
            }
            // TCP 为字节流：send 的部分写字节已进入流中，会让订阅端帧错位。
            // timeout=0 的 sendAll 只尝试一次（不等待、不阻塞 owner loop），
            // 失败即视为该连接损坏，由断开路径回收；订阅端重连后重放订阅
            if (tit->second->sendAll(send_buf.data(), send_buf.size(), 0, NULL) != 0) {
                OMNI_LOG_WARN(LOG_TAG,
                              "broadcast_tcp_send_incomplete client=%d service=%s disconnect",
                              client_id, service_name.c_str());
                broken_tcp_clients.push_back(std::make_pair(service_name, client_id));
            }
        }
    }

    // SHM 订阅者：send 要么整帧写入、要么返回 0（ring 满）；ring 满只丢帧，不断开
    {
        const std::vector<TopicRuntime::ShmSubscriber>& shm_subscribers = topic_runtime_.shmSubscribers(topic_id);
        for (size_t i = 0; i < shm_subscribers.size(); ++i) {
            const std::string service_name = shm_subscribers[i].service_name;
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(service_name);
            if (eit == local_services_.end()) {
                continue;
            }
            std::map<int, IClientTransport*>::iterator tit =
                eit->second->clients.find(static_cast<int>(shm_subscribers[i].client_id));
            if (tit == eit->second->clients.end() || !tit->second) {
                continue;
            }
            int sent = tit->second->send(send_buf.data(), send_buf.size());
            if (sent != static_cast<int>(send_buf.size())) {
                OMNI_LOG_DEBUG(LOG_TAG, "broadcast dropped for shm client=%u (ring full)",
                               shm_subscribers[i].client_id);
            }
        }
    }

    for (size_t i = 0; i < broken_tcp_clients.size(); ++i) {
        onServiceClientDisconnected(broken_tcp_clients[i].first, broken_tcp_clients[i].second);
    }

    return 0;
}

int OmniRuntime::Impl::subscribeTopic(const std::string& topic_name, uint32_t expected_idl_hash,
                                       const TopicCallback& on_message,
                                       const TopicErrorCallback& on_error) {
    return callSerialized([this, &topic_name, expected_idl_hash, &on_message, &on_error]() -> int {
        int ret = subscribeTopicInternal(topic_name, expected_idl_hash, on_message);
        if (ret == 0) {
            topic_runtime_.setErrorCallback(topic_name, on_error);
        }
        return ret;
    });
}

int OmniRuntime::Impl::subscribeTopicInternal(const std::string& topic_name,
                                             uint32_t expected_idl_hash,
                                             const TopicCallback& callback) {
    // 期望哈希必须在发起 SM 请求前登记：SM 可能在应答之后紧接推送
    // MSG_TOPIC_PUBLISHER_NOTIFY，且两者可能在同一次 socket 读取中被连续分发，
    // 通知处理需要能查到期望哈希才能完成严格校验
    topic_runtime_.setExpectedSubscriptionHash(topic_name, expected_idl_hash);

    Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) {
        topic_runtime_.forgetSubscription(topic_name);
        return ret;
    }

    Buffer reply_payload(reply.payload.data(), reply.payload.size());
    bool accepted = false;
    if (!reply_payload.tryReadBool(accepted)) {
        topic_runtime_.forgetSubscription(topic_name);
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        topic_runtime_.forgetSubscription(topic_name);
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    uint32_t publisher_idl_hash = 0;
    if (reply_payload.remaining() >= sizeof(uint32_t)
        && !reply_payload.tryReadUint32(publisher_idl_hash)) {
        topic_runtime_.forgetSubscription(topic_name);
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }

    // 严格 topic 哈希校验：发布者已上线且声明了哈希时，必须与订阅者期望一致。
    // 发布者尚未上线（SM 返回 0）时延迟到 MSG_TOPIC_PUBLISHER_NOTIFY 再校验。
    if (expected_idl_hash != 0 && publisher_idl_hash != 0
        && publisher_idl_hash != expected_idl_hash) {
        OMNI_LOG_WARN(LOG_TAG,
                      "topic_idl_mismatch topic=%s expected_hash=0x%08x publisher_hash=0x%08x",
                      topic_name.c_str(), expected_idl_hash, publisher_idl_hash);
        unsubscribeTopicInternal(topic_name);
        return static_cast<int>(ErrorCode::ERR_IDL_MISMATCH);
    }

    topic_runtime_.rememberSubscription(topic_name, callback, expected_idl_hash);
    return 0;
}

int OmniRuntime::Impl::unsubscribeTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
        return unsubscribeTopicInternal(topic_name);
    });
}

int OmniRuntime::Impl::unsubscribeTopicInternal(const std::string& topic_name) {
    Message msg(MessageType::MSG_UNSUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    sendToSM(msg);
    topic_runtime_.forgetSubscription(topic_name);
    // 同步移除连接上的订阅绑定：否则直连重连会重放已取消的订阅
    removeTopicSubscriptionBinding(topic_name);
    return 0;
}

} // namespace omnibinder
