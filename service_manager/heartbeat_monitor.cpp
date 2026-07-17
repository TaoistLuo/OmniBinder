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
        HeartbeatEntry entry;
        entry.last_heartbeat_ms = platform::currentTimeMs();
        entries_[service_name] = entry;
        OMNI_LOG_DEBUG(TAG, "Started tracking heartbeat for: %s", service_name.c_str());
    } else {
        it->second.last_heartbeat_ms = platform::currentTimeMs();
        OMNI_LOG_DEBUG(TAG, "Heartbeat received from: %s", service_name.c_str());
    }
}

void HeartbeatMonitor::startTracking(const std::string& service_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    HeartbeatEntry entry;
    entry.last_heartbeat_ms = platform::currentTimeMs();
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
            uint32_t missed = static_cast<uint32_t>(elapsed / static_cast<int64_t>(timeout_ms_));

            OMNI_LOG_DEBUG(TAG, "Service %s missed heartbeat (%u/%u)",
                            it->first.c_str(), missed, max_missed_);

            if (missed >= max_missed_) {
                timed_out.push_back(it->first);
                OMNI_LOG_WARN(TAG, "Service %s timed out (missed %u heartbeats)",
                               it->first.c_str(), missed);
            }
        }
    }

    for (size_t i = 0; i < timed_out.size(); ++i) {
        entries_.erase(timed_out[i]);
    }

    return timed_out;
}

size_t HeartbeatMonitor::trackedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace omnibinder
