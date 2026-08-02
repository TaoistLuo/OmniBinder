#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntimeSM"

namespace omnibinder {

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
        if (storeAndConsumeReply(seq, msg)) {
            return;
        }
    }

    // 主动消息（MSG_DEATH_NOTIFY、MSG_TOPIC_PUBLISHER_NOTIFY、MSG_DIAG_* 控制消息等）
    // 即使 seq 恰好命中正在等待的槽，也绝不进入 pending 槽——否则会被误当成
    // 请求应答（seq 碰撞应答错配）。SM 侧主动消息已改用 0x40000000 起分配 seq，
    // 此处类型校验是双端配合的客户端侧兜底。

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
            // 先拷贝到局部变量再调用：回调内可能 unsubscribeServiceDeath erase 该节点，
            // 消除对失效迭代器/容器状态的脆弱依赖
            DeathCallback callback = it->second;
            callback(svc_name);
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
            // 发布者连接建立失败：通知订阅者 on_err 回调
            topic_runtime_.notifyError(fnv1a_32(topic), ErrorCode::ERR_CONNECT_FAILED);
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled SM message: %s", messageTypeToString(type));
        break;
    }
}

// ============================================================
// SM 连接生命周期
// ============================================================

int OmniRuntime::Impl::reconnectServiceManager() {
    stats_.sm_reconnect_attempts++;
    OMNI_LOG_WARN(LOG_TAG, "sm_reconnect_begin host=%s port=%u attempt=%lu",
                  sm_host_.c_str(), sm_port_,
                  static_cast<unsigned long>(stats_.sm_reconnect_attempts));
    if (!loop_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }

    if (sm_channel_.transport && sm_channel_.transport->fd() >= 0) {
        loop_->removeFd(sm_channel_.transport->fd());
    }
    if (sm_channel_.transport) {
        sm_channel_.transport->close();
        delete sm_channel_.transport;
        sm_channel_.transport = NULL;
    }
    sm_channel_.clearReplies();

    sm_channel_.transport = new TcpTransport();
    int ret = sm_channel_.transport->connect(sm_host_, sm_port_);
    if (ret < 0) {
        delete sm_channel_.transport;
        sm_channel_.transport = NULL;
        return static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
    }

    if (ret == 1) {
        platform::waitSocketWritable(sm_channel_.transport->fd(), 1000);
        sm_channel_.transport->checkConnectComplete();
        if (sm_channel_.transport->state() != ConnectionState::CONNECTED) {
            sm_channel_.transport->close();
            delete sm_channel_.transport;
            sm_channel_.transport = NULL;
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }
    }

    loop_->addFd(sm_channel_.transport->fd(), EventLoop::EVENT_READ,
        [this](int fd, uint32_t events) { this->onSMData(fd, events); });
    sm_reconnect_needed_ = false;
    stats_.sm_reconnect_successes++;
    service_cache_.clear();
    if (conn_mgr_) {
        conn_mgr_->closeAll();
    }
    OMNI_LOG_INFO(LOG_TAG, "sm_reconnect_success host=%s port=%u success_count=%lu",
                  sm_host_.c_str(), sm_port_,
                  static_cast<unsigned long>(stats_.sm_reconnect_successes));
    return 0;
}

int OmniRuntime::Impl::restoreControlPlaneState() {
    int hello_ret = sendRuntimeHello();
    if (hello_ret != 0) {
        return hello_ret;
    }

    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        LocalServiceEntry* entry = it->second;
        if (!entry || !entry->service) {
            continue;
        }

        Message msg(MessageType::MSG_REGISTER, allocSequence());
        ServiceInfo svc_info;
        svc_info.name = it->first;
        svc_info.host = resolveRegisterHost(entry->service,
                                            platform::getSocketAddress(entry->server->fd()));
        svc_info.port = entry->port;
        svc_info.host_id = host_id_;
        svc_info.shm_config = entry->service->shmConfig();
        svc_info.interfaces.push_back(entry->service->interfaceInfo());
        serializeServiceInfo(svc_info, msg.payload);
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
        uint32_t handle = 0;
        if (!decodeUint32ReplyPayload(reply, handle) || handle == INVALID_HANDLE) {
            OMNI_LOG_ERROR(LOG_TAG, "restore register failed for %s", it->first.c_str());
            return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
        }
    }

    for (std::map<std::string, DeathCallback>::iterator it = death_callbacks_.begin();
         it != death_callbacks_.end(); ++it) {
        Message msg(MessageType::MSG_SUBSCRIBE_SERVICE, allocSequence());
        msg.payload.writeString(it->first);
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
        bool accepted = false;
        if (!decodeBoolReplyPayload(reply, accepted) || !accepted) {
            OMNI_LOG_WARN(LOG_TAG, "restore subscribe death failed for %s", it->first.c_str());
        }
    }

    std::map<std::string, std::string> published_owners = topic_runtime_.publishedTopicOwners();
    for (std::map<std::string, std::string>::iterator it = published_owners.begin();
         it != published_owners.end(); ++it) {
        std::map<std::string, LocalServiceEntry*>::iterator sit = local_services_.find(it->second);
        if (sit == local_services_.end() || !sit->second || !sit->second->service) {
            continue;
        }
        Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
        msg.payload.writeString(it->first);
        ServiceInfo pub_info;
        pub_info.name = it->second;
        pub_info.host = normalizeAdvertiseHost(platform::getSocketAddress(sit->second->server->fd()));
        pub_info.port = sit->second->port;
        pub_info.host_id = host_id_;
        pub_info.shm_config = sit->second->service->shmConfig();
        serializeServiceInfo(pub_info, msg.payload);
        msg.payload.writeUint32(0);  // idl_hash placeholder, generated proxy will overwrite

        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
        bool pub_accepted = false;
        if (!decodeBoolReplyPayload(reply, pub_accepted) || !pub_accepted) {
            OMNI_LOG_WARN(LOG_TAG, "restore publish topic failed for %s", it->first.c_str());
        }
    }

    std::map<std::string, TopicCallback> subscriptions = topic_runtime_.subscriptions();
    for (std::map<std::string, TopicCallback>::iterator it = subscriptions.begin();
         it != subscriptions.end(); ++it) {
        Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
        msg.payload.writeString(it->first);
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
        bool sub_accepted = false;
        if (!decodeBoolReplyPayload(reply, sub_accepted) || !sub_accepted) {
            OMNI_LOG_WARN(LOG_TAG, "restore subscribe topic failed for %s", it->first.c_str());
        }
    }

    return 0;
}

int OmniRuntime::Impl::reconnectServiceManagerIfNeeded() {
    if (!initialized_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }

    // 恢复流程重入保护：restoreControlPlaneState 内部经 sendToSM 可能再次进入
    // 本函数。此时不应再嵌套执行完整 reconnect+restore，直接按当前连接状态返回。
    if (restoring_) {
        return sm_channel_.isConnected() ? 0 : static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
    }

    if (!sm_reconnect_needed_.load() && sm_channel_.isConnected()) {
        return 0;
    }

    // RAII scope guard：无论 reconnect/restore 正常返回还是抛异常（bad_alloc）
    // 都复位 restoring_，避免异常路径导致 SM 重连恢复永久被禁用
    struct RestoreScopeGuard {
        bool& restoring;
        explicit RestoreScopeGuard(bool& r) : restoring(r) { restoring = true; }
        ~RestoreScopeGuard() { restoring = false; }
    } restore_guard(restoring_);

    int ret = reconnectServiceManager();
    if (ret != 0) {
        return ret;
    }
    ret = restoreControlPlaneState();
    if (ret != 0) {
        // 恢复失败：保留重连标志，让后续心跳重试完整恢复流程。
        // SM 侧已对同 host_id 注册做幂等更新（service_registry.cpp），
        // 重试不会因"服务重名"被拒绝，此路径必然收敛。
        sm_reconnect_needed_ = true;
        return ret;
    }

    // 数据面恢复：reconnectServiceManager 内 conn_mgr_->closeAll() 已清空所有
    // 数据面连接。对用户显式连接（reconnect_configs_ 中 enabled 的条目）立即
    // 重新调度重连；topic 订阅者连接则依赖重订阅后 SM 重发的
    // MSG_TOPIC_PUBLISHER_NOTIFY → ensureTopicPublisherConnection 自动重建。
    for (std::map<std::string, ReconnectConfig>::iterator rc = reconnect_configs_.begin();
         rc != reconnect_configs_.end(); ++rc) {
        if (rc->second.enabled) {
            tryReconnectService(rc->first);
        }
    }
    return 0;
}

// ============================================================
// 控制面心跳 — 向 SM 周期发送 MSG_HEARTBEAT
// ============================================================

void OmniRuntime::Impl::sendHeartbeat() {
    // 先检查 SM 是否需要重连（客户端也可能没有注册服务，但仍需保持 SM 连接）
    reconnectServiceManagerIfNeeded();

    // 为每个已注册的本地服务向 SM 发送心跳
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        Message msg(MessageType::MSG_HEARTBEAT, 0);
        if (!msg.payload.writeString(it->first)) {
            OMNI_LOG_ERROR(LOG_TAG, "heartbeat_serialize_failed service=%s", it->first.c_str());
            continue;
        }
        sendToSM(msg);
    }
}

} // namespace omnibinder
