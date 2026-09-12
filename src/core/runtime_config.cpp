#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#include <thread>

#define LOG_TAG "OmniRuntimeConfig"

namespace omnibinder {

// ============================================================
// 配置 — 线程安全的设置/查询
// ============================================================

void OmniRuntime::Impl::setHeartbeatInterval(uint32_t ms) {
    callSerialized([this, ms]() {
        heartbeat_interval_ms_ = ms;
        if (loop_ && initialized_ && heartbeat_timer_id_ > 0) {
            loop_->cancelTimer(heartbeat_timer_id_);
            heartbeat_timer_id_ = loop_->addTimer(heartbeat_interval_ms_,
                [this]() { this->sendHeartbeat(); }, true);
        }
    });
}

void OmniRuntime::Impl::setRegisterHost(const std::string& host) {
    // register_host_ 统一由 api_mutex_ 保护：设置/读取/resolveRegisterHost 同一锁纪律。
    // 不再经 callSerialized（owner 线程执行时不持锁），避免与带锁读取并发产生数据竞争
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    register_host_ = host;
}

std::string OmniRuntime::Impl::getRegisterHost() const {
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    return register_host_;
}

void OmniRuntime::Impl::setDefaultTimeout(uint32_t ms) {
    callSerialized([this, ms]() {
        rpc_runtime_.setDefaultTimeout(ms);
    });
}

void OmniRuntime::Impl::setReplySendTimeout(uint32_t ms) {
    callSerialized([this, ms]() {
        reply_send_timeout_ms_ = ms;
    });
}

std::string OmniRuntime::Impl::hostId() const {
    // 直接加锁拷贝，避免返回内部引用导致跨线程数据竞争
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    return host_id_;
}

// ============================================================
// 统计
// ============================================================

void OmniRuntime::Impl::updateConnectionStats(RuntimeStats& stats) const {
    if (!conn_mgr_) {
        stats.active_connections = 0;
        stats.tcp_connections = 0;
        stats.shm_connections = 0;
        return;
    }
    conn_mgr_->connectionCounts(stats.active_connections, stats.tcp_connections,
                                stats.shm_connections);
}

int OmniRuntime::Impl::getStats(RuntimeStats& stats) {
    return callSerialized([this, &stats]() -> int {
        return getStatsInternal(stats);
    });
}

int OmniRuntime::Impl::getStatsInternal(RuntimeStats& stats) {
    stats = stats_;
    updateConnectionStats(stats);
    return 0;
}

int OmniRuntime::Impl::resetStats() {
    return callSerialized([this]() -> int {
        resetStatsInternal();
        return 0;
    });
}

int OmniRuntime::Impl::resetStatsInternal() {
    stats_ = RuntimeStats();
    return 0;
}

void OmniRuntime::Impl::clearServiceCache() {
    callSerialized([this]() {
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
    });
}

void OmniRuntime::Impl::closeAllConnections() {
    callSerialized([this]() {
        if (conn_mgr_) {
            conn_mgr_->closeAll();
        }
    });
}

} // namespace omnibinder
