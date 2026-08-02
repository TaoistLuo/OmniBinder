#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntimeConnection"

namespace omnibinder {

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

int OmniRuntime::Impl::connectServiceInternal(const std::string& service_name) {
    ServiceInfo info;
    int ret = lookupServiceInfo(service_name, info);
    if (ret != 0) return ret;

    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        service_name, info.host, info.port, info.host_id, info.shm_config);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "connectService failed for %s", service_name.c_str());
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    // 记录显式连接：SM 重连恢复时数据面连接会被 closeAll 清空，
    // 需要该记录才能在恢复流程中重建直连（约束 6：恢复必须覆盖数据面）
    reconnect_configs_[service_name];  // 默认 enabled=true
    OMNI_LOG_INFO(LOG_TAG, "connectService success for %s", service_name.c_str());
    return 0;
}

int OmniRuntime::Impl::disconnectServiceInternal(const std::string& service_name) {
    if (conn_mgr_) {
        conn_mgr_->removeConnection(service_name);
    }
    service_cache_.erase(service_name);
    std::map<std::string, ReconnectConfig>::iterator reconnect_it =
        reconnect_configs_.find(service_name);
    if (reconnect_it != reconnect_configs_.end()) {
        if (reconnect_it->second.timer_id > 0) {
            loop_->cancelTimer(reconnect_it->second.timer_id);
        }
        reconnect_configs_.erase(reconnect_it);
    }
    pauseHeartbeat(service_name);
    heartbeat_states_.erase(service_name);
    OMNI_LOG_INFO(LOG_TAG, "disconnectService for %s", service_name.c_str());
    return 0;
}

void OmniRuntime::Impl::enableAutoReconnect(const std::string& service_name, bool enable) {
    callSerialized([this, &service_name, enable]() {
        ReconnectConfig& config = reconnect_configs_[service_name];
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
        ReconnectConfig& config = reconnect_configs_[service_name];
        config.interval_ms = interval_ms > 0 ? interval_ms : 1000;
    });
}

void OmniRuntime::Impl::tryReconnectService(const std::string& service_name) {
    std::map<std::string, ReconnectConfig>::iterator it = reconnect_configs_.find(service_name);
    if (it == reconnect_configs_.end() || !it->second.enabled) {
        return;
    }

    ReconnectConfig& config = it->second;
    OMNI_LOG_INFO(LOG_TAG, "Trying to reconnect to %s (attempt %u)",
                   service_name.c_str(), config.current_retry + 1);

    int ret = connectServiceInternal(service_name);
    if (ret == 0) {
        OMNI_LOG_INFO(LOG_TAG, "Successfully reconnected to %s", service_name.c_str());
        config.current_retry = 0;
        config.timer_id = 0;
        resumeHeartbeat(service_name);
    } else {
        config.current_retry++;
        if (config.max_retries > 0 && config.current_retry >= config.max_retries) {
            OMNI_LOG_WARN(LOG_TAG, "Max reconnect attempts reached for %s", service_name.c_str());
            config.enabled = false;
            config.timer_id = 0;
        } else {
            // 指数退避：用 int64 计算避免 uint32 溢出，并 clamp 到 30s 上限
            uint32_t shift = config.current_retry < 5 ? config.current_retry : 5;
            int64_t delay64 = static_cast<int64_t>(config.interval_ms)
                              * (static_cast<int64_t>(1) << shift);
            uint32_t delay_ms = delay64 > 30000 ? 30000 : static_cast<uint32_t>(delay64);
            scheduleReconnect(service_name, delay_ms);
        }
    }
}

static uint32_t addReconnectJitter(uint32_t delay_ms) {
    if (delay_ms <= 10) return delay_ms;
    // ±25% jitter using low bits of current time as entropy source.
    // Prevents thundering herd when multiple clients reconnect simultaneously.
    uint32_t r = static_cast<uint32_t>(platform::currentTimeMs()) % 51;
    int32_t jitter_pct = static_cast<int32_t>(r) - 25;
    uint32_t result = static_cast<uint32_t>(
        static_cast<int64_t>(delay_ms) * (100 + jitter_pct) / 100);
    return result > 0 ? result : 1;
}

void OmniRuntime::Impl::scheduleReconnect(const std::string& service_name, uint32_t delay_ms) {
    std::map<std::string, ReconnectConfig>::iterator it = reconnect_configs_.find(service_name);
    if (it == reconnect_configs_.end() || !it->second.enabled) {
        return;
    }

    ReconnectConfig& config = it->second;
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
        HeartbeatState& state = heartbeat_states_[service_name];
        state.interval_ms = interval_ms > 0 ? interval_ms : DEFAULT_HEARTBEAT_INTERVAL;
        state.timeout_ms = timeout_ms > 0 ? timeout_ms : DEFAULT_HEARTBEAT_TIMEOUT;
        resumeHeartbeat(service_name);

        OMNI_LOG_INFO(LOG_TAG, "Started heartbeat for %s (interval=%u ms, timeout=%u ms)",
                       service_name.c_str(), state.interval_ms, state.timeout_ms);
    });
}

void OmniRuntime::Impl::stopHeartbeat(const std::string& service_name) {
    callSerialized([this, &service_name]() {
        std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
        if (it != heartbeat_states_.end()) {
            pauseHeartbeat(service_name);
            heartbeat_states_.erase(it);
            OMNI_LOG_INFO(LOG_TAG, "Stopped heartbeat for %s", service_name.c_str());
        }
    });
}

void OmniRuntime::Impl::pauseHeartbeat(const std::string& service_name) {
    std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
    if (it == heartbeat_states_.end()) return;
    if (it->second.timer_id > 0) {
        loop_->cancelTimer(it->second.timer_id);
        it->second.timer_id = 0;
    }
    it->second.pending = false;
}

void OmniRuntime::Impl::resumeHeartbeat(const std::string& service_name) {
    std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
    if (it == heartbeat_states_.end()) return;
    HeartbeatState& state = it->second;
    if (state.timer_id > 0) loop_->cancelTimer(state.timer_id);
    state.last_ack_time = platform::currentTimeMs();
    state.pending = false;
    state.timer_id = loop_->addTimer(state.interval_ms, [this, service_name]() {
        sendHeartbeatToService(service_name);
        checkHeartbeatTimeout(service_name);
    }, true);
}

void OmniRuntime::Impl::sendHeartbeatToService(const std::string& service_name) {
    std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
    if (it == heartbeat_states_.end()) {
        return;
    }

    HeartbeatState& state = it->second;
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
    std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
    if (it == heartbeat_states_.end()) {
        return;
    }

    HeartbeatState& state = it->second;
    if (!state.pending) {
        return;
    }

    int64_t now = platform::currentTimeMs();
    int64_t elapsed = now - state.last_ack_time;
    if (elapsed > static_cast<int64_t>(state.timeout_ms)) {
        OMNI_LOG_WARN(LOG_TAG, "Heartbeat timeout for %s (elapsed=%lld ms)", service_name.c_str(), elapsed);

        service_cache_.erase(service_name);
        if (conn_mgr_) {
            conn_mgr_->removeConnection(service_name);
        }

        std::map<std::string, ReconnectConfig>::iterator rc_it = reconnect_configs_.find(service_name);
        const bool reconnect_enabled = rc_it != reconnect_configs_.end()
            && rc_it->second.enabled;
        if (reconnect_enabled) {
            rc_it->second.current_retry = 0;
            scheduleReconnect(service_name, rc_it->second.interval_ms);
        }
        pauseHeartbeat(service_name);
        if (!reconnect_enabled) {
            heartbeat_states_.erase(service_name);
        }
    }
}

// ============================================================
// 数据面辅助
// ============================================================

bool OmniRuntime::Impl::sendOnFd(ITransport* transport, const Message& msg) {
    if (!transport) return false;
    Buffer buf;
    if (!msg.serialize(buf)) {
        return false;
    }
    // 通过 ITransport 抽象发送：循环处理部分写；ret<=0 表示 socket 暂不可写或出错
    size_t sent = 0;
    while (sent < buf.size()) {
        int ret = transport->send(buf.data() + sent, buf.size() - sent);
        if (ret <= 0) {
            return false;
        }
        sent += static_cast<size_t>(ret);
    }
    return true;
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

bool OmniRuntime::Impl::ensureTopicPublisherConnection(const std::string& topic_name,
                                                      const ServiceInfo& pub_info) {
    std::string pub_name = !pub_info.name.empty() ? pub_info.name : topicPublisherServiceName(topic_name);
    // 仅当使用自动生成的 "topic_pub_" 连接名时标记为话题发布者专属连接，
    // 使 onDirectDisconnect 的订阅清理与原先字符串前缀判定的触发范围一致
    bool is_auto_topic_name = pub_info.name.empty();
    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        pub_name, pub_info.host, pub_info.port, pub_info.host_id, pub_info.shm_config,
        is_auto_topic_name, topic_name);
    if (!conn) {
        return false;
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    Message sub_msg(MessageType::MSG_SUBSCRIBE_BROADCAST, 0);
    sub_msg.payload.writeUint32(topic_id);
    sub_msg.payload.writeString(topic_name);

    OMNI_LOG_INFO(LOG_TAG, "Topic %s uses %s data path to publisher %s",
                    topic_name.c_str(), dataChannelKindName(conn->transport->type()),
                    pub_name.c_str());
    return conn_mgr_->sendMessage(pub_name, sub_msg);
}

} // namespace omnibinder
