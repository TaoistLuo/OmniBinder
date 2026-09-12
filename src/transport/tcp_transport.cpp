#include "transport/tcp_transport.h"
#include "platform/event_backend.h"
#include "omnibinder/log.h"

#include <cstring>

#define LOG_TAG "TcpClientTransport"

namespace omnibinder {

// ============================================================
// TcpClientTransport 实现
// ============================================================

TcpClientTransport::TcpClientTransport()
    : fd_(platform::INVALID_SOCKET_FD)
    , state_(ConnectionState::DISCONNECTED)
    , remote_port_(0)
{
}

TcpClientTransport::TcpClientTransport(platform::SocketFd connected_fd)
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

TcpClientTransport::~TcpClientTransport()
{
    close();
}

int TcpClientTransport::connect(const std::string& host, uint16_t port)
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

int TcpClientTransport::send(const uint8_t* data, size_t length)
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

int TcpClientTransport::sendAll(const uint8_t* data, size_t length,
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

void TcpClientTransport::consumeReadiness()
{
    // TCP 为字节流，无底层就绪通知需要消费
}

bool TcpClientTransport::isFramed() const
{
    return false;
}

int TcpClientTransport::peekFrameSize(size_t& out_length)
{
    (void)out_length;
    return 0;
}

int TcpClientTransport::recv(uint8_t* buf, size_t buf_size)
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

void TcpClientTransport::close()
{
    if (fd_ != platform::INVALID_SOCKET_FD) {
        OMNI_LOG_DEBUG(LOG_TAG, "Closing fd=%d", static_cast<int>(fd_));
        platform::closeSocket(fd_);
        fd_ = platform::INVALID_SOCKET_FD;
    }
    state_ = ConnectionState::DISCONNECTED;
}

ConnectionState TcpClientTransport::state() const
{
    return state_;
}

int TcpClientTransport::fd() const
{
    return static_cast<int>(fd_);
}

TransportType TcpClientTransport::type() const
{
    return TransportType::TCP;
}

bool TcpClientTransport::checkConnectComplete()
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

// ============================================================
// TcpServerTransport 实现
// ============================================================

TcpServerTransport::TcpServerTransport()
    : listen_fd_(platform::INVALID_SOCKET_FD)
    , listen_port_(0)
{
}

TcpServerTransport::~TcpServerTransport()
{
    close();
}

int TcpServerTransport::start(const std::string& host, uint16_t port, const TransportConfig& config)
{
    (void)config;

    if (listen_fd_ != platform::INVALID_SOCKET_FD) {
        OMNI_LOG_WARN(LOG_TAG, "start() called while already listening, closing old socket");
        close();
    }

    listen_fd_ = platform::createTcpSocket();
    if (listen_fd_ == platform::INVALID_SOCKET_FD) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create listen socket");
        return -1;
    }

    if (!platform::setReuseAddr(listen_fd_)) {
        OMNI_LOG_WARN(LOG_TAG, "Failed to set SO_REUSEADDR on listen socket");
    }

    if (!platform::setNonBlocking(listen_fd_)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to set non-blocking on listen socket");
        platform::closeSocket(listen_fd_);
        listen_fd_ = platform::INVALID_SOCKET_FD;
        return -1;
    }

    if (!platform::bindSocket(listen_fd_, host, port)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to bind on %s:%u", host.c_str(), port);
        platform::closeSocket(listen_fd_);
        listen_fd_ = platform::INVALID_SOCKET_FD;
        return -1;
    }

    if (!platform::listenSocket(listen_fd_)) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to listen on %s:%u", host.c_str(), port);
        platform::closeSocket(listen_fd_);
        listen_fd_ = platform::INVALID_SOCKET_FD;
        return -1;
    }

    // 获取实际端口（请求 port=0 时尤为重要）
    listen_port_ = platform::getSocketPort(listen_fd_);
    listen_host_ = host;

    OMNI_LOG_INFO(LOG_TAG, "Listening on %s:%u (fd=%d)",
                    listen_host_.c_str(), listen_port_, static_cast<int>(listen_fd_));

    return static_cast<int>(listen_port_);
}

void TcpServerTransport::close()
{
    if (listen_fd_ != platform::INVALID_SOCKET_FD) {
        OMNI_LOG_INFO(LOG_TAG, "Closing listen socket fd=%d (%s:%u)",
                        static_cast<int>(listen_fd_),
                        listen_host_.c_str(), listen_port_);
        platform::closeSocket(listen_fd_);
        listen_fd_ = platform::INVALID_SOCKET_FD;
    }
    listen_port_ = 0;
    for (std::map<int, IClientTransport*>::iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        it->second->close();
        delete it->second;
    }
    clients_.clear();
}

TransportType TcpServerTransport::type() const
{
    return TransportType::TCP;
}

void TcpServerTransport::pollFds(std::vector<int>& fds) const
{
    if (listen_fd_ != platform::INVALID_SOCKET_FD) {
        fds.push_back(static_cast<int>(listen_fd_));
    }
    for (std::map<int, IClientTransport*>::const_iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        fds.push_back(it->first);
    }
}

void TcpServerTransport::onPollEvent(int fd, uint32_t events)
{
    if (fd == static_cast<int>(listen_fd_)) {
        // 先完成本轮所有接入再逐个回调：回调链可能注销服务并删除本 transport，
        // 回调仅使用本地拷贝的 id/指针，之后不再访问成员
        std::vector<std::pair<int, IClientTransport*> > accepted;
        while (true) {
            std::string remote_host;
            uint16_t remote_port = 0;
            platform::SocketFd client_fd =
                platform::acceptSocket(listen_fd_, remote_host, remote_port);
            if (client_fd == platform::INVALID_SOCKET_FD) {
                // 非阻塞 accept 下 EAGAIN/EWOULDBLOCK 属正常现象，不是错误
                int err = platform::getSocketError();
                if (!platform::isWouldBlock(err)) {
                    OMNI_LOG_ERROR(LOG_TAG, "accept failed: error=%d", err);
                }
                break;
            }

            OMNI_LOG_INFO(LOG_TAG, "Accepted connection from %s:%u (fd=%d)",
                          remote_host.c_str(), remote_port, static_cast<int>(client_fd));

            IClientTransport* client = new TcpClientTransport(client_fd);
            int client_id = client->fd();
            clients_[client_id] = client;
            accepted.push_back(std::make_pair(client_id, client));
        }

        AcceptCallback cb = accept_cb_;
        for (size_t i = 0; i < accepted.size() && cb; ++i) {
            cb(accepted[i].first, accepted[i].second);
        }
        return;
    }

    std::map<int, IClientTransport*>::iterator it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    // 读事件优先：对端 FIN 可能伴随未消费数据，先交给读路径消费完；
    // 回调可能 removeClient 删除本 client，回调返回后不得再访问成员
    if (events & platform::EVENT_READ) {
        ReadableCallback cb = readable_cb_;
        if (cb) cb(fd);
        return;
    }

    // EPOLLHUP / EPOLLERR 等断开事件：与 SHM 端点语义一致，上报 disconnect 回调，
    // 由调用方先摘除 fd/清理状态，再调用 removeClient 释放
    if (events & platform::EVENT_ERROR) {
        DisconnectCallback cb = disconnect_cb_;
        if (cb) cb(fd);
    }
}

void TcpServerTransport::setAcceptCallback(const AcceptCallback& cb)
{
    accept_cb_ = cb;
}

void TcpServerTransport::setReadableCallback(const ReadableCallback& cb)
{
    readable_cb_ = cb;
}

void TcpServerTransport::setDisconnectCallback(const DisconnectCallback& cb)
{
    disconnect_cb_ = cb;
}

void TcpServerTransport::removeClient(int client_id)
{
    std::map<int, IClientTransport*>::iterator it = clients_.find(client_id);
    if (it == clients_.end()) {
        return;
    }
    IClientTransport* client = it->second;
    clients_.erase(it);
    if (client) {
        client->close();
        delete client;
    }
}

} // namespace omnibinder
