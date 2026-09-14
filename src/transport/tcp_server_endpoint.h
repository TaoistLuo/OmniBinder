/**************************************************************************************************
 * @file        tcp_server_endpoint.h
 * @brief       TCP 服务端接入端点（TcpServerEndpoint）
 * @details     TcpServerEndpoint 实现 IServerEndpoint，创建一个监听 socket 并托管所有
 *              入站连接。端点自身不读取业务数据：
 *                - start()       绑定/监听，返回实际端口
 *                - pollFds()     返回需注册到 EventLoop 的 fd（监听 fd + 已接入连接 fd）
 *                - onPollEvent() 按 fd 语义分派：监听 fd 接入新连接（产出 TcpConnection），
 *                                连接 fd 的读事件触发 readable 回调、断开事件触发 disconnect 回调
 *                - 入站 TcpConnection 由端点持有，调用方通过 removeClient() 释放
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef OMNIBINDER_TCP_SERVER_ENDPOINT_H
#define OMNIBINDER_TCP_SERVER_ENDPOINT_H

#include "omnibinder/transport.h"
#include "platform/platform.h"

#include <string>
#include <map>
#include <vector>

namespace omnibinder {

// ============================================================
// TcpServerEndpoint — TCP 服务端接入端点（IServerEndpoint 实现）
// ============================================================

class TcpServerEndpoint : public IServerEndpoint {
public:
    TcpServerEndpoint();
    virtual ~TcpServerEndpoint();

    // 禁止拷贝
    TcpServerEndpoint(const TcpServerEndpoint&) = delete;
    TcpServerEndpoint& operator=(const TcpServerEndpoint&) = delete;

    // IServerEndpoint
    TransportType type() const override;
    int  start(const std::string& host, uint16_t port, const TransportConfig& config) override;
    void close() override;
    void pollFds(std::vector<int>& fds) const override;
    void onPollEvent(int fd, uint32_t events) override;
    void setAcceptCallback(const AcceptCallback& cb) override;
    void setReadableCallback(const ReadableCallback& cb) override;
    void setDisconnectCallback(const DisconnectCallback& cb) override;
    void removeClient(int client_id) override;

private:
    platform::SocketFd listen_fd_;
    uint16_t    listen_port_;
    std::string listen_host_;

    AcceptCallback     accept_cb_;
    ReadableCallback   readable_cb_;
    DisconnectCallback disconnect_cb_;
    std::map<int, IMessageConnection*> clients_;
};

} // namespace omnibinder

#endif // OMNIBINDER_TCP_SERVER_ENDPOINT_H
