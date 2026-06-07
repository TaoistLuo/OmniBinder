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

// ============================================================
// IocpBackend — IOCP-based EventBackend for Windows
//
//   Socket readiness: WSAPoll (no FD_SETSIZE limit)
//   Named pipe readiness: overlapped ReadFile on IOCP
//   Cross-thread wakeup: PostQueuedCompletionStatus
//   Per-thread pipe state: thread_local (supports multi-EventLoop)
// ============================================================

static HANDLE       g_iocp_wakeup = NULL;  // cross-thread wakeup
thread_local HANDLE tls_iocp      = NULL;  // per-thread pipe I/O
static const int    IOCP_WAKEUP_FD  = 0x7FFFFFFF;
static const ULONG_PTR IOCP_WAKEUP_KEY = 0;

struct PipeEntry {
    HANDLE      pipe;
    OVERLAPPED  ov;
    char        buf[1];
    bool        read_pending;
    bool        is_server;
    bool        client_connected;
};

thread_local std::map<int, PipeEntry> g_pipes;

static bool postPipeRead(int fd);

bool iocpRegisterPipeFd(int fd, HANDLE hPipe, bool is_server) {
    if (!tls_iocp || fd < 0 || !hPipe) return false;

    if (CreateIoCompletionPort(hPipe, tls_iocp, (ULONG_PTR)fd, 0) == NULL) {
        OMNI_LOG_ERROR(LOG_TAG, "CreateIoCompletionPort failed for pipe fd=%d: %lu",
                       fd, GetLastError());
        return false;
    }

    PipeEntry pe;
    memset(&pe, 0, sizeof(pe));
    pe.pipe = hPipe;
    pe.read_pending = false;
    pe.is_server = is_server;
    pe.client_connected = !is_server;  // client-side is already connected
    g_pipes[fd] = pe;

    if (is_server) {
        memset(&g_pipes[fd].ov, 0, sizeof(OVERLAPPED));
        BOOL cn = ConnectNamedPipe(hPipe, &g_pipes[fd].ov);
        if (!cn && GetLastError() == ERROR_PIPE_CONNECTED) {
            g_pipes[fd].client_connected = true;
            postPipeRead(fd);
        }
    } else {
        postPipeRead(fd);
    }
    return true;
}

void iocpUnregisterPipeFd(int fd) {
    g_pipes.erase(fd);
}

static bool postPipeRead(int fd) {
    std::map<int, PipeEntry>::iterator it = g_pipes.find(fd);
    if (it == g_pipes.end()) return false;

    PipeEntry& pe = it->second;
    if (pe.read_pending) return false;

    memset(&pe.ov, 0, sizeof(pe.ov));
    DWORD nread = 0;
    BOOL ok = ReadFile(pe.pipe, pe.buf, sizeof(pe.buf), &nread, &pe.ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) {
        pe.read_pending = true;
        return ok;
    }
    DWORD err = GetLastError();
    if ((err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) && pe.is_server) {
        DisconnectNamedPipe(pe.pipe);
        memset(&pe.ov, 0, sizeof(pe.ov));
        BOOL cn = ConnectNamedPipe(pe.pipe, &pe.ov);
        if (!cn && GetLastError() == ERROR_PIPE_CONNECTED)
            postPipeRead(fd);
    } else if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED) {
        OMNI_LOG_ERROR(LOG_TAG, "ReadFile failed for pipe fd=%d: %lu", fd, err);
    }
    return false;
}

class IocpBackend : public EventBackend {
public:
    IocpBackend() : iocp_(NULL) {}
    ~IocpBackend() { destroy(); }

    bool init() override {
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!iocp_) {
            OMNI_LOG_ERROR(LOG_TAG, "CreateIoCompletionPort failed: %lu", GetLastError());
            return false;
        }
        tls_iocp = iocp_;
        g_iocp_wakeup = iocp_;
        return true;
    }

    void destroy() override {
        if (iocp_) { CloseHandle(iocp_); iocp_ = NULL; }
        tls_iocp = NULL;
        g_iocp_wakeup = NULL;
        g_pipes.clear();
        fd_events_.clear();
        pollfds_.clear();
    }

    bool addFd(int fd, uint32_t events) override {
        fd_events_[fd] = events;
        if (g_pipes.find(fd) != g_pipes.end()) {
            PipeEntry& pe = g_pipes[fd];
            if ((events & EVENT_READ) && pe.client_connected)
                postPipeRead(fd);
            return true;
        }
        if (fd == IOCP_WAKEUP_FD) return true;
        rebuildPollFds();
        return true;
    }

    bool modifyFd(int fd, uint32_t events) override {
        std::map<int, uint32_t>::iterator it = fd_events_.find(fd);
        if (it == fd_events_.end()) return false;
        it->second = events;
        if (g_pipes.find(fd) != g_pipes.end()) {
            PipeEntry& pe = g_pipes[fd];
            if ((events & EVENT_READ) && pe.client_connected)
                postPipeRead(fd);
            return true;
        }
        if (fd != IOCP_WAKEUP_FD) rebuildPollFds();
        return true;
    }

    bool removeFd(int fd) override {
        fd_events_.erase(fd);
        if (fd != IOCP_WAKEUP_FD && g_pipes.find(fd) == g_pipes.end())
            rebuildPollFds();
        return true;
    }

    int poll(ReadyEvent* events, int max_events, int timeout_ms) override {
        int n = checkIocpCompletions(events, max_events);
        if (n > 0) return n;

        int sock_count = 0;
        for (std::map<int, uint32_t>::iterator it = fd_events_.begin();
             it != fd_events_.end(); ++it) {
            if (it->first != IOCP_WAKEUP_FD &&
                g_pipes.find(it->first) == g_pipes.end()) ++sock_count;
        }

        if (sock_count == 0) {
            DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
            DWORD bytes; ULONG_PTR key; OVERLAPPED* ov;
            if (GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, ms)) {
                int fd = static_cast<int>(key);
                if (fd == IOCP_WAKEUP_KEY) {
                    drainWakeup();
                    events[0].fd = IOCP_WAKEUP_FD;
                    events[0].events = EVENT_READ;
                    return 1;
                }
                return handlePipeCompletion(fd, events, 0, max_events);
            }
            return 0;
        }

        const int CHUNK_MS = 50;
        int64_t deadline_ms = (timeout_ms >= 0) ? currentTimeMs() + timeout_ms : INT64_MAX;

        while (true) {
            n = checkIocpCompletions(events, max_events);
            if (n > 0) return n;

            if (timeout_ms >= 0 && currentTimeMs() >= deadline_ms) return 0;

            int chunk = CHUNK_MS;
            if (timeout_ms >= 0) {
                int remaining = (int)(deadline_ms - currentTimeMs());
                if (remaining <= 0) return 0;
                if (remaining < CHUNK_MS) chunk = remaining;
            }

            if (pollfds_.empty()) rebuildPollFds();
            int ret = WSAPoll(pollfds_.data(), (ULONG)pollfds_.size(), chunk);
            if (ret > 0) {
                int count = 0;
                for (size_t i = 0; i < pollfds_.size() && count < max_events; ++i) {
                    uint32_t revents = 0;
                    if (pollfds_[i].revents & (POLLIN | POLLHUP)) revents |= EVENT_READ;
                    if (pollfds_[i].revents & POLLOUT)            revents |= EVENT_WRITE;
                    if (pollfds_[i].revents & POLLERR)            revents |= EVENT_ERROR;
                    if (revents != 0) {
                        events[count].fd     = pollfds_[i].fd;
                        events[count].events = revents;
                        ++count;
                    }
                }
                if (count > 0) return count;
            }
            if (ret < 0) return ret;
        }
    }

private:
    int checkIocpCompletions(ReadyEvent* events, int max_events) {
        int count = 0;
        DWORD bytes; ULONG_PTR key; OVERLAPPED* ov;
        while (count < max_events) {
            if (!GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, 0)) {
                if (GetLastError() == WAIT_TIMEOUT) break;
            }
            if (key == IOCP_WAKEUP_KEY) {
                drainWakeup();
                events[count].fd     = IOCP_WAKEUP_FD;
                events[count].events = EVENT_READ;
                return count + 1;
            }
            count += handlePipeCompletion((int)key, events, count, max_events);
        }
        return count;
    }

    int handlePipeCompletion(int fd, ReadyEvent* events, int idx, int max_events) {
        if (idx >= max_events) return 0;
        std::map<int, PipeEntry>::iterator it = g_pipes.find(fd);
        if (it == g_pipes.end()) return 0;

        if (!it->second.read_pending) {
            it->second.client_connected = true;
            if (postPipeRead(fd)) {
                events[idx].fd = fd; events[idx].events = EVENT_READ;
                return 1;
            }
            return 0;
        }
        it->second.read_pending = false;
        events[idx].fd = fd; events[idx].events = EVENT_READ;
        std::map<int, uint32_t>::iterator ev = fd_events_.find(fd);
        if (ev != fd_events_.end() && (ev->second & EVENT_READ))
            postPipeRead(fd);
        return 1;
    }

    void drainWakeup() {
        DWORD bytes; ULONG_PTR key; OVERLAPPED* ov;
        while (GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, 0)) {
            if (key != IOCP_WAKEUP_KEY) break;
        }
    }

    int64_t currentTimeMs() {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli; uli.LowPart = ft.dwLowDateTime; uli.HighPart = ft.dwHighDateTime;
        return (int64_t)((uli.QuadPart - 116444736000000000ULL) / 10000);
    }

    void rebuildPollFds() {
        pollfds_.clear();
        for (std::map<int, uint32_t>::iterator it = fd_events_.begin();
             it != fd_events_.end(); ++it) {
            if (it->first == IOCP_WAKEUP_FD) continue;
            if (g_pipes.find(it->first) != g_pipes.end()) continue;
            WSAPOLLFD pfd;
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = (SOCKET)it->first;
            if (it->second & EVENT_READ)  pfd.events |= POLLIN;
            if (it->second & EVENT_WRITE) pfd.events |= POLLOUT;
            pfd.revents = 0;
            pollfds_.push_back(pfd);
        }
    }

    HANDLE iocp_;
    std::map<int, uint32_t> fd_events_;
    std::vector<WSAPOLLFD>  pollfds_;
};

EventBackend* createEventBackend() { return new IocpBackend(); }

int iocpGetWakeupFd() { return IOCP_WAKEUP_FD; }

bool iocpPostWakeup() {
    return g_iocp_wakeup &&
        PostQueuedCompletionStatus(g_iocp_wakeup, 0, IOCP_WAKEUP_KEY, NULL);
}

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_WINDOWS
