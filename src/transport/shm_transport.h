/**************************************************************************************************
 * @file        shm_transport.h
 * @brief       共享内存传输层实现
 * @details     基于 POSIX 共享内存的高性能同机传输实现。采用 per-client SHM 架构：
 *              每个客户端创建自己的 SHM，通过 UDS 将 SHM 名称 + eventfd 传递给服务端。
 *              服务端打开客户端 SHM，实现双向通信。无需共享请求队列、自旋锁、固定客户端上限。
 *              使用 eventfd 实现跨进程事件通知（通过 UDS SCM_RIGHTS 交换 fd），
 *              完全融入 EventLoop 的 epoll 事件驱动模型，无需轮询。
 *
 * @author      taoist.luo
 * @version     2.0.0
 * @date        2025-06-20
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
#ifndef OMNIBINDER_SHM_TRANSPORT_H
#define OMNIBINDER_SHM_TRANSPORT_H

#include "omnibinder/transport.h"
#include "platform/platform.h"
#include <string>
#include <stdint.h>
#include <vector>
#include <map>
#include <functional>
#include <atomic>

namespace omnibinder {

// ============================================================
// Per-client 共享内存布局
// ============================================================
//
// 每个客户端创建自己的 SHM，服务端通过 UDS 获知 SHM 名称后打开。
//
// 内存布局 (单客户端):
//   [ShmControlBlock]
//   [Request Ring header + data]   -- 客户端写入请求，服务端读取
//   [Response Ring header + data]  -- 服务端写入响应，客户端读取
//
// UDS 握手流程:
//   客户端 → 服务端: [shm_name as data, no fd]
//   服务端 → 客户端: [resp_eventfd, master_eventfd via SCM_RIGHTS] (2 fds)
//   客户端 send() 时 notify master_eventfd → 唤醒服务端 epoll
//   服务端 serverSend() 时 notify resp_eventfd → 唤醒客户端 epoll

struct ShmRingHeader {
    std::atomic<uint32_t> write_pos;
    std::atomic<uint32_t> read_pos;
    uint32_t capacity;
    uint32_t reserved;
};

#pragma pack(push, 1)

struct ShmControlBlock {
    uint32_t magic;                  // SHM_MAGIC
    uint32_t version;                // 协议版本
    uint32_t req_ring_capacity;      // 请求 ring 容量
    uint32_t resp_ring_capacity;     // 响应 ring 容量
    volatile uint32_t ready_flag;    // 客户端初始化完成标志
    uint32_t reserved[7];            // 对齐填充 (含 SHM_FEATURE_EVENTFD 在 reserved[0])
};

#pragma pack(pop)

const uint32_t SHM_MAGIC = 0x53484D42u;       // "SHMB"
const size_t   SHM_DEFAULT_REQ_RING_CAPACITY = 4 * 1024;
const size_t   SHM_DEFAULT_RESP_RING_CAPACITY = 4 * 1024;

// Feature flags (stored in ShmControlBlock.reserved[0])
const uint32_t SHM_FEATURE_EVENTFD = 0x0001;  // 支持 eventfd 事件通知

// ============================================================
// ClientShmContext - 服务端持有的每个客户端上下文
// ============================================================
struct ClientShmContext {
    uint32_t client_id;
    std::string shm_name;
    void* shm_addr;           // 客户端 SHM 映射地址
    size_t shm_size;
    ShmControlBlock* ctrl;
    int resp_eventfd;         // 服务端响应通知 fd（创建后通过 UDS 发送给客户端）
};

// ============================================================
// ShmTransport - 共享内存传输
// ============================================================
//
// 两种角色:
//   - 服务端 (is_server=true): 创建 UDS 监听，接受客户端连接，
//     为每个客户端打开其 SHM，从请求 ring 读取请求，向响应 ring 写入响应。
//     内部维护 client_contexts_ 管理所有已连接客户端。
//   - 客户端 (is_server=false): 创建自己的 SHM，通过 UDS 将 SHM 名称
//     发送给服务端，向自己的请求 ring 写入请求，从自己的响应 ring 读取响应。
//
// ITransport 接口适配:
//   - 客户端: send() 写请求 ring, recv() 读响应 ring
//   - 服务端: 使用 serverRecv()/serverSend() 处理各客户端
//
// eventfd 事件通知:
//   - 服务端创建 master eventfd（req_eventfd_），发送给客户端
//   - 服务端为每个客户端创建 resp_eventfd，发送给客户端
//   - 客户端 send() 后 notify 服务端 master eventfd → 唤醒服务端 epoll
//   - 服务端 serverSend() 后 notify resp_eventfd → 唤醒客户端 epoll
//
class ShmTransport : public ITransport {
public:
    // shm_name: 服务名（服务端用于生成 UDS 路径；客户端用于生成自身 SHM 名）
    // is_server: true=服务端, false=客户端
    ShmTransport(const std::string& shm_name, bool is_server,
                 size_t req_ring_capacity = SHM_DEFAULT_REQ_RING_CAPACITY,
                 size_t resp_ring_capacity = SHM_DEFAULT_RESP_RING_CAPACITY);
    ~ShmTransport();

    // ITransport 接口（客户端侧使用）
    int connect(const std::string& host, uint16_t port) override;
    int send(const uint8_t* data, size_t length) override;
    int recv(uint8_t* buf, size_t buf_size) override;
    void close() override;
    ConnectionState state() const override;
    int fd() const override;
    TransportType type() const override;

    // 获取共享内存名称
    const std::string& shmName() const { return shm_name_; }

    // 获取客户端 ID（客户端侧有效）
    uint32_t clientId() const { return client_id_; }

    // 服务端：从各客户端的请求 ring 读取一条请求
    // buf: 输出缓冲区
    // buf_size: 缓冲区大小
    // out_client_id: 输出请求来源的 client_id
    // 返回值: >0 = 读取的字节数, 0 = 无数据, <0 = 错误
    int serverRecv(uint8_t* buf, size_t buf_size, uint32_t& out_client_id);

    // 服务端：向指定客户端的响应 ring 写入响应
    // client_id: 目标客户端 ID
    // data: 响应数据
    // length: 数据长度
    // 返回值: >0 = 写入的字节数, 0 = ring 满, <0 = 错误
    int serverSend(uint32_t client_id, const uint8_t* data, size_t length);

    // 等待 SHM 就绪（客户端调用，UDS 握手完成后即就绪）
    bool waitReady(uint32_t timeout_ms);

    // 获取当前连接的客户端数
    uint32_t clientCount() const;

    // 获取当前活跃客户端 ID 列表（调试/测试用）
    std::vector<uint32_t> activeClientIds() const;

    // 是否是服务端
    bool isServer() const { return is_server_; }

    // 服务端: 获取请求通知 eventfd（注册到 EventLoop 监听请求到达）
    int reqEventFd() const { return req_eventfd_; }

    // 服务端: 获取 UDS 监听 fd（注册到 EventLoop 接受客户端 fd 交换）
    int handshakeListenFd() const { return handshake_listen_fd_; }

    // 服务端: 处理 UDS 客户端连接（接收 SHM 名称，发送 resp + master eventfd）
    void onHandshakeClientConnect();

    // 是否启用了 eventfd 通知
    bool eventfdEnabled() const { return eventfd_enabled_; }

    // 服务端: 设置回调，当新客户端连接并创建通知 fd 时调用
    // (Windows 上为 per-client pipe，需注册到 EventLoop)
    void setOnNewClientNotifyFd(const std::function<void(int)>& cb) {
        on_new_client_fd_cb_ = cb;
    }

private:
    ShmTransport(const ShmTransport&);
    ShmTransport& operator=(const ShmTransport&);

    bool initServer();   // 服务端：创建 UDS 监听
    bool initClient();   // 客户端：创建 SHM + UDS 握手

    void cleanup();
    void cleanupClientContext(ClientShmContext& ctx);

    // 服务端：直接向已查找到的上下文发送
    int serverSendToContext(ClientShmContext& ctx, uint32_t client_id,
                            const uint8_t* data, size_t length);

    // 环形缓冲区操作
    uint32_t ringAvailableRead(const ShmRingHeader* ring) const;
    uint32_t ringAvailableWrite(const ShmRingHeader* ring) const;
    uint32_t ringWrite(ShmRingHeader* ring, uint8_t* ring_data,
                       const uint8_t* data, uint32_t length);
    uint32_t ringRead(ShmRingHeader* ring, const uint8_t* ring_data,
                      uint8_t* buf, uint32_t length);

    // 从 SHM 基址获取各 ring 指针（用于当前实例或客户端上下文）
    static ShmRingHeader* getRequestRingFromBase(uint8_t* base);
    static uint8_t*       getRequestDataFromBase(uint8_t* base);
    static ShmRingHeader* getResponseRingFromBase(uint8_t* base, ShmControlBlock* ctrl);
    static uint8_t*       getResponseDataFromBase(uint8_t* base, ShmControlBlock* ctrl);

    // 当前实例的 ring 指针（客户端侧：指向自己的 SHM）
    ShmRingHeader* getRequestRing() const;
    uint8_t*       getRequestData() const;
    ShmRingHeader* getResponseRing() const;
    uint8_t*       getResponseData() const;

    // 服务端：从 ClientShmContext 获取 ring 指针
    ShmRingHeader* getRequestRing(const ClientShmContext& ctx) const;
    uint8_t*       getRequestData(const ClientShmContext& ctx) const;
    ShmRingHeader* getResponseRing(const ClientShmContext& ctx) const;
    uint8_t*       getResponseData(const ClientShmContext& ctx) const;


    // 根据服务名生成确定性 UDS 路径
    static std::string getHandshakePath(const std::string& shm_name);

    std::string     shm_name_;
    bool            is_server_;
    ConnectionState state_;
    uint32_t        client_id_;    // 客户端的 ID
    size_t          requested_req_ring_capacity_;
    size_t          requested_resp_ring_capacity_;

    // 共享内存映射（客户端：自己的 SHM；服务端：不使用，见 client_contexts_）
    void*           shm_addr_;
    size_t          shm_size_;

    // 控制块指针（指向 shm_addr_ 内部，客户端侧有效）
    ShmControlBlock* ctrl_;

    // eventfd 通知
    int req_eventfd_;    // 服务端：主控 eventfd（epoll 注册用），通过 UDS 发送给客户端
    int event_fd_;       // 客户端：服务端响应通知 fd 副本（fd() 返回此 fd）
                          // 服务端：未使用
    int peer_notify_fd_; // 客户端：服务端主控 eventfd 副本（用于 send() 时 notify 服务端 epoll）

    // UDS
    int handshake_listen_fd_;
    std::string handshake_path_;

    // eventfd 是否启用
    bool eventfd_enabled_;

    // 服务端：每个已连接客户端的上下文
    std::map<uint32_t, ClientShmContext> client_contexts_;
    uint32_t next_client_id_;  // 单调递增的客户端 ID 分配器

    // Windows per-client notification callback
    std::function<void(int)> on_new_client_fd_cb_;
};

// 计算单客户端共享内存总大小
size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity,
                        uint32_t max_clients = 0);

// 根据服务名生成确定性 SHM 名称（客户端使用）
std::string generateShmName(const std::string& service_name);

} // namespace omnibinder

#endif // OMNIBINDER_SHM_TRANSPORT_H
