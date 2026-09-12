/**************************************************************************************************
 * @file        event_loop.h
 * @brief       单线程事件循环
 * @details     单线程事件循环实现，支持 fd 事件监听（读/写/错误）、
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

namespace omnibinder { namespace platform { class EventBackend; } }

namespace omnibinder {

/*
 * @brief  单线程事件循环
 * @details 支持：
 *            - fd 事件监听（读/写/错误）
 *            - 定时器（一次性和周期性）
 *            - 跨线程投递回调（通过 eventfd 唤醒）
 *          通过 EventBackend 接口实现平台无关。
 */
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

    /*
     * @brief  运行事件循环（阻塞，直到 stop() 被调用）
     * @note   stop 是持久状态：若 stop 已在 run 之前/启动窗口内被请求，
     *         run 立即返回，不会被重新武装（running_ 不再置 true）
     */
    void run();

    /*
     * @brief  处理一轮事件
     * @param[in] timeout_ms 最大等待时间，0 = 不等待，-1 = 无限等待
     */
    void pollOnce(int timeout_ms = -1);

    void pollOnceWithoutFunctors(int timeout_ms = -1);

    /*
     * @brief  停止事件循环
     * @note   幂等；将事件循环置为持久关闭状态（closed_），此后 post() 一律拒绝，
     *         run() 不再进入驱动循环
     */
    void stop();

    /*
     * @brief  查询 stop 是否已被请求（持久状态，run() 不清除）
     */
    bool stopRequested() const { return stop_requested_.load(); }

    // ============================================================
    // FD 事件管理
    // ============================================================

    /*
     * @brief  添加 fd 监听
     */
    void addFd(int fd, uint32_t events, const EventCallback& callback);

    /*
     * @brief  修改 fd 监听的事件类型
     */
    void modifyFd(int fd, uint32_t events);

    /*
     * @brief  移除 fd 监听
     */
    void removeFd(int fd);

    // ============================================================
    // 定时器管理
    // ============================================================

    /*
     * @brief  添加定时器，返回 timer_id
     * @param[in] delay_ms 延迟毫秒数
     * @param[in] callback 回调函数
     * @param[in] repeat   是否周期性重复
     */
    uint32_t addTimer(uint32_t delay_ms, const Functor& callback, bool repeat = false);

    /*
     * @brief  取消定时器
     */
    void cancelTimer(uint32_t timer_id);

    // ============================================================
    // 跨线程投递（线程安全）
    // ============================================================

    /*
     * @brief  投递一个回调到事件循环线程执行
     * @return true 已入队；false 事件循环已 stop（closed），回调未入队，
     *         调用方应快速失败而不是等待
     */
    bool post(const Functor& func);

private:
    /*
     * @brief  fd 信息
     */
    struct FdEntry {
        int            fd;
        uint32_t       events;
        EventCallback  callback;

        FdEntry() : fd(-1), events(0) {}
    };

    /*
     * @brief  定时器信息
     */
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

    /*
     * @brief  处理 eventfd 上的唤醒通知
     */
    void onWakeup(int fd, uint32_t events);

    /*
     * @brief  执行所有待处理的投递回调
     */
    void processPendingFunctors();

    void pollOnceInternal(int timeout_ms, bool process_functors);

    /*
     * @brief  处理到期的定时器，返回距下一个定时器到期的毫秒数（-1 表示无定时器）
     */
    int processTimers();

    /*
     * @brief  计算 poll 超时值（考虑定时器）
     */
    int calculateTimeout(int requested_timeout_ms);

    /*
     * @brief  唤醒事件循环
     */
    void wakeup();

    // ============================================================
    // 成员变量
    // ============================================================

    std::atomic<bool>           running_;
    // stop 的持久请求状态：run() 只能据此退出，不得清除或重新武装（stop 为终态）
    std::atomic<bool>           stop_requested_;
    platform::EventBackend*     backend_;
    int                         wakeup_fd_;

    // fd -> FdEntry 映射
    std::map<int, FdEntry>      fd_entries_;

    // 定时器列表
    std::vector<TimerEntry>     timers_;
    uint32_t                    next_timer_id_;

    // 投递队列（线程安全）
    std::mutex                  pending_mutex_;
    std::vector<Functor>        pending_functors_;
    // 投递队列是否已随 stop 关闭。由 pending_mutex_ 保护，且必须先于
    // running_=false 置位：run() 退出后的最终排空发生在关闭之后，
    // 关闭前入队的 functor 必被排空，关闭后被 post 拒绝，杜绝"投递丢失"。
    bool                        closed_;

    // 上一次 poll 以 process_functors=false 消费了唤醒、但未执行 functor 时置位。
    // 使下一次 pollOnce(true) 优先补处理，避免"唤醒丢失"导致投递线程等待超时/永久挂起。
    bool                        functor_wakeup_pending_;
};

} // namespace omnibinder

#endif // OMNIBINDER_EVENT_LOOP_H
