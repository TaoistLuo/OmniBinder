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
// TcpTransport - TCP client transport implementation
//
// Wraps a TCP socket for non-blocking I/O. Can be created in
// two ways:
//   1. Default constructor + connect() for outgoing connections
//   2. Constructor with existing fd for accepted connections
// ============================================================

class TcpTransport : public ITransport {
public:
    // Create a disconnected transport (use connect() to establish connection)
    TcpTransport();

    // Create a transport from an already-connected socket (from accept)
    explicit TcpTransport(SocketFd connected_fd);

    virtual ~TcpTransport();

    // Disable copy
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // ITransport interface
    virtual int connect(const std::string& host, uint16_t port);
    virtual int send(const uint8_t* data, size_t length);
    virtual int recv(uint8_t* buf, size_t buf_size);
    virtual void close();
    virtual ConnectionState state() const;
    virtual int fd() const;
    virtual TransportType type() const;

    // Check and update connection state after async connect completes
    // Call this when the socket becomes writable during CONNECTING state
    // Returns true if connected successfully, false on error
    bool checkConnectComplete();

private:
    SocketFd         fd_;
    ConnectionState  state_;
    std::string      remote_host_;
    uint16_t         remote_port_;
};

// ============================================================
// TcpTransportServer - TCP server transport implementation
//
// Creates a listening socket and accepts incoming connections.
// Each accepted connection returns a new TcpTransport instance.
// ============================================================

class TcpTransportServer : public ITransportServer {
public:
    TcpTransportServer();
    virtual ~TcpTransportServer();

    // Disable copy
    TcpTransportServer(const TcpTransportServer&) = delete;
    TcpTransportServer& operator=(const TcpTransportServer&) = delete;

    // ITransportServer interface
    virtual int listen(const std::string& host, uint16_t port);
    virtual void close();
    virtual uint16_t port() const;
    virtual int fd() const;
    virtual ITransport* accept();

private:
    SocketFd    listen_fd_;
    uint16_t    listen_port_;
    std::string listen_host_;
};

} // namespace omnibinder

#endif // OMNIBINDER_TCP_TRANSPORT_H
