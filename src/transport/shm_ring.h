/**************************************************************************************************
 * @file        shm_ring.h
 * @brief       per-client 共享内存布局与 ring 操作（传输角色无关）
 * @details     定义 SHM 控制块 / ring 头布局、容量常量，以及 ring 读写、帧探测、
 *              布局计算等纯函数。client / server 两个传输实现共用本文件。
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
#ifndef OMNIBINDER_SHM_RING_H
#define OMNIBINDER_SHM_RING_H

#include <string>
#include <stdint.h>
#include <stddef.h>
#include <atomic>

namespace omnibinder {

// ============================================================
// Per-client 共享内存布局
//   [ShmControlBlock]
//   [请求 ring 头 + 数据]   -- 客户端写入请求，服务端读取
//   [响应 ring 头 + 数据]   -- 服务端写入响应，客户端读取
// ============================================================

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
    uint32_t reserved[7];            // 对齐填充
};

#pragma pack(pop)

const uint32_t SHM_MAGIC = 0x53484D42u;       // "SHMB"
const size_t   SHM_DEFAULT_REQ_RING_CAPACITY = 4 * 1024;
const size_t   SHM_DEFAULT_RESP_RING_CAPACITY = 4 * 1024;

// ============================================================
// 容量与布局（纯函数）
// ============================================================

// ring 最小容量（低于该值的容量统一规范化到该值）
const size_t SHM_MIN_RING_CAPACITY = 64;

/*
 * @brief  容量是否有效（>= 最小值且可表示为 uint32）
 * @param[in]  capacity 待校验的容量
 * @return true 有效；false 无效
 */
bool shmIsValidRingCapacity(size_t capacity);

/*
 * @brief  容量规范化：0 或低于最小值的容量统一提升到 SHM_MIN_RING_CAPACITY
 * @param[in]  capacity 原始容量
 * @return 规范化后的容量
 */
size_t shmNormalizeRingCapacity(size_t capacity);

/*
 * @brief  计算单客户端共享内存总大小
 * @param[in]  req_ring_capacity  请求 ring 容量
 * @param[in]  resp_ring_capacity 响应 ring 容量
 * @return 共享内存总大小
 */
size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity);

/*
 * @brief  根据服务名、实例标识与 host_id 生成确定性 SHM 名称（客户端使用）
 * @param[in]  service_name 服务名
 * @param[in]  instance_id  实例标识；空串表示该服务的默认实例
 * @param[in]  host_id      板级主机身份；空串表示不参与派生（兼容旧调用）
 * @return SHM 名称
 * @note   实例标识与 host_id 只参与哈希，不进入名称前缀，保证名称长度有界。
 *         同机多板仿真（同服务名同实例但不同 host_id）必须派生不同握手路径 */
std::string generateShmName(const std::string& service_name,
                            const std::string& instance_id = "",
                            const std::string& host_id = "");

/*
 * @brief  由 SHM 名派生的握手通道路径
 * @param[in]  shm_name SHM 名称
 * @return 握手通道路径
 */
std::string shmHandshakePath(const std::string& shm_name);

// ============================================================
// 从 SHM 基址导航到各区域指针
// ============================================================

ShmRingHeader* shmRequestRingFromBase(uint8_t* base);
uint8_t*       shmRequestDataFromBase(uint8_t* base);
ShmRingHeader* shmResponseRingFromBase(uint8_t* base, uint32_t req_capacity);
uint8_t*       shmResponseDataFromBase(uint8_t* base, uint32_t req_capacity);

// ============================================================
// ring 读写与帧探测
// ============================================================

/*
 * @brief  写入一帧（含长度前缀）到 ring
 * @param[in]  ring      ring 头
 * @param[in]  ring_data ring 数据区
 * @param[in]  capacity  ring 容量
 * @param[in]  frame     帧数据
 * @param[in]  length    帧长度
 * @param[in]  notify_fd 通知 fd（<0 表示不通知）
 * @return 实际写入的帧字节数（0=空间不足，原子写入不拆分）
 * @note   在 空→非空 跃迁时通知 notify_fd；调用方只负责传入自己的 ring 与通知 fd
 */
uint32_t shmRingSendFrame(ShmRingHeader* ring, uint8_t* ring_data, uint32_t capacity,
                          const uint8_t* frame, uint32_t length, int notify_fd);

/*
 * @brief  角色无关的限时全量写帧
 * @param[in]  ring       ring 头
 * @param[in]  ring_data  ring 数据区
 * @param[in]  capacity   ring 容量
 * @param[in]  frame      帧数据
 * @param[in]  length     帧长度
 * @param[in]  notify_fd  通知 fd（<0 不通知）
 * @param[in]  timeout_ms 超时预算
 * @return 0 成功；-1 帧过大，ring 永远放不下（total > capacity-1）；-2 超时（含 timeout_ms==0：只尝试一次）
 * @note   在 timeout_ms 预算内反复尝试把整帧（含 4B 长度前缀）写入 ring；空→非空 跃迁时经 notify_fd 通知
 */
int shmRingSendFrameWithinTimeout(ShmRingHeader* ring, uint8_t* ring_data, uint32_t capacity,
                                  const uint8_t* frame, uint32_t length, int notify_fd,
                                  uint32_t timeout_ms);

/*
 * @brief  从 ring 读取一帧（含长度前缀解析 + 损坏回滚）
 * @param[in]  ring       ring 头
 * @param[in]  ring_data  ring 数据区
 * @param[in]  capacity   ring 容量
 * @param[out] out        输出缓冲区
 * @param[in]  out_size   输出缓冲区大小
 * @param[out] out_length 帧载荷字节数（同步输出）
 * @return >0 帧载荷字节数；0 无完整帧；-1 损坏（长度前缀非法/容量不匹配/缓冲不足/读取失败，且已回滚读游标）
 */
int shmRingRecvFrame(ShmRingHeader* ring, const uint8_t* ring_data, uint32_t capacity,
                     uint8_t* out, size_t out_size, size_t& out_length);

/*
 * @brief  探测 ring 中下一完整帧
 * @param[in]  ring             ring 头
 * @param[in]  ring_data        ring 数据区
 * @param[in]  trusted_capacity 可信 ring 容量
 * @param[out] out_length       完整帧的 payload 长度
 * @return 1 有完整帧（out_length 有效）；0 无完整帧；-1 ring 元数据损坏（长度前缀非法/容量不匹配）
 */
int shmInspectFrame(const ShmRingHeader* ring, const uint8_t* ring_data,
                    uint32_t trusted_capacity, size_t& out_length);

/*
 * @brief  校验已映射 SHM 的控制块 + ring 布局
 * @param[in]  addr              SHM 基址
 * @param[in]  mapped_size       映射大小
 * @param[out] out_ctrl          控制块指针
 * @param[out] out_req_capacity  请求侧容量
 * @param[out] out_resp_capacity 响应侧容量
 * @return true 布局有效（成功时输出 ctrl 与两侧容量）；false 无效
 */
bool shmValidateMappedLayout(void* addr, size_t mapped_size,
                             ShmControlBlock*& out_ctrl,
                             uint32_t& out_req_capacity,
                             uint32_t& out_resp_capacity);

/*
 * @brief  初始化一个已映射 SHM 的控制块与 ring 头（创建方调用）
 * @param[in]  addr          SHM 基址
 * @param[in]  mapped_size   映射大小
 * @param[in]  req_capacity  请求 ring 容量
 * @param[in]  resp_capacity 响应 ring 容量
 */
void shmInitLayout(void* addr, size_t mapped_size,
                   uint32_t req_capacity, uint32_t resp_capacity);

} // namespace omnibinder

#endif // OMNIBINDER_SHM_RING_H
