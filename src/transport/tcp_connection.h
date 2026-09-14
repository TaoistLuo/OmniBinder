/**************************************************************************************************
 * @file        tcp_connection.h
 * @brief       TCP 双向消息连接实现（TcpConnection）
 * @details     TcpConnection 实现 IMessageConnection，是一条 TCP 双向消息连接：
 *                - 出站：默认构造 + connect() 主动拨号（由 createClientConnection 使用）
 *                - 入站：传入 accept 得到的已连接 fd（由 TcpServerEndpoint 使用）
 *              TCP 连接与方向无关，所以出站/入站共用本类；方向由"谁创建、谁持有"表达。
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
#ifndef OMNIBINDER_TCP_CONNECTION_H
#define OMNIBINDER_TCP_CONNECTION_H

#include "omnibinder/transport.h"
#include "platform/platform.h"

#include <string>
#include <map>
#include <vector>

namespace omnibinder {

// ============================================================
// TcpConnection — TCP 客户端传输实现
//
// 封装非阻塞 TCP socket。两种构造方式：
//   1. 默认构造 + connect() 发起主动连接
//   2. 传入已连接的 fd（来自 accept）
// ============================================================

class TcpConnection : public IMessageConnection {
public:
    /*
     * @brief  创建未连接状态的传输，后续调用 connect() 建立连接
     */
    TcpConnection();

    /*
     * @brief  从已建立的 socket 创建传输（来自 accept）
     * @param[in]  connected_fd 已连接的 socket 描述符
     */
    explicit TcpConnection(platform::SocketFd connected_fd);

    virtual ~TcpConnection();

    // 禁止拷贝
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // 出站拨号（仅客户端路径调用；入站连接不调用）
    int connect(const std::string& host, uint16_t port);

    // IMessageConnection 接口
    virtual int send(const uint8_t* data, size_t length);
    virtual int sendAll(const uint8_t* data, size_t length,
                        uint32_t timeout_ms, uint32_t* elapsed_ms);
    virtual int recv(uint8_t* buf, size_t buf_size);
    int peekFrameSize(size_t& out_length) override;
    virtual void consumeReadiness();
    virtual bool isFramed() const;
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

} // namespace omnibinder

#endif // OMNIBINDER_TCP_CONNECTION_H
