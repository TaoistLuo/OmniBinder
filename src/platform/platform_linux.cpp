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
#include <algorithm>


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

void* shmCreate(const std::string& name, size_t size, bool create, size_t* mapped_size) {
    if (mapped_size) {
        *mapped_size = 0;
    }
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

    if (mapped_size) {
        *mapped_size = size;
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
// SHM 握手通道实现（Linux: AF_UNIX + SCM_RIGHTS）
// ============================================================
struct handshake_listener { int fd; std::string path; };
struct handshake_channel   { int fd; int64_t deadline_ms; };

namespace {

const uint32_t HANDSHAKE_MAGIC = 0x484E4453u;
const int64_t HANDSHAKE_TRANSACTION_TIMEOUT_MS = 500;

struct HandshakeHeader {
    uint32_t magic;
    uint32_t payload_len;
    uint32_t fd_count;
};

bool validUnixPath(const std::string& path)
{
    return !path.empty() && path.size() < sizeof(sockaddr_un::sun_path);
}

bool waitForHandshakeIo(int fd, short events, int64_t deadline_ms)
{
    for (;;) {
        int64_t remaining = deadline_ms - currentTimeMs();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return false;
        }

        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = events;
        int timeout = remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
        int ret = poll(&pfd, 1, timeout);
        if (ret > 0) {
            return (pfd.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        if (ret == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (errno != EINTR) return false;
    }
}

bool sendAll(int fd, const char* data, size_t len, int64_t deadline_ms)
{
    while (len > 0) {
        if (currentTimeMs() >= deadline_ms) {
            errno = ETIMEDOUT;
            return false;
        }
        ssize_t sent = ::send(fd, data, len, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            len -= static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitForHandshakeIo(fd, POLLOUT, deadline_ms)) continue;
        }
        return false;
    }
    return true;
}

bool recvAll(int fd, char* data, size_t len, int64_t deadline_ms)
{
    while (len > 0) {
        if (currentTimeMs() >= deadline_ms) {
            errno = ETIMEDOUT;
            return false;
        }
        ssize_t received = ::recv(fd, data, len, 0);
        if (received > 0) {
            data += received;
            len -= static_cast<size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitForHandshakeIo(fd, POLLIN, deadline_ms)) continue;
        }
        return false;
    }
    return true;
}

void closeReceivedFds(int* fds, int count)
{
    for (int i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

} // namespace

handshake_listener* handshakeListen(const std::string& path)
{
    if (!validUnixPath(path)) {
        OMNI_LOG_ERROR(LOG_TAG, "Invalid handshake path length: %zu", path.size());
        return NULL;
    }

    ::unlink(path.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS listen socket: %s", strerror(errno));
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "UDS bind failed on %s: %s", path.c_str(), strerror(errno));
        close(fd);
        return NULL;
    }

    if (listen(fd, 8) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "handshake listen failed on %s: %s", path.c_str(), strerror(errno));
        close(fd);
        ::unlink(path.c_str());
        return NULL;
    }

    handshake_listener* listener = new handshake_listener;
    listener->fd = fd;
    listener->path = path;
    return listener;
}

handshake_channel* handshakeAccept(handshake_listener* listener) {
    if (!listener) return NULL;
    int fd;
    do {
        fd = accept4(listener->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return NULL;
    handshake_channel* ch = new handshake_channel;
    ch->fd = fd;
    ch->deadline_ms = currentTimeMs() + HANDSHAKE_TRANSACTION_TIMEOUT_MS;
    return ch;
}

handshake_channel* handshakeConnect(const std::string& path)
{
    if (!validUnixPath(path)) {
        OMNI_LOG_ERROR(LOG_TAG, "Invalid handshake path length: %zu", path.size());
        return NULL;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS connect socket: %s", strerror(errno));
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    int64_t deadline_ms = currentTimeMs() + HANDSHAKE_TRANSACTION_TIMEOUT_MS;
    int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && (errno == EINPROGRESS || errno == EALREADY || errno == EINTR)) {
        if (waitForHandshakeIo(fd, POLLOUT, deadline_ms)) {
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0
                && socket_error == 0) {
                rc = 0;
            } else {
                errno = socket_error != 0 ? socket_error : errno;
            }
        }
    }
    if (rc < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "UDS connect failed to %s: %s", path.c_str(), strerror(errno));
        close(fd);
        return NULL;
    }

    handshake_channel* ch = new handshake_channel;
    ch->fd = fd;
    ch->deadline_ms = deadline_ms;
    return ch;
}

void handshakeCloseListener(handshake_listener* listener)
{
    if (!listener) return;
    if (listener->fd >= 0) close(listener->fd);
    if (!listener->path.empty()) ::unlink(listener->path.c_str());
    delete listener;
}

bool handshakeSend(handshake_channel* ch, const void* data, size_t len,
                   const int* fds, int fd_count)
{
    if (!ch || len > HANDSHAKE_MAX_PAYLOAD || fd_count < 0 || fd_count > 2
        || (len > 0 && !data) || (fd_count > 0 && !fds)) return false;

    HandshakeHeader header = {HANDSHAKE_MAGIC, static_cast<uint32_t>(len),
                              static_cast<uint32_t>(fd_count)};
    std::string frame(reinterpret_cast<const char*>(&header), sizeof(header));
    if (len > 0) frame.append(static_cast<const char*>(data), len);

    struct iovec iov = {const_cast<char*>(frame.data()), frame.size()};

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int) * 2)];
    if (fd_count > 0) {
        size_t cmsg_space = CMSG_SPACE(sizeof(int) * static_cast<size_t>(fd_count));
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
    for (;;) {
        if (currentTimeMs() >= ch->deadline_ms) {
            errno = ETIMEDOUT;
            ret = -1;
            break;
        }
        ret = sendmsg(ch->fd, &msg, MSG_NOSIGNAL);
        if (ret >= 0) break;
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK)
            && waitForHandshakeIo(ch->fd, POLLOUT, ch->deadline_ms)) continue;
        break;
    }

    if (ret <= 0) {
        OMNI_LOG_ERROR(LOG_TAG, "handshakeSend failed: %s", strerror(errno));
        return false;
    }
    size_t sent = static_cast<size_t>(ret);
    return sent == frame.size()
        || sendAll(ch->fd, frame.data() + sent, frame.size() - sent, ch->deadline_ms);
}

bool handshakeRecv(handshake_channel* ch, void* buf, size_t bufsz, size_t* out_len,
                   int* fds, int max_fds, int* out_fd_count)
{
    if (out_len) *out_len = 0;
    if (out_fd_count) *out_fd_count = 0;
    if (fds && max_fds > 0) std::fill(fds, fds + max_fds, -1);
    if (!ch || max_fds < 0 || max_fds > 2 || (max_fds > 0 && !fds)) return false;

    HandshakeHeader header = {};
    struct iovec iov = {&header, sizeof(header)};
    char cmsg_buf[CMSG_SPACE(sizeof(int) * 2)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t ret;
    for (;;) {
        if (currentTimeMs() >= ch->deadline_ms) {
            errno = ETIMEDOUT;
            ret = -1;
            break;
        }
        ret = recvmsg(ch->fd, &msg, MSG_CMSG_CLOEXEC);
        if (ret >= 0) break;
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK)
            && waitForHandshakeIo(ch->fd, POLLIN, ch->deadline_ms)) continue;
        break;
    }

    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "handshakeRecv failed: %s", strerror(errno));
        return false;
    }
    int received_fds[2] = {-1, -1};
    int received_count = 0;
    bool ancillary_valid = (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) == 0;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS
            || cmsg->cmsg_len < CMSG_LEN(0)) {
            ancillary_valid = false;
            continue;
        }
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0) {
            ancillary_valid = false;
            continue;
        }
        size_t count = bytes / sizeof(int);
        const int* rights = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        if (received_count != 0) ancillary_valid = false;
        for (size_t i = 0; i < count; ++i) {
            if (received_count < 2) {
                received_fds[received_count++] = rights[i];
            } else {
                close(rights[i]);
                ancillary_valid = false;
            }
        }
    }

    if (ret > 0 && ret < static_cast<ssize_t>(sizeof(header))
        && !recvAll(ch->fd, reinterpret_cast<char*>(&header) + ret,
                    sizeof(header) - static_cast<size_t>(ret), ch->deadline_ms)) {
        closeReceivedFds(received_fds, received_count);
        return false;
    }

    if (ret <= 0 || header.magic != HANDSHAKE_MAGIC
        || header.payload_len > HANDSHAKE_MAX_PAYLOAD || header.fd_count > 2
        || static_cast<int>(header.fd_count) != received_count || !ancillary_valid
        || header.payload_len > bufsz || (header.payload_len > 0 && !buf)
        || received_count > max_fds) {
        closeReceivedFds(received_fds, received_count);
        return false;
    }
    std::string payload(header.payload_len, '\0');
    if (header.payload_len > 0
        && !recvAll(ch->fd, &payload[0], header.payload_len, ch->deadline_ms)) {
        closeReceivedFds(received_fds, received_count);
        return false;
    }
    if (header.payload_len > 0) memcpy(buf, payload.data(), header.payload_len);
    for (int i = 0; i < received_count; ++i) fds[i] = received_fds[i];
    if (out_len) *out_len = header.payload_len;
    if (out_fd_count) *out_fd_count = received_count;
    return true;
}

int handshakeTakeLocalNotifyFd(handshake_channel*)
{
    return -1;
}

void handshakeClose(handshake_channel* ch)
{
    if (!ch) return;
    if (ch->fd >= 0) close(ch->fd);
    delete ch;
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


int handshakeGetFd(handshake_channel* ch) {
    return ch ? ch->fd : -1;
}

int handshakeGetListenerFd(handshake_listener* listener) {
    return listener ? listener->fd : -1;
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
