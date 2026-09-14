#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#include <algorithm>

#define LOG_TAG "OmniRuntimeConnection"

namespace omnibinder {

OmniRuntime::Impl::ServiceState* OmniRuntime::Impl::findServiceState(const std::string& name) {
    std::map<std::string, ServiceState>::iterator it = services_.find(name);
    return it == services_.end() ? NULL : &it->second;
}

OmniRuntime::Impl::ServiceState& OmniRuntime::Impl::ensureServiceState(const std::string& name) {
    return services_[name];
}

void OmniRuntime::Impl::eraseServiceStateIfUnused(const std::string& name) {
    std::map<std::string, ServiceState>::iterator it = services_.find(name);
    if (it == services_.end()) {
        return;
    }
    if (!it->second.has_info && !it->second.has_death
        && !it->second.has_reconnect && !it->second.has_heartbeat) {
        services_.erase(it);
    }
}

// ============================================================
// 连接管理
// ============================================================

int OmniRuntime::Impl::connectService(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
        return connectServiceInternal(service_name);
    });
}

int OmniRuntime::Impl::disconnectService(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
        return disconnectServiceInternal(service_name);
    });
}

bool OmniRuntime::Impl::isServiceConnected(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> bool {
        if (!conn_mgr_) return false;
        return conn_mgr_->getConnection(service_name) != NULL;
    });
}

int OmniRuntime::Impl::connectServiceInternal(const std::string& service_name,
                                             bool explicit_connect) {
    ServiceInfo info;
    int ret = lookupServiceInfo(service_name, info);
    if (ret != 0) return ret;

    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        service_name, info.host, info.port, info.host_id, info.shm_config);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "connectService failed for %s", service_name.c_str());
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    // 记录连接恢复意图：SM 重连恢复时数据面连接会被 closeAll 清空，
    // 需要该记录才能在恢复流程中重建直连（约束 6：恢复必须覆盖数据面）。
    // explicit_connect 区分用户显式连接与话题订阅附带的连接，后者在订阅
    // 全部取消后可整体回收
    ServiceState& conn_state = ensureServiceState(service_name);
    conn_state.has_reconnect = true;
    ReconnectConfig& config = conn_state.reconnect;  // 默认 enabled=true
    if (explicit_connect) {
        config.explicit_connect = true;
    }
    OMNI_LOG_INFO(LOG_TAG, "connectService success for %s", service_name.c_str());
    return 0;
}

int OmniRuntime::Impl::disconnectServiceInternal(const std::string& service_name) {
    if (conn_mgr_) {
        conn_mgr_->removeConnection(service_name);
    }
    pauseHeartbeat(service_name);

    ServiceState* svc_state = findServiceState(service_name);
    if (svc_state) {
        svc_state->info = ServiceInfo();
        svc_state->has_info = false;
        if (svc_state->has_reconnect && svc_state->reconnect.timer_id > 0) {
            loop_->cancelTimer(svc_state->reconnect.timer_id);
        }
        svc_state->has_reconnect = false;
        svc_state->reconnect = ReconnectConfig();
        svc_state->has_heartbeat = false;
    }
    eraseServiceStateIfUnused(service_name);
    OMNI_LOG_INFO(LOG_TAG, "disconnectService for %s", service_name.c_str());
    return 0;
}

void OmniRuntime::Impl::enableAutoReconnect(const std::string& service_name, bool enable) {
    callSerialized([this, &service_name, enable]() {
        ServiceState& svc_state = ensureServiceState(service_name);
        svc_state.has_reconnect = true;
        ReconnectConfig& config = svc_state.reconnect;
        config.enabled = enable;
        config.current_retry = 0;
        if (!enable && config.timer_id > 0) {
            loop_->cancelTimer(config.timer_id);
            config.timer_id = 0;
        }
        OMNI_LOG_INFO(LOG_TAG, "AutoReconnect %s for %s",
                       enable ? "enabled" : "disabled", service_name.c_str());
    });
}

void OmniRuntime::Impl::setReconnectInterval(const std::string& service_name, uint32_t interval_ms) {
    callSerialized([this, &service_name, interval_ms]() {
        ServiceState& svc_state = ensureServiceState(service_name);
        svc_state.has_reconnect = true;
        ReconnectConfig& config = svc_state.reconnect;
        config.interval_ms = interval_ms > 0 ? interval_ms : 1000;
    });
}

void OmniRuntime::Impl::tryReconnectService(const std::string& service_name) {
    ServiceState* rc_state = findServiceState(service_name);
    if (!rc_state || !rc_state->has_reconnect || !rc_state->reconnect.enabled) {
        return;
    }

    // 拷贝决策所需配置：connectServiceInternal 的 waitForReply 期间用户回调可能
    // disconnectService → 清除本条目，持引用访问会悬垂（约束 1）
    const ReconnectConfig config = rc_state->reconnect;

    OMNI_LOG_INFO(LOG_TAG, "Trying to reconnect to %s (attempt %u)",
                   service_name.c_str(), config.current_retry + 1);

    // 话题发布者直连优先用 SM 最近下发的端点直接重建，避免额外 SM 往返；
    // 自动生成的 "topic_pub_" 连接名没有对应注册服务，只能走此路径
    int ret = static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    if (config.has_topic_endpoint) {
        ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
            service_name, config.topic_endpoint.host, config.topic_endpoint.port,
            config.topic_endpoint.host_id, config.topic_endpoint.shm_config);
        ret = conn ? 0 : static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }
    if (ret != 0) {
        ret = connectServiceInternal(service_name, false);
    }

    if (ret == 0) {
        OMNI_LOG_INFO(LOG_TAG, "Successfully reconnected to %s", service_name.c_str());
        // 发布者已在断开路径移除本订阅者，必须重放 MSG_SUBSCRIBE_BROADCAST，
        // 否则连接虽恢复但广播永久丢失。重放失败不视为恢复成功，走退避重试
        const bool resubscribed =
            resubscribeTopicPublisher(service_name, config.topic_subscriptions);
        rc_state = findServiceState(service_name);
        if (rc_state == NULL || !rc_state->has_reconnect) {
            return;
        }
        if (resubscribed) {
            rc_state->reconnect.current_retry = 0;
            rc_state->reconnect.timer_id = 0;
            resumeHeartbeat(service_name);
            return;
        }
        OMNI_LOG_WARN(LOG_TAG, "topic_resubscribe_incomplete publisher=%s, will retry",
                      service_name.c_str());
    }

    rc_state = findServiceState(service_name);
    if (rc_state == NULL || !rc_state->has_reconnect || !rc_state->reconnect.enabled) {
        return;  // 重连等待期间用户已 disconnectService 或关闭自动重连
    }
    ReconnectConfig& live = rc_state->reconnect;
    live.current_retry = config.current_retry + 1;
    if (live.max_retries > 0 && live.current_retry >= live.max_retries) {
        OMNI_LOG_WARN(LOG_TAG, "Max reconnect attempts reached for %s", service_name.c_str());
        live.enabled = false;
        live.timer_id = 0;
    } else {
        // 指数退避：用 int64 计算避免 uint32 溢出，并 clamp 到 30s 上限
        uint32_t shift = live.current_retry < 5 ? live.current_retry : 5;
        int64_t delay64 = static_cast<int64_t>(live.interval_ms)
                          * (static_cast<int64_t>(1) << shift);
        uint32_t delay_ms = delay64 > 30000 ? 30000 : static_cast<uint32_t>(delay64);
        scheduleReconnect(service_name, delay_ms);
    }
}

static uint32_t addReconnectJitter(uint32_t delay_ms) {
    if (delay_ms <= 10) return delay_ms;
    // 用当前时间低比特作为熵源做 ±25% 抖动，
    // 避免多个客户端同时重连引发惊群
    uint32_t r = static_cast<uint32_t>(platform::currentTimeMs()) % 51;
    int32_t jitter_pct = static_cast<int32_t>(r) - 25;
    uint32_t result = static_cast<uint32_t>(
        static_cast<int64_t>(delay_ms) * (100 + jitter_pct) / 100);
    return result > 0 ? result : 1;
}

void OmniRuntime::Impl::scheduleReconnect(const std::string& service_name, uint32_t delay_ms) {
    ServiceState* sched_state = findServiceState(service_name);
    if (sched_state == NULL || !sched_state->has_reconnect || !sched_state->reconnect.enabled) {
        return;
    }

    ReconnectConfig& config = sched_state->reconnect;
    if (config.timer_id > 0) {
        loop_->cancelTimer(config.timer_id);
    }

    uint32_t jittered_ms = addReconnectJitter(delay_ms);
    config.timer_id = loop_->addTimer(jittered_ms, [this, service_name]() {
        tryReconnectService(service_name);
    }, false);

    OMNI_LOG_DEBUG(LOG_TAG, "Scheduled reconnect for %s in %u ms (base=%u ms)",
                     service_name.c_str(), jittered_ms, delay_ms);
}

void OmniRuntime::Impl::startHeartbeat(const std::string& service_name, uint32_t interval_ms, uint32_t timeout_ms) {
    callSerialized([this, &service_name, interval_ms, timeout_ms]() {
        ServiceState& svc_state = ensureServiceState(service_name);
        svc_state.has_heartbeat = true;
        HeartbeatState& state = svc_state.heartbeat;
        state.interval_ms = interval_ms > 0 ? interval_ms : DEFAULT_HEARTBEAT_INTERVAL;
        state.timeout_ms = timeout_ms > 0 ? timeout_ms : DEFAULT_HEARTBEAT_TIMEOUT;
        resumeHeartbeat(service_name);

        OMNI_LOG_INFO(LOG_TAG, "Started heartbeat for %s (interval=%u ms, timeout=%u ms)",
                       service_name.c_str(), state.interval_ms, state.timeout_ms);
    });
}

void OmniRuntime::Impl::stopHeartbeat(const std::string& service_name) {
    callSerialized([this, &service_name]() {
        ServiceState* svc_state = findServiceState(service_name);
        if (svc_state && svc_state->has_heartbeat) {
            pauseHeartbeat(service_name);
            svc_state->has_heartbeat = false;
            eraseServiceStateIfUnused(service_name);
            OMNI_LOG_INFO(LOG_TAG, "Stopped heartbeat for %s", service_name.c_str());
        }
    });
}

void OmniRuntime::Impl::pauseHeartbeat(const std::string& service_name) {
    ServiceState* svc_state = findServiceState(service_name);
    if (!svc_state || !svc_state->has_heartbeat) return;
    if (svc_state->heartbeat.timer_id > 0) {
        loop_->cancelTimer(svc_state->heartbeat.timer_id);
        svc_state->heartbeat.timer_id = 0;
    }
    svc_state->heartbeat.pending = false;
}

bool OmniRuntime::Impl::handleServiceLost(const std::string& service_name) {
    if (conn_mgr_) {
        conn_mgr_->removeConnection(service_name);
    }
    pauseHeartbeat(service_name);

    ServiceState* svc_state = findServiceState(service_name);
    if (svc_state) {
        svc_state->info = ServiceInfo();
        svc_state->has_info = false;
    }
    const bool reconnect_enabled = svc_state && svc_state->has_reconnect
        && svc_state->reconnect.enabled;
    if (reconnect_enabled) {
        svc_state->reconnect.current_retry = 0;
        scheduleReconnect(service_name, svc_state->reconnect.interval_ms);
    }
    eraseServiceStateIfUnused(service_name);
    return reconnect_enabled;
}

void OmniRuntime::Impl::resumeHeartbeat(const std::string& service_name) {
    ServiceState* svc_state = findServiceState(service_name);
    if (!svc_state || !svc_state->has_heartbeat) return;
    HeartbeatState& state = svc_state->heartbeat;
    if (state.timer_id > 0) loop_->cancelTimer(state.timer_id);
    state.last_ack_time = platform::currentTimeMs();
    state.pending = false;
    state.timer_id = loop_->addTimer(state.interval_ms, [this, service_name]() {
        sendHeartbeatToService(service_name);
        checkHeartbeatTimeout(service_name);
    }, true);
}

void OmniRuntime::Impl::sendHeartbeatToService(const std::string& service_name) {
    ServiceState* svc_state = findServiceState(service_name);
    if (!svc_state || !svc_state->has_heartbeat) {
        return;
    }

    HeartbeatState& state = svc_state->heartbeat;
    if (state.pending) {
        return;
    }

    Message msg(MessageType::MSG_HEARTBEAT, allocSequence());
    if (conn_mgr_->sendMessage(service_name, msg)) {
        state.pending = true;
        OMNI_LOG_DEBUG(LOG_TAG, "Sent heartbeat to %s", service_name.c_str());
    }
}

void OmniRuntime::Impl::checkHeartbeatTimeout(const std::string& service_name) {
    ServiceState* svc_state = findServiceState(service_name);
    if (!svc_state || !svc_state->has_heartbeat) {
        return;
    }

    HeartbeatState& state = svc_state->heartbeat;
    if (!state.pending) {
        return;
    }

    int64_t now = platform::currentTimeMs();
    int64_t elapsed = now - state.last_ack_time;
    if (elapsed > static_cast<int64_t>(state.timeout_ms)) {
        OMNI_LOG_WARN(LOG_TAG, "Heartbeat timeout for %s (elapsed=%lld ms)", service_name.c_str(), elapsed);

        const bool reconnect_enabled = handleServiceLost(service_name);
        if (!reconnect_enabled) {
            ServiceState* lost_state = findServiceState(service_name);
            if (lost_state) {
                lost_state->has_heartbeat = false;
            }
            eraseServiceStateIfUnused(service_name);
        }
    }
}

// ============================================================
// 数据面辅助
// ============================================================

bool OmniRuntime::Impl::sendRawOnFd(IMessageConnection* transport, const uint8_t* data, size_t size) {
    if (!transport) return false;
    // 服务端回复发送：使用专用有界预算（默认 1000ms，可经 setReplySendTimeout 调整），
    // 不再无条件占用默认 RPC 超时（5s），避免慢客户端卡住 owner event-loop
    return transport->sendAll(data, size, reply_send_timeout_ms_, NULL) == 0;
}

bool OmniRuntime::Impl::sendOnFd(IMessageConnection* transport, Message& msg) {
    if (!transport || !msg.serializeInPlace()) {
        return false;
    }
    return sendRawOnFd(transport, msg.payload.data(), msg.payload.size());
}

bool OmniRuntime::Impl::sendOnFdBestEffort(IMessageConnection* transport, Message& msg) {
    if (!transport || !msg.serializeInPlace()) {
        return false;
    }
    // timeout=0：只尝试一次、不等待。心跳 ACK 属可丢弃报文，绝不占用
    // owner event-loop 的发送预算（TCP 字节流下小帧一次即可写完）
    return transport->sendAll(msg.payload.data(), msg.payload.size(), 0, NULL) == 0;
}

bool OmniRuntime::Impl::populateInvokeMessage(Message& msg, uint32_t interface_id,
                                              uint32_t method_id, uint32_t idl_hash,
                                              const Buffer& request) const {
    if (!msg.payload.writeUint32(interface_id)
        || !msg.payload.writeUint32(idl_hash)
        || !msg.payload.writeUint32(method_id)
        || !msg.payload.writeUint32(static_cast<uint32_t>(request.size()))
        || (request.size() > 0 && !msg.payload.writeRaw(request.data(), request.size()))) {
        OMNI_LOG_ERROR(LOG_TAG,
                       "invoke_payload_serialize_failed iface=0x%08x method=0x%08x err=%d",
                       interface_id, method_id, static_cast<int>(ErrorCode::ERR_SERIALIZE));
        return false;
    }
    return true;
}

std::string OmniRuntime::Impl::topicPublisherServiceName(const std::string& topic_name) const {
    return "topic_pub_" + topic_name;
}

bool OmniRuntime::Impl::sendTopicBroadcastSubscription(const std::string& pub_name,
                                                       const std::string& topic_name) {
    uint32_t topic_id = fnv1a_32(topic_name);
    Message sub_msg(MessageType::MSG_SUBSCRIBE_BROADCAST, 0);
    if (!sub_msg.payload.writeUint32(topic_id) || !sub_msg.payload.writeString(topic_name)) {
        OMNI_LOG_ERROR(LOG_TAG, "topic_subscribe_serialize_failed topic=%s",
                       topic_name.c_str());
        return false;
    }
    if (!conn_mgr_->sendMessage(pub_name, sub_msg)) {
        OMNI_LOG_WARN(LOG_TAG, "topic_subscribe_send_failed topic=%s publisher=%s",
                      topic_name.c_str(), pub_name.c_str());
        return false;
    }
    return true;
}

bool OmniRuntime::Impl::resubscribeTopicPublisher(const std::string& pub_name,
                                                  const std::vector<std::string>& topics) {
    bool all_ok = true;
    for (size_t i = 0; i < topics.size(); ++i) {
        if (!sendTopicBroadcastSubscription(pub_name, topics[i])) {
            all_ok = false;
        }
    }
    return all_ok;
}

void OmniRuntime::Impl::removeTopicSubscriptionBinding(const std::string& topic_name,
                                                       const std::string& keep_pub_name) {
    std::vector<std::string> to_clear;
    for (std::map<std::string, ServiceState>::iterator it = services_.begin();
         it != services_.end(); ++it) {
        ServiceState& st = it->second;
        if (!st.has_reconnect) {
            continue;
        }
        if (!keep_pub_name.empty() && it->first == keep_pub_name) {
            continue;
        }
        ReconnectConfig& config = st.reconnect;
        std::vector<std::string>& topics = config.topic_subscriptions;
        topics.erase(std::remove(topics.begin(), topics.end(), topic_name), topics.end());
        // 仅由话题订阅建立的连接：最后一个订阅移除后已无恢复意图，整体回收
        if (topics.empty() && !config.explicit_connect) {
            if (config.timer_id > 0) {
                loop_->cancelTimer(config.timer_id);
            }
            to_clear.push_back(it->first);
        }
    }
    for (size_t i = 0; i < to_clear.size(); ++i) {
        ServiceState* st = findServiceState(to_clear[i]);
        if (st) {
            st->has_reconnect = false;
            st->reconnect = ReconnectConfig();
            eraseServiceStateIfUnused(to_clear[i]);
        }
    }
}

bool OmniRuntime::Impl::ensureTopicPublisherConnection(const std::string& topic_name,
                                                      const ServiceInfo& pub_info) {
    if (!conn_mgr_) {
        return false;
    }
    std::string pub_name = !pub_info.name.empty() ? pub_info.name : topicPublisherServiceName(topic_name);
    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        pub_name, pub_info.host, pub_info.port, pub_info.host_id, pub_info.shm_config);
    if (!conn) {
        return false;
    }

    // 发布者切换（同一话题换服务实例）时，先摘除旧发布者连接上的同名绑定，
    // 否则旧连接断开会向已非发布者的服务重放订阅
    removeTopicSubscriptionBinding(topic_name, pub_name);

    // 把订阅意图绑定到发布者连接：记录话题并保存 SM 最新端点，掉线后由
    // tryReconnectService 重建直连并重放 MSG_SUBSCRIBE_BROADCAST（约束 6）。
    // 无论发布者使用真实服务名还是自动生成连接名，绑定语义一致
    ServiceState& pub_state = ensureServiceState(pub_name);
    pub_state.has_reconnect = true;
    ReconnectConfig& config = pub_state.reconnect;
    config.has_topic_endpoint = true;
    config.topic_endpoint = pub_info;
    if (std::find(config.topic_subscriptions.begin(), config.topic_subscriptions.end(),
                  topic_name) == config.topic_subscriptions.end()) {
        config.topic_subscriptions.push_back(topic_name);
    }

    OMNI_LOG_INFO(LOG_TAG, "Topic %s uses %s data path to publisher %s",
                    topic_name.c_str(), dataChannelKindName(conn->transport->type()),
                    pub_name.c_str());
    return sendTopicBroadcastSubscription(pub_name, topic_name);
}

} // namespace omnibinder
