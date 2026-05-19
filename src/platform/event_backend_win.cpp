#include "platform/event_backend.h"

#ifdef OMNIBINDER_WINDOWS

#include "omnibinder/log.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <cstring>
#include <map>
#include <vector>

#define LOG_TAG "EventBackend"

namespace omnibinder {
namespace platform {

class SelectBackend : public EventBackend {
public:
    SelectBackend() {}
    
    ~SelectBackend() {
        destroy();
    }
    
    bool init() override {
        return true;
    }
    
    void destroy() override {
        fd_events_.clear();
    }
    
    bool addFd(int fd, uint32_t events) override {
        fd_events_[fd] = events;
        return true;
    }
    
    bool modifyFd(int fd, uint32_t events) override {
        std::map<int, uint32_t>::iterator it = fd_events_.find(fd);
        if (it == fd_events_.end()) {
            return false;
        }
        it->second = events;
        return true;
    }
    
    bool removeFd(int fd) override {
        fd_events_.erase(fd);
        return true;
    }
    
    int poll(ReadyEvent* events, int max_events, int timeout_ms) override {
        fd_set read_fds, write_fds, except_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_ZERO(&except_fds);
        
        int max_fd = 0;
        
        for (std::map<int, uint32_t>::iterator it = fd_events_.begin();
             it != fd_events_.end(); ++it) {
            if (read_fds.fd_count >= FD_SETSIZE) {
                OMNI_LOG_ERROR(LOG_TAG, "FD_SETSIZE exceeded");
                break;
            }
            
            SOCKET s = static_cast<SOCKET>(it->first);
            if (it->second & EVENT_READ)  FD_SET(s, &read_fds);
            if (it->second & EVENT_WRITE) FD_SET(s, &write_fds);
            FD_SET(s, &except_fds);
            
            if (it->first > max_fd) max_fd = it->first;
        }
        
        struct timeval tv;
        struct timeval* ptv = NULL;
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ptv = &tv;
        }
        
        int ret = select(max_fd + 1, &read_fds, &write_fds, &except_fds, ptv);
        
        if (ret < 0) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) return 0;
            OMNI_LOG_ERROR(LOG_TAG, "select failed: %d", err);
            return -1;
        }
        
        int count = 0;
        for (std::map<int, uint32_t>::iterator it = fd_events_.begin();
             it != fd_events_.end() && count < max_events; ++it) {
            SOCKET s = static_cast<SOCKET>(it->first);
            uint32_t revents = 0;
            
            if (FD_ISSET(s, &read_fds))   revents |= EVENT_READ;
            if (FD_ISSET(s, &write_fds))  revents |= EVENT_WRITE;
            if (FD_ISSET(s, &except_fds)) revents |= EVENT_ERROR;
            
            if (revents != 0) {
                events[count].fd = it->first;
                events[count].events = revents;
                ++count;
            }
        }
        
        return count;
    }

private:
    std::map<int, uint32_t> fd_events_;
};

EventBackend* createEventBackend() {
    return new SelectBackend();
}

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_WINDOWS
