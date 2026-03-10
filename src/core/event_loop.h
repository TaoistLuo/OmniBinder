/**************************************************************************************************
 * @file        event_loop.h
 * @brief       单线程事件循环
 * @details     基于 epoll（Linux）的单线程事件循环实现，支持 fd 事件监听（读/写/错误）、
 *              一次性和周期性定时器、以及通过 eventfd 实现的跨线程回调投递。
 *              OmniRuntime 和 ServiceManager 的核心驱动引擎。
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
#ifndef OMNIBINDER_EVENT_LOOP_H
#define OMNIBINDER_EVENT_LOOP_H

#include <stdint.h>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

namespace omnibinder {

// ============================================================
// EventLoop - 单线程事件循环
//
// 支持：
//   - fd 事件监听（读/写/错误）
//   - 定时器（一次性和周期性）
//   - 跨线程投递回调（通过 eventfd 唤醒）
//
// Linux 使用 epoll，Windows 使用 select（桩实现）
// ============================================================

class EventLoop {
public:
    typedef std::function<void()> Functor;
    typedef std::function<void(int fd, uint32_t events)> EventCallback;

    // 事件标志
    static const uint32_t EVENT_READ  = 0x01;
    static const uint32_t EVENT_WRITE = 0x02;
    static const uint32_t EVENT_ERROR = 0x04;

    EventLoop();
    ~EventLoop();

    // 禁止拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 运行事件循环（阻塞，直到 stop() 被调用）
    void run();

    // 处理一轮事件
    // timeout_ms: 最大等待时间，0 = 不等待，-1 = 无限等待
    void pollOnce(int timeout_ms = -1);

    // 停止事件循环
    void stop();

    // 是否正在运行
    bool isRunning() const;

    // ============================================================
    // FD 事件管理
    // ============================================================

    // 添加 fd 监听
    void addFd(int fd, uint32_t events, const EventCallback& callback);

    // 修改 fd 监听的事件类型
    void modifyFd(int fd, uint32_t events);

    // 移除 fd 监听
    void removeFd(int fd);

    // ============================================================
    // 定时器管理
    // ============================================================

    // 添加定时器，返回 timer_id
    // delay_ms: 延迟毫秒数
    // callback: 回调函数
    // repeat:   是否周期性重复
    uint32_t addTimer(uint32_t delay_ms, const Functor& callback, bool repeat = false);

    // 取消定时器
    void cancelTimer(uint32_t timer_id);

    // ============================================================
    // 跨线程投递（线程安全）
    // ============================================================

    // 投递一个回调到事件循环线程执行
    void post(const Functor& func);

private:
    // fd 信息
    struct FdEntry {
        int            fd;
        uint32_t       events;
        EventCallback  callback;

        FdEntry() : fd(-1), events(0) {}
    };

    // 定时器信息
    struct TimerEntry {
        uint32_t  id;
        int64_t   expire_ms;    // 绝对到期时间
        uint32_t  interval_ms;  // 周期间隔（0 表示一次性）
        bool      repeat;
        bool      cancelled;
        Functor   callback;

        TimerEntry()
            : id(0), expire_ms(0), interval_ms(0)
            , repeat(false), cancelled(false) {}
    };

    // 初始化平台后端
    void initBackend();

    // 销毁平台后端
    void destroyBackend();

    // 处理 eventfd 上的唤醒通知
    void handleWakeup(int fd, uint32_t events);

    // 执行所有待处理的投递回调
    void processPendingFunctors();

    // 处理到期的定时器，返回距下一个定时器到期的毫秒数（-1 表示无定时器）
    int processTimers();

    // 计算 epoll_wait 的超时值
    int calculateTimeout(int requested_timeout_ms);

    // 唤醒事件循环
    void wakeup();

    // 将内部事件标志转换为平台 epoll 事件
    static uint32_t toPlatformEvents(uint32_t events);

    // 将平台 epoll 事件转换为内部事件标志
    static uint32_t fromPlatformEvents(uint32_t epoll_events);

    // ============================================================
    // 成员变量
    // ============================================================

    std::atomic<bool>           running_;
    int                         epoll_fd_;
    int                         wakeup_fd_;

    // fd -> FdEntry 映射
    std::map<int, FdEntry>      fd_entries_;

    // 定时器列表
    std::vector<TimerEntry>     timers_;
    uint32_t                    next_timer_id_;

    // 投递队列（线程安全）
    std::mutex                  pending_mutex_;
    std::vector<Functor>        pending_functors_;
};

} // namespace omnibinder

#endif // OMNIBINDER_EVENT_LOOP_H
