#ifndef OMNIBINDER_TEST_TCP_TEST_SERVER_H
#define OMNIBINDER_TEST_TCP_TEST_SERVER_H

#include "omnibinder/transport.h"
#include "transport/transport_selector.h"
#include "core/event_loop.h"
#include "platform/platform.h"
#include <string>
#include <vector>
#include <stdint.h>

namespace omnibinder {
namespace test {

// 用 IServerEndpoint 契约驱动的最小 TCP 服务端测试夹具：start() 绑定端口，
// waitAccept() 轮询监听 fd 并分派 onPollEvent() 触发接入回调。接入的
// IMessageConnection 由端点持有，用 releaseAccepted()/close() 释放。
class TcpTestServer {
public:
    TcpTestServer()
        : server_(NULL), listen_fd_(-1), port_(0), accepted_(NULL), accepted_id_(-1) {}

    ~TcpTestServer() {
        close();
    }

    bool start(const std::string& host = "127.0.0.1", uint16_t port = 0) {
        server_ = createServerEndpoint("test_tcp_server", TransportType::TCP, TransportConfig());
        if (!server_) {
            return false;
        }
        server_->setAcceptCallback([this](int client_id, IMessageConnection* client) {
            accepted_id_ = client_id;
            accepted_ = client;
        });
        int ret = server_->start(host, port, TransportConfig());
        if (ret < 0) {
            delete server_;
            server_ = NULL;
            return false;
        }
        port_ = static_cast<uint16_t>(ret);

        std::vector<int> fds;
        server_->pollFds(fds);
        if (fds.empty()) {
            close();
            return false;
        }
        listen_fd_ = fds[0];
        return true;
    }

    uint16_t port() const { return port_; }

    IMessageConnection* waitAccept(uint32_t timeout_ms = 5000) {
        for (uint32_t elapsed = 0; elapsed < timeout_ms && !accepted_; elapsed += 5) {
            if (listen_fd_ >= 0 && platform::waitFdReadable(listen_fd_, 5)) {
                server_->onPollEvent(listen_fd_, EventLoop::EVENT_READ);
            }
        }
        return accepted_;
    }

    IMessageConnection* accepted() const { return accepted_; }

    void releaseAccepted() {
        if (server_ && accepted_id_ >= 0) {
            server_->removeClient(accepted_id_);
        }
        accepted_ = NULL;
        accepted_id_ = -1;
    }

    void close() {
        accepted_ = NULL;
        accepted_id_ = -1;
        if (server_) {
            server_->close();
            delete server_;
            server_ = NULL;
        }
        listen_fd_ = -1;
    }

private:
    TcpTestServer(const TcpTestServer&);
    TcpTestServer& operator=(const TcpTestServer&);

    IServerEndpoint* server_;
    int listen_fd_;
    uint16_t port_;
    IMessageConnection* accepted_;
    int accepted_id_;
};

} // namespace test
} // namespace omnibinder

#endif // OMNIBINDER_TEST_TCP_TEST_SERVER_H
