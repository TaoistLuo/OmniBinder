/**************************************************************************************************
 * @file        transport.h
 * @brief       传输层抽象
 * @details     定义客户端侧传输抽象接口 IClientTransport（connect/send/sendAll/recv/
 *              consumeReadiness/isFramed）。基础 I/O 为非阻塞设计，配合 EventLoop 使用；
 *              sendAll 提供限时全量发送语义，供 core 在有超时预算的发送路径使用。
 *              成帧能力（isFramed）让 core 无需按 TransportType 分支即可选择正确的
 *              读帧策略。具体实现包括 TcpClientTransport 和 ShmClientTransport。
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
#include <vector>
#include <functional>
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

/* @brief 端点容量配置
 * @details 与具体传输无关；非 SHM 传输可忽略 */
struct TransportConfig {
    size_t req_capacity;
    size_t resp_capacity;

    TransportConfig() : req_capacity(0), resp_capacity(0) {}
    TransportConfig(size_t req, size_t resp) : req_capacity(req), resp_capacity(resp) {}
};

// ============================================================
// 前置声明
// ============================================================
class IClientTransport;
class IServerTransport;

// ============================================================
// IClientTransport — 客户端传输接口
//
// 表示一条双向消息连接，可能是字节流（TCP）或成帧（SHM）。
// 基础 I/O（send/recv）为非阻塞模式，配合 EventLoop 使用：
// 按需将 fd() 注册为读写事件；sendAll 在超时预算内等待可写/空间。
// ============================================================

class IClientTransport {
public:
    virtual ~IClientTransport() {}

    /* @brief 发起非阻塞连接
     * @param[in]  host 目标主机
     * @param[in]  port 目标端口
     * @return 0 立即成功，1 连接进行中（状态变为 CONNECTING），-1 失败（状态变为 ERROR） */
    virtual int connect(const std::string& host, uint16_t port) = 0;

    /* @brief 发送数据（尽力而为，非阻塞）
     * @param[in]  data   数据缓冲区
     * @param[in]  length 数据长度
     * @return 实际发送字节数（可能少于 length），-1 表示错误
     * @note   内部处理部分写，循环直到全部发送或 socket 将阻塞。
     *         广播等可容忍丢帧的路径使用本方法；需要完整送达的路径用 sendAll。 */
    virtual int send(const uint8_t* data, size_t length) = 0;

    /* @brief 限时全量发送
     * @param[in]  data       数据缓冲区
     * @param[in]  length     数据长度
     * @param[in]  timeout_ms 发送超时预算（0 表示只尝试一次，不等待）
     * @param[out] elapsed_ms 实际耗时（可为 NULL）
     * @return 0 全部发送成功；<0 超时/错误（未保证全部送达）
     * @note   内部处理部分写/背压/EAGAIN：TCP 等待可写，SHM 等待 ring 空间，
     *         直到完整写入或超过 timeout_ms。取代 core 直接调用 platform 发送原语。 */
    virtual int sendAll(const uint8_t* data, size_t length,
                        uint32_t timeout_ms, uint32_t* elapsed_ms) = 0;

    /* @brief 接收数据（非阻塞）
     * @param[out] buf      接收缓冲区
     * @param[in]  buf_size 缓冲区大小
     * @return 读取字节数，0 无数据（将阻塞），-1 错误（含对端断开） */
    virtual int recv(uint8_t* buf, size_t buf_size) = 0;

    /* @brief 消费底层就绪通知
     * @note   成帧传输（SHM）消费其通知 eventfd；TCP 为空操作。
     *         取代 core 直接调用 platform::eventFdConsume。 */
    virtual void consumeReadiness() = 0;

    /* @brief 是否为成帧传输
     * @return true  recv 返回完整一帧 Message 字节流（SHM）
     *         false 字节流，需按流式方式组帧（TCP）
     * @note   取代 core 中按 TransportType 分叉的读帧行为选择 */
    virtual bool isFramed() const = 0;

    /* @brief 探测下一完整帧的长度（仅成帧传输有意义，如 SHM）
     * @param[out] out_length 下一帧字节数
     * @return 1 有成帧待读（out_length 有效），0 暂无可读帧（含 TCP 流式），-1 元数据损坏
     * @note   core 据此分配接收缓冲，避免对具体传输类型做 downcast */
    virtual int peekFrameSize(size_t& out_length) = 0;

    /* @brief 关闭连接并释放 socket */
    virtual void close() = 0;

    /* @brief 返回当前连接状态
     * @return 连接状态枚举值 */
    virtual ConnectionState state() const = 0;

    /* @brief 返回底层文件描述符
     * @return fd，未连接时返回 -1
     * @note   供 EventLoop 注册用 */
    virtual int fd() const = 0;

    /* @brief 返回传输类型
     * @return TCP 或 SHM */
    virtual TransportType type() const = 0;
};

// ============================================================
// IServerTransport — 服务端托管端点接口
//
// 与传输种类无关：TCP 监听/accept 与 SHM 握手/ring 都实现同一接口。
// 端点只负责"接入、事件、生命周期"，并把每个已接入的客户端产出为
// 一条 IClientTransport 交给 core；core 只用 IClientTransport 读写数据。
// ============================================================

class IServerTransport {
public:
    typedef std::function<void(int client_id, IClientTransport* client)> AcceptCallback;
    typedef std::function<void(int client_id)> ReadableCallback;
    typedef std::function<void(int client_id)> DisconnectCallback;

    virtual ~IServerTransport() {}

    virtual TransportType type() const = 0;

    /* @brief 启动端点
     * @param[in]  host       监听/绑定地址
     * @param[in]  port       监听端口（0 由系统分配）
     * @param[in]  config     端点容量配置（非 SHM 传输可忽略）
     * @return 实际端口，失败返回 -1 */
    virtual int  start(const std::string& host, uint16_t port,
                       const TransportConfig& config) = 0;

    /* @brief 关闭端点并释放资源 */
    virtual void close() = 0;

    /* @brief 返回需要 core 注册到 EventLoop 的端点级 fd（监听/事件聚合） */
    virtual void pollFds(std::vector<int>& fds) const = 0;

    /* @brief 处理一次端点级 fd 事件（accept / 握手 / 可读映射）
     * @note   端点内部据 fd 语义完成工作，并通过回调上报接入/可读/断开 */
    virtual void onPollEvent(int fd, uint32_t events) = 0;

    virtual void setAcceptCallback(const AcceptCallback& cb) = 0;
    virtual void setReadableCallback(const ReadableCallback& cb) = 0;
    virtual void setDisconnectCallback(const DisconnectCallback& cb) = 0;

    /* @brief 移除一个客户端并回收其资源 */
    virtual void removeClient(int client_id) = 0;
};

} // namespace omnibinder

#endif // OMNIBINDER_TRANSPORT_H
