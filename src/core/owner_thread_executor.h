#ifndef OMNIBINDER_OWNER_THREAD_EXECUTOR_H
#define OMNIBINDER_OWNER_THREAD_EXECUTOR_H

#include "event_loop.h"
#include "omnibinder/log.h"

#include <pthread.h>

#include <atomic>
#include <memory>
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
        pthread_mutex_init(&mutex_, NULL);
        pthread_cond_init(&cond_, NULL);
    }

    ~SyncCallState() {
        delete result_;
        pthread_cond_destroy(&cond_);
        pthread_mutex_destroy(&mutex_);
    }

    void completeSuccess(const T& value) {
        complete(ExecutionResult<T>::makeSuccess(value));
    }

    void completeFailure(const std::string& error) {
        complete(ExecutionResult<T>::makeFailure(error));
    }

    ExecutionResult<T> wait() {
        pthread_mutex_lock(&mutex_);
        while (!done_) {
            pthread_cond_wait(&cond_, &mutex_);
        }
        ExecutionResult<T> result = *result_;
        pthread_mutex_unlock(&mutex_);
        return result;
    }

private:
    void complete(const ExecutionResult<T>& result) {
        pthread_mutex_lock(&mutex_);
        if (!done_) {
            delete result_;
            result_ = new ExecutionResult<T>(result);
            done_ = true;
            pthread_cond_signal(&cond_);
        }
        pthread_mutex_unlock(&mutex_);
    }

    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    bool done_;
    ExecutionResult<T>* result_;
};

template<>
class SyncCallState<void> {
public:
    SyncCallState()
        : done_(false)
        , result_(NULL) {
        pthread_mutex_init(&mutex_, NULL);
        pthread_cond_init(&cond_, NULL);
    }

    ~SyncCallState() {
        delete result_;
        pthread_cond_destroy(&cond_);
        pthread_mutex_destroy(&mutex_);
    }

    void completeSuccess() {
        complete(ExecutionResult<void>::makeSuccess());
    }

    void completeFailure(const std::string& error) {
        complete(ExecutionResult<void>::makeFailure(error));
    }

    ExecutionResult<void> wait() {
        pthread_mutex_lock(&mutex_);
        while (!done_) {
            pthread_cond_wait(&cond_, &mutex_);
        }
        ExecutionResult<void> result = *result_;
        pthread_mutex_unlock(&mutex_);
        return result;
    }

private:
    void complete(const ExecutionResult<void>& result) {
        pthread_mutex_lock(&mutex_);
        if (!done_) {
            delete result_;
            result_ = new ExecutionResult<void>(result);
            done_ = true;
            pthread_cond_signal(&cond_);
        }
        pthread_mutex_unlock(&mutex_);
    }

    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
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
        if (!result.ok()) {
            return Result();
        }
        return result.value();
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
        if (!result.ok()) {
            return -1;
        }
        return result.value();
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
