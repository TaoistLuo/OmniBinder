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
// HeartbeatMonitor — 跟踪每个服务的心跳时间戳
//
// 已注册服务定期发送心跳，监控器跟踪最近心跳时间，
// 检测超过超时阈值的服务（连续丢失过多心跳）。
// ============================================================
class HeartbeatMonitor {
public:
    /*
     * @brief  创建心跳监控器
     * @param[in]  timeout_ms 单次心跳判定为丢失的超时时间
     * @param[in]  max_missed 连续丢失多少次心跳后判定服务离线
     */
    HeartbeatMonitor(uint32_t timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT,
                     uint32_t max_missed = DEFAULT_MAX_MISSED_HEARTBEATS);

    ~HeartbeatMonitor();

    // 禁止拷贝
    HeartbeatMonitor(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor& operator=(const HeartbeatMonitor&) = delete;

    /*
     * @brief  记录指定服务的本次心跳
     * @param[in]  service_name 服务名称
     * @note   如果该服务尚未被跟踪，自动开始跟踪
     */
    void updateHeartbeat(const std::string& service_name);

    /*
     * @brief  开始跟踪指定服务
     * @param[in]  service_name 服务名称
     * @note   心跳起始时间设为当前时刻
     */
    void startTracking(const std::string& service_name);

    /*
     * @brief  停止跟踪指定服务
     * @param[in]  service_name 服务名称
     */
    void stopTracking(const std::string& service_name);

    /*
     * @brief  检查所有已跟踪服务是否超时
     * @return 已超时的服务名称列表
     */
    std::vector<std::string> checkTimeouts();

    /*
     * @brief  获取当前已跟踪的服务数量
     * @return 已跟踪服务数
     */
    size_t trackedCount() const;

private:
    struct HeartbeatEntry {
        int64_t last_heartbeat_ms;  // 最近一次心跳的时间戳

        HeartbeatEntry() : last_heartbeat_ms(0) {}
    };

    mutable std::mutex mutex_;
    std::map<std::string, HeartbeatEntry> entries_;
    uint32_t timeout_ms_;
    uint32_t max_missed_;
};

} // namespace omnibinder

#endif // OMNIBINDER_HEARTBEAT_MONITOR_H
