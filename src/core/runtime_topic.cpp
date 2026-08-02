#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

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

    death_callbacks_[service_name] = callback;
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
    death_callbacks_.erase(service_name);
    return 0;
}

// ============================================================
// 话题
// ============================================================

int OmniRuntime::Impl::publishTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
        return publishTopicInternal(topic_name);
    });
}

int OmniRuntime::Impl::publishTopicInternal(const std::string& topic_name) {
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
        pub_info.host = normalizeAdvertiseHost(platform::getSocketAddress(entry->server->fd()));
        pub_info.port = entry->port;
        if (entry->service) {
            pub_info.shm_config = entry->service->shmConfig();
        }
    } else {
        OMNI_LOG_WARN(LOG_TAG, "publishTopic requires a registered local service to advertise publisher endpoint");
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    serializeServiceInfo(pub_info, msg.payload);
    
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
    topic_runtime_.rememberPublishedTopic(topic_name, topic_id, pub_info.name);
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

    // Diagnostic BROADCAST hook (skip for diag topics to prevent recursion)
    if (diag_active_count_ > 0 && !isDiagDataTopic(topic_id)) {
        bool is_diag_topic = false;
        for (auto& kv : local_services_) {
            if (kv.second->diag_topic_id == topic_id) { is_diag_topic = true; break; }
        }
        if (!is_diag_topic) {
            for (auto& kv : local_services_) {
                emitDiagHook(kv.second, DIAG_EVENT_BROADCAST, msg);
            }
        }
    }
    if (!isDiagDataTopic(topic_id)) {
        emitDiagEvent(DIAG_EVENT_BROADCAST, msg);
    }

    Buffer send_buf;
    msg.serialize(send_buf);
    
    const std::vector<int>& tcp_subscribers = topic_runtime_.tcpSubscribers(topic_id);
    if (!tcp_subscribers.empty()) {
        OMNI_LOG_DEBUG(LOG_TAG, "Broadcast topic 0x%08x over TCP to %zu subscribers",
                        topic_id, tcp_subscribers.size());
        // Iterate in reverse so we can safely erase stale entries
        for (int i = static_cast<int>(tcp_subscribers.size()) - 1; i >= 0; --i) {
            int fd = tcp_subscribers[i];
            std::map<int, std::string>::iterator sit = client_fd_to_service_.find(fd);
            if (sit == client_fd_to_service_.end()) {
                topic_runtime_.removeTcpSubscriber(topic_id, fd);
                continue;
            }
            std::string svc = sit->second;
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(svc);
            if (eit != local_services_.end()) {
                std::map<int, ITransport*>::iterator tit = eit->second->client_transports.find(fd);
                if (tit != eit->second->client_transports.end()) {
                    // Use sendOnFd for proper partial-write handling (fix #1)
                    Message broadcast_msg;
                    broadcast_msg.header = msg.header;
                    broadcast_msg.payload.assign(msg.payload.data(), msg.payload.size());
                    if (!sendOnFd(tit->second, broadcast_msg)) {
                        OMNI_LOG_WARN(LOG_TAG, "broadcast send failed to fd=%d", fd);
                    }
                }
            }
        }
    }

    // Broadcast via SHM to all SHM-connected subscribers
    {
        const std::vector<TopicRuntime::ShmSubscriber>& shm_subscribers = topic_runtime_.shmSubscribers(topic_id);
        for (size_t i = 0; i < shm_subscribers.size(); ++i) {
            const std::string& svc_name = shm_subscribers[i].service_name;
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(svc_name);
            if (eit != local_services_.end() && eit->second->shm_transport) {
                eit->second->shm_transport->serverSend(shm_subscribers[i].client_id,
                                                       send_buf.data(), send_buf.size());
            }
        }
    }

    return 0;
}

int OmniRuntime::Impl::subscribeTopic(const std::string& topic_name,
                                       const TopicCallback& on_message,
                                       const TopicErrorCallback& on_error) {
    return callSerialized([this, &topic_name, &on_message, &on_error]() -> int {
        int ret = subscribeTopicInternal(topic_name, on_message);
        if (ret == 0) {
            topic_runtime_.setErrorCallback(topic_name, on_error);
        }
        return ret;
    });
}

int OmniRuntime::Impl::subscribeTopicInternal(const std::string& topic_name,
                                             const TopicCallback& callback) {
    Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    Buffer reply_payload(reply.payload.data(), reply.payload.size());
    bool accepted = false;
    if (!reply_payload.tryReadBool(accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    uint32_t publisher_idl_hash = 0;
    reply_payload.tryReadUint32(publisher_idl_hash);
    (void)publisher_idl_hash;

    topic_runtime_.rememberSubscription(topic_name, callback);
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
    return 0;
}

} // namespace omnibinder
