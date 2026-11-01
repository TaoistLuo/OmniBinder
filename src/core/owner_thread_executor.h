/**************************************************************************************************
 * @file        owner_thread_executor.h
 * @brief       Owner 线程执行器
 * @details     提供线程归属判断和跨线程函数投递能力。用于 OmniRuntime::Impl 的
 *              callSerialized() 模板，确保所有回调在 owner event-loop 线程执行。
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
#ifndef OMNIBINDER_OWNER_THREAD_EXECUTOR_H
#define OMNIBINDER_OWNER_THREAD_EXECUTOR_H

#include "event_loop.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <type_traits>

namespace omnibinder {

template<typename T>
class SyncCallState {
public:
    explicit SyncCallState(bool spin_wait = false)
        : done_(false)
        , spin_wait_(spin_wait)
        , value_() {
    }

    void completeSuccess(const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_.load(std::memory_order_relaxed)) {
            value_ = value;
            done_.store(true, std::memory_order_release);
            cond_.notify_one();
        }
    }

    T wait() {
        if (spin_wait_) {
            while (!done_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return value_;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]{ return done_.load(std::memory_order_acquire); });
        return value_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> done_;
    bool spin_wait_;
    T value_;
};

template<>
class SyncCallState<void> {
public:
    explicit SyncCallState(bool spin_wait = false)
        : done_(false)
        , spin_wait_(spin_wait) {
    }

    void completeSuccess() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_.load(std::memory_order_relaxed)) {
            done_.store(true, std::memory_order_release);
            cond_.notify_one();
        }
    }

    void wait() {
        if (spin_wait_) {
            while (!done_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]{ return done_.load(std::memory_order_acquire); });
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> done_;
    bool spin_wait_;
};

class OwnerThreadExecutor {
public:
    OwnerThreadExecutor()
        : loop_(NULL)
        , owner_thread_id_()
        , spin_wait_(false) {
    }

    void bindLoop(EventLoop* loop) {
        loop_ = loop;
    }

    void setSpinWait(bool enabled) {
        spin_wait_.store(enabled);
    }

    void setOwnerThread(const std::thread::id& tid) {
        owner_thread_id_.store(tid);
    }

    bool hasOwnerThread() const {
        return owner_thread_id_.load() != std::thread::id();
    }

    bool isOwnerThread() const {
        return hasOwnerThread() && owner_thread_id_.load() == std::this_thread::get_id();
    }

    /*
     * @brief  尝试把 func 投递到 owner 线程执行
     * @return true  func 已在 owner 线程（或就地）执行完成；
     *         false event-loop 已 stop（post 被拒绝），func 未执行，调用方应快速失败
     * @note   与 EventLoop::stop 的关闭动作通过投递队列锁互斥，不存在
     *         "post 成功后 loop 停止导致 functor 永不执行"的丢失唤醒窗口
     */
    template<typename F>
    typename std::enable_if<!std::is_void<typename std::result_of<F()>::type>::value,
                            bool>::type
    tryInvokeOnOwner(F func, typename std::result_of<F()>::type& out) {
        typedef typename std::result_of<F()>::type Result;
        if (loop_ == NULL || !hasOwnerThread() || isOwnerThread()) {
            out = func();
            return true;
        }

        std::shared_ptr< SyncCallState<Result> > state =
            std::make_shared< SyncCallState<Result> >(spin_wait_.load());
        if (!loop_->post([func, state]() {
                state->completeSuccess(func());
            })) {
            return false;
        }
        out = state->wait();
        return true;
    }

    template<typename F>
    typename std::enable_if<std::is_void<typename std::result_of<F()>::type>::value,
                            bool>::type
    tryInvokeOnOwner(F func) {
        if (loop_ == NULL || !hasOwnerThread() || isOwnerThread()) {
            func();
            return true;
        }

        std::shared_ptr< SyncCallState<void> > state =
            std::make_shared< SyncCallState<void> >(spin_wait_.load());
        if (!loop_->post([func, state]() {
                func();
                state->completeSuccess();
            })) {
            return false;
        }

        state->wait();
        return true;
    }

    EventLoop* loop_;
    // atomic：owner_thread_id_ 可能被多线程并发首设（run 的 driver 争夺），
    // 无锁读写 std::thread::id 有数据竞争，改为 atomic 消除
    std::atomic<std::thread::id> owner_thread_id_;
    std::atomic<bool> spin_wait_;
};

}

#endif
