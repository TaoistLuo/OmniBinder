#include "platform/platform.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"

#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef OMNIBINDER_LINUX

#include <sys/time.h>
#include <sys/un.h>
#include <netdb.h>

#define LOG_TAG "Platform"

namespace omnibinder {
namespace platform {

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

bool isInProgress(int error_code) {
    return error_code == EINPROGRESS;
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
    int fd = shm_open(shm_name.c_str(), flags, 0666);
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
// 命名信号量
// ============================================================

SemHandle semCreate(const std::string& name, unsigned int initial_value) {
    std::string sem_name = "/" + name;
    sem_t* sem = sem_open(sem_name.c_str(), O_CREAT, 0666, initial_value);
    if (sem == SEM_FAILED) {
        OMNI_LOG_ERROR(LOG_TAG, "sem_open create failed for %s: %s",
                         sem_name.c_str(), strerror(errno));
        return INVALID_SEM;
    }
    return static_cast<SemHandle>(sem);
}

SemHandle semOpen(const std::string& name) {
    std::string sem_name = "/" + name;
    sem_t* sem = sem_open(sem_name.c_str(), 0);
    if (sem == SEM_FAILED) {
        OMNI_LOG_ERROR(LOG_TAG, "sem_open failed for %s: %s",
                         sem_name.c_str(), strerror(errno));
        return INVALID_SEM;
    }
    return static_cast<SemHandle>(sem);
}

bool semWait(SemHandle sem, uint32_t timeout_ms) {
    if (!sem) return false;
    sem_t* s = static_cast<sem_t*>(sem);

    if (timeout_ms == 0) {
        return sem_trywait(s) == 0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    return sem_timedwait(s, &ts) == 0;
}

bool semPost(SemHandle sem) {
    if (!sem) return false;
    return sem_post(static_cast<sem_t*>(sem)) == 0;
}

void semClose(SemHandle sem) {
    if (sem) {
        sem_close(static_cast<sem_t*>(sem));
    }
}

void semUnlink(const std::string& name) {
    std::string sem_name = "/" + name;
    sem_unlink(sem_name.c_str());
}

// ============================================================
// Unix Domain Socket（用于 SHM eventfd 交换）
// ============================================================

int udsCreate() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS socket: %s", strerror(errno));
    }
    return fd;
}

int udsBindListen(const std::string& path, int backlog) {
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
        OMNI_LOG_ERROR(LOG_TAG, "UDS listen failed on %s: %s", path.c_str(), strerror(errno));
        close(fd);
        ::unlink(path.c_str());
        return -1;
    }

    return fd;
}

int udsAccept(int listen_fd) {
    int fd;
    do {
        fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int udsConnect(const std::string& path) {
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

bool udsSendFds(int uds_fd, const int* fds, int fd_count,
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

    // 构造 cmsg
    size_t cmsg_space = CMSG_SPACE(sizeof(int) * static_cast<size_t>(fd_count));
    // 使用栈上分配（fd_count 最多 2-3 个，cmsg_space 很小）
    char cmsg_buf[256];
    if (cmsg_space > sizeof(cmsg_buf)) {
        OMNI_LOG_ERROR(LOG_TAG, "udsSendFds: too many fds (%d)", fd_count);
        return false;
    }
    memset(cmsg_buf, 0, cmsg_space);

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = cmsg_space;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * static_cast<size_t>(fd_count));
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * static_cast<size_t>(fd_count));

    ssize_t ret;
    do {
        ret = sendmsg(uds_fd, &msg, MSG_NOSIGNAL);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "udsSendFds failed: %s", strerror(errno));
        return false;
    }
    return true;
}

bool udsRecvFds(int uds_fd, int* fds, int fd_count,
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
        OMNI_LOG_ERROR(LOG_TAG, "udsRecvFds: too many fds (%d)", fd_count);
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
        ret = recvmsg(uds_fd, &msg, 0);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "udsRecvFds failed: %s", strerror(errno));
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

    // 没有收到 fd
    OMNI_LOG_WARN(LOG_TAG, "udsRecvFds: no SCM_RIGHTS in message");
    for (int i = 0; i < fd_count; ++i) {
        fds[i] = -1;
    }
    return false;
}

void udsClose(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

void udsUnlink(const std::string& path) {
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
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

std::string getHostName() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        return std::string(buf);
    }
    return "unknown";
}

void sleepMs(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

} // namespace platform
} // namespace omnibinder

#elif defined(OMNIBINDER_WINDOWS)

// Windows 桩实现 - 后续完善
namespace omnibinder {
namespace platform {

bool netInit() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void netCleanup() {
    WSACleanup();
}

SocketFd createTcpSocket() {
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

bool isInProgress(int error_code) {
    return error_code == WSAEWOULDBLOCK;
}

// Windows eventfd 模拟（使用 pipe 或 socket pair）
int createEventFd() {
    // Windows 使用自连接的 UDP socket 模拟
    SocketFd fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }

    int addr_len = sizeof(addr);
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }

    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);

    return static_cast<int>(fd);
}

bool eventFdNotify(int efd) {
    char c = 1;
    return send(static_cast<SocketFd>(efd), &c, 1, 0) == 1;
}

bool eventFdConsume(int efd) {
    char buf[64];
    return recv(static_cast<SocketFd>(efd), buf, sizeof(buf), 0) > 0;
}

void closeEventFd(int efd) {
    if (efd >= 0) {
        closesocket(static_cast<SocketFd>(efd));
    }
}

// Windows 共享内存（使用 Named File Mapping）
void* shmCreate(const std::string& name, size_t size, bool create) {
    std::string map_name = "Local\\omnibinder_" + name;
    HANDLE hMap;
    if (create) {
        hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, static_cast<DWORD>(size), map_name.c_str());
    } else {
        hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name.c_str());
    }
    if (!hMap) return NULL;

    void* addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
    // 注意：这里不关闭 hMap，因为关闭后映射仍然有效
    // 但需要在某处保存 hMap 以便后续清理
    return addr;
}

void shmDetach(void* addr, size_t /*size*/) {
    if (addr) {
        UnmapViewOfFile(addr);
    }
}

void shmUnlink(const std::string& /*name*/) {
    // Windows 的 file mapping 在所有句柄关闭后自动删除
}

SemHandle semCreate(const std::string& name, unsigned int initial_value) {
    std::string sem_name = "Local\\omnibinder_sem_" + name;
    HANDLE sem = CreateSemaphoreA(NULL, initial_value, 0x7FFFFFFF, sem_name.c_str());
    return static_cast<SemHandle>(sem);
}

SemHandle semOpen(const std::string& name) {
    std::string sem_name = "Local\\omnibinder_sem_" + name;
    HANDLE sem = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, sem_name.c_str());
    return static_cast<SemHandle>(sem);
}

bool semWait(SemHandle sem, uint32_t timeout_ms) {
    if (!sem) return false;
    DWORD t = (timeout_ms == 0) ? 0 : timeout_ms;
    return WaitForSingleObject(static_cast<HANDLE>(sem), t) == WAIT_OBJECT_0;
}

bool semPost(SemHandle sem) {
    if (!sem) return false;
    return ReleaseSemaphore(static_cast<HANDLE>(sem), 1, NULL) != 0;
}

void semClose(SemHandle sem) {
    if (sem) {
        CloseHandle(static_cast<HANDLE>(sem));
    }
}

void semUnlink(const std::string& /*name*/) {
    // Windows 信号量在所有句柄关闭后自动删除
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

std::string getHostName() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
    return "unknown";
}

void sleepMs(uint32_t ms) {
    ::Sleep(ms);
}

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_WINDOWS
