/**************************************************************************************************
 * @file        shm_client_connection.h
 * @brief       共享内存客户端传输实现
 * @details     基于 per-client SHM 架构的 IMessageConnection 实现：
 *              客户端创建自己的 SHM，通过握手通道把 SHM 名称发给服务端，
 *              并接收 [响应通知句柄, 服务端主控通知句柄]。
 *              send() 写入请求 ring 后通知服务端主控 eventfd；
 *              recv() 从响应 ring 读取服务端响应。
 *              具体 ring 布局与读写见 shm_ring.h。
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
#ifndef OMNIBINDER_SHM_SERVER_CONNECTION_H
#define OMNIBINDER_SHM_SERVER_CONNECTION_H

#include "omnibinder/transport.h"
#include "transport/shm_ring.h"
#include "platform/platform.h"

#include <string>
#include <stdint.h>

namespace omnibinder {

// ============================================================
// ShmServerConnection — 服务端持有的单个客户端连接
// ============================================================

class ShmServerConnection : public IMessageConnection {
public:
    ShmServerConnection(int client_id, const std::string& shm_name,
                              void* shm_addr, size_t shm_size, ShmControlBlock* ctrl,
                              uint32_t req_ring_capacity, uint32_t resp_ring_capacity,
                              int resp_eventfd, int request_notify_fd,
                              platform::handshake_channel* liveness_channel)
        : client_id_(client_id)
        , shm_name_(shm_name)
        , shm_addr_(shm_addr)
        , shm_size_(shm_size)
        , ctrl_(ctrl)
        , req_ring_capacity_(req_ring_capacity)
        , resp_ring_capacity_(resp_ring_capacity)
        , resp_eventfd_(resp_eventfd)
        , request_notify_fd_(request_notify_fd)
        , liveness_channel_(liveness_channel)
        , state_(ConnectionState::CONNECTED)
    {
    }

    virtual ~ShmServerConnection()
    {
        cleanup();
    }

    // 服务端不主动发起连接
    virtual int connect(const std::string&, uint16_t) { return -1; }

    virtual int send(const uint8_t* data, size_t length);
    virtual int sendAll(const uint8_t* data, size_t length,
                        uint32_t timeout_ms, uint32_t* elapsed_ms);
    virtual int recv(uint8_t* buf, size_t buf_size);
    int peekFrameSize(size_t& out_length) override;

    // 端点 onPollEvent 已消费 master eventfd，故连接级无需再消费
    virtual void consumeReadiness() {}
    virtual bool isFramed() const { return true; }

    // 最小化 close：真正释放由 ShmServerEndpoint::removeClient 触发（析构 cleanup）
    virtual void close() { state_ = ConnectionState::DISCONNECTED; }
    virtual ConnectionState state() const { return state_; }

    // 返回 liveness channel fd（死亡检测）
    virtual int fd() const { return livenessFd(); }
    virtual TransportType type() const { return TransportType::SHM; }

    int clientId() const { return client_id_; }
    int requestNotifyFd() const { return request_notify_fd_; }

    int livenessFd() const
    {
        return liveness_channel_ ? platform::handshakeGetFd(liveness_channel_) : -1;
    }

    // 释放 SHM 映射（服务端负责 unlink）/ eventfd / liveness channel，幂等
    void cleanup();

private:
    ShmRingHeader* requestRing() const;
    uint8_t*       requestData() const;
    ShmRingHeader* responseRing() const;
    uint8_t*       responseData() const;

    int             client_id_;
    std::string     shm_name_;
    void*           shm_addr_;
    size_t          shm_size_;
    ShmControlBlock* ctrl_;
    uint32_t        req_ring_capacity_;
    uint32_t        resp_ring_capacity_;
    int             resp_eventfd_;
    int             request_notify_fd_;
    platform::handshake_channel* liveness_channel_;
    ConnectionState state_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SHM_SERVER_CONNECTION_H
