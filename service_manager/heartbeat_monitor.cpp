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

void HeartbeatMonitor::updateHeartbeat(const ServiceIdentity& identity)
{
    const std::string key = makeServiceIdentityKey(identity);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        HeartbeatEntry entry;
        entry.identity = identity;
        entry.last_heartbeat_ms = platform::currentTimeMs();
        entries_[key] = entry;
        OMNI_LOG_DEBUG(TAG, "Started tracking heartbeat for: %s", key.c_str());
    } else {
        it->second.last_heartbeat_ms = platform::currentTimeMs();
        OMNI_LOG_DEBUG(TAG, "Heartbeat received from: %s", key.c_str());
    }
}

void HeartbeatMonitor::startTracking(const ServiceIdentity& identity)
{
    updateHeartbeat(identity);
}

void HeartbeatMonitor::stopTracking(const ServiceIdentity& identity)
{
    auto it = entries_.find(makeServiceIdentityKey(identity));
    if (it != entries_.end()) {
        entries_.erase(it);
        OMNI_LOG_DEBUG(TAG, "Stopped tracking heartbeat for: %s", identity.name.c_str());
    }
}

std::vector<ServiceIdentity> HeartbeatMonitor::checkTimeouts()
{
    std::vector<ServiceIdentity> timed_out;
    int64_t now = platform::currentTimeMs();

    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        HeartbeatEntry& entry = it->second;
        int64_t elapsed = now - entry.last_heartbeat_ms;

        if (elapsed >= static_cast<int64_t>(timeout_ms_)) {
            uint32_t missed = static_cast<uint32_t>(elapsed / static_cast<int64_t>(timeout_ms_));

            OMNI_LOG_DEBUG(TAG, "Service %s missed heartbeat (%u/%u)",
                            it->first.c_str(), missed, max_missed_);

            if (missed >= max_missed_) {
                timed_out.push_back(entry.identity);
                OMNI_LOG_WARN(TAG, "Service %s timed out (missed %u heartbeats)",
                               it->first.c_str(), missed);
            }
        }
    }

    for (size_t i = 0; i < timed_out.size(); ++i) {
        entries_.erase(makeServiceIdentityKey(timed_out[i]));
    }

    return timed_out;
}

size_t HeartbeatMonitor::trackedCount() const
{
    return entries_.size();
}

} // namespace omnibinder
