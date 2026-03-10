#include "transport/tcp_transport.h"
#include "omnibinder/log.h"

#include <cstring>

#define LOG_TAG "TcpTransport"

namespace omnibinder {

// ============================================================
// TcpTransport implementation
// ============================================================

TcpTransport::TcpTransport()
    : fd_(INVALID_SOCKET_FD)
    , state_(ConnectionState::DISCONNECTED)
    , remote_port_(0)
{
}

TcpTransport::TcpTransport(SocketFd connected_fd)
    : fd_(connected_fd)
    , state_(ConnectionState::DISCONNECTED)
    , remote_port_(0)
{
    if (fd_ != INVALID_SOCKET_FD) {
        platform::setNonBlocking(fd_);
        platform::setTcpNoDelay(fd_);
        platform::setKeepAlive(fd_);
        state_ = ConnectionState::CONNECTED;
        OMNI_LOG_DEBUG(LOG_TAG, "Created from accepted fd=%d", static_cast<int>(fd_));
    }
}

TcpTransport::~TcpTransport()
{
    close();
}

int TcpTransport::connect(const std::string& host, uint16_t port)
{
    if (state_ == ConnectionState::CONNECTED || state_ == ConnectionState::CONNECTING) {
        OMNI_LOG_WARN(LOG_TAG, "connect() called in state %d, closing existing connection",
                        static_cast<int>(state_));
        close();
    }

    fd_ = platform::createTcpSocket();
    if (fd_ == INVALID_SOCKET_FD) {
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
        // Immediate success (rare for non-blocking, but possible for localhost)
        state_ = ConnectionState::CONNECTED;
        OMNI_LOG_INFO(LOG_TAG, "Connected to %s:%u (fd=%d)",
                        host.c_str(), port, static_cast<int>(fd_));
        return 0;
    } else if (ret == 1) {
        // Connection in progress
        state_ = ConnectionState::CONNECTING;
        OMNI_LOG_DEBUG(LOG_TAG, "Connecting to %s:%u (fd=%d, in progress)",
                         host.c_str(), port, static_cast<int>(fd_));
        return 1;
    } else {
        // Failure
        OMNI_LOG_ERROR(LOG_TAG, "Failed to connect to %s:%u (error=%d)",
                         host.c_str(), port, platform::getSocketError());
        platform::closeSocket(fd_);
        fd_ = INVALID_SOCKET_FD;
        state_ = ConnectionState::ERROR;
        return -1;
    }
}

int TcpTransport::send(const uint8_t* data, size_t length)
{
    if (state_ != ConnectionState::CONNECTED) {
        OMNI_LOG_ERROR(LOG_TAG, "send() called in state %d", static_cast<int>(state_));
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
                // Socket buffer full, return what we've sent so far
                break;
            }
            // Real error
            OMNI_LOG_ERROR(LOG_TAG, "send() failed on fd=%d: error=%d",
                             static_cast<int>(fd_), err);
            state_ = ConnectionState::ERROR;
            return -1;
        } else {
            // n == 0: no bytes sent, treat as would-block
            break;
        }
    }

    return static_cast<int>(total_sent);
}

int TcpTransport::recv(uint8_t* buf, size_t buf_size)
{
    if (state_ != ConnectionState::CONNECTED) {
        OMNI_LOG_ERROR(LOG_TAG, "recv() called in state %d", static_cast<int>(state_));
        return -1;
    }

    if (buf_size == 0) {
        return 0;
    }

    int n = platform::socketRecv(fd_, buf, buf_size);
    if (n > 0) {
        return n;
    } else if (n == 0) {
        // Peer closed the connection gracefully
        OMNI_LOG_INFO(LOG_TAG, "Peer closed connection on fd=%d", static_cast<int>(fd_));
        state_ = ConnectionState::DISCONNECTED;
        return -1;
    } else {
        // n < 0
        int err = platform::getSocketError();
        if (platform::isWouldBlock(err)) {
            // No data available right now
            return 0;
        }
        // Real error
        OMNI_LOG_ERROR(LOG_TAG, "recv() failed on fd=%d: error=%d",
                         static_cast<int>(fd_), err);
        state_ = ConnectionState::ERROR;
        return -1;
    }
}

void TcpTransport::close()
{
    if (fd_ != INVALID_SOCKET_FD) {
        OMNI_LOG_DEBUG(LOG_TAG, "Closing fd=%d", static_cast<int>(fd_));
        platform::closeSocket(fd_);
        fd_ = INVALID_SOCKET_FD;
    }
    state_ = ConnectionState::DISCONNECTED;
}

ConnectionState TcpTransport::state() const
{
    return state_;
}

int TcpTransport::fd() const
{
    return static_cast<int>(fd_);
}

TransportType TcpTransport::type() const
{
    return TransportType::TCP;
}

bool TcpTransport::checkConnectComplete()
{
    if (state_ != ConnectionState::CONNECTING) {
        return state_ == ConnectionState::CONNECTED;
    }

    // Check for socket-level error to determine if connect succeeded
    int so_error = 0;
#ifdef OMNIBINDER_LINUX
    socklen_t len = sizeof(so_error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "getsockopt SO_ERROR failed on fd=%d: %s",
                         static_cast<int>(fd_), strerror(errno));
        state_ = ConnectionState::ERROR;
        return false;
    }
#elif defined(OMNIBINDER_WINDOWS)
    int len = sizeof(so_error);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR,
               reinterpret_cast<char*>(&so_error), &len) != 0) {
        OMNI_LOG_ERROR(LOG_TAG, "getsockopt SO_ERROR failed on fd=%d",
                         static_cast<int>(fd_));
        state_ = ConnectionState::ERROR;
        return false;
    }
#else
    // Fallback: try a zero-byte send to check connection
    char dummy = 0;
    if (::send(fd_, &dummy, 0, 0) < 0) {
        so_error = errno;
    }
#endif

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

// ============================================================
// TcpTransportServer implementation
// ============================================================

TcpTransportServer::TcpTransportServer()
    : listen_fd_(INVALID_SOCKET_FD)
    , listen_port_(0)
{
}

TcpTransportServer::~TcpTransportServer()
{
    close();
}

int TcpTransportServer::listen(const std::string& host, uint16_t port)
{
    if (listen_fd_ != INVALID_SOCKET_FD) {
        OMNI_LOG_WARN(LOG_TAG, "listen() called while already listening, closing old socket");
        close();
    }

    listen_fd_ = platform::createTcpSocket();
    if (listen_fd_ == INVALID_SOCKET_FD) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create listen socket");
        return -1;
    }

    if (!platform::setReuseAddr(listen_fd_)) {
        OMNI_LOG_WARN(LOG_TAG, "Failed to set SO_REUSEADDR on listen socket");
    }

    if (!platform::setNonBlocking(listen_fd_)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to set non-blocking on listen socket");
        platform::closeSocket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_FD;
        return -1;
    }

    if (!platform::bindSocket(listen_fd_, host, port)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to bind on %s:%u", host.c_str(), port);
        platform::closeSocket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_FD;
        return -1;
    }

    if (!platform::listenSocket(listen_fd_)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to listen on %s:%u", host.c_str(), port);
        platform::closeSocket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_FD;
        return -1;
    }

    // Retrieve the actual port (important when port=0 was requested)
    listen_port_ = platform::getSocketPort(listen_fd_);
    listen_host_ = host;

    OMNI_LOG_INFO(LOG_TAG, "Listening on %s:%u (fd=%d)",
                    listen_host_.c_str(), listen_port_, static_cast<int>(listen_fd_));

    return static_cast<int>(listen_port_);
}

void TcpTransportServer::close()
{
    if (listen_fd_ != INVALID_SOCKET_FD) {
        OMNI_LOG_INFO(LOG_TAG, "Closing listen socket fd=%d (%s:%u)",
                        static_cast<int>(listen_fd_),
                        listen_host_.c_str(), listen_port_);
        platform::closeSocket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_FD;
    }
    listen_port_ = 0;
}

uint16_t TcpTransportServer::port() const
{
    return listen_port_;
}

int TcpTransportServer::fd() const
{
    return static_cast<int>(listen_fd_);
}

ITransport* TcpTransportServer::accept()
{
    if (listen_fd_ == INVALID_SOCKET_FD) {
        OMNI_LOG_ERROR(LOG_TAG, "accept() called on closed server");
        return NULL;
    }

    std::string remote_host;
    uint16_t remote_port = 0;
    SocketFd client_fd = platform::acceptSocket(listen_fd_, remote_host, remote_port);

    if (client_fd == INVALID_SOCKET_FD) {
        // EAGAIN/EWOULDBLOCK is normal for non-blocking accept, not an error
        int err = platform::getSocketError();
        if (!platform::isWouldBlock(err)) {
            OMNI_LOG_ERROR(LOG_TAG, "accept() failed: error=%d", err);
        }
        return NULL;
    }

    OMNI_LOG_INFO(LOG_TAG, "Accepted connection from %s:%u (fd=%d)",
                    remote_host.c_str(), remote_port, static_cast<int>(client_fd));

    return new TcpTransport(client_fd);
}

} // namespace omnibinder
