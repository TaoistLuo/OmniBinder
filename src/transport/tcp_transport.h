/**************************************************************************************************
 * @file        tcp_transport.h
 * @brief       TCP 传输层实现
 * @details     ITransport 和 ITransportServer 的 TCP 实现。TcpTransport 封装非阻塞
 *              TCP Socket，支持主动连接和从 accept 创建两种方式；TcpTransportServer
 *              封装监听 Socket，接受入站连接并返回 TcpTransport 实例。
 *              配合 EventLoop 实现完全非阻塞的网络 I/O。
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
#ifndef OMNIBINDER_TCP_TRANSPORT_H
#define OMNIBINDER_TCP_TRANSPORT_H

#include "omnibinder/transport.h"
#include "platform/platform.h"

#include <string>

namespace omnibinder {

// ============================================================
// TcpTransport — TCP 客户端传输实现
//
// 封装非阻塞 TCP socket。两种构造方式：
//   1. 默认构造 + connect() 发起主动连接
//   2. 传入已连接的 fd（来自 accept）
// ============================================================

class TcpTransport : public ITransport {
public:
    /*
     * @brief  创建未连接状态的传输，后续调用 connect() 建立连接
     */
    TcpTransport();

    /*
     * @brief  从已建立的 socket 创建传输（来自 accept）
     * @param[in]  connected_fd 已连接的 socket 描述符
     */
    explicit TcpTransport(platform::SocketFd connected_fd);

    virtual ~TcpTransport();

    // 禁止拷贝
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // ITransport 接口
    virtual int connect(const std::string& host, uint16_t port);
    virtual int send(const uint8_t* data, size_t length);
    virtual int recv(uint8_t* buf, size_t buf_size);
    virtual void close();
    virtual ConnectionState state() const;
    virtual int fd() const;
    virtual TransportType type() const;

    /*
     * @brief  异步 connect 完成后检查连接状态
     * @return true 连接成功，false 错误
     * @note   当 socket 在 CONNECTING 状态下变为可写时调用
     */
    bool checkConnectComplete();

private:
    platform::SocketFd fd_;
    ConnectionState  state_;
    std::string      remote_host_;
    uint16_t         remote_port_;
};

// ============================================================
// TcpTransportServer — TCP 服务端传输实现
//
// 创建监听 socket，接受入站连接。
// 每个接受的连接返回一个新的 TcpTransport 实例。
// ============================================================

class TcpTransportServer : public ITransportServer {
public:
    TcpTransportServer();
    virtual ~TcpTransportServer();

    // 禁止拷贝
    TcpTransportServer(const TcpTransportServer&) = delete;
    TcpTransportServer& operator=(const TcpTransportServer&) = delete;

    // ITransportServer 接口
    virtual int listen(const std::string& host, uint16_t port);
    virtual void close();
    virtual uint16_t port() const;
    virtual int fd() const;
    virtual ITransport* accept();

private:
    platform::SocketFd listen_fd_;
    uint16_t    listen_port_;
    std::string listen_host_;
};

} // namespace omnibinder

#endif // OMNIBINDER_TCP_TRANSPORT_H
