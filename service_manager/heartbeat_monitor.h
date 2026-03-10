/**************************************************************************************************
 * @file        heartbeat_monitor.h
 * @brief       服务心跳监控器
 * @details     跟踪每个已注册服务的心跳时间戳，检测心跳超时的服务。
 *              ServiceManager 周期性调用 checkTimeouts() 获取超时服务列表，
 *              并触发相应的死亡通知和清理流程。支持可配置的超时时间和
 *              最大允许丢失心跳次数。线程安全。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef OMNIBINDER_HEARTBEAT_MONITOR_H
#define OMNIBINDER_HEARTBEAT_MONITOR_H

#include "omnibinder/types.h"
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <stdint.h>

namespace omnibinder {

// ============================================================
// HeartbeatMonitor - Tracks heartbeat timestamps per service
//
// Each registered service is expected to send periodic heartbeats.
// The monitor tracks the last heartbeat time and can detect
// services that have timed out (missed too many heartbeats).
// ============================================================
class HeartbeatMonitor {
public:
    // Create a heartbeat monitor with the given timeout and max missed count.
    // timeout_ms: time after which a single heartbeat is considered missed
    // max_missed: number of consecutive missed heartbeats before declaring timeout
    HeartbeatMonitor(uint32_t timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT,
                     uint32_t max_missed = DEFAULT_MAX_MISSED_HEARTBEATS);

    ~HeartbeatMonitor();

    // Disable copy
    HeartbeatMonitor(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor& operator=(const HeartbeatMonitor&) = delete;

    // Record a heartbeat for the given service.
    // If the service is not yet tracked, it will be added.
    void updateHeartbeat(const std::string& service_name);

    // Start tracking a service (sets initial heartbeat time to now).
    void startTracking(const std::string& service_name);

    // Stop tracking a service.
    void stopTracking(const std::string& service_name);

    // Check all tracked services for timeouts.
    // Returns the list of service names that have timed out.
    std::vector<std::string> checkTimeouts();

    // Check if a specific service has timed out.
    bool isTimedOut(const std::string& service_name) const;

    // Get the number of tracked services.
    size_t trackedCount() const;

    // Set timeout parameters
    void setTimeout(uint32_t timeout_ms);
    void setMaxMissed(uint32_t max_missed);

    // Get timeout parameters
    uint32_t getTimeout() const { return timeout_ms_; }
    uint32_t getMaxMissed() const { return max_missed_; }

private:
    struct HeartbeatEntry {
        int64_t last_heartbeat_ms;  // Timestamp of last heartbeat
        uint32_t missed_count;      // Number of consecutive missed heartbeats

        HeartbeatEntry() : last_heartbeat_ms(0), missed_count(0) {}
    };

    mutable std::mutex mutex_;
    std::map<std::string, HeartbeatEntry> entries_;
    uint32_t timeout_ms_;
    uint32_t max_missed_;
};

} // namespace omnibinder

#endif // OMNIBINDER_HEARTBEAT_MONITOR_H
