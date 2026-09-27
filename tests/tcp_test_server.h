#ifndef OMNIBINDER_TEST_TCP_TEST_SERVER_H
#define OMNIBINDER_TEST_TCP_TEST_SERVER_H

#include "omnibinder/transport.h"
#include "transport/transport_selector.h"
#include "core/event_loop.h"
#include "platform/platform.h"
#include "platform/platform_test.h"
#include <string>
#include <vector>
#include <atomic>
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
        // accept 回调在驱动线程执行，主线程经 waitAccept()/close() 读取，需原子访问
        server_->setAcceptCallback([this](int client_id, IMessageConnection* client) {
            accepted_id_.store(client_id, std::memory_order_release);
            accepted_.store(client, std::memory_order_release);
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
        for (uint32_t elapsed = 0; elapsed < timeout_ms && !accepted_.load(std::memory_order_acquire); elapsed += 5) {
            if (listen_fd_ >= 0 && platform::waitFdReadable(listen_fd_, 5)) {
                server_->onPollEvent(listen_fd_, EventLoop::EVENT_READ);
            }
        }
        return accepted_.load(std::memory_order_acquire);
    }

    IMessageConnection* accepted() const { return accepted_.load(std::memory_order_acquire); }

    void releaseAccepted() {
        if (server_) {
            const int id = accepted_id_.load(std::memory_order_acquire);
            if (id >= 0) {
                server_->removeClient(id);
            }
        }
        accepted_.store(NULL, std::memory_order_release);
        accepted_id_.store(-1, std::memory_order_release);
    }

    void close() {
        accepted_.store(NULL, std::memory_order_release);
        accepted_id_.store(-1, std::memory_order_release);
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
    std::atomic<IMessageConnection*> accepted_;
    std::atomic<int> accepted_id_;
};

} // namespace test
} // namespace omnibinder

#endif // OMNIBINDER_TEST_TCP_TEST_SERVER_H
