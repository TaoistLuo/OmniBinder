#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "transport/transport_selector.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntimeSM"

namespace {
// 控制面保活发送的小超时预算。心跳是周期性 fire-and-forget 消息，不得占用默认
// RPC 超时（5s）阻塞 owner event-loop。发送未完成时 sendToSMWithinTimeout 会
// 标记重连（半帧流随旧连接丢弃），下一心跳 tick 自然重试。
const uint32_t CONTROL_HEARTBEAT_SEND_TIMEOUT_MS = 200;
} // namespace

namespace omnibinder {

// ============================================================
// SM 通信
// ============================================================

bool OmniRuntime::Impl::sendToSM(const Message& msg) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        return false;
    }
    bool ok = sm_channel_.sendMessage(msg);
    if (!ok) {
        // 发送失败（半开连接/对端关闭）：标记重连，让后续心跳或 API 触发恢复
        sm_reconnect_needed_ = true;
    }
    return ok;
}

bool OmniRuntime::Impl::sendToSMWithinTimeout(const Message& msg, uint32_t timeout_ms,
                                              uint32_t* elapsed_ms) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        if (elapsed_ms) {
            *elapsed_ms = 0;
        }
        return false;
    }
    bool ok = sm_channel_.sendMessageWithinTimeout(msg, timeout_ms, elapsed_ms);
    if (!ok) {
        // 发送失败（半开连接/对端关闭）：标记重连，让后续心跳或 API 触发恢复
        sm_reconnect_needed_ = true;
    }
    return ok;
}

int OmniRuntime::Impl::sendSMRequestAndWaitReply(Message& msg, Message& reply) {
    return sendSMRequestAndWaitReply(msg, reply, 0);
}

int OmniRuntime::Impl::sendSMRequestAndWaitReply(Message& msg, Message& reply,
                                                 uint32_t timeout_ms) {
    uint32_t total_timeout_ms = effectiveTimeout(timeout_ms);
    uint32_t send_elapsed_ms = 0;
    if (!sendToSMWithinTimeout(msg, total_timeout_ms, &send_elapsed_ms)) {
        // 与数据面 invoke 语义对齐：发送阶段耗尽超时预算 → ERR_TIMEOUT，
        // 真实发送错误（连接不可用）→ ERR_SEND_FAILED
        if (send_elapsed_ms >= total_timeout_ms) {
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }
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
    if (!serializeRuntimeInfo(info, msg.payload)) {
        OMNI_LOG_ERROR(LOG_TAG, "serialize_runtime_hello_payload_failed");
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

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

void OmniRuntime::Impl::dropServiceManagerConnection() {
    diag_watch_active_ = false;
    sm_reconnect_needed_ = true;
    sm_channel_.closeTransport(*loop_);
}

void OmniRuntime::Impl::onSMData(int fd, uint32_t events) {
    (void)fd; (void)events;
    Message msg;
    while (true) {
        // onSMMessage 可能触发 SM 重连/断开（transport 被替换或置空），
        // 每轮重新检查，避免在失效连接上继续读取
        if (!sm_channel_.transport()) {
            return;
        }
        int ret = sm_channel_.recvMessage(msg);
        if (ret == 0) {
            return;
        }
        if (ret < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "sm_connection_lost host=%s port=%u err=%d",
                           sm_host_.c_str(), sm_port_, static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED));
            dropServiceManagerConnection();
            return;
        }
        onSMMessage(msg);
    }
}

void OmniRuntime::Impl::onSMMessage(Message& msg) {
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
        if (storeAndConsumeControlReply(seq, msg)) {
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
    case MessageType::MSG_DEATH_NOTIFY:
        handleDeathNotify(msg);
        break;
    case MessageType::MSG_TOPIC_PUBLISHER_NOTIFY:
        handleTopicPublisherNotify(msg);
        break;
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled SM message: %s", messageTypeToString(type));
        break;
    }
}

void OmniRuntime::Impl::handleDeathNotify(const Message& msg) {
    std::string svc_name;
    if (!decodeSingleStringPayload(msg, svc_name)) {
        OMNI_LOG_WARN(LOG_TAG, "malformed_death_notify seq=%u err=%d",
                      msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        return;
    }
    OMNI_LOG_WARN(LOG_TAG, "Service died: %s", svc_name.c_str());
    ServiceState* dead_state = findServiceState(svc_name);
    if (dead_state && dead_state->has_death && dead_state->death_cb) {
        // 先拷贝到局部变量再调用：回调内可能 unsubscribeServiceDeath 清除该节点，
        // 消除对失效迭代器/容器状态的脆弱依赖
        DeathCallback callback = dead_state->death_cb;
        callback(svc_name);
    }
    handleServiceLost(svc_name);
}

void OmniRuntime::Impl::handleTopicPublisherNotify(const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    std::string topic;
    if (!buf.tryReadString(topic)) {
        OMNI_LOG_WARN(LOG_TAG, "malformed_topic_publisher_notify seq=%u err=%d",
                      msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        return;
    }
    ServiceInfo pub_info;
    if (!deserializeServiceInfo(buf, pub_info)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
        return;
    }
    uint32_t publisher_idl_hash = 0;
    if (buf.remaining() >= sizeof(uint32_t) && !buf.tryReadUint32(publisher_idl_hash)) {
        OMNI_LOG_WARN(LOG_TAG, "malformed_topic_publisher_notify tail topic=%s seq=%u",
                      topic.c_str(), msg.getSequence());
        return;
    }

    // 发布者声明了哈希且与订阅者期望不一致时，通知 on_err 并摘除订阅，不建立数据面连接。
    // 发布者未声明哈希（0）视为无法校验，保持兼容放行。
    const uint32_t expected_idl_hash = topic_runtime_.expectedSubscriptionHash(topic);
    if (expected_idl_hash != 0 && publisher_idl_hash != expected_idl_hash) {
        OMNI_LOG_WARN(LOG_TAG,
                      "topic_idl_mismatch topic=%s expected_hash=0x%08x publisher_hash=0x%08x",
                      topic.c_str(), expected_idl_hash, publisher_idl_hash);
        // notifyError 可能经用户回调修改订阅表（约束 2），topic 为局部副本可安全继续使用
        topic_runtime_.notifyError(fnv1a_32(topic), ErrorCode::ERR_IDL_MISMATCH);
        unsubscribeTopicInternal(topic);
        return;
    }

    OMNI_LOG_INFO(LOG_TAG, "Topic %s publisher at %s:%u",
                    topic.c_str(), pub_info.host.c_str(), pub_info.port);
    if (!ensureTopicPublisherConnection(topic, pub_info)) {
        OMNI_LOG_WARN(LOG_TAG, "Failed to establish topic publisher path for %s", topic.c_str());
        // 发布者连接建立失败：通知订阅者 on_err 回调
        topic_runtime_.notifyError(fnv1a_32(topic), ErrorCode::ERR_CONNECT_FAILED);
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

    sm_channel_.closeTransport(*loop_);
    // 只清理控制面等待槽；数据面等待槽由 RpcRuntime 独立持有，在途直连 RPC 不受影响
    sm_channel_.clearReplies();
    // 旧连接上的半帧字节不得混入新连接的数据流
    sm_channel_.clearReceiveBuffer();
    // watcher 与 SM 的关联随旧连接失效，标记待重放（约束 6）
    for (std::map<uint32_t, DiagWatcherEntry>::iterator wit = diag_watchers_.begin();
         wit != diag_watchers_.end(); ++wit) {
        wit->second.active = false;
    }

    int connect_err = 0;
    IMessageConnection* control_transport = createControlConnection(sm_host_, sm_port_, connect_err);
    if (!control_transport) {
        return connect_err;
    }
    sm_channel_.resetTransport(control_transport);

    loop_->addFd(sm_channel_.transport()->fd(), EventLoop::EVENT_READ,
        [this](int fd, uint32_t events) { this->onSMData(fd, events); });
    sm_reconnect_needed_ = false;
    stats_.sm_reconnect_successes++;
    for (std::map<std::string, ServiceState>::iterator it = services_.begin();
         it != services_.end();) {
        it->second.info = ServiceInfo();
        it->second.has_info = false;
        if (!it->second.has_reconnect && !it->second.has_death && !it->second.has_heartbeat) {
            it = services_.erase(it);
        } else {
            ++it;
        }
    }
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

    // 先拷贝 key 再遍历：waitForReply 期间 SM 消息可触发用户回调
    // （如死亡通知回调内 unregisterService）修改 local_services_（约束 2）
    std::vector<std::string> service_names;
    service_names.reserve(local_services_.size());
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        service_names.push_back(it->first);
    }
    for (size_t i = 0; i < service_names.size(); ++i) {
        int ret = restoreRegisterService(service_names[i]);
        if (ret != 0) {
            return ret;
        }
    }

    // 同上：waitForReply 期间用户可能 unsubscribeServiceDeath 清除该状态（约束 2）
    std::vector<std::string> death_names;
    for (std::map<std::string, ServiceState>::iterator it = services_.begin();
         it != services_.end(); ++it) {
        if (it->second.has_death) {
            death_names.push_back(it->first);
        }
    }
    for (size_t i = 0; i < death_names.size(); ++i) {
        int ret = restoreSubscribeDeath(death_names[i]);
        if (ret != 0) {
            return ret;
        }
    }

    std::map<std::string, std::string> published_owners = topic_runtime_.publishedTopicOwners();
    std::map<std::string, uint32_t> published_hashes = topic_runtime_.publishedTopicHashes();
    for (std::map<std::string, std::string>::iterator it = published_owners.begin();
         it != published_owners.end(); ++it) {
        uint32_t idl_hash = 0;
        std::map<std::string, uint32_t>::iterator hit = published_hashes.find(it->first);
        if (hit != published_hashes.end()) {
            idl_hash = hit->second;
        }
        int ret = restorePublishTopic(it->first, it->second, idl_hash);
        if (ret != 0) {
            return ret;
        }
    }

    std::map<std::string, TopicCallback> subscriptions = topic_runtime_.subscriptions();
    for (std::map<std::string, TopicCallback>::iterator it = subscriptions.begin();
         it != subscriptions.end(); ++it) {
        int ret = restoreSubscribeTopic(it->first,
                                        topic_runtime_.expectedSubscriptionHash(it->first));
        if (ret != 0) {
            return ret;
        }
    }

    // watcher 重放必须在 topic 订阅重放之后：先恢复 __diag_pid_<pid> 本地/控制面订阅，
    // 再通知 SM 重新关联 watcher。失败不阻断恢复（目标可能晚于 watcher 重连），
    // 由 sendHeartbeat 周期兜底重试
    restoreDiagWatchers();

    return 0;
}

int OmniRuntime::Impl::restoreRegisterService(const std::string& name) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
    if (it == local_services_.end()) {
        return 0;
    }
    LocalServiceEntry* entry = it->second;
    if (!entry || !entry->service) {
        return 0;
    }

    int ret = sendRegisterToManager(name, entry->service, entry->port,
                                    resolveRegisterHost(entry->service, listenerAdvertiseHost()),
                                    NULL);
    // 恢复路径的历史语义：反序列化失败与句柄无效统一映射为 ERR_REGISTER_FAILED
    // 并记录错误日志；发送/等待类错误码原样透传
    if (ret == static_cast<int>(ErrorCode::ERR_DESERIALIZE)
        || ret == static_cast<int>(ErrorCode::ERR_REGISTER_FAILED)) {
        OMNI_LOG_ERROR(LOG_TAG, "restore register failed for %s", name.c_str());
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    return ret;
}

int OmniRuntime::Impl::restoreSubscribeDeath(const std::string& name) {
    ServiceState* death_state = findServiceState(name);
    if (!death_state || !death_state->has_death) {
        return 0;
    }
    Message msg(MessageType::MSG_SUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(name);
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
        OMNI_LOG_WARN(LOG_TAG, "restore subscribe death failed for %s", name.c_str());
    }
    return 0;
}

int OmniRuntime::Impl::restorePublishTopic(const std::string& topic, const std::string& owner,
                                           uint32_t idl_hash) {
    std::map<std::string, LocalServiceEntry*>::iterator sit = local_services_.find(owner);
    if (sit == local_services_.end() || !sit->second || !sit->second->service) {
        return 0;
    }
    Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
    msg.payload.writeString(topic);
    ServiceInfo pub_info;
    pub_info.name = owner;
    pub_info.host = listenerAdvertiseHost();
    pub_info.port = sit->second->port;
    pub_info.host_id = host_id_;
    pub_info.shm_config = sit->second->service->shmConfig();
    if (!serializeServiceInfo(pub_info, msg.payload)) {
        OMNI_LOG_ERROR(LOG_TAG, "serialize_restore_publish_topic_failed topic=%s", topic.c_str());
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    if (!msg.payload.writeUint32(idl_hash)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

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
        OMNI_LOG_WARN(LOG_TAG, "restore publish topic failed for %s", topic.c_str());
    }
    return 0;
}

int OmniRuntime::Impl::restoreSubscribeTopic(const std::string& topic,
                                             uint32_t expected_idl_hash) {
    topic_runtime_.setExpectedSubscriptionHash(topic, expected_idl_hash);

    Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic);
    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        return ret;
    }
    Buffer reply_payload(reply.payload.data(), reply.payload.size());
    bool sub_accepted = false;
    if (!reply_payload.tryReadBool(sub_accepted) || !sub_accepted) {
        OMNI_LOG_WARN(LOG_TAG, "restore subscribe topic failed for %s", topic.c_str());
        return 0;
    }

    uint32_t publisher_idl_hash = 0;
    if (reply_payload.remaining() >= sizeof(uint32_t)
        && !reply_payload.tryReadUint32(publisher_idl_hash)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    // 重连恢复时若发布者哈希与期望不一致：通知错误并摘除该订阅，
    // 但不阻断恢复流程（无效订阅已被移除，恢复继续收敛）
    if (expected_idl_hash != 0 && publisher_idl_hash != 0
        && publisher_idl_hash != expected_idl_hash) {
        OMNI_LOG_WARN(LOG_TAG,
                      "topic_idl_mismatch restore topic=%s expected_hash=0x%08x publisher_hash=0x%08x",
                      topic.c_str(), expected_idl_hash, publisher_idl_hash);
        topic_runtime_.notifyError(fnv1a_32(topic), ErrorCode::ERR_IDL_MISMATCH);
        unsubscribeTopicInternal(topic);
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
    // 数据面连接。对 services_ 中 has_reconnect 且 enabled 的条目立即
    // 重新调度重连；topic 订阅者连接则依赖重订阅后 SM 重发的
    // MSG_TOPIC_PUBLISHER_NOTIFY → ensureTopicPublisherConnection 自动重建。
    std::vector<std::string> reconnect_names;
    for (std::map<std::string, ServiceState>::iterator rc = services_.begin();
         rc != services_.end(); ++rc) {
        if (rc->second.has_reconnect && rc->second.reconnect.enabled) {
            reconnect_names.push_back(rc->first);
        }
    }
    for (size_t i = 0; i < reconnect_names.size(); ++i) {
        tryReconnectService(reconnect_names[i]);
    }

    // 诊断 watch 恢复：SM 断线时 onSMData 只置 diag_watch_active_ = false 而保留
    // diag_watch_topic_id_。重连成功后 restoreControlPlaneState 已重新注册 diag 服务并
    // 重放 topic 发布，这里只需恢复本地事件发射开关。不能再调 initDiagDataService：
    // 其内部重复 publish 会被 SM 以"topic 已有发布者"拒绝，导致 topic_id 被清零、
    // active 保持 false，监控恢复失败。
    if (diag_watch_topic_id_ != 0 && !diag_watch_active_) {
        diag_watch_active_ = true;
    }
    return 0;
}

// ============================================================
// 控制面心跳 — 向 SM 周期发送 MSG_HEARTBEAT
// ============================================================

void OmniRuntime::Impl::sendHeartbeat() {
    // 先检查 SM 是否需要重连（客户端也可能没有注册服务，但仍需保持 SM 连接）
    reconnectServiceManagerIfNeeded();
    // 周期兜底：SM 重连时目标 pid 可能尚未重新 hello，watch 重放会先失败
    retryInactiveDiagWatchers();

    // 先拷贝 key 再遍历：reconnectServiceManagerIfNeeded 可能触发 restore 流程，
    // 其内部 waitForReply 期间用户回调可修改 local_services_（约束 2）
    std::vector<std::string> names;
    names.reserve(local_services_.size());
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        names.push_back(it->first);
    }
    for (size_t i = 0; i < names.size(); ++i) {
        Message msg(MessageType::MSG_HEARTBEAT, 0);
        if (!msg.payload.writeString(names[i])) {
            OMNI_LOG_ERROR(LOG_TAG, "heartbeat_serialize_failed service=%s", names[i].c_str());
            continue;
        }
        sendToSMWithinTimeout(msg, CONTROL_HEARTBEAT_SEND_TIMEOUT_MS, NULL);
    }
}

} // namespace omnibinder
