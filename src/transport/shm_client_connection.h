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
#ifndef OMNIBINDER_SHM_CLIENT_CONNECTION_H
#define OMNIBINDER_SHM_CLIENT_CONNECTION_H

#include "omnibinder/transport.h"
#include "transport/shm_ring.h"
#include "platform/platform.h"

#include <string>
#include <stdint.h>

namespace omnibinder {

// ============================================================
// ShmClientConnection — 共享内存客户端传输
//
// 生命周期：
//   connect() 创建 SHM + 握手；close()/析构 detach + unlink 并关闭通知句柄。
//   connect() 失败路径保证不残留 /dev/shm 对象。
// ============================================================
class ShmClientConnection : public IMessageConnection {
public:
    /*
     * @brief  创建未连接的 SHM 客户端传输
     * @param[in]  service_name       目标服务名（派生服务端 SHM 名与握手路径）
     * @param[in]  req_ring_capacity  请求 ring 容量，0/过小值按默认规范化
     * @param[in]  resp_ring_capacity 响应 ring 容量，0/过小值按默认规范化
     */
    explicit ShmClientConnection(const std::string& service_name,
                                size_t req_ring_capacity = SHM_DEFAULT_REQ_RING_CAPACITY,
                                size_t resp_ring_capacity = SHM_DEFAULT_RESP_RING_CAPACITY);

    virtual ~ShmClientConnection();

    // 禁止拷贝
    ShmClientConnection(const ShmClientConnection&) = delete;
    ShmClientConnection& operator=(const ShmClientConnection&) = delete;

    // 出站拨号（创建 SHM + 握手；仅客户端路径调用）
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
     * @brief  返回本客户端创建的 SHM 名称
     * @return SHM 名称（连接前为空）
     */
    const std::string& shmName() const { return shm_name_; }

private:
    bool initClient();
    void cleanup();

    ShmRingHeader* requestRing() const;
    uint8_t*       requestData() const;
    ShmRingHeader* responseRing() const;
    uint8_t*       responseData() const;

    std::string     service_name_;          // 目标服务名（握手路径依据）
    std::string     shm_name_;              // 本客户端的唯一 SHM 名
    ConnectionState state_;
    size_t          requested_req_ring_capacity_;
    size_t          requested_resp_ring_capacity_;

    void*           shm_addr_;              // 本客户端 SHM 映射
    size_t          shm_size_;
    ShmControlBlock* ctrl_;

    int             event_fd_;              // 响应通知（fd() 返回）
    int             peer_notify_fd_;        // 服务端主控通知（send 时触发）

    platform::handshake_channel* handshake_channel_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SHM_CLIENT_CONNECTION_H
