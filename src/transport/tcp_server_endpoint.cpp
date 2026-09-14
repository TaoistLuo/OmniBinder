#include "transport/tcp_server_endpoint.h"
#include "transport/tcp_connection.h"
#include "platform/event_backend.h"
#include "omnibinder/log.h"

#define LOG_TAG "TcpServerEndpoint"

namespace omnibinder {

// ============================================================
// TcpServerEndpoint 实现
// ============================================================

TcpServerEndpoint::TcpServerEndpoint()
    : listen_fd_(platform::INVALID_SOCKET_FD)
    , listen_port_(0)
{
}

TcpServerEndpoint::~TcpServerEndpoint()
{
    close();
}

int TcpServerEndpoint::start(const std::string& host, uint16_t port, const TransportConfig& config)
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

void TcpServerEndpoint::close()
{
    if (listen_fd_ != platform::INVALID_SOCKET_FD) {
        OMNI_LOG_INFO(LOG_TAG, "Closing listen socket fd=%d (%s:%u)",
                        static_cast<int>(listen_fd_),
                        listen_host_.c_str(), listen_port_);
        platform::closeSocket(listen_fd_);
        listen_fd_ = platform::INVALID_SOCKET_FD;
    }
    listen_port_ = 0;
    for (std::map<int, IMessageConnection*>::iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        it->second->close();
        delete it->second;
    }
    clients_.clear();
}

TransportType TcpServerEndpoint::type() const
{
    return TransportType::TCP;
}

void TcpServerEndpoint::pollFds(std::vector<int>& fds) const
{
    if (listen_fd_ != platform::INVALID_SOCKET_FD) {
        fds.push_back(static_cast<int>(listen_fd_));
    }
    for (std::map<int, IMessageConnection*>::const_iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        fds.push_back(it->first);
    }
}

void TcpServerEndpoint::onPollEvent(int fd, uint32_t events)
{
    if (fd == static_cast<int>(listen_fd_)) {
        // 先完成本轮所有接入再逐个回调：回调链可能注销服务并删除本 transport，
        // 回调仅使用本地拷贝的 id/指针，之后不再访问成员
        std::vector<std::pair<int, IMessageConnection*> > accepted;
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

            IMessageConnection* client = new TcpConnection(client_fd);
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

    std::map<int, IMessageConnection*>::iterator it = clients_.find(fd);
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

void TcpServerEndpoint::setAcceptCallback(const AcceptCallback& cb)
{
    accept_cb_ = cb;
}

void TcpServerEndpoint::setReadableCallback(const ReadableCallback& cb)
{
    readable_cb_ = cb;
}

void TcpServerEndpoint::setDisconnectCallback(const DisconnectCallback& cb)
{
    disconnect_cb_ = cb;
}

void TcpServerEndpoint::removeClient(int client_id)
{
    std::map<int, IMessageConnection*>::iterator it = clients_.find(client_id);
    if (it == clients_.end()) {
        return;
    }
    IMessageConnection* client = it->second;
    clients_.erase(it);
    if (client) {
        client->close();
        delete client;
    }
}

} // namespace omnibinder
