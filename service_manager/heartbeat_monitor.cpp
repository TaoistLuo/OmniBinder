#include "heartbeat_monitor.h"
#include "omnibinder/log.h"
#include "platform/platform.h"

#define TAG "HeartbeatMonitor"

namespace omnibinder {

HeartbeatMonitor::HeartbeatMonitor(uint32_t timeout_ms, uint32_t max_missed)
    : timeout_ms_(timeout_ms)
    , max_missed_(max_missed)
{
}

HeartbeatMonitor::~HeartbeatMonitor()
{
}

void HeartbeatMonitor::updateHeartbeat(const std::string& service_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(service_name);
    if (it == entries_.end()) {
        // Not tracked yet, start tracking
        HeartbeatEntry entry;
        entry.last_heartbeat_ms = platform::currentTimeMs();
        entry.missed_count = 0;
        entries_[service_name] = entry;
        OMNI_LOG_DEBUG(TAG, "Started tracking heartbeat for: %s", service_name.c_str());
    } else {
        it->second.last_heartbeat_ms = platform::currentTimeMs();
        it->second.missed_count = 0;
        OMNI_LOG_DEBUG(TAG, "Heartbeat received from: %s", service_name.c_str());
    }
}

void HeartbeatMonitor::startTracking(const std::string& service_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    HeartbeatEntry entry;
    entry.last_heartbeat_ms = platform::currentTimeMs();
    entry.missed_count = 0;
    entries_[service_name] = entry;

    OMNI_LOG_DEBUG(TAG, "Started tracking heartbeat for: %s", service_name.c_str());
}

void HeartbeatMonitor::stopTracking(const std::string& service_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(service_name);
    if (it != entries_.end()) {
        entries_.erase(it);
        OMNI_LOG_DEBUG(TAG, "Stopped tracking heartbeat for: %s", service_name.c_str());
    }
}

std::vector<std::string> HeartbeatMonitor::checkTimeouts()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> timed_out;
    int64_t now = platform::currentTimeMs();

    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        HeartbeatEntry& entry = it->second;
        int64_t elapsed = now - entry.last_heartbeat_ms;

        if (elapsed >= static_cast<int64_t>(timeout_ms_)) {
            // Heartbeat missed
            entry.missed_count++;
            entry.last_heartbeat_ms = now;  // Reset timer for next check

            OMNI_LOG_DEBUG(TAG, "Service %s missed heartbeat (%u/%u)",
                            it->first.c_str(), entry.missed_count, max_missed_);

            if (entry.missed_count >= max_missed_) {
                timed_out.push_back(it->first);
                OMNI_LOG_WARN(TAG, "Service %s timed out (missed %u heartbeats)",
                               it->first.c_str(), entry.missed_count);
            }
        }
    }

    // Remove timed out entries
    for (size_t i = 0; i < timed_out.size(); ++i) {
        entries_.erase(timed_out[i]);
    }

    return timed_out;
}

bool HeartbeatMonitor::isTimedOut(const std::string& service_name) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(service_name);
    if (it == entries_.end()) {
        // Not tracked, consider it timed out
        return true;
    }

    int64_t now = platform::currentTimeMs();
    int64_t elapsed = now - it->second.last_heartbeat_ms;

    // Calculate total timeout threshold
    int64_t total_timeout = static_cast<int64_t>(timeout_ms_) * max_missed_;

    return elapsed >= total_timeout;
}

size_t HeartbeatMonitor::trackedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void HeartbeatMonitor::setTimeout(uint32_t timeout_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    timeout_ms_ = timeout_ms;
}

void HeartbeatMonitor::setMaxMissed(uint32_t max_missed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    max_missed_ = max_missed;
}

} // namespace omnibinder
