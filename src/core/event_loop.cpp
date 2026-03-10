#include "core/event_loop.h"
#include "platform/platform.h"
#include "omnibinder/log.h"

#include <algorithm>
#include <cstring>

#ifdef OMNIBINDER_LINUX
    #include <sys/epoll.h>
    #include <unistd.h>
#elif defined(OMNIBINDER_WINDOWS)
    #include <winsock2.h>
#endif

#define LOG_TAG "EventLoop"

namespace omnibinder {

// ============================================================
// 构造与析构
// ============================================================

EventLoop::EventLoop()
    : running_(false)
    , epoll_fd_(-1)
    , wakeup_fd_(-1)
    , next_timer_id_(1)
{
    initBackend();
}

EventLoop::~EventLoop()
{
    destroyBackend();
}

// ============================================================
// 平台后端初始化
// ============================================================

void EventLoop::initBackend()
{
#ifdef OMNIBINDER_LINUX
    // 创建 epoll 实例
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create epoll: %s", strerror(errno));
        return;
    }

    // 创建 eventfd 用于跨线程唤醒
    wakeup_fd_ = platform::createEventFd();
    if (wakeup_fd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create wakeup eventfd");
        close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // 将 wakeup_fd 添加到 epoll
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to add wakeup fd to epoll: %s", strerror(errno));
        platform::closeEventFd(wakeup_fd_);
        close(epoll_fd_);
        epoll_fd_ = -1;
        wakeup_fd_ = -1;
        return;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "EventLoop initialized (epoll_fd=%d, wakeup_fd=%d)",
                     epoll_fd_, wakeup_fd_);

#elif defined(OMNIBINDER_WINDOWS)
    // Windows select 桩实现
    wakeup_fd_ = platform::createEventFd();
    if (wakeup_fd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create wakeup socket");
        return;
    }
    OMNI_LOG_DEBUG(LOG_TAG, "EventLoop initialized (select mode, wakeup_fd=%d)", wakeup_fd_);
#endif
}

void EventLoop::destroyBackend()
{
#ifdef OMNIBINDER_LINUX
    if (wakeup_fd_ >= 0) {
        platform::closeEventFd(wakeup_fd_);
        wakeup_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
#elif defined(OMNIBINDER_WINDOWS)
    if (wakeup_fd_ >= 0) {
        platform::closeEventFd(wakeup_fd_);
        wakeup_fd_ = -1;
    }
#endif

    fd_entries_.clear();
    timers_.clear();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_functors_.clear();
    }

    OMNI_LOG_DEBUG(LOG_TAG, "EventLoop destroyed");
}

// ============================================================
// 事件循环控制
// ============================================================

void EventLoop::run()
{
    running_ = true;
    OMNI_LOG_INFO(LOG_TAG, "EventLoop started");

    while (running_) {
        pollOnce(-1);
    }

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

// ============================================================
// pollOnce - 处理一轮事件
// ============================================================

void EventLoop::pollOnce(int timeout_ms)
{
#ifdef OMNIBINDER_LINUX
    if (epoll_fd_ < 0) {
        return;
    }

    // 计算实际超时（考虑定时器）
    int actual_timeout = calculateTimeout(timeout_ms);

    // 等待事件
    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, actual_timeout);

    if (nfds < 0) {
        if (errno != EINTR) {
            OMNI_LOG_ERROR(LOG_TAG, "epoll_wait failed: %s", strerror(errno));
        }
        return;
    }

    // 处理就绪的 fd 事件
    for (int i = 0; i < nfds; ++i) {
        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;

        // 检查是否是 wakeup fd
        if (fd == wakeup_fd_) {
            handleWakeup(fd, fromPlatformEvents(revents));
            continue;
        }

        // 查找对应的 FdEntry
        std::map<int, FdEntry>::iterator it = fd_entries_.find(fd);
        if (it != fd_entries_.end()) {
            uint32_t translated_events = fromPlatformEvents(revents);
            if (it->second.callback) {
                it->second.callback(fd, translated_events);
            }
        }
    }

    // 处理到期的定时器
    processTimers();

    // 处理投递的回调
    processPendingFunctors();

#elif defined(OMNIBINDER_WINDOWS)
    // Windows select 桩实现
    fd_set read_fds, write_fds, except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);

    int max_fd = 0;

    // 添加 wakeup fd
    if (wakeup_fd_ >= 0) {
        FD_SET(static_cast<SOCKET>(wakeup_fd_), &read_fds);
        if (wakeup_fd_ > max_fd) max_fd = wakeup_fd_;
    }

    // 添加所有注册的 fd (with FD_SETSIZE overflow check - fix #7)
    for (std::map<int, FdEntry>::iterator it = fd_entries_.begin();
         it != fd_entries_.end(); ++it) {
        if (read_fds.fd_count >= FD_SETSIZE) {
            OMNI_LOG_ERROR(LOG_TAG, "FD_SETSIZE (%d) exceeded, some fds will not be monitored",
                             FD_SETSIZE);
            break;
        }
        SOCKET s = static_cast<SOCKET>(it->first);
        if (it->second.events & EVENT_READ) {
            FD_SET(s, &read_fds);
        }
        if (it->second.events & EVENT_WRITE) {
            FD_SET(s, &write_fds);
        }
        FD_SET(s, &except_fds);
        if (it->first > max_fd) max_fd = it->first;
    }

    // 计算超时
    int actual_timeout = calculateTimeout(timeout_ms);
    struct timeval tv;
    struct timeval* ptv = NULL;
    if (actual_timeout >= 0) {
        tv.tv_sec = actual_timeout / 1000;
        tv.tv_usec = (actual_timeout % 1000) * 1000;
        ptv = &tv;
    }

    int ret = select(max_fd + 1, &read_fds, &write_fds, &except_fds, ptv);

    if (ret < 0) {
        int err = WSAGetLastError();
        if (err != WSAEINTR) {
            OMNI_LOG_ERROR(LOG_TAG, "select failed: %d", err);
        }
        return;
    }

    // 检查 wakeup fd
    if (wakeup_fd_ >= 0 && FD_ISSET(static_cast<SOCKET>(wakeup_fd_), &read_fds)) {
        handleWakeup(wakeup_fd_, EVENT_READ);
    }

    // 处理其他 fd (fix #7: collect events first, then dispatch to avoid
    // iterator invalidation when callbacks modify fd_entries_)
    struct FdEvent { int fd; uint32_t events; };
    std::vector<FdEvent> ready_events;
    for (std::map<int, FdEntry>::iterator it = fd_entries_.begin();
         it != fd_entries_.end(); ++it) {
        SOCKET s = static_cast<SOCKET>(it->first);
        uint32_t revents = 0;

        if (FD_ISSET(s, &read_fds)) {
            revents |= EVENT_READ;
        }
        if (FD_ISSET(s, &write_fds)) {
            revents |= EVENT_WRITE;
        }
        if (FD_ISSET(s, &except_fds)) {
            revents |= EVENT_ERROR;
        }

        if (revents != 0) {
            FdEvent fe;
            fe.fd = it->first;
            fe.events = revents;
            ready_events.push_back(fe);
        }
    }
    for (size_t i = 0; i < ready_events.size(); ++i) {
        std::map<int, FdEntry>::iterator it = fd_entries_.find(ready_events[i].fd);
        if (it != fd_entries_.end() && it->second.callback) {
            it->second.callback(ready_events[i].fd, ready_events[i].events);
        }
    }

    // 处理定时器
    processTimers();

    // 处理投递的回调
    processPendingFunctors();
#endif
}

// ============================================================
// FD 事件管理
// ============================================================

void EventLoop::addFd(int fd, uint32_t events, const EventCallback& callback)
{
    if (fd < 0) {
        OMNI_LOG_WARN(LOG_TAG, "addFd: invalid fd %d", fd);
        return;
    }

    // 检查是否已存在
    if (fd_entries_.find(fd) != fd_entries_.end()) {
        OMNI_LOG_WARN(LOG_TAG, "addFd: fd %d already registered, use modifyFd instead", fd);
        return;
    }

    FdEntry entry;
    entry.fd = fd;
    entry.events = events;
    entry.callback = callback;
    fd_entries_[fd] = entry;

#ifdef OMNIBINDER_LINUX
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = toPlatformEvents(events);
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "epoll_ctl ADD failed for fd %d: %s", fd, strerror(errno));
        fd_entries_.erase(fd);
        return;
    }
#endif

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

#ifdef OMNIBINDER_LINUX
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = toPlatformEvents(events);
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "epoll_ctl MOD failed for fd %d: %s", fd, strerror(errno));
        return;
    }
#endif

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

#ifdef OMNIBINDER_LINUX
    // EPOLL_CTL_DEL 不需要 ev 参数（Linux 2.6.9+）
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL) < 0) {
        // 如果 fd 已经关闭，epoll 会自动移除，这不是错误
        if (errno != EBADF && errno != ENOENT) {
            OMNI_LOG_WARN(LOG_TAG, "epoll_ctl DEL failed for fd %d: %s", fd, strerror(errno));
        }
    }
#endif

    OMNI_LOG_DEBUG(LOG_TAG, "Removed fd %d", fd);
}

// ============================================================
// 定时器管理
// ============================================================

uint32_t EventLoop::addTimer(uint32_t delay_ms, const Functor& callback, bool repeat)
{
    TimerEntry timer;
    timer.id = next_timer_id_++;
    // Handle wraparound: skip 0 since it's used as "no timer" sentinel
    if (next_timer_id_ == 0) next_timer_id_ = 1;
    timer.expire_ms = platform::currentTimeMs() + static_cast<int64_t>(delay_ms);
    timer.interval_ms = repeat ? delay_ms : 0;
    timer.repeat = repeat;
    timer.cancelled = false;
    timer.callback = callback;

    timers_.push_back(timer);

    OMNI_LOG_DEBUG(LOG_TAG, "Added timer %u (delay=%u ms, repeat=%s)",
                     timer.id, delay_ms, repeat ? "yes" : "no");

    // 如果事件循环正在等待，唤醒它以重新计算超时
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

    // 收集到期的定时器回调（避免在遍历时修改容器）
    std::vector<Functor> expired_callbacks;

    for (size_t i = 0; i < timers_.size(); ) {
        TimerEntry& timer = timers_[i];

        // 跳过已取消的定时器
        if (timer.cancelled) {
            timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }

        int64_t remaining = timer.expire_ms - now;

        if (remaining <= 0) {
            // 定时器到期
            expired_callbacks.push_back(timer.callback);

            if (timer.repeat && timer.interval_ms > 0) {
                // 周期性定时器：重新计算下次到期时间
                timer.expire_ms = now + static_cast<int64_t>(timer.interval_ms);
                remaining = static_cast<int64_t>(timer.interval_ms);
                ++i;
            } else {
                // 一次性定时器：移除
                timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        } else {
            ++i;
        }

        // 更新最小剩余时间
        if (remaining > 0) {
            int remaining_int = static_cast<int>(remaining);
            if (min_remaining < 0 || remaining_int < min_remaining) {
                min_remaining = remaining_int;
            }
        }
    }

    // 执行到期的回调
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
            // 已有到期的定时器，立即返回
            return 0;
        }

        int remaining_int = static_cast<int>(remaining);
        if (min_timer_timeout < 0 || remaining_int < min_timer_timeout) {
            min_timer_timeout = remaining_int;
        }
    }

    // 取请求超时和定时器超时的较小值
    if (requested_timeout_ms < 0) {
        return min_timer_timeout;
    }
    if (min_timer_timeout < 0) {
        return requested_timeout_ms;
    }
    return (requested_timeout_ms < min_timer_timeout) ? requested_timeout_ms : min_timer_timeout;
}

// ============================================================
// 跨线程投递
// ============================================================

void EventLoop::post(const Functor& func)
{
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_functors_.push_back(func);
    }
    wakeup();
}

void EventLoop::handleWakeup(int fd, uint32_t events)
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

// ============================================================
// 事件标志转换
// ============================================================

uint32_t EventLoop::toPlatformEvents(uint32_t events)
{
#ifdef OMNIBINDER_LINUX
    uint32_t epoll_events = 0;
    if (events & EVENT_READ) {
        epoll_events |= EPOLLIN;
    }
    if (events & EVENT_WRITE) {
        epoll_events |= EPOLLOUT;
    }
    if (events & EVENT_ERROR) {
        epoll_events |= EPOLLERR;
    }
    // 总是监听 EPOLLHUP 和 EPOLLERR（即使未请求）
    epoll_events |= EPOLLHUP | EPOLLERR;
    return epoll_events;
#else
    return events;
#endif
}

uint32_t EventLoop::fromPlatformEvents(uint32_t epoll_events)
{
#ifdef OMNIBINDER_LINUX
    uint32_t events = 0;
    if (epoll_events & EPOLLIN) {
        events |= EVENT_READ;
    }
    if (epoll_events & EPOLLOUT) {
        events |= EVENT_WRITE;
    }
    if (epoll_events & (EPOLLERR | EPOLLHUP)) {
        events |= EVENT_ERROR;
    }
    return events;
#else
    return epoll_events;
#endif
}

} // namespace omnibinder
