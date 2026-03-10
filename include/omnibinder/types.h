/**************************************************************************************************
 * @file        types.h
 * @brief       类型定义
 * @details     定义 OmniBinder 框架的基础数据类型和常量，包括 ServiceHandle 句柄、
 *              默认配置常量（心跳间隔、超时、缓冲区大小等）、MethodInfo/InterfaceInfo/
 *              ServiceInfo 元数据结构体，以及用于生成 interface_id/method_id/topic_id
 *              的 FNV-1a 32 位哈希函数。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-02-11
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
#ifndef OMNIBINDER_TYPES_H
#define OMNIBINDER_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <functional>

namespace omnibinder {

// ============================================================
// 服务句柄
// ============================================================
typedef uint32_t ServiceHandle;
const ServiceHandle INVALID_HANDLE = 0;

// ============================================================
// 默认配置常量
// ============================================================
const uint16_t DEFAULT_SM_PORT              = 9900;
const uint32_t DEFAULT_HEARTBEAT_INTERVAL   = 3000;   // ms
const uint32_t DEFAULT_HEARTBEAT_TIMEOUT    = 10000;  // ms
const uint32_t DEFAULT_MAX_MISSED_HEARTBEATS = 3;
const uint32_t DEFAULT_INVOKE_TIMEOUT       = 5000;   // ms
const size_t   DEFAULT_BUFFER_SIZE          = 4096;
const size_t   MAX_SERVICE_NAME_LENGTH      = 256;
const size_t   MAX_TOPIC_NAME_LENGTH        = 256;
const size_t   MAX_MESSAGE_SIZE             = 16 * 1024 * 1024;  // 16MB

// ============================================================
// 方法信息
// ============================================================
struct MethodInfo {
    uint32_t    method_id;
    std::string name;
    std::string param_types;   // 参数类型（如 "ControlCommand" 或空表示无参数）
    std::string return_type;   // 返回类型（如 "StatusResponse" 或 "void"）

    MethodInfo() : method_id(0) {}
    MethodInfo(uint32_t id, const std::string& n) : method_id(id), name(n) {}
    MethodInfo(uint32_t id, const std::string& n, const std::string& p, const std::string& r)
        : method_id(id), name(n), param_types(p), return_type(r) {}
};

// ============================================================
// 接口信息
// ============================================================
struct InterfaceInfo {
    uint32_t    interface_id;
    std::string name;
    std::vector<MethodInfo> methods;

    InterfaceInfo() : interface_id(0) {}
};

// ============================================================
// SHM 配置
// ============================================================
struct ShmConfig {
    size_t req_ring_capacity;
    size_t resp_ring_capacity;

    ShmConfig()
        : req_ring_capacity(0)
        , resp_ring_capacity(0) {}

    ShmConfig(size_t req_cap, size_t resp_cap)
        : req_ring_capacity(req_cap)
        , resp_ring_capacity(resp_cap) {}
};

// ============================================================
// 服务信息
// ============================================================
struct ServiceInfo {
    std::string name;
    std::string host;
    uint16_t    port;
    std::string host_id;
    std::string shm_name;   // 共享内存名称（同机通信用，空表示不支持 SHM）
    ShmConfig   shm_config; // SHM 队列容量配置（0 表示使用默认值）
    std::vector<InterfaceInfo> interfaces;

    ServiceInfo() : port(0) {}
};

struct RuntimeStats {
    uint64_t total_rpc_calls;
    uint64_t total_rpc_success;
    uint64_t total_rpc_failures;
    uint64_t total_rpc_timeouts;
    uint64_t connection_errors;
    uint64_t sm_reconnect_attempts;
    uint64_t sm_reconnect_successes;
    uint32_t active_connections;
    uint32_t tcp_connections;
    uint32_t shm_connections;

    RuntimeStats()
        : total_rpc_calls(0)
        , total_rpc_success(0)
        , total_rpc_failures(0)
        , total_rpc_timeouts(0)
        , connection_errors(0)
        , sm_reconnect_attempts(0)
        , sm_reconnect_successes(0)
        , active_connections(0)
        , tcp_connections(0)
        , shm_connections(0) {}
};

// ============================================================
// FNV-1a 32位哈希（用于生成 interface_id, method_id, topic_id）
// ============================================================
inline uint32_t fnv1a_32(const char* str) {
    uint32_t hash = 0x811c9dc5u;
    while (*str) {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 0x01000193u;
    }
    return hash;
}

inline uint32_t fnv1a_32(const std::string& str) {
    return fnv1a_32(str.c_str());
}

} // namespace omnibinder

#endif // OMNIBINDER_TYPES_H
