/**************************************************************************************************
 * @file        transport.h
 * @brief       传输层抽象
 * @details     定义传输层的抽象接口，包括客户端侧 ITransport（connect/send/recv）
 *              和服务端侧 ITransportServer（listen/accept）。所有 I/O 均为非阻塞设计，
 *              配合 EventLoop 使用。具体实现包括 TcpTransport 和 ShmTransport。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-02-11
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
#ifndef OMNIBINDER_TRANSPORT_H
#define OMNIBINDER_TRANSPORT_H

#include <string>
#include <stdint.h>
#include <stddef.h>

namespace omnibinder {

// ============================================================
// 传输类型与连接状态
// ============================================================

enum class TransportType {
    TCP,
    SHM,
};

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR,
};

// ============================================================
// 前置声明
// ============================================================
class ITransport;

// ============================================================
// ITransportServer — 服务端传输接口
//
// 创建监听端点，接受入站连接。
// 配合 EventLoop 非阻塞使用：将 fd() 注册为读事件，
// fd 可读时调用 accept()。
// ============================================================

class ITransportServer {
public:
    virtual ~ITransportServer() {}

    /*
     * @brief  在指定地址上开始监听
     * @param[in]  host 监听地址
     * @param[in]  port 监听端口（0 表示由 OS 分配临时端口）
     * @return 成功返回实际端口号，失败返回 -1
     */
    virtual int listen(const std::string& host, uint16_t port) = 0;

    /*
     * @brief  停止监听并释放 socket
     */
    virtual void close() = 0;

    /*
     * @brief  返回当前监听端口
     * @return 监听端口号，未监听时返回 0
     */
    virtual uint16_t port() const = 0;

    /*
     * @brief  返回底层文件描述符
     * @return fd，未监听时返回 -1
     * @note   供 EventLoop 注册用
     */
    virtual int fd() const = 0;

    /*
     * @brief  接受一个待处理的入站连接
     * @return 成功返回 CONNECTED 状态的 ITransport 实例（调用方拥有所有权），
     *         无待处理连接（EAGAIN）或出错时返回 NULL
     */
    virtual ITransport* accept() = 0;
};

// ============================================================
// ITransport — 客户端传输接口
//
// 表示一条双向字节流连接。
// 所有 I/O 为非阻塞模式，配合 EventLoop 使用：
// 按需将 fd() 注册为读写事件。
// ============================================================

class ITransport {
public:
    virtual ~ITransport() {}

    /*
     * @brief  发起非阻塞连接
     * @param[in]  host 目标主机
     * @param[in]  port 目标端口
     * @return 0 立即成功，1 连接进行中（状态变为 CONNECTING），-1 失败（状态变为 ERROR）
     */
    virtual int connect(const std::string& host, uint16_t port) = 0;

    /*
     * @brief  发送数据
     * @param[in]  data   数据缓冲区
     * @param[in]  length 数据长度
     * @return 实际发送字节数（可能少于 length），-1 表示错误
     * @note   内部处理部分写，循环直到全部发送或 socket 将阻塞
     */
    virtual int send(const uint8_t* data, size_t length) = 0;

    /*
     * @brief  接收数据（非阻塞）
     * @param[out] buf      接收缓冲区
     * @param[in]  buf_size 缓冲区大小
     * @return 读取字节数，0 无数据（将阻塞），-1 错误（含对端断开）
     */
    virtual int recv(uint8_t* buf, size_t buf_size) = 0;

    /*
     * @brief  关闭连接并释放 socket
     */
    virtual void close() = 0;

    /*
     * @brief  返回当前连接状态
     * @return 连接状态枚举值
     */
    virtual ConnectionState state() const = 0;

    /*
     * @brief  返回底层文件描述符
     * @return fd，未连接时返回 -1
     * @note   供 EventLoop 注册用
     */
    virtual int fd() const = 0;

    /*
     * @brief  返回传输类型
     * @return TCP 或 SHM
     */
    virtual TransportType type() const = 0;
};

} // namespace omnibinder

#endif // OMNIBINDER_TRANSPORT_H
