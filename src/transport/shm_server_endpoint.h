/**************************************************************************************************
 * @file        shm_server_endpoint.h
 * @brief       共享内存服务端传输实现
 * @details     基于 per-client SHM 架构的 IServerEndpoint 实现：
 *              服务端创建握手监听 + 主控 eventfd，接受客户端握手后打开客户端 SHM，
 *              为每个客户端产出私有 IMessageConnection（仅在本文件内定义/使用），
 *              并统一上报接入/可读/断开事件。
 *
 *              客户端 ID 命名空间：SHM 客户端 ID 从 SHM_CLIENT_ID_BASE 起
 *              进程内单调递增，与 TCP 传输使用的小整数 fd 号隔离，保证
 *              core 的全局 client_id 映射在 TCP/SHM 端点并存时不冲突。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-09-12
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
#ifndef OMNIBINDER_SHM_SERVER_ENDPOINT_H
#define OMNIBINDER_SHM_SERVER_ENDPOINT_H

#include "omnibinder/transport.h"
#include "transport/shm_ring.h"
#include "platform/platform.h"

#include <string>
#include <stdint.h>
#include <map>
#include <vector>

namespace omnibinder {

// SHM 客户端 ID 命名空间起点：远离进程 fd 号，避免与 TCP 端点 ID 冲突
const int SHM_CLIENT_ID_BASE = 0x40000000;

// ============================================================
// ShmServerConnection — 服务端持有的单个客户端连接（私有）
//
// 具体定义位于 shm_server_transport.cpp，不对外暴露。
// 它把该客户端的请求/响应 ring 适配为 IMessageConnection：
//   - recv/peekFrameSize 读请求 ring（客户端→服务端）
//   - send              写响应 ring 并通知客户端 resp eventfd
//   - fd()              返回 liveness channel fd（用于死亡检测）
// ============================================================
class ShmServerConnection;

// ============================================================
// ShmServerEndpoint — 共享内存服务端端点
// ============================================================
class ShmServerEndpoint : public IServerEndpoint {
public:
    /*
     * @brief  创建 SHM 服务端端点（未启动）
     * @param[in]  service_name       服务名（派生握手通道路径）
     * @param[in]  req_ring_capacity  请求 ring 默认容量
     * @param[in]  resp_ring_capacity 响应 ring 默认容量
     */
    explicit ShmServerEndpoint(const std::string& service_name,
                                size_t req_ring_capacity = SHM_DEFAULT_REQ_RING_CAPACITY,
                                size_t resp_ring_capacity = SHM_DEFAULT_RESP_RING_CAPACITY);
    virtual ~ShmServerEndpoint();

    // 禁止拷贝
    ShmServerEndpoint(const ShmServerEndpoint&) = delete;
    ShmServerEndpoint& operator=(const ShmServerEndpoint&) = delete;

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

    // 测试/诊断辅助（非 IServerEndpoint 契约）
    size_t clientCount() const { return clients_.size(); }

    /*
     * @brief  握手监听 fd（注册/等待握手用）
     * @return fd，未启动时返回 -1
     */
    int handshakeListenFd() const;

    /*
     * @brief  主控请求 eventfd（客户端写入请求时唤醒服务端扫描）
     * @return fd，未启动时返回 -1
     */
    int requestEventFd() const { return master_eventfd_; }

private:
    ShmServerConnection* createClientFromHandshake(platform::handshake_channel* ch);
    void acceptHandshakeClients();
    void scanClientsForReadable();
    static int allocClientId();

    std::string     service_name_;
    size_t          req_ring_capacity_;
    size_t          resp_ring_capacity_;

    platform::handshake_listener* handshake_listener_;
    std::string                   handshake_path_;
    int                           master_eventfd_;

    std::map<int, ShmServerConnection*> clients_;

    AcceptCallback     accept_cb_;
    ReadableCallback   readable_cb_;
    DisconnectCallback disconnect_cb_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SHM_SERVER_ENDPOINT_H
