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
#include "omnibinder/log.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <type_traits>

namespace omnibinder {

#define OWNER_THREAD_EXECUTOR_LOG_TAG "OwnerThreadExecutor"

template<typename T>
class ExecutionResult {
public:
    ExecutionResult()
        : ok_(false)
        , value_(NULL) {
    }

    static ExecutionResult makeSuccess(const T& value) {
        ExecutionResult result;
        result.ok_ = true;
        result.value_ = new T(value);
        return result;
    }

    static ExecutionResult makeFailure(const std::string& error) {
        ExecutionResult result;
        result.ok_ = false;
        result.error_ = error;
        return result;
    }

    ExecutionResult(const ExecutionResult& other)
        : ok_(other.ok_)
        , value_(other.value_ ? new T(*other.value_) : NULL)
        , error_(other.error_) {
    }

    ExecutionResult& operator=(const ExecutionResult& other) {
        if (this != &other) {
            delete value_;
            ok_ = other.ok_;
            value_ = other.value_ ? new T(*other.value_) : NULL;
            error_ = other.error_;
        }
        return *this;
    }

    ~ExecutionResult() {
        delete value_;
    }

    bool ok() const { return ok_; }
    const T& value() const { return *value_; }
    const std::string& error() const { return error_; }

private:
    bool ok_;
    T* value_;
    std::string error_;
};

template<>
class ExecutionResult<void> {
public:
    ExecutionResult()
        : ok_(false) {
    }

    static ExecutionResult makeSuccess() {
        ExecutionResult result;
        result.ok_ = true;
        return result;
    }

    static ExecutionResult makeFailure(const std::string& error) {
        ExecutionResult result;
        result.ok_ = false;
        result.error_ = error;
        return result;
    }

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

private:
    bool ok_;
    std::string error_;
};

template<typename T>
class SyncCallState {
public:
    SyncCallState()
        : done_(false)
        , result_(NULL) {
    }

    ~SyncCallState() {
        delete result_;
    }

    void completeSuccess(const T& value) {
        complete(ExecutionResult<T>::makeSuccess(value));
    }

    void completeFailure(const std::string& error) {
        complete(ExecutionResult<T>::makeFailure(error));
    }

    ExecutionResult<T> wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]{ return done_; });
        ExecutionResult<T> result = *result_;
        return result;
    }

private:
    void complete(const ExecutionResult<T>& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_) {
            delete result_;
            result_ = new ExecutionResult<T>(result);
            done_ = true;
            cond_.notify_one();
        }
    }

    std::mutex mutex_;
    std::condition_variable cond_;
    bool done_;
    ExecutionResult<T>* result_;
};

template<>
class SyncCallState<void> {
public:
    SyncCallState()
        : done_(false)
        , result_(NULL) {
    }

    ~SyncCallState() {
        delete result_;
    }

    void completeSuccess() {
        complete(ExecutionResult<void>::makeSuccess());
    }

    void completeFailure(const std::string& error) {
        complete(ExecutionResult<void>::makeFailure(error));
    }

    ExecutionResult<void> wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]{ return done_; });
        ExecutionResult<void> result = *result_;
        return result;
    }

private:
    void complete(const ExecutionResult<void>& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_) {
            delete result_;
            result_ = new ExecutionResult<void>(result);
            done_ = true;
            cond_.notify_one();
        }
    }

    std::mutex mutex_;
    std::condition_variable cond_;
    bool done_;
    ExecutionResult<void>* result_;
};

class OwnerThreadExecutor {
public:
    OwnerThreadExecutor()
        : loop_(NULL)
        , owner_thread_id_()
        , loop_owned_(false) {
    }

    void bindLoop(EventLoop* loop) {
        loop_ = loop;
    }

    void setOwnerThread(const std::thread::id& tid) {
        owner_thread_id_ = tid;
    }

    bool hasOwnerThread() const {
        return owner_thread_id_ != std::thread::id();
    }

    void setLoopOwned(bool owned) {
        loop_owned_.store(owned);
    }

    bool isOwnerThread() const {
        return hasOwnerThread() && owner_thread_id_ == std::this_thread::get_id();
    }

    bool canRunInline() const {
        return loop_ == NULL || !loop_owned_.load() || isOwnerThread();
    }

    template<typename F>
    typename std::enable_if<!std::is_void<typename std::result_of<F()>::type>::value,
                            typename std::result_of<F()>::type>::type
    invoke(F func) {
        typedef typename std::result_of<F()>::type Result;
        if (canRunInline()) {
            return func();
        }

        std::shared_ptr< SyncCallState<Result> > state(new SyncCallState<Result>());
        loop_->post([func, state]() {
            state->completeSuccess(func());
        });

        ExecutionResult<Result> result = state->wait();
        return result.ok() ? result.value() : Result();
    }

    template<typename F>
    typename std::enable_if<std::is_void<typename std::result_of<F()>::type>::value,
                            int>::type
    invoke(F func) {
        if (canRunInline()) {
            func();
            return 0;
        }

        std::shared_ptr< SyncCallState<int> > state(new SyncCallState<int>());
        loop_->post([func, state]() {
            func();
            state->completeSuccess(0);
        });

        ExecutionResult<int> result = state->wait();
        return result.ok() ? result.value() : -1;
    }

    template<typename F>
    typename std::enable_if<!std::is_void<typename std::result_of<F()>::type>::value,
                           typename std::result_of<F()>::type>::type
    invokeOnOwner(F func) {
        typedef typename std::result_of<F()>::type Result;
        if (loop_ == NULL || !hasOwnerThread() || isOwnerThread()) {
            return func();
        }

        std::shared_ptr< SyncCallState<Result> > state(new SyncCallState<Result>());
        loop_->post([func, state]() {
            state->completeSuccess(func());
        });

        ExecutionResult<Result> result = state->wait();
        if (!result.ok()) {
            return Result();
        }
        return result.value();
    }

    template<typename F>
    typename std::enable_if<std::is_void<typename std::result_of<F()>::type>::value,
                           void>::type
    invokeOnOwner(F func) {
        if (loop_ == NULL || !hasOwnerThread() || isOwnerThread()) {
            func();
            return;
        }

        std::shared_ptr< SyncCallState<void> > state(new SyncCallState<void>());
        loop_->post([func, state]() {
            func();
            state->completeSuccess();
        });

        ExecutionResult<void> result = state->wait();
    }

    EventLoop* loop_;
    std::thread::id owner_thread_id_;
    std::atomic<bool> loop_owned_;
};

}

#undef OWNER_THREAD_EXECUTOR_LOG_TAG

#endif
