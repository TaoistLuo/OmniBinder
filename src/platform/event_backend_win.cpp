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
#include <set>

#define LOG_TAG "EventBackend"

namespace omnibinder {
namespace platform {

// ============================================================
// IocpBackend — IOCP-based EventBackend for Windows
//
//   - Named Pipes:        overlapped ReadFile → IOCP completion
//                          (immediate wakeup, zero polling)
//   - TCP sockets
//       (listen + data):  select(0) non-blocking probe, only
//                          checked when IOCP has no completions
//   - Cross-thread wakeup: PostQueuedCompletionStatus
//   - Waiting:            single GetQueuedCompletionStatus
//                          blocks until pipe event or timeout;
//                          no fixed-interval WSAPoll loop
// ============================================================

static HANDLE       g_iocp_wakeup = NULL;
thread_local HANDLE tls_iocp      = NULL;
static const int    IOCP_WAKEUP_FD  = 0x7FFFFFFF;
static const ULONG_PTR IOCP_WAKEUP_KEY = 0;

// ── Pipe state ──────────────────────────────────────────────

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
    pe.client_connected = !is_server;
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

// ── Backend class ───────────────────────────────────────────

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
        sock_fds_.clear();
        fd_events_.clear();
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

        // TCP socket — track in sock_fds_ for select(0) probing
        sock_fds_.insert(fd);
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
        return true;
    }

    bool removeFd(int fd) override {
        fd_events_.erase(fd);
        sock_fds_.erase(fd);
        return true;
    }

    int poll(ReadyEvent* events, int max_events, int timeout_ms) override {
        int64_t deadline = (timeout_ms >= 0)
            ? currentTimeMs() + timeout_ms
            : INT64_MAX;

        while (true) {
            // 1. Drain already-queued IOCP pipe completions (non-blocking)
            int n = drainPipeCompletions(events, max_events);
            if (n > 0) return n;

            // 2. Probe all TCP sockets with select(0) (non-blocking)
            n = probeSockets(events, max_events);
            if (n > 0) return n;

            // 3. Deadline check
            if (timeout_ms >= 0 && currentTimeMs() >= deadline)
                return 0;

            // 4. Block on IOCP.
            //    Pipes deliver completions through IOCP → wake immediately.
            //    Sockets are probed above (select(0)); when sockets exist we
            //    use a 1ms IOCP timeout so that socket events are detected
            //    promptly (pipe completions still wake the IOCP inline).
            DWORD wait_ms = INFINITE;
            if (!sock_fds_.empty()) {
                wait_ms = 1;
            }
            if (timeout_ms >= 0) {
                int64_t rem = deadline - currentTimeMs();
                if (rem <= 0) return 0;
                if ((DWORD)rem < wait_ms) wait_ms = (DWORD)rem;
            }

            DWORD bytes; ULONG_PTR key; OVERLAPPED* ov;
            BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, wait_ms);
            if (!ok)
                continue;

            int fd = static_cast<int>(key);
            if (fd == 0 && key == IOCP_WAKEUP_KEY) {
                drainWakeup();
                events[0].fd     = IOCP_WAKEUP_FD;
                events[0].events = EVENT_READ;
                return 1;
            }

            if (g_pipes.find(fd) != g_pipes.end())
                return handlePipeCompletion(fd, events, 0, max_events);

            // Unknown completion — ignore and loop
        }
    }

private:
    int64_t currentTimeMs() {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli; uli.LowPart = ft.dwLowDateTime; uli.HighPart = ft.dwHighDateTime;
        return (int64_t)((uli.QuadPart - 116444736000000000ULL) / 10000);
    }

    // ── Pipe IOCP ─────────────────────────────────────────

    int drainPipeCompletions(ReadyEvent* events, int max_events) {
        int count = 0;
        DWORD bytes; ULONG_PTR key; OVERLAPPED* ov;
        while (count < max_events) {
            if (!GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, 0)) {
                if (GetLastError() == WAIT_TIMEOUT) break;
                continue;
            }
            int fd = static_cast<int>(key);
            if (fd == 0 && key == IOCP_WAKEUP_KEY) {
                drainWakeup();
                events[count].fd     = IOCP_WAKEUP_FD;
                events[count].events = EVENT_READ;
                return count + 1;
            }
            if (g_pipes.find(fd) != g_pipes.end()) {
                count += handlePipeCompletion(fd, events, count, max_events);
            }
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

    // ── Socket probing (select-based, non-blocking) ───────

    int probeSockets(ReadyEvent* events, int max_events) {
        if (sock_fds_.empty() || max_events <= 0) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        int nfds = 0;
        for (std::set<int>::iterator it = sock_fds_.begin();
             it != sock_fds_.end(); ++it) {
            FD_SET((SOCKET)*it, &rfds);
            if (*it > nfds) nfds = *it;
        }

        struct timeval tv = {0, 0};
        int ret = select(nfds + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) return 0;

        int count = 0;
        for (std::set<int>::iterator it = sock_fds_.begin();
             it != sock_fds_.end() && count < max_events; ++it) {
            if (FD_ISSET((SOCKET)*it, &rfds)) {
                std::map<int, uint32_t>::iterator ev = fd_events_.find(*it);
                uint32_t revents = EVENT_READ;
                if (ev != fd_events_.end() && (ev->second & EVENT_ERROR))
                    revents |= EVENT_ERROR;
                events[count].fd     = *it;
                events[count].events = revents;
                ++count;
            }
        }
        return count;
    }

    // ── members ─────────────────────────────────────────────

    HANDLE              iocp_;
    std::map<int, uint32_t> fd_events_;
    std::set<int>       sock_fds_;    // all TCP sockets (listen + data)
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
