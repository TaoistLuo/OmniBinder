#include "core/omni_runtime.h"

// ============================================================
// SM 通信
// ============================================================

bool OmniRuntime::Impl::sendToSM(const Message& msg) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        return false;
    }
    return sm_channel_.sendMessage(msg);
}

bool OmniRuntime::Impl::sendToSMWithinTimeout(const Message& msg, uint32_t timeout_ms,
                                              uint32_t* elapsed_ms) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        if (elapsed_ms) {
            *elapsed_ms = 0;
        }
        return false;
    }
    return sm_channel_.sendMessageWithinTimeout(msg, timeout_ms, elapsed_ms);
}

int OmniRuntime::Impl::sendSMRequestAndWaitReply(Message& msg, Message& reply) {
    return sendSMRequestAndWaitReply(msg, reply, 0);
}

int OmniRuntime::Impl::sendSMRequestAndWaitReply(Message& msg, Message& reply,
                                                 uint32_t timeout_ms) {
    uint32_t total_timeout_ms = effectiveTimeout(timeout_ms);
    uint32_t send_elapsed_ms = 0;
    if (!sendToSMWithinTimeout(msg, total_timeout_ms, &send_elapsed_ms)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    uint32_t reply_timeout_ms = total_timeout_ms > send_elapsed_ms
        ? total_timeout_ms - send_elapsed_ms : 0;
    return waitForReply(msg.getSequence(), reply_timeout_ms, reply);
}

std::string OmniRuntime::Impl::runtimeProcessName() const {
    std::string process_name = platform::getProcessName();
    if (!process_name.empty()) {
        return process_name;
    }

    std::ostringstream os;
    os << "pid-" << platform::getPid();
    return os.str();
}

std::string OmniRuntime::Impl::diagDataServiceName(uint32_t pid) const {
    std::ostringstream os;
    os << "__diag_pid_" << pid;
    return os.str();
}

bool OmniRuntime::Impl::initDiagDataService() {
    if (diag_watch_topic_id_ != 0) {
        return true;
    }
    std::string name = diagDataServiceName(pid_);
    RuntimeDiagService* service = NULL;
    if (local_services_.empty()) {
        service = new RuntimeDiagService(name);
        service->setShmConfig(ShmConfig(SHM_DEFAULT_REQ_RING_CAPACITY, SHM_DEFAULT_RESP_RING_CAPACITY));
        int register_ret = registerServiceInternal(service);
        if (register_ret != 0) {
            delete service;
            return false;
        }
        diag_data_service_ = service;
    }
    int ret = publishTopicInternal(name);
    if (ret != 0) {
        if (service) {
            unregisterServiceInternal(service);
            delete service;
            diag_data_service_ = NULL;
        }
        return false;
    }
    diag_watch_topic_id_ = fnv1a_32(name);
    return true;
}

void OmniRuntime::Impl::destroyDiagDataService() {
    std::string name = diagDataServiceName(pid_);
    if (diag_watch_topic_id_ != 0) {
        Message unpublish(MessageType::MSG_UNPUBLISH_TOPIC, allocSequence());
        unpublish.payload.writeString(name);
        sendToSM(unpublish);
        topic_runtime_.forgetPublishedTopic(name);
        diag_watch_topic_id_ = 0;
    }

    if (diag_data_service_) {
        Service* service = diag_data_service_;
        diag_data_service_ = NULL;
        unregisterServiceInternal(service);
        delete service;
    }
}

bool OmniRuntime::Impl::isDiagDataTopic(uint32_t topic_id) const {
    return diag_watch_topic_id_ != 0 && topic_id == diag_watch_topic_id_;
}

int OmniRuntime::Impl::sendRuntimeHello() {
    Message msg(MessageType::MSG_RUNTIME_HELLO, allocSequence());
    RuntimeInfo info;
    info.pid = pid_;
    info.process_name = process_name_;
    info.log_level = static_cast<uint32_t>(g_omni_log_level);
    info.diag_capabilities = RUNTIME_DIAG_CAP_WATCH;
    serializeRuntimeInfo(info, msg.payload);

    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) {
        return ret;
    }
    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted) || !accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    return 0;
}

void OmniRuntime::Impl::onSMData(int fd, uint32_t events) {
    (void)fd; (void)events;
    uint8_t buf[4096];
    int ret = sm_channel_.recvSome(buf, sizeof(buf));
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "sm_connection_lost host=%s port=%u err=%d",
                       sm_host_.c_str(), sm_port_, static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED));
        diag_watch_active_ = false;
        sm_reconnect_needed_ = true;
        if (sm_channel_.transport && sm_channel_.transport->fd() >= 0) {
            loop_->removeFd(sm_channel_.transport->fd());
        }
        if (sm_channel_.transport) {
            sm_channel_.transport->close();
            delete sm_channel_.transport;
            sm_channel_.transport = NULL;
        }
        return;
    }
    if (ret == 0) return;
    sm_channel_.appendReceived(buf, static_cast<size_t>(ret));
    processSMMessages();
}

void OmniRuntime::Impl::processSMMessages() {
    Message msg;
    while (sm_channel_.tryPopMessage(msg)) {
        onSMMessage(msg);
    }
}

void OmniRuntime::Impl::onSMMessage(const Message& msg) {
    MessageType type = msg.getType();
    uint32_t seq = msg.getSequence();

    if (type == MessageType::MSG_LOOKUP_REPLY
        || type == MessageType::MSG_LIST_SERVICES_REPLY
        || type == MessageType::MSG_QUERY_INTERFACES_REPLY
        || type == MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY
        || type == MessageType::MSG_SUBSCRIBE_SERVICE_REPLY
        || type == MessageType::MSG_REGISTER_REPLY
        || type == MessageType::MSG_UNREGISTER_REPLY
        || type == MessageType::MSG_PUBLISH_TOPIC_REPLY
        || type == MessageType::MSG_SUBSCRIBE_TOPIC_REPLY
        || type == MessageType::MSG_RUNTIME_HELLO_REPLY
        || type == MessageType::MSG_RUNTIME_LIST_REPLY
        || type == MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY
        || type == MessageType::MSG_DIAG_WATCH_START_REPLY
        || type == MessageType::MSG_DIAG_WATCH_STOP_REPLY) {
        storePendingReply(seq, msg);
        if (sm_channel_.pendingReply(seq) != NULL) {
            return;
        }
    }
    
    if (sm_channel_.isWaiting(seq)) {
        storePendingReply(seq, msg);
        return;
    }
    
    switch (type) {
    case MessageType::MSG_DIAG_SET_LOG_LEVEL: {
        BufferView buf(msg.payload.data(), msg.payload.size());
        uint32_t level = 0;
        bool ok = buf.tryReadUint32(level) && level <= static_cast<uint32_t>(OMNI_LOG_OFF);
        if (ok) {
            setLogLevel(static_cast<LogLevel>(level));
        }
        Message reply(MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY, msg.getSequence());
        reply.payload.writeBool(ok);
        reply.payload.writeUint32(static_cast<uint32_t>(g_omni_log_level));
        sendToSM(reply);
        break;
    }
    case MessageType::MSG_DIAG_WATCH_START: {
        bool ok = initDiagDataService();
        diag_watch_active_ = ok;
        Message reply(MessageType::MSG_DIAG_WATCH_START_REPLY, msg.getSequence());
        reply.payload.writeBool(ok);
        sendToSM(reply);
        break;
    }
    case MessageType::MSG_DIAG_WATCH_STOP: {
        diag_watch_active_ = false;
        destroyDiagDataService();
        Message reply(MessageType::MSG_DIAG_WATCH_STOP_REPLY, msg.getSequence());
        reply.payload.writeBool(true);
        sendToSM(reply);
        break;
    }
    case MessageType::MSG_HEARTBEAT_ACK:
        break;
    case MessageType::MSG_DEATH_NOTIFY: {
        std::string svc_name;
        if (!decodeSingleStringPayload(msg, svc_name)) {
            OMNI_LOG_WARN(LOG_TAG, "malformed_death_notify seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        OMNI_LOG_WARN(LOG_TAG, "Service died: %s", svc_name.c_str());
        std::map<std::string, DeathCallback>::iterator it = death_callbacks_.find(svc_name);
        if (it != death_callbacks_.end() && it->second) {
            it->second(svc_name);
        }
        service_cache_.erase(svc_name);
        conn_mgr_->removeConnection(svc_name);

        pauseHeartbeat(svc_name);

        std::map<std::string, ReconnectConfig>::iterator rc_it = reconnect_configs_.find(svc_name);
        if (rc_it != reconnect_configs_.end() && rc_it->second.enabled) {
            rc_it->second.current_retry = 0;
            scheduleReconnect(svc_name, rc_it->second.interval_ms);
        }
        break;
    }
    case MessageType::MSG_TOPIC_PUBLISHER_NOTIFY: {
        BufferView buf(msg.payload.data(), msg.payload.size());
        std::string topic;
        if (!buf.tryReadString(topic)) {
            OMNI_LOG_WARN(LOG_TAG, "malformed_topic_publisher_notify seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        ServiceInfo pub_info;
        if (!deserializeServiceInfo(buf, pub_info)) {
            OMNI_LOG_ERROR(LOG_TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
            break;
        }
        OMNI_LOG_INFO(LOG_TAG, "Topic %s publisher at %s:%u",
                        topic.c_str(), pub_info.host.c_str(), pub_info.port);
        if (!ensureTopicPublisherConnection(topic, pub_info)) {
            OMNI_LOG_WARN(LOG_TAG, "Failed to establish topic publisher path for %s", topic.c_str());
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled SM message: %s", messageTypeToString(type));
        break;
    }
}

