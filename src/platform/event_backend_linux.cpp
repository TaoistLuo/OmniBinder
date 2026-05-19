#include "platform/event_backend.h"

#ifdef OMNIBINDER_LINUX

#include "omnibinder/log.h"

#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#define LOG_TAG "EventBackend"

namespace omnibinder {
namespace platform {

class EpollBackend : public EventBackend {
public:
    EpollBackend() : epoll_fd_(-1) {}
    
    ~EpollBackend() {
        destroy();
    }
    
    bool init() override {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "epoll_create1 failed: %s", strerror(errno));
            return false;
        }
        OMNI_LOG_DEBUG(LOG_TAG, "EpollBackend initialized (fd=%d)", epoll_fd_);
        return true;
    }
    
    void destroy() override {
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }
    
    bool addFd(int fd, uint32_t events) override {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = toEpollEvents(events);
        ev.data.fd = fd;
        
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "epoll_ctl ADD failed for fd %d: %s", fd, strerror(errno));
            return false;
        }
        return true;
    }
    
    bool modifyFd(int fd, uint32_t events) override {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = toEpollEvents(events);
        ev.data.fd = fd;
        
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "epoll_ctl MOD failed for fd %d: %s", fd, strerror(errno));
            return false;
        }
        return true;
    }
    
    bool removeFd(int fd) override {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL) < 0) {
            if (errno != EBADF && errno != ENOENT) {
                OMNI_LOG_WARN(LOG_TAG, "epoll_ctl DEL failed for fd %d: %s", fd, strerror(errno));
                return false;
            }
        }
        return true;
    }
    
    int poll(ReadyEvent* events, int max_events, int timeout_ms) override {
        const int MAX_EPOLL_EVENTS = 64;
        struct epoll_event epoll_events[MAX_EPOLL_EVENTS];
        
        int nfds = epoll_wait(epoll_fd_, epoll_events, MAX_EPOLL_EVENTS, timeout_ms);
        
        if (nfds < 0) {
            if (errno == EINTR) {
                return 0;
            }
            OMNI_LOG_ERROR(LOG_TAG, "epoll_wait failed: %s", strerror(errno));
            return -1;
        }
        
        int count = (nfds < max_events) ? nfds : max_events;
        for (int i = 0; i < count; ++i) {
            events[i].fd = epoll_events[i].data.fd;
            events[i].events = fromEpollEvents(epoll_events[i].events);
        }
        
        return count;
    }

private:
    static uint32_t toEpollEvents(uint32_t events) {
        uint32_t epoll_events = 0;
        if (events & EVENT_READ)  epoll_events |= EPOLLIN;
        if (events & EVENT_WRITE) epoll_events |= EPOLLOUT;
        if (events & EVENT_ERROR) epoll_events |= EPOLLERR;
        epoll_events |= EPOLLHUP | EPOLLERR;
        return epoll_events;
    }
    
    static uint32_t fromEpollEvents(uint32_t epoll_events) {
        uint32_t events = 0;
        if (epoll_events & EPOLLIN)               events |= EVENT_READ;
        if (epoll_events & EPOLLOUT)              events |= EVENT_WRITE;
        if (epoll_events & (EPOLLERR | EPOLLHUP)) events |= EVENT_ERROR;
        return events;
    }
    
    int epoll_fd_;
};

EventBackend* createEventBackend() {
    return new EpollBackend();
}

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_LINUX
