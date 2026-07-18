#ifdef OMNIBINDER_LINUX

#include "platform/platform.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"

// 平台实现依赖的系统头文件（原本在 platform.h 的 #ifdef 中，现已移至此）
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <cstring>
#include <cstdio>
#include <climits>
#include <fstream>
#include <sstream>
#include <string>


#include <sys/time.h>
#include <sys/un.h>
#include <netdb.h>
#include <time.h>

#define LOG_TAG "Platform"

namespace omnibinder {
namespace platform {

namespace {

std::string basenameFromPath(const char* path) {
    if (!path || path[0] == '\0') {
        return std::string();
    }

    const char* name = strrchr(path, '/');
    return name ? std::string(name + 1) : std::string(path);
}

} // namespace

// ============================================================
// 网络初始化
// ============================================================
bool netInit() {
    // Linux 不需要特殊初始化
    // 忽略 SIGPIPE 信号
    signal(SIGPIPE, SIG_IGN);
    return true;
}

void netCleanup() {
    // Linux 无需清理
}

// ============================================================
// Socket 操作
// ============================================================

SocketFd createTcpSocket() {
    SocketFd fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create socket: %s", strerror(errno));
    }
    return fd;
}

bool setNonBlocking(SocketFd fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool setReuseAddr(SocketFd fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
}

bool setTcpNoDelay(SocketFd fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
}

bool setKeepAlive(SocketFd fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) == 0;
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

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Bind failed on %s:%u: %s",
                         host.c_str(), port, strerror(errno));
        return false;
    }
    return true;
}

bool listenSocket(SocketFd fd, int backlog) {
    if (listen(fd, backlog) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Listen failed: %s", strerror(errno));
        return false;
    }
    return true;
}

SocketFd acceptSocket(SocketFd fd, std::string& remote_host, uint16_t& remote_port) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    SocketFd client_fd;
    do {
        client_fd = accept(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
    } while (client_fd < 0 && errno == EINTR);
    if (client_fd < 0) {
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
        // 尝试 DNS 解析
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
    if (ret < 0) {
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            return 1;  // 连接中
        }
        return -1;  // 失败
    }
    return 0;  // 成功
}

int socketSend(SocketFd fd, const void* data, size_t length) {
    int ret;
    do {
        ret = static_cast<int>(send(fd, data, length, MSG_NOSIGNAL));
    } while (ret < 0 && errno == EINTR);
    return ret;
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

int socketRecv(SocketFd fd, void* buf, size_t buf_size) {
    int ret;
    do {
        ret = static_cast<int>(recv(fd, buf, buf_size, 0));
    } while (ret < 0 && errno == EINTR);
    return ret;
}

void closeSocket(SocketFd fd) {
    if (fd != INVALID_SOCKET_FD) {
        close(fd);
    }
}

uint16_t getSocketPort(SocketFd fd) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) < 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

std::string getSocketAddress(SocketFd fd) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) < 0) {
        return "";
    }

    char ip_buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf))) {
        return "";
    }
    return std::string(ip_buf);
}

int getSocketError() {
    return errno;
}

bool isWouldBlock(int error_code) {
    return error_code == EAGAIN || error_code == EWOULDBLOCK;
}


bool isConnectionReset(int error_code) {
    return error_code == ECONNRESET || error_code == EPIPE;
}

bool waitSocketWritable(SocketFd fd, uint32_t timeout_ms) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int timeout = timeout_ms > static_cast<uint32_t>(INT_MAX)
        ? INT_MAX
        : static_cast<int>(timeout_ms);
    int ret;
    do {
        ret = poll(&pfd, 1, timeout);
    } while (ret < 0 && errno == EINTR);

    if (ret <= 0) {
        return false;
    }

    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
    }

    return (pfd.revents & POLLOUT) != 0;
}

bool waitFdReadable(int fd, int timeout_ms) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret;
    do {
        ret = poll(&pfd, 1, timeout_ms);
    } while (ret < 0 && errno == EINTR);
    return ret > 0 && (pfd.revents & POLLIN);
}

// ============================================================
// 事件通知
// ============================================================

int createEventFd() {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create eventfd: %s", strerror(errno));
    }
    return efd;
}

int createNamedEventFd(const std::string& /*name*/) {
    // Linux: eventfd is anonymous; cross-process sharing via UDS SCM_RIGHTS.
    return createEventFd();
}

int openNamedEventFd(const std::string& /*name*/) {
    // Linux: fd exchange happens via handshake, not by name.
    return -1;
}

bool eventFdNotify(int efd) {
    uint64_t val = 1;
    return write(efd, &val, sizeof(val)) == sizeof(val);
}

bool eventFdConsume(int efd) {
    uint64_t val;
    return read(efd, &val, sizeof(val)) == sizeof(val);
}

void closeEventFd(int efd) {
    if (efd >= 0) {
        close(efd);
    }
}

// ============================================================
// 共享内存
// ============================================================

void* shmCreate(const std::string& name, size_t size, bool create) {
    int flags = O_RDWR;
    if (create) {
        flags |= O_CREAT;
    }

    std::string shm_name = "/" + name;
    int fd = shm_open(shm_name.c_str(), flags, 0600);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "shm_open failed for %s: %s",
                         shm_name.c_str(), strerror(errno));
        return NULL;
    }

    if (create) {
        if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "ftruncate failed: %s", strerror(errno));
            close(fd);
            return NULL;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "fstat failed for %s: %s",
                             shm_name.c_str(), strerror(errno));
            close(fd);
            return NULL;
        }
        if (st.st_size <= 0) {
            OMNI_LOG_ERROR(LOG_TAG, "invalid shm size for %s: %lld",
                             shm_name.c_str(), static_cast<long long>(st.st_size));
            close(fd);
            return NULL;
        }
        size = static_cast<size_t>(st.st_size);
    }

    void* addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        OMNI_LOG_ERROR(LOG_TAG, "mmap failed: %s", strerror(errno));
        return NULL;
    }

    return addr;
}

void shmDetach(void* addr, size_t size) {
    if (addr) {
        munmap(addr, size);
    }
}

void shmUnlink(const std::string& name) {
    std::string shm_name = "/" + name;
    shm_unlink(shm_name.c_str());
}


// ============================================================
// handshake channel（用于 SHM eventfd 交换）
// ============================================================


int shmHandshakeListen(const std::string& path, int backlog) {
    // 清理残留
    ::unlink(path.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS listen socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "UDS bind failed on %s: %s", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "handshake listen failed on %s: %s", path.c_str(), strerror(errno));
        close(fd);
        ::unlink(path.c_str());
        return -1;
    }

    return fd;
}

int shmHandshakeAccept(int listen_fd) {
    int fd;
    do {
        fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int shmHandshakeConnect(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS connect socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "UDS connect failed to %s: %s", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

bool shmHandshakeSendFds(int fd, const int* fds, int fd_count,
                const void* data, size_t data_len) {
    // 至少需要 1 字节的 iov 数据（即使 data_len == 0）
    char dummy = 0;
    struct iovec iov;
    if (data && data_len > 0) {
        iov.iov_base = const_cast<void*>(data);
        iov.iov_len = data_len;
    } else {
        iov.iov_base = &dummy;
        iov.iov_len = 1;
    }

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[256];
    if (fd_count > 0) {
        if (!fds) {
            OMNI_LOG_ERROR(LOG_TAG, "shmHandshakeSendFds: fd_count=%d but fds is NULL", fd_count);
            return false;
        }

        // 构造 cmsg
        size_t cmsg_space = CMSG_SPACE(sizeof(int) * static_cast<size_t>(fd_count));
        // 使用栈上分配（fd_count 最多 2-3 个，cmsg_space 很小）
        if (cmsg_space > sizeof(cmsg_buf)) {
            OMNI_LOG_ERROR(LOG_TAG, "shmHandshakeSendFds: too many fds (%d)", fd_count);
            return false;
        }
        memset(cmsg_buf, 0, cmsg_space);
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = cmsg_space;

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * static_cast<size_t>(fd_count));
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * static_cast<size_t>(fd_count));
    }

    ssize_t ret;
    do {
        ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "shmHandshakeSendFds failed: %s", strerror(errno));
        return false;
    }
    return true;
}

bool shmHandshakeRecvFds(int fd, int* fds, int fd_count,
                void* buf, size_t buf_size, size_t* out_data_len) {
    char dummy;
    struct iovec iov;
    if (buf && buf_size > 0) {
        iov.iov_base = buf;
        iov.iov_len = buf_size;
    } else {
        iov.iov_base = &dummy;
        iov.iov_len = 1;
    }

    size_t cmsg_space = CMSG_SPACE(sizeof(int) * static_cast<size_t>(fd_count));
    char cmsg_buf[256];
    if (cmsg_space > sizeof(cmsg_buf)) {
        OMNI_LOG_ERROR(LOG_TAG, "shmHandshakeRecvFds: too many fds (%d)", fd_count);
        return false;
    }
    memset(cmsg_buf, 0, cmsg_space);

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = cmsg_space;

    ssize_t ret;
    do {
        ret = recvmsg(fd, &msg, 0);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "shmHandshakeRecvFds failed: %s", strerror(errno));
        return false;
    }

    if (out_data_len) {
        *out_data_len = static_cast<size_t>(ret);
    }

    // 提取 fd
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * static_cast<size_t>(fd_count));
        return true;
    }

    // No SCM_RIGHTS in message — valid for data-only datagrams (e.g., SHM name exchange)
    if (fd_count > 0) {
        OMNI_LOG_DEBUG(LOG_TAG, "shmHandshakeRecvFds: no SCM_RIGHTS in message");
    }
    for (int i = 0; i < fd_count; ++i) {
        fds[i] = -1;
    }
    return false;
}

bool shmHandshakeSendResponse(int client_fd, int resp_eventfd, int master_eventfd,
                           int* out_new_fd) {
    *out_new_fd = -1;
    int fds[2] = { resp_eventfd, master_eventfd };
    return shmHandshakeSendFds(client_fd, fds, 2, NULL, 0);
}

void shmHandshakeClose(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

void shmHandshakeCleanup(const std::string& path) {
    ::unlink(path.c_str());
}

// ============================================================
// 系统信息
// ============================================================

std::string getMachineId() {
    // 尝试读取 /etc/machine-id
    std::ifstream f("/etc/machine-id");
    if (f.is_open()) {
        std::string id;
        std::getline(f, id);
        if (!id.empty()) {
            return id;
        }
    }

    // 尝试 /var/lib/dbus/machine-id
    std::ifstream f2("/var/lib/dbus/machine-id");
    if (f2.is_open()) {
        std::string id;
        std::getline(f2, id);
        if (!id.empty()) {
            return id;
        }
    }

    // 回退：使用 hostname 的哈希
    std::string hostname = getHostName();
    char buf[32];
    snprintf(buf, sizeof(buf), "%08x", fnv1a_32(hostname));
    return std::string(buf);
}

int64_t currentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string getHostName() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        return std::string(buf);
    }
    return "unknown";
}

int getPid() {
    return static_cast<int>(getpid());
}

std::string getProcessName() {
    const size_t PROCESS_PATH_BUFFER_SIZE = 4096;
    char path[PROCESS_PATH_BUFFER_SIZE];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        std::string name = basenameFromPath(path);
        if (!name.empty()) {
            return name;
        }
    }

    return std::string();
}

void sleepMs(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}




bool checkSocketConnected(SocketFd fd, int* out_error) {
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
        if (out_error) *out_error = errno;
        return false;
    }
    if (out_error) *out_error = so_error;
    return so_error == 0;
}

int64_t currentTimeUs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}
void getLocalTime(struct tm* out_tm, int* out_ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, out_tm);
    if (out_ms) *out_ms = static_cast<int>(tv.tv_usec / 1000);
}


void setupSignalHandlers(SignalHandler handler) {
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
}

bool isShmHandshakeAvailable() {
    return true;
}

void memoryBarrier() {
    __sync_synchronize();
}

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_LINUX
