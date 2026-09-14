#include "transport/tcp_connection.h"
#include "platform/event_backend.h"
#include "omnibinder/log.h"

#include <cstring>

#define LOG_TAG "TcpConnection"

namespace omnibinder {

// ============================================================
// TcpConnection 实现
// ============================================================

TcpConnection::TcpConnection()
    : fd_(platform::INVALID_SOCKET_FD)
    , state_(ConnectionState::DISCONNECTED)
    , remote_port_(0)
{
}

TcpConnection::TcpConnection(platform::SocketFd connected_fd)
    : fd_(connected_fd)
    , state_(ConnectionState::DISCONNECTED)
    , remote_port_(0)
{
    if (fd_ != platform::INVALID_SOCKET_FD) {
        platform::setNonBlocking(fd_);
        platform::setTcpNoDelay(fd_);
        platform::setKeepAlive(fd_);
        state_ = ConnectionState::CONNECTED;
        OMNI_LOG_DEBUG(LOG_TAG, "Created from accepted fd=%d", static_cast<int>(fd_));
    }
}

TcpConnection::~TcpConnection()
{
    close();
}

int TcpConnection::connect(const std::string& host, uint16_t port)
{
    if (state_ == ConnectionState::CONNECTED || state_ == ConnectionState::CONNECTING) {
        OMNI_LOG_WARN(LOG_TAG, "connect() called in state %d, closing existing connection",
                        static_cast<int>(state_));
        close();
    }

    fd_ = platform::createTcpSocket();
    if (fd_ == platform::INVALID_SOCKET_FD) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create socket for connect to %s:%u",
                         host.c_str(), port);
        state_ = ConnectionState::ERROR;
        return -1;
    }

    platform::setNonBlocking(fd_);
    platform::setTcpNoDelay(fd_);
    platform::setKeepAlive(fd_);

    remote_host_ = host;
    remote_port_ = port;

    int ret = platform::connectSocket(fd_, host, port);
    if (ret == 0) {
        // 立即成功（非阻塞场景少见，但 localhost 下可能发生）
        state_ = ConnectionState::CONNECTED;
        OMNI_LOG_INFO(LOG_TAG, "Connected to %s:%u (fd=%d)",
                        host.c_str(), port, static_cast<int>(fd_));
        return 0;
    } else if (ret == 1) {
        // 连接进行中
        state_ = ConnectionState::CONNECTING;
        OMNI_LOG_DEBUG(LOG_TAG, "Connecting to %s:%u (fd=%d, in progress)",
                         host.c_str(), port, static_cast<int>(fd_));
        return 1;
    } else {
        // 失败
        OMNI_LOG_ERROR(LOG_TAG, "Failed to connect to %s:%u (error=%d)",
                         host.c_str(), port, platform::getSocketError());
        platform::closeSocket(fd_);
        fd_ = platform::INVALID_SOCKET_FD;
        state_ = ConnectionState::ERROR;
        return -1;
    }
}

int TcpConnection::send(const uint8_t* data, size_t length)
{
    if (state_ != ConnectionState::CONNECTED) {
        if (state_ == ConnectionState::ERROR) {
            OMNI_LOG_WARN(LOG_TAG, "send() called in error state on fd=%d",
                          static_cast<int>(fd_));
        }
        return -1;
    }

    if (length == 0) {
        return 0;
    }

    size_t total_sent = 0;
    while (total_sent < length) {
        int n = platform::socketSend(fd_,
                                     data + total_sent,
                                     length - total_sent);
        if (n > 0) {
            total_sent += static_cast<size_t>(n);
        } else if (n < 0) {
            int err = platform::getSocketError();
            if (platform::isWouldBlock(err)) {
                // socket 缓冲已满，返回当前已发送字节数
                break;
            }
            // 真实错误
            OMNI_LOG_ERROR(LOG_TAG, "send() failed on fd=%d: error=%d",
                             static_cast<int>(fd_), err);
            state_ = ConnectionState::ERROR;
            return -1;
        } else {
            // n == 0：未发送任何字节，按 would-block 处理
            break;
        }
    }

    return static_cast<int>(total_sent);
}

int TcpConnection::sendAll(const uint8_t* data, size_t length,
                                uint32_t timeout_ms, uint32_t* elapsed_ms)
{
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }
    if (state_ != ConnectionState::CONNECTED) {
        if (state_ == ConnectionState::ERROR) {
            OMNI_LOG_WARN(LOG_TAG, "sendAll() called in error state on fd=%d",
                          static_cast<int>(fd_));
        }
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    if (!platform::socketSendAll(fd_, data, length, timeout_ms, elapsed_ms)) {
        OMNI_LOG_WARN(LOG_TAG, "sendAll() incomplete on fd=%d: bytes=%zu timeout_ms=%u",
                      static_cast<int>(fd_), length, timeout_ms);
        return -1;
    }
    return 0;
}

void TcpConnection::consumeReadiness()
{
    // TCP 为字节流，无底层就绪通知需要消费
}

bool TcpConnection::isFramed() const
{
    return false;
}

int TcpConnection::peekFrameSize(size_t& out_length)
{
    (void)out_length;
    return 0;
}

int TcpConnection::recv(uint8_t* buf, size_t buf_size)
{
    if (state_ != ConnectionState::CONNECTED) {
        if (state_ == ConnectionState::ERROR) {
            OMNI_LOG_WARN(LOG_TAG, "recv() called in error state on fd=%d",
                          static_cast<int>(fd_));
        }
        return -1;
    }

    if (buf_size == 0) {
        return 0;
    }

    int n = platform::socketRecv(fd_, buf, buf_size);
    if (n > 0) {
        return n;
    } else if (n == 0) {
        // 对端正常关闭连接
        OMNI_LOG_INFO(LOG_TAG, "Peer closed connection on fd=%d", static_cast<int>(fd_));
        state_ = ConnectionState::DISCONNECTED;
        return -1;
    } else {
        // n < 0
        int err = platform::getSocketError();
        if (platform::isWouldBlock(err)) {
            // 当前无数据可读
            return 0;
        }
        // 真实错误
        if (platform::isConnectionReset(err)) {
            OMNI_LOG_INFO(LOG_TAG, "Peer reset connection on fd=%d",
                          static_cast<int>(fd_));
        } else {
            OMNI_LOG_ERROR(LOG_TAG, "recv() failed on fd=%d: error=%d",
                             static_cast<int>(fd_), err);
        }
        state_ = ConnectionState::ERROR;
        return -1;
    }
}

void TcpConnection::close()
{
    if (fd_ != platform::INVALID_SOCKET_FD) {
        OMNI_LOG_DEBUG(LOG_TAG, "Closing fd=%d", static_cast<int>(fd_));
        platform::closeSocket(fd_);
        fd_ = platform::INVALID_SOCKET_FD;
    }
    state_ = ConnectionState::DISCONNECTED;
}

ConnectionState TcpConnection::state() const
{
    return state_;
}

int TcpConnection::fd() const
{
    return static_cast<int>(fd_);
}

TransportType TcpConnection::type() const
{
    return TransportType::TCP;
}

bool TcpConnection::checkConnectComplete()
{
    if (state_ != ConnectionState::CONNECTING) {
        return state_ == ConnectionState::CONNECTED;
    }

    int so_error = 0;
    if (!platform::checkSocketConnected(fd_, &so_error)) {
        if (so_error == 0) {
            OMNI_LOG_ERROR(LOG_TAG, "getsockopt SO_ERROR failed on fd=%d",
                             static_cast<int>(fd_));
        }
        state_ = ConnectionState::ERROR;
        return false;
    }

    if (so_error == 0) {
        state_ = ConnectionState::CONNECTED;
        OMNI_LOG_INFO(LOG_TAG, "Connected to %s:%u (fd=%d)",
                        remote_host_.c_str(), remote_port_, static_cast<int>(fd_));
        return true;
    } else {
        OMNI_LOG_ERROR(LOG_TAG, "Async connect to %s:%u failed: error=%d",
                         remote_host_.c_str(), remote_port_, so_error);
        state_ = ConnectionState::ERROR;
        return false;
    }
}

} // namespace omnibinder
