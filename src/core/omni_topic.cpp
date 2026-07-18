#include "core/omni_runtime.h"
#include "core/omni_runtime_helpers.h"
#include "omnibinder/buffer_view.h"
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
    
    // Build publisher ServiceInfo so SM can tell subscribers where to connect.
    // Use the first registered local service's listen port. If no local service
    // is registered, create a dedicated publisher listener.
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
    if (diag_active_count_ > 0) {
        bool is_diag_topic = false;
        for (auto& kv : local_services_) {
            if (kv.second->diag_topic_id == topic_id) { is_diag_topic = true; break; }
        }
        if (!is_diag_topic) {
            for (auto& kv : local_services_) {
                if (kv.second->diag_enabled && kv.second->diag_topic_id != 0) {
                    Buffer diag_buf;
                    diag_serialize_event(diag_buf, DIAG_EVENT_BROADCAST, msg);
                    broadcastInternal(kv.second->diag_topic_id, diag_buf);
                }
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

void OmniRuntime::Impl::onTopicBroadcastMessage(const Message& msg) {
    uint32_t topic_id = 0;
    Buffer data;
    if (!decodeBroadcastPayload(msg, topic_id, data)) {
        OMNI_LOG_WARN(LOG_TAG, "Invalid topic broadcast payload: topic=0x%08x len=%u remaining=%zu",
                        topic_id, msg.payload.size(), static_cast<size_t>(0));
        return;
    }

    emitDiagEvent(DIAG_EVENT_BROADCAST, msg);
    topic_runtime_.dispatch(topic_id, data);
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
