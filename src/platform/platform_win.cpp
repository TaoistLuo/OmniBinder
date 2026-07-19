#ifdef OMNIBINDER_WINDOWS

#include "platform/platform.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef ERROR
    #undef ERROR
#endif
#ifdef IGNORE
    #undef IGNORE
#endif

#include <cstring>
#include <cstdio>
#include <climits>
#include <ctime>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>


#define LOG_TAG "Platform"

// Windows 桩实现 - 后续完善
namespace omnibinder {
namespace platform {

namespace {

std::string basenameFromPath(const char* path) {
    if (!path || path[0] == '\0') {
        return std::string();
    }

    const char* slash = strrchr(path, '\\');
    const char* alt_slash = strrchr(path, '/');
    const char* name = slash && alt_slash ? (slash > alt_slash ? slash : alt_slash) : (slash ? slash : alt_slash);
    return name ? std::string(name + 1) : std::string(path);
}

} // namespace

static bool g_net_initialized = false;

bool netInit() {
    if (g_net_initialized) {
        return true;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    g_net_initialized = true;
    return true;
}

void netCleanup() {
    if (g_net_initialized) {
        WSACleanup();
        g_net_initialized = false;
    }
}

SocketFd createTcpSocket() {
    if (!netInit()) {
        return INVALID_SOCKET_FD;
    }
    return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

bool setNonBlocking(SocketFd fd) {
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
}

bool setReuseAddr(SocketFd fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

bool setTcpNoDelay(SocketFd fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

bool setKeepAlive(SocketFd fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                      reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

bool bindSocket(SocketFd fd, const std::string& host, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            OMNI_LOG_ERROR(LOG_TAG, "Invalid address: %s", host.c_str());
            return false;
        }
    }
    return bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool listenSocket(SocketFd fd, int backlog) {
    return listen(fd, backlog) == 0;
}

SocketFd acceptSocket(SocketFd fd, std::string& remote_host, uint16_t& remote_port) {
    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    SocketFd client_fd = accept(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
    if (client_fd == INVALID_SOCKET) {
        return INVALID_SOCKET_FD;
    }
    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf));
    remote_host = ip_buf;
    remote_port = ntohs(addr.sin_port);
    return client_fd;
}

int connectSocket(SocketFd fd, const std::string& host, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        // Try DNS resolution
        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        if (getaddrinfo(host.c_str(), port_str, &hints, &result) != 0) {
            OMNI_LOG_ERROR(LOG_TAG, "Cannot resolve host: %s", host.c_str());
            return -1;
        }
        memcpy(&addr, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);
    }
    int ret = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 1;
        return -1;
    }
    return 0;
}

int socketSend(SocketFd fd, const void* data, size_t length) {
    return send(fd, static_cast<const char*>(data), static_cast<int>(length), 0);
}

int socketRecv(SocketFd fd, void* buf, size_t buf_size) {
    return recv(fd, static_cast<char*>(buf), static_cast<int>(buf_size), 0);
}

bool socketSendAll(SocketFd fd, const void* data, size_t length,
                   uint32_t timeout_ms, uint32_t* elapsed_ms) {
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    int64_t start_ms = currentTimeMs();
    int64_t deadline_ms = start_ms + static_cast<int64_t>(timeout_ms);
    size_t sent = 0;
    while (sent < length) {
        int ret = socketSend(fd, bytes + sent, length - sent);
        if (ret < 0) {
            int err = getSocketError();
            if (isWouldBlock(err)) {
                int64_t now_ms = currentTimeMs();
                if (now_ms >= deadline_ms) {
                    break;
                }
                uint32_t remaining_ms = static_cast<uint32_t>(deadline_ms - now_ms);
                if (!waitSocketWritable(fd, remaining_ms)) {
                    break;
                }
                continue;
            }
            if (elapsed_ms) {
                int64_t spent_ms = currentTimeMs() - start_ms;
                *elapsed_ms = spent_ms > 0 ? static_cast<uint32_t>(spent_ms) : 0;
            }
            return false;
        }
        if (ret > 0) {
            sent += static_cast<size_t>(ret);
            continue;
        }
        int64_t now_ms = currentTimeMs();
        if (now_ms >= deadline_ms) {
            break;
        }
        uint32_t remaining_ms = static_cast<uint32_t>(deadline_ms - now_ms);
        if (!waitSocketWritable(fd, remaining_ms)) {
            break;
        }
    }
    if (elapsed_ms) {
        int64_t spent_ms = currentTimeMs() - start_ms;
        *elapsed_ms = spent_ms > 0 ? static_cast<uint32_t>(spent_ms) : 0;
    }
    return sent == length;
}

void closeSocket(SocketFd fd) {
    if (fd != INVALID_SOCKET_FD) {
        closesocket(fd);
    }
}

uint16_t getSocketPort(SocketFd fd) {
    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0) {
        return ntohs(addr.sin_port);
    }
    return 0;
}

std::string getSocketAddress(SocketFd fd) {
    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) != 0) {
        return "";
    }

    char ip_buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf)) == NULL) {
        return "";
    }
    return std::string(ip_buf);
}

int getSocketError() {
    return WSAGetLastError();
}

bool isWouldBlock(int error_code) {
    return error_code == WSAEWOULDBLOCK;
}


bool isConnectionReset(int error_code) {
    return error_code == WSAECONNRESET || error_code == WSAECONNABORTED;
}

bool waitSocketWritable(SocketFd fd, uint32_t timeout_ms) {
    fd_set write_fds;
    fd_set except_fds;
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);
    FD_SET(fd, &write_fds);
    FD_SET(fd, &except_fds);

    struct timeval tv;
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    int ret = select(static_cast<int>(fd) + 1, NULL, &write_fds, &except_fds, &tv);
    if (ret <= 0) {
        return false;
    }

    if (FD_ISSET(fd, &except_fds)) {
        return false;
    }

    return FD_ISSET(fd, &write_fds) != 0;
}

bool waitFdReadable(int fd, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
    return ret > 0 && FD_ISSET(fd, &read_fds);
}

// ============================================================
// Eventfd — Named Pipe implementation (replaces UDS exchange)
// ============================================================

// Forward declarations from event_backend_win.cpp
bool iocpRegisterPipeFd(int fd, HANDLE hPipe, bool is_server);
void iocpUnregisterPipeFd(int fd);
int  iocpGetWakeupFd();
bool iocpPostWakeup();

// Eventfd entry with pipe name for handshake token serialization
struct EventFdEntryV2 {
    HANDLE      pipe;
    bool        is_server;
    std::string pipe_name;
};

thread_local std::map<int, EventFdEntryV2> g_efd_map2;
static std::atomic<int>                     g_efd_next_2{1};  // process-wide: prevents pipe name collision across threads
// Follow-up: thread-local eventfd/IOCP registry ownership across thread and
// process teardown is not verified on Windows; keep this broader issue out of
// the handshake change until it can be tested with a Windows toolchain.

static std::string makePipeName(const std::string& name) {
    std::string path = "\\\\.\\pipe\\omnibinder_evt_";
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':') c = '_';
        path += c;
    }
    return path;
}

int openNamedEventFdByPipeName(const std::string& pipe_name) {
    HANDLE hPipe = CreateFileA(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) return -1;

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    int fd = g_efd_next_2.fetch_add(1);
    g_efd_map2[fd] = {hPipe, false, pipe_name};

    iocpRegisterPipeFd(fd, hPipe, false);
    return fd;
}

thread_local bool g_efd_first_call = true;

int createEventFd() {
    // Legacy/risky first-call heuristic: the first call is assumed to be the
    // EventLoop cross-thread wakeup; later calls are SHM notifications.
    // Use the IOCP magic singleton — no pipe needed within a single process.
    if (g_efd_first_call) {
        g_efd_first_call = false;
        OMNI_LOG_DEBUG(LOG_TAG, "createEventFd: first call, returning IOCP wakeup fd");
        return iocpGetWakeupFd();
    }
    // Subsequent calls are from SHM transport for cross-process notification.
    // Each gets a uniquely-named pipe whose name is serialized by handshakeSend.
    int id = g_efd_next_2.fetch_add(1);
    std::string name = "auto_" + std::to_string(GetCurrentProcessId())
                       + "_" + std::to_string(id);
    OMNI_LOG_DEBUG(LOG_TAG, "createEventFd: creating named pipe '%s'", name.c_str());
    return createNamedEventFd(name);
}

int createNamedEventFd(const std::string& name) {
    std::string pipe_name = makePipeName(name);

    HANDLE hPipe = CreateNamedPipeA(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 256, 256, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        OMNI_LOG_ERROR(LOG_TAG, "CreateNamedPipe failed for '%s': %lu",
                       pipe_name.c_str(), GetLastError());
        return -1;
    }

    int fd = g_efd_next_2.fetch_add(1);
    g_efd_map2[fd] = {hPipe, true, pipe_name};

    if (!iocpRegisterPipeFd(fd, hPipe, true)) {
        OMNI_LOG_WARN(LOG_TAG, "IOCP pipe registration failed for fd=%d", fd);
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Created named eventfd %d: %s", fd, pipe_name.c_str());
    return fd;
}

int openNamedEventFd(const std::string& name) {
    return openNamedEventFdByPipeName(makePipeName(name));
}

bool eventFdNotify(int efd) {
    if (efd == iocpGetWakeupFd()) {
        return iocpPostWakeup();
    }
    std::map<int, EventFdEntryV2>::iterator it = g_efd_map2.find(efd);
    if (it == g_efd_map2.end()) {
        OMNI_LOG_WARN(LOG_TAG, "eventFdNotify: fd=%d not found in g_efd_map2", efd);
        return false;
    }
    char c = 1;
    DWORD written = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    BOOL ok = WriteFile(it->second.pipe, &c, 1, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, INFINITE);
        ok = GetOverlappedResult(it->second.pipe, &ov, &written, FALSE);
    }
    CloseHandle(ov.hEvent);
    OMNI_LOG_DEBUG(LOG_TAG, "eventFdNotify: fd=%d pipe=%s ok=%d written=%lu err=%lu",
                   efd, it->second.pipe_name.c_str(), ok, written,
                   ok ? 0 : GetLastError());
    return ok && written == 1;
}

bool eventFdConsume(int efd) {
    if (efd == iocpGetWakeupFd()) return true;
    // The async IOCP ReadFile posted by event_backend_win.cpp already
    // consumed the notification byte when the completion fired.
    // eventFdConsume is called for parity with Linux (eventfd read to
    // clear the counter) but is a no-op on Windows.
    return true;
}

void closeEventFd(int efd) {
    if (efd == iocpGetWakeupFd()) return;
    std::map<int, EventFdEntryV2>::iterator it = g_efd_map2.find(efd);
    if (it != g_efd_map2.end()) {
        iocpUnregisterPipeFd(efd);
        CloseHandle(it->second.pipe);
        g_efd_map2.erase(it);
    }
}

// Windows 共享内存（使用 Named File Mapping）
static std::map<std::string, HANDLE> g_shm_handles;
static std::mutex g_shm_handles_mutex;

void* shmCreate(const std::string& name, size_t size, bool create, size_t* mapped_size) {
    if (mapped_size) {
        *mapped_size = 0;
    }
    std::string map_name = "Local\\omnibinder_" + name;
    HANDLE hMap;
    if (create) {
        hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, static_cast<DWORD>(size), map_name.c_str());
    } else {
        hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name.c_str());
    }
    if (!hMap) return NULL;

    {
        std::lock_guard<std::mutex> lock(g_shm_handles_mutex);
        g_shm_handles[name] = hMap;
    }

    void* addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, create ? size : 0);
    if (!addr) {
        CloseHandle(hMap);
        std::lock_guard<std::mutex> lock(g_shm_handles_mutex);
        g_shm_handles.erase(name);
        return NULL;
    }
    MEMORY_BASIC_INFORMATION mapping_info;
    if (VirtualQuery(addr, &mapping_info, sizeof(mapping_info)) == 0
        || mapping_info.RegionSize == 0) {
        UnmapViewOfFile(addr);
        CloseHandle(hMap);
        std::lock_guard<std::mutex> lock(g_shm_handles_mutex);
        g_shm_handles.erase(name);
        return NULL;
    }
    if (mapped_size) {
        *mapped_size = mapping_info.RegionSize;
    }
    return addr;
}

void shmDetach(void* addr, size_t /*size*/) {
    if (addr) {
        UnmapViewOfFile(addr);
    }
}

void shmUnlink(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_shm_handles_mutex);
    auto it = g_shm_handles.find(name);
    if (it != g_shm_handles.end()) {
        CloseHandle(it->second);
        g_shm_handles.erase(it);
    }
}


std::string getMachineId() {
    // 读取 Windows MachineGuid
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        char buf[256];
        DWORD buf_size = sizeof(buf);
        if (RegQueryValueExA(hKey, "MachineGuid", NULL, NULL,
                             reinterpret_cast<LPBYTE>(buf), &buf_size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(buf);
        }
        RegCloseKey(hKey);
    }
    return getHostName();
}

int64_t currentTimeMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Windows FILETIME 是从 1601-01-01 开始的 100ns 间隔
    // 转换为 Unix 毫秒
    return static_cast<int64_t>((uli.QuadPart - 116444736000000000ULL) / 10000);
}

int64_t currentTimeUs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((uli.QuadPart - 116444736000000000ULL) / 10);
}

std::string getHostName() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
    return "unknown";
}

int getPid() {
    return static_cast<int>(GetCurrentProcessId());
}

std::string getProcessName() {
    const DWORD PROCESS_PATH_BUFFER_SIZE = MAX_PATH;
    char path[PROCESS_PATH_BUFFER_SIZE];
    DWORD len = GetModuleFileNameA(NULL, path, PROCESS_PATH_BUFFER_SIZE);
    if (len > 0 && len < PROCESS_PATH_BUFFER_SIZE) {
        std::string name = basenameFromPath(path);
        if (!name.empty()) {
            return name;
        }
    }

    return std::string();
}

void sleepMs(uint32_t ms) {
    ::Sleep(ms);
}

void getLocalTime(struct tm* out_tm, int* out_ms) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    int64_t unix_ms = static_cast<int64_t>((uli.QuadPart - 116444736000000000ULL) / 10000);
    time_t t = static_cast<time_t>(unix_ms / 1000);
    struct tm* tmp = localtime(&t);
    if (tmp) *out_tm = *tmp;
    if (out_ms) *out_ms = static_cast<int>(unix_ms % 1000);
}

// ============================================================
// handshake channel — TCP loopback plus Named Pipe tokens on Windows
//
// Linux:  AF_UNIX + SCM_RIGHTS for fd transfer
// Windows: TCP on 127.0.0.1.  fd values are sent as pipe NAME
//          strings over the control connection; the receiver
//          opens them via CreateFile.
// Port:    derived from path hash (50000 + hash % 10000)
// ============================================================

static uint16_t udsPathToPort(const std::string& path) {
    uint32_t h = 5381;
    for (size_t i = 0; i < path.size(); ++i)
        h = ((h << 5) + h) + static_cast<unsigned char>(path[i]);
    return static_cast<uint16_t>(50000 + (h % 10000));
}
struct handshake_listener { SocketFd fd; };
struct handshake_channel   {
    SocketFd fd;
    int local_notify_fd;
    int64_t deadline_ms;
};

bool checkSocketConnected(SocketFd fd, int* out_error);

namespace {

const size_t HANDSHAKE_MAX_PIPE_NAME = 512;
const int64_t HANDSHAKE_TRANSACTION_TIMEOUT_MS = 500;

bool waitForHandshakeIo(SocketFd fd, bool writable, int64_t deadline_ms)
{
    for (;;) {
        int64_t remaining = deadline_ms - currentTimeMs();
        if (remaining <= 0) return false;

        fd_set read_fds;
        fd_set write_fds;
        fd_set except_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_ZERO(&except_fds);
        if (writable) {
            FD_SET(fd, &write_fds);
        } else {
            FD_SET(fd, &read_fds);
        }
        FD_SET(fd, &except_fds);

        struct timeval tv;
        tv.tv_sec = static_cast<long>(remaining / 1000);
        tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        int ret = select(0, &read_fds, &write_fds, &except_fds, &tv);
        if (ret > 0) {
            if (FD_ISSET(fd, &except_fds)) return false;
            return writable ? FD_ISSET(fd, &write_fds) != 0
                            : FD_ISSET(fd, &read_fds) != 0;
        }
        if (ret == 0) return false;
        if (WSAGetLastError() != WSAEINTR) return false;
    }
}

bool socketSendAll(SocketFd fd, const char* data, size_t len, int64_t deadline_ms)
{
    while (len > 0) {
        if (currentTimeMs() >= deadline_ms) return false;
        int chunk = len > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(len);
        int sent = ::send(fd, data, chunk, 0);
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEINTR) continue;
            if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
                if (waitForHandshakeIo(fd, true, deadline_ms)) continue;
            }
            return false;
        }
        if (sent == 0) return false;
        data += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

bool socketRecvAll(SocketFd fd, char* data, size_t len, int64_t deadline_ms)
{
    while (len > 0) {
        if (currentTimeMs() >= deadline_ms) return false;
        int chunk = len > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(len);
        int received = ::recv(fd, data, chunk, 0);
        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEINTR) continue;
            if (error == WSAEWOULDBLOCK) {
                if (waitForHandshakeIo(fd, false, deadline_ms)) continue;
            }
            return false;
        }
        if (received == 0) return false;
        data += received;
        len -= static_cast<size_t>(received);
    }
    return true;
}

void closeOpenedEventFds(int* fds, int count)
{
    for (int i = 0; i < count; ++i) {
        if (fds[i] >= 0) closeEventFd(fds[i]);
    }
}

} // namespace

handshake_listener* handshakeListen(const std::string& path)
{
    uint16_t port = udsPathToPort(path);
    SocketFd fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return NULL;

    setReuseAddr(fd);
    setNonBlocking(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        OMNI_LOG_ERROR(LOG_TAG, "handshakeListen bind failed port=%u: %d", port, WSAGetLastError());
        closesocket(fd);
        return NULL;
    }
    if (::listen(fd, 8) != 0) {
        OMNI_LOG_ERROR(LOG_TAG, "handshakeListen listen failed port=%u: %d", port, WSAGetLastError());
        closesocket(fd);
        return NULL;
    }

    handshake_listener* listener = new handshake_listener;
    listener->fd = fd;
    OMNI_LOG_DEBUG(LOG_TAG, "handshakeListen: port=%u path=%s", port, path.c_str());
    return listener;
}

handshake_channel* handshakeAccept(handshake_listener* listener)
{
    if (!listener) return NULL;
    int64_t accept_deadline_ms = currentTimeMs() + HANDSHAKE_TRANSACTION_TIMEOUT_MS;
    SocketFd fd = INVALID_SOCKET;
    for (;;) {
        fd = ::accept(listener->fd, NULL, NULL);
        if (fd != INVALID_SOCKET) break;
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAEWOULDBLOCK) {
            if (waitForHandshakeIo(listener->fd, false, accept_deadline_ms)) continue;
        }
        if (err != WSAEWOULDBLOCK) {
            OMNI_LOG_WARN(LOG_TAG, "handshakeAccept failed: %d", err);
        }
        return NULL;
    }
    if (!setNonBlocking(fd)) {
        closesocket(fd);
        return NULL;
    }
    handshake_channel* ch = new handshake_channel;
    ch->fd = fd;
    ch->local_notify_fd = -1;
    ch->deadline_ms = currentTimeMs() + HANDSHAKE_TRANSACTION_TIMEOUT_MS;
    return ch;
}

handshake_channel* handshakeConnect(const std::string& path)
{
    uint16_t port = udsPathToPort(path);
    SocketFd fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return NULL;

    int64_t deadline_ms = currentTimeMs() + HANDSHAKE_TRANSACTION_TIMEOUT_MS;
    if (!setNonBlocking(fd)) {
        closesocket(fd);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    int result = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (result != 0) {
        int error = WSAGetLastError();
        if (error == WSAEINTR || error == WSAEINPROGRESS || error == WSAEWOULDBLOCK) {
            if (!waitForHandshakeIo(fd, true, deadline_ms)
                || !checkSocketConnected(fd, &error)) {
                OMNI_LOG_WARN(LOG_TAG, "handshakeConnect failed port=%u: %d", port, error);
                closesocket(fd);
                return NULL;
            }
        } else {
            OMNI_LOG_WARN(LOG_TAG, "handshakeConnect failed port=%u: %d", port, error);
            closesocket(fd);
            return NULL;
        }
    }
    handshake_channel* ch = new handshake_channel;
    ch->fd = fd;
    ch->local_notify_fd = -1;
    ch->deadline_ms = deadline_ms;
    return ch;
}

void handshakeCloseListener(handshake_listener* listener)
{
    if (!listener) return;
    if (listener->fd != INVALID_SOCKET) closesocket(listener->fd);
    delete listener;
}

bool handshakeSend(handshake_channel* ch, const void* data, size_t len,
                   const int* fds, int fd_count)
{
    if (!ch || len > HANDSHAKE_MAX_PAYLOAD || fd_count < 0 || fd_count > 2
        || (len > 0 && !data) || (fd_count > 0 && !fds)
        || ch->local_notify_fd >= 0) return false;

    int wire_fds[2] = {-1, -1};
    for (int i = 0; i < fd_count; ++i) wire_fds[i] = fds[i];

    // A Windows Named Pipe endpoint is single-client. Replace the logical
    // shared request notification with a server-owned per-client endpoint.
    if (fd_count == 2) {
        ch->local_notify_fd = createEventFd();
        if (ch->local_notify_fd < 0) return false;
        wire_fds[1] = ch->local_notify_fd;
    }

    // Wire format:
    //   [uint32_t data_len][uint8_t data[data_len]][uint32_t fd_count]
    //   [for each fd: uint32_t name_len][char name[name_len]]
    uint32_t dlen = static_cast<uint32_t>(len);
    size_t total = sizeof(uint32_t) * 2 + len;
    for (int i = 0; i < fd_count; ++i) {
        std::map<int, EventFdEntryV2>::iterator eit = g_efd_map2.find(wire_fds[i]);
        if (eit == g_efd_map2.end() || eit->second.pipe_name.empty()
            || eit->second.pipe_name.size() > HANDSHAKE_MAX_PIPE_NAME) {
            if (ch->local_notify_fd >= 0) {
                closeEventFd(ch->local_notify_fd);
                ch->local_notify_fd = -1;
            }
            return false;
        }
        total += sizeof(uint32_t) + eit->second.pipe_name.size();
    }

    std::vector<char> payload(total);
    char* p = payload.data();
    memcpy(p, &dlen, 4); p += 4;
    if (dlen > 0) {
        memcpy(p, data, dlen); p += dlen;
    }
    {
        uint32_t count32 = static_cast<uint32_t>(fd_count);
        memcpy(p, &count32, 4); p += 4;
    }
    for (int i = 0; i < fd_count; ++i) {
        std::map<int, EventFdEntryV2>::iterator eit = g_efd_map2.find(wire_fds[i]);
        uint32_t name_len = static_cast<uint32_t>(eit->second.pipe_name.size());
        memcpy(p, &name_len, 4); p += 4;
        memcpy(p, eit->second.pipe_name.data(), name_len); p += name_len;
    }

    if (!socketSendAll(ch->fd, payload.data(), payload.size(), ch->deadline_ms)) {
        if (ch->local_notify_fd >= 0) {
            closeEventFd(ch->local_notify_fd);
            ch->local_notify_fd = -1;
        }
        return false;
    }
    return true;
}

bool handshakeRecv(handshake_channel* ch, void* buf, size_t bufsz, size_t* out_len,
                   int* fds, int max_fds, int* out_fd_count)
{
    if (out_len) *out_len = 0;
    if (out_fd_count) *out_fd_count = 0;
    if (fds && max_fds > 0) std::fill(fds, fds + max_fds, -1);
    if (!ch || max_fds < 0 || max_fds > 2 || (max_fds > 0 && !fds)) return false;

    uint32_t dlen = 0;
    int opened[2] = {-1, -1};
    int opened_count = 0;
    if (!socketRecvAll(ch->fd, reinterpret_cast<char*>(&dlen), sizeof(dlen), ch->deadline_ms)
        || dlen > HANDSHAKE_MAX_PAYLOAD || dlen > bufsz || (dlen > 0 && !buf)) {
        closeOpenedEventFds(opened, opened_count);
        return false;
    }
    std::vector<char> data(dlen);
    if (dlen > 0 && !socketRecvAll(ch->fd, data.data(), dlen, ch->deadline_ms)) {
        closeOpenedEventFds(opened, opened_count);
        return false;
    }

    uint32_t count = 0;
    if (!socketRecvAll(ch->fd, reinterpret_cast<char*>(&count), sizeof(count), ch->deadline_ms)
        || count > 2 || count > static_cast<uint32_t>(max_fds)) {
        closeOpenedEventFds(opened, opened_count);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t name_len = 0;
        if (!socketRecvAll(ch->fd, reinterpret_cast<char*>(&name_len), sizeof(name_len), ch->deadline_ms)
            || name_len == 0 || name_len > HANDSHAKE_MAX_PIPE_NAME) {
            closeOpenedEventFds(opened, opened_count);
            return false;
        }

        std::vector<char> name_buf(name_len);
        if (!socketRecvAll(ch->fd, name_buf.data(), name_len, ch->deadline_ms)) {
            closeOpenedEventFds(opened, opened_count);
            return false;
        }
        std::string pipe_name(name_buf.data(), name_len);
        opened[opened_count] = openNamedEventFdByPipeName(pipe_name);
        if (opened[opened_count] < 0) {
            closeOpenedEventFds(opened, opened_count);
            return false;
        }
        ++opened_count;
    }
    if (dlen > 0) memcpy(buf, data.data(), dlen);
    for (int i = 0; i < opened_count; ++i) fds[i] = opened[i];
    if (out_len) *out_len = dlen;
    if (out_fd_count) *out_fd_count = opened_count;
    return true;
}

int handshakeTakeLocalNotifyFd(handshake_channel* ch)
{
    if (!ch) return -1;
    int fd = ch->local_notify_fd;
    ch->local_notify_fd = -1;
    return fd;
}

void handshakeClose(handshake_channel* ch)
{
    if (!ch) return;
    if (ch->local_notify_fd >= 0) closeEventFd(ch->local_notify_fd);
    if (ch->fd != INVALID_SOCKET) closesocket(ch->fd);
    delete ch;
}

bool checkSocketConnected(SocketFd fd, int* out_error) {
    int so_error = 0;
    int len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&so_error), &len) != 0) {
        if (out_error) *out_error = WSAGetLastError();
        return false;
    }
    if (out_error) *out_error = so_error;
    return so_error == 0;
}


void setupSignalHandlers(SignalHandler handler) {
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
}


int handshakeGetFd(handshake_channel* ch) { return ch ? static_cast<int>(ch->fd) : -1; }
int handshakeGetListenerFd(handshake_listener* l) { return l ? static_cast<int>(l->fd) : -1; }

bool isShmHandshakeAvailable() {
    return true;  // Emulated via Named Pipes
}

void memoryBarrier() {
    MemoryBarrier();
}

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_WINDOWS
