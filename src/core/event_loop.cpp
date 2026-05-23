#include "core/event_loop.h"
#include "platform/platform.h"
#include "platform/event_backend.h"
#include "omnibinder/log.h"

#include <algorithm>

#define LOG_TAG "EventLoop"

namespace omnibinder {

EventLoop::EventLoop()
    : running_(false)
    , backend_(NULL)
    , wakeup_fd_(-1)
    , next_timer_id_(1)
{
    backend_ = platform::createEventBackend();
    if (!backend_->init()) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to init event backend");
        delete backend_;
        backend_ = NULL;
        return;
    }

    wakeup_fd_ = platform::createEventFd();
    if (wakeup_fd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create wakeup eventfd");
        backend_->destroy();
        delete backend_;
        backend_ = NULL;
        return;
    }

    backend_->addFd(wakeup_fd_, platform::EVENT_READ);
    OMNI_LOG_DEBUG(LOG_TAG, "EventLoop initialized (wakeup_fd=%d)", wakeup_fd_);
}

EventLoop::~EventLoop()
{
    if (wakeup_fd_ >= 0) {
        platform::closeEventFd(wakeup_fd_);
        wakeup_fd_ = -1;
    }
    if (backend_) {
        backend_->destroy();
        delete backend_;
        backend_ = NULL;
    }

    fd_entries_.clear();
    timers_.clear();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_functors_.clear();
    }

    OMNI_LOG_DEBUG(LOG_TAG, "EventLoop destroyed");
}

void EventLoop::run()
{
    running_ = true;
    OMNI_LOG_INFO(LOG_TAG, "EventLoop started");

    while (running_) {
        pollOnce(-1);
    }

    processPendingFunctors();

    OMNI_LOG_INFO(LOG_TAG, "EventLoop stopped");
}

void EventLoop::stop()
{
    running_ = false;
    wakeup();
}

bool EventLoop::isRunning() const
{
    return running_;
}

void EventLoop::wakeup()
{
    if (wakeup_fd_ >= 0) {
        platform::eventFdNotify(wakeup_fd_);
    }
}

void EventLoop::pollOnce(int timeout_ms)
{
    pollOnceInternal(timeout_ms, true);
}

void EventLoop::pollOnceWithoutFunctors(int timeout_ms)
{
    pollOnceInternal(timeout_ms, false);
}

void EventLoop::pollOnceInternal(int timeout_ms, bool process_functors)
{
    if (!backend_) {
        return;
    }

    int actual_timeout = calculateTimeout(timeout_ms);

    const int MAX_EVENTS = 64;
    platform::ReadyEvent events[MAX_EVENTS];

    int nfds = backend_->poll(events, MAX_EVENTS, actual_timeout);

    if (nfds < 0) {
        return;
    }

    for (int i = 0; i < nfds; ++i) {
        int fd = events[i].fd;
        uint32_t revents = events[i].events;

        if (fd == wakeup_fd_) {
            onWakeup(fd, revents);
            continue;
        }

        std::map<int, FdEntry>::iterator it = fd_entries_.find(fd);
        if (it != fd_entries_.end() && it->second.callback) {
            it->second.callback(fd, revents);
        }
    }

    processTimers();

    if (process_functors) {
        processPendingFunctors();
    }
}

void EventLoop::addFd(int fd, uint32_t events, const EventCallback& callback)
{
    if (fd < 0) {
        OMNI_LOG_WARN(LOG_TAG, "addFd: invalid fd %d", fd);
        return;
    }

    if (fd_entries_.find(fd) != fd_entries_.end()) {
        OMNI_LOG_WARN(LOG_TAG, "addFd: fd %d already registered, use modifyFd instead", fd);
        return;
    }

    FdEntry entry;
    entry.fd = fd;
    entry.events = events;
    entry.callback = callback;
    fd_entries_[fd] = entry;

    if (backend_ && !backend_->addFd(fd, events)) {
        OMNI_LOG_ERROR(LOG_TAG, "backend addFd failed for fd %d", fd);
        fd_entries_.erase(fd);
        return;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Added fd %d with events 0x%x", fd, events);
}

void EventLoop::modifyFd(int fd, uint32_t events)
{
    std::map<int, FdEntry>::iterator it = fd_entries_.find(fd);
    if (it == fd_entries_.end()) {
        OMNI_LOG_WARN(LOG_TAG, "modifyFd: fd %d not found", fd);
        return;
    }

    it->second.events = events;

    if (backend_ && !backend_->modifyFd(fd, events)) {
        OMNI_LOG_ERROR(LOG_TAG, "backend modifyFd failed for fd %d", fd);
        return;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Modified fd %d with events 0x%x", fd, events);
}

void EventLoop::removeFd(int fd)
{
    std::map<int, FdEntry>::iterator it = fd_entries_.find(fd);
    if (it == fd_entries_.end()) {
        OMNI_LOG_DEBUG(LOG_TAG, "removeFd: fd %d not found (may already be removed)", fd);
        return;
    }

    fd_entries_.erase(it);

    if (backend_) {
        backend_->removeFd(fd);
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Removed fd %d", fd);
}

uint32_t EventLoop::addTimer(uint32_t delay_ms, const Functor& callback, bool repeat)
{
    TimerEntry timer;
    timer.id = next_timer_id_++;
    if (next_timer_id_ == 0) next_timer_id_ = 1;
    timer.expire_ms = platform::currentTimeMs() + static_cast<int64_t>(delay_ms);
    timer.interval_ms = repeat ? delay_ms : 0;
    timer.repeat = repeat;
    timer.cancelled = false;
    timer.callback = callback;

    timers_.push_back(timer);

    OMNI_LOG_DEBUG(LOG_TAG, "Added timer %u (delay=%u ms, repeat=%s)",
                     timer.id, delay_ms, repeat ? "yes" : "no");

    wakeup();
    return timer.id;
}

void EventLoop::cancelTimer(uint32_t timer_id)
{
    for (size_t i = 0; i < timers_.size(); ++i) {
        if (timers_[i].id == timer_id) {
            timers_[i].cancelled = true;
            OMNI_LOG_DEBUG(LOG_TAG, "Cancelled timer %u", timer_id);
            return;
        }
    }
    OMNI_LOG_DEBUG(LOG_TAG, "cancelTimer: timer %u not found", timer_id);
}

int EventLoop::processTimers()
{
    if (timers_.empty()) {
        return -1;
    }

    int64_t now = platform::currentTimeMs();
    int min_remaining = -1;

    std::vector<Functor> expired_callbacks;

    for (size_t i = 0; i < timers_.size(); ) {
        TimerEntry& timer = timers_[i];

        if (timer.cancelled) {
            timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }

        int64_t remaining = timer.expire_ms - now;

        if (remaining <= 0) {
            expired_callbacks.push_back(timer.callback);

            if (timer.repeat && timer.interval_ms > 0) {
                timer.expire_ms = now + static_cast<int64_t>(timer.interval_ms);
                remaining = static_cast<int64_t>(timer.interval_ms);
                ++i;
            } else {
                timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        } else {
            ++i;
        }

        if (remaining > 0) {
            int remaining_int = static_cast<int>(remaining);
            if (min_remaining < 0 || remaining_int < min_remaining) {
                min_remaining = remaining_int;
            }
        }
    }

    for (size_t i = 0; i < expired_callbacks.size(); ++i) {
        if (expired_callbacks[i]) {
            expired_callbacks[i]();
        }
    }

    return min_remaining;
}

int EventLoop::calculateTimeout(int requested_timeout_ms)
{
    if (timers_.empty()) {
        return requested_timeout_ms;
    }

    int64_t now = platform::currentTimeMs();
    int min_timer_timeout = -1;

    for (size_t i = 0; i < timers_.size(); ++i) {
        if (timers_[i].cancelled) {
            continue;
        }

        int64_t remaining = timers_[i].expire_ms - now;
        if (remaining <= 0) {
            return 0;
        }

        int remaining_int = static_cast<int>(remaining);
        if (min_timer_timeout < 0 || remaining_int < min_timer_timeout) {
            min_timer_timeout = remaining_int;
        }
    }

    if (requested_timeout_ms < 0) {
        return min_timer_timeout;
    }
    if (min_timer_timeout < 0) {
        return requested_timeout_ms;
    }
    return (requested_timeout_ms < min_timer_timeout) ? requested_timeout_ms : min_timer_timeout;
}

void EventLoop::post(const Functor& func)
{
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_functors_.push_back(func);
    }
    wakeup();
}

void EventLoop::onWakeup(int fd, uint32_t events)
{
    (void)events;
    platform::eventFdConsume(fd);
}

void EventLoop::processPendingFunctors()
{
    std::vector<Functor> functors;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        functors.swap(pending_functors_);
    }

    for (size_t i = 0; i < functors.size(); ++i) {
        if (functors[i]) {
            functors[i]();
        }
    }
}

uint32_t EventLoop::toBackendEvents(uint32_t events)
{
    return events;
}

uint32_t EventLoop::fromBackendEvents(uint32_t backend_events)
{
    return backend_events;
}

} // namespace omnibinder
