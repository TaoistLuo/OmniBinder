/**************************************************************************************************
 * @file        shm_transport.h
 * @brief       共享内存传输层实现
 * @details     基于 POSIX 共享内存的高性能同机传输实现。采用多客户端共享单块 SHM 的
 *              架构：一个 RequestQueue（多写入者自旋锁保护）+ N 个 ResponseSlot
 *              （每客户端独占）。服务端创建 SHM，客户端通过原子递增获取 slot_id。
 *              使用 eventfd 实现跨进程事件通知（通过 UDS SCM_RIGHTS 交换 fd），
 *              完全融入 EventLoop 的 epoll 事件驱动模型，无需轮询。
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
#ifndef OMNIBINDER_SHM_TRANSPORT_H
#define OMNIBINDER_SHM_TRANSPORT_H

#include "omnibinder/transport.h"
#include "platform/platform.h"
#include <string>
#include <stdint.h>
#include <vector>

namespace omnibinder {

// ============================================================
// 多客户端共享内存布局
// ============================================================
//
// 一个服务在初始化时创建一块 SHM，所有同机客户端共享访问。
//
// 内存布局:
//   [ShmControlBlock]
//   [Global RequestQueue ring header + data]  -- 多客户端写入(自旋锁), 服务端读取
//   [ResponseArena block 0]
//   [ResponseArena block 1]
//   ...
//   [ResponseArena block MAX_CLIENTS-1]
//
// 客户端连接时扫描空闲 slot 并复用。
// 请求消息格式: [client_id(4B)] [length(4B)] [payload(NB)]
// 服务端据此将响应写入对应 response block。
//
// 客户端断开时 slot 回到空闲状态，对应 response block 重新可复用。

#pragma pack(push, 1)

struct ShmRingHeader {
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    uint32_t capacity;
    uint32_t reserved;
};

// 自旋锁（用于保护 RequestQueue 的多写入者）
struct ShmSpinLock {
    volatile uint32_t lock;

    void init() { lock = 0; }
    void acquire() {
        while (__sync_lock_test_and_set(&lock, 1)) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("pause");
#endif
        }
    }
    void release() {
        __sync_lock_release(&lock);
    }
};

// 每个客户端 slot 的状态
struct ShmSlotStatus {
    volatile uint32_t active;    // 1 = 客户端在线, 0 = 已断开/空闲
    uint32_t response_offset;    // ResponseArena 中对应响应 ring 的偏移
    uint32_t response_capacity;  // 当前 slot 的响应 ring 容量
    uint32_t reserved;
};

struct ShmControlBlock {
    uint32_t magic;                  // SHM_MAGIC
    uint32_t version;                // 协议版本
    uint32_t max_clients;            // 最大客户端数
    volatile uint32_t active_clients; // 当前活跃客户端数
    uint32_t req_ring_capacity;      // RequestQueue 容量
    uint32_t resp_ring_capacity;     // 每个响应 ring 容量
    volatile uint32_t ready_flag;    // 服务端初始化完成标志
    volatile uint32_t response_bitmap;// ResponseArena 使用位图（最多 32 个块）
    ShmSpinLock req_lock;            // RequestQueue 写入锁
    ShmSlotStatus slots[32];         // 每个 slot 的状态 (SHM_MAX_CLIENTS)
    uint32_t reserved[3];
};

#pragma pack(pop)

const uint32_t SHM_MAGIC = 0x53484D42u;       // "SHMB"
const uint32_t SHM_MAX_CLIENTS = 32;
const size_t   SHM_DEFAULT_REQ_RING_CAPACITY = 4 * 1024;
const size_t   SHM_DEFAULT_RESP_RING_CAPACITY = 4 * 1024;

// Feature flags (stored in ShmControlBlock.reserved[0])
const uint32_t SHM_FEATURE_EVENTFD = 0x0001;  // 支持 eventfd 事件通知

// ============================================================
// ShmTransport - 共享内存传输
// ============================================================
//
// 两种角色:
//   - 服务端 (is_server=true): 创建 SHM，从 RequestQueue 读取请求，
//     向各客户端的 ResponseSlot 写入响应。
//   - 客户端 (is_server=false): 打开已有 SHM，获取 slot_id，
//     向 RequestQueue 写入请求（加锁），从自己的 ResponseSlot 读取响应。
//
// ITransport 接口适配:
//   - 客户端: send() 写 RequestQueue, recv() 读 ResponseSlot
//   - 服务端: 不使用 ITransport 的 send/recv，而是用 serverRecv/serverSend
//
// eventfd 事件通知:
//   - 服务端创建 req_eventfd_ + resp_eventfds_[0..31]
//   - 客户端通过 UDS SCM_RIGHTS 获取 fd 副本
//   - 客户端 send() 后 notify req_eventfd_ → 唤醒服务端 epoll
//   - 服务端 serverSend() 后 notify resp_eventfds_[i] → 唤醒客户端 epoll
//
class ShmTransport : public ITransport {
public:
    // shm_name: 共享内存名称（由服务名确定性生成）
    // is_server: true=服务端创建, false=客户端连接
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

    // 获取客户端 slot ID（客户端侧有效）
    uint32_t clientId() const { return client_id_; }

    // 服务端：从 RequestQueue 读取一条请求
    // buf: 输出缓冲区
    // buf_size: 缓冲区大小
    // out_client_id: 输出请求来源的 client_id
    // 返回值: >0 = 读取的字节数, 0 = 无数据, <0 = 错误
    int serverRecv(uint8_t* buf, size_t buf_size, uint32_t& out_client_id);

    // 服务端：向指定客户端的 ResponseSlot 写入响应
    // client_id: 目标客户端 slot ID
    // data: 响应数据
    // length: 数据长度
    // 返回值: >0 = 写入的字节数, 0 = slot 满, <0 = 错误
    int serverSend(uint32_t client_id, const uint8_t* data, size_t length);

    // 服务端：广播数据到所有活跃客户端的 ResponseSlot
    // data: 广播数据
    // length: 数据长度
    // 返回: 成功发送的客户端数
    int serverBroadcast(const uint8_t* data, size_t length);

    // 等待 SHM 就绪（客户端调用，等待服务端初始化完成）
    bool waitReady(uint32_t timeout_ms);

    // 获取当前连接的客户端数
    uint32_t clientCount() const;

    // 获取当前活跃 slot 列表（调试/测试用）
    std::vector<uint32_t> activeClientIds() const;

    // 获取响应区已占用块数（调试/测试用）
    uint32_t responseSlotsInUse() const;

    // 获取响应区总容量（字节）
    size_t totalResponseArenaSize() const;

    // 获取当前活跃响应区容量（字节）
    size_t activeResponseArenaSize() const;

    // 获取最大客户端数
    uint32_t maxClients() const { return SHM_MAX_CLIENTS; }

    // 是否是服务端
    bool isServer() const { return is_server_; }

    // 服务端: 获取请求通知 eventfd（注册到 EventLoop 监听请求到达）
    int reqEventFd() const { return req_eventfd_; }

    // 服务端: 获取 UDS 监听 fd（注册到 EventLoop 接受客户端 fd 交换）
    int udsListenFd() const { return uds_listen_fd_; }

    // 服务端: 处理 UDS 客户端连接（交换 eventfd）
    void onUdsClientConnect();

    // 是否启用了 eventfd 通知
    bool eventfdEnabled() const { return eventfd_enabled_; }

private:
    ShmTransport(const ShmTransport&);
    ShmTransport& operator=(const ShmTransport&);

    bool initServer();   // 服务端：创建并初始化 SHM
    bool initClient();   // 客户端：打开 SHM 并分配 slot

    void cleanup();
    bool allocateResponseSlot(uint32_t slot);
    void releaseResponseSlot(uint32_t slot);

    // 环形缓冲区操作
    uint32_t ringAvailableRead(const ShmRingHeader* ring) const;
    uint32_t ringAvailableWrite(const ShmRingHeader* ring) const;
    uint32_t ringWrite(ShmRingHeader* ring, uint8_t* ring_data,
                       const uint8_t* data, uint32_t length);
    uint32_t ringRead(ShmRingHeader* ring, const uint8_t* ring_data,
                      uint8_t* buf, uint32_t length);

    // 获取指定 ring 的指针
    ShmRingHeader* getRequestRing() const;
    uint8_t*       getRequestData() const;
    ShmRingHeader* getResponseRing(uint32_t slot) const;
    uint8_t*       getResponseData(uint32_t slot) const;
    uint8_t*       getResponseArenaBase() const;
    size_t         responseBlockSize() const;
    uint32_t       countBits(uint32_t value) const;

    // 生成 UDS 路径
    static std::string getShmUdsPath(const std::string& shm_name);

    std::string     shm_name_;
    bool            is_server_;
    ConnectionState state_;
    uint32_t        client_id_;    // 客户端的 slot ID
    size_t          requested_req_ring_capacity_;
    size_t          requested_resp_ring_capacity_;

    // 共享内存映射
    void*           shm_addr_;
    size_t          shm_size_;

    // 控制块指针（指向 shm_addr_ 内部）
    ShmControlBlock* ctrl_;

    // eventfd 通知（替代信号量）
    int req_eventfd_;                        // 服务端创建: 请求到达通知
    int resp_eventfds_[SHM_MAX_CLIENTS];     // 服务端创建: 每个 slot 的响应通知

    // 客户端持有的对端 fd 副本（通过 UDS SCM_RIGHTS 获得）
    int peer_req_eventfd_;                   // 客户端: 服务端 req_eventfd_ 的副本
    // event_fd_ 复用为客户端自己 slot 的 resp_eventfd 副本
    // fd() 返回 event_fd_，ConnectionManager 已有的 addFd 逻辑自动生效
    int event_fd_;

    // UDS 交换（服务端）
    int uds_listen_fd_;
    std::string uds_path_;

    // eventfd 是否启用
    bool eventfd_enabled_;
};

// 计算共享内存总大小
size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity,
                        uint32_t max_clients);

// 根据服务名生成确定性 SHM 名称
std::string generateShmName(const std::string& service_name);

} // namespace omnibinder

#endif // OMNIBINDER_SHM_TRANSPORT_H
