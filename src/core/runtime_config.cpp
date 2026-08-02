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
    });
}

void OmniRuntime::Impl::setRegisterHost(const std::string& host) {
    callSerialized([this, host]() {
        register_host_ = host;
    });
}

std::string OmniRuntime::Impl::getRegisterHost() const {
    // 直接加锁拷贝，避免返回内部引用导致跨线程数据竞争
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    return register_host_;
}

void OmniRuntime::Impl::setDefaultTimeout(uint32_t ms) {
    callSerialized([this, ms]() {
        rpc_runtime_.setDefaultTimeout(ms);
    });
}

std::string OmniRuntime::Impl::hostId() const {
    // 直接加锁拷贝，避免返回内部引用导致跨线程数据竞争
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    return host_id_;
}

// ============================================================
// 线程模型基础设施
// ============================================================

bool OmniRuntime::Impl::isOwnerThread() const {
    return owner_executor_.isOwnerThread();
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
    stats.active_connections = conn_mgr_->activeConnectionCount();
    stats.tcp_connections = conn_mgr_->tcpConnectionCount();
    stats.shm_connections = conn_mgr_->shmConnectionCount();
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
        service_cache_.clear();
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
