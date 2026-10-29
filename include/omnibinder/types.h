/**************************************************************************************************
 * @file        types.h
 * @brief       类型定义
 * @details     定义 OmniBinder 框架的基础数据类型和常量，包括默认配置常量、
 *              MethodInfo/InterfaceInfo/ServiceInfo 元数据结构体，以及用于生成
 *              interface_id/method_id/topic_id 的 FNV-1a 32 位哈希函数。
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
#ifndef OMNIBINDER_TYPES_H
#define OMNIBINDER_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "omnibinder/log.h"
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
const char     DIAG_SERVICE_NAME_PREFIX[]   = "__diag_pid_";
const uint32_t DEFAULT_HEARTBEAT_INTERVAL   = 3000;   // ms
const uint32_t DEFAULT_HEARTBEAT_TIMEOUT    = 10000;  // ms
// 会话协商：接收方按 max(requested, interval * factor) 裁定判死超时。
// max_timeout_ms = 0 表示不设上限（低功耗客户端可协商分钟/小时级超时）。
const uint32_t MIN_HEARTBEAT_INTERVAL_MS    = 100;    // ms
const uint32_t HEARTBEAT_MISSED_FACTOR      = 3;
const uint32_t DEFAULT_MAX_SESSION_TIMEOUT  = 0;      // 0 = 不限制
const uint32_t DEFAULT_SESSION_HELLO_TIMEOUT= 5000;   // ms
const uint32_t SESSION_PROTOCOL_VERSION     = 1;
const uint32_t DEFAULT_INVOKE_TIMEOUT       = 5000;   // ms
// 服务端 RPC 回复发送的专用超时预算：独立于 DEFAULT_INVOKE_TIMEOUT，避免慢客户端
// 长时间占住 owner event-loop；可经 OmniRuntime::setReplySendTimeout 调整
const uint32_t DEFAULT_REPLY_SEND_TIMEOUT   = 1000;   // ms
const size_t   DEFAULT_BUFFER_SIZE          = 4096;
const size_t   MAX_SERVICE_NAME_LENGTH      = 256;
const size_t   MAX_TOPIC_NAME_LENGTH        = 256;
const size_t   MAX_MESSAGE_SIZE             = 16u * 1024u * 1024u;
const size_t   MAX_ARRAY_ELEMENTS           = 1024u * 1024u;
const size_t   MAX_ZERO_WIRE_ARRAY_ELEMENTS = 4096u;
const size_t   MAX_PUBLISHED_TOPICS         = 4096u;
const size_t   MAX_PUBLISHED_TOPICS_BYTES   = 512u * 1024u;
const uint32_t RUNTIME_DIAG_CAP_WATCH       = 1u;

/* @brief 规范化心跳间隔：0 使用默认值，低于下限则抬到下限 */
inline uint32_t clampHeartbeatInterval(uint32_t interval_ms) {
    if (interval_ms == 0) return DEFAULT_HEARTBEAT_INTERVAL;
    return interval_ms < MIN_HEARTBEAT_INTERVAL_MS ? MIN_HEARTBEAT_INTERVAL_MS : interval_ms;
}

/* @brief 协商接收方判死超时
 * @param[in] interval_ms  发送方声明的心跳间隔（已由 clampHeartbeatInterval 规范化）
 * @param[in] requested_ms 发送方请求的判死超时；0 表示使用默认值
 * @param[in] max_timeout_ms 服务端可选上限；0 表示不限制
 * @return 有效判死超时：max(requested, interval * factor)，再按上限裁剪
 * @note 无上限时低功耗可协商分钟/小时级超时；下限由 interval*factor 保证，
 *       避免发送方发得慢却被短超时误杀。 */
inline uint32_t negotiateSessionTimeout(uint32_t interval_ms, uint32_t requested_ms,
                                        uint32_t max_timeout_ms) {
    uint64_t floor_ms = static_cast<uint64_t>(interval_ms) * HEARTBEAT_MISSED_FACTOR;
    uint64_t effective = requested_ms == 0 ? floor_ms : requested_ms;
    if (requested_ms == 0 && effective < DEFAULT_HEARTBEAT_TIMEOUT) {
        effective = DEFAULT_HEARTBEAT_TIMEOUT;
    }
    if (effective < floor_ms) {
        effective = floor_ms;
    }
    if (max_timeout_ms != 0 && effective > max_timeout_ms) {
        effective = max_timeout_ms;
    }
    if (effective > 0xFFFFFFFFu) {
        effective = 0xFFFFFFFFu;
    }
    return effective == 0 ? 1u : static_cast<uint32_t>(effective);
}

/* @brief 控制面/数据面稳定会话键：host_id + runtime_id（无歧义分隔） */
inline std::string makeRuntimeSessionKey(const std::string& host_id,
                                         const std::string& runtime_id) {
    std::string key;
    key.reserve(host_id.size() + runtime_id.size() + 1);
    key.append(host_id);
    key.push_back('\x1f');
    key.append(runtime_id);
    return key;
}

// ============================================================
// 方法信息
// ============================================================
/* @brief 单个 RPC 方法的元信息（方法 ID、IDL 哈希、参数/返回类型描述） */
struct MethodInfo {
    uint32_t    method_id;
    uint32_t    idl_hash;      // 方法签名+参数/返回值类型递归哈希，检测 IDL 不匹配
    std::string name;
    std::string param_types;   // 参数类型（如 "ControlCommand" 或空表示无参数）
    std::string return_type;   // 返回类型（如 "StatusResponse" 或 "void"）

    MethodInfo() : method_id(0), idl_hash(0) {}
    MethodInfo(uint32_t id, const std::string& n) : method_id(id), idl_hash(0), name(n) {}
    MethodInfo(uint32_t id, const std::string& n, const std::string& p, const std::string& r)
        : method_id(id), idl_hash(0), name(n), param_types(p), return_type(r) {}
};

// ============================================================
// 接口信息
// ============================================================
/* @brief 一个接口的元信息（接口 ID、名称、方法列表） */
struct InterfaceInfo {
    uint32_t    interface_id;
    std::string name;
    std::vector<MethodInfo> methods;

    InterfaceInfo() : interface_id(0) {}
};

// ============================================================
// SHM 配置
// ============================================================
/* @brief 服务级 SHM ring 容量配置（request / response 各一段环形缓冲） */
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
/* @brief 服务注册信息（发现结果）：名称、直连地址、实例身份、SHM 配置与接口列表 */
struct ServiceInfo {
    std::string name;
    std::string host;
    uint16_t    port;
    std::string host_id;
    /* @brief 实例标识：同一 host_id 上同名服务的多实例区分
     * @note  由服务显式声明（Service::setInstanceId）；默认空串表示该板上该服务的
     *        默认实例 */
    std::string instance_id;
    ShmConfig    shm_config;
    std::vector<InterfaceInfo> interfaces;

    ServiceInfo() : port(0) {}
};

// ============================================================
// 服务实例身份 (service_name, host_id, instance_id)
// ============================================================
/* @brief 服务实例身份：唯一确定一个真实服务实例
 * @details 服务名 + 板级 host_id + 实例标识。host_id 复用机器身份，
 *          instance_id 由服务显式声明（默认空串表示单实例）。 */
struct ServiceIdentity {
    std::string name;
    std::string host_id;
    std::string instance_id;

    ServiceIdentity() {}
    explicit ServiceIdentity(const std::string& n)
        : name(n) {}
    ServiceIdentity(const std::string& n, const std::string& h, const std::string& i)
        : name(n), host_id(h), instance_id(i) {}

    bool operator==(const ServiceIdentity& other) const {
        return name == other.name
            && host_id == other.host_id
            && instance_id == other.instance_id;
    }
    bool operator!=(const ServiceIdentity& other) const {
        return !(*this == other);
    }
};

/* @brief 生成无歧义的 (host_id, instance_id) 实例键
 * @param[in]  host_id     板级主机身份
 * @param[in]  instance_id 实例标识
 * @return 可用于 map key 的字符串
 * @note   采用 "host_id长度|host_id|instance_id" 编码：先写 host_id 字节数再写原串，
 *         因此即使字段中本身含有分割符也不会与其它组合碰撞；instance_id 位于末尾无需长度。
 *         实现在 src/core/runtime_connection.cpp。 */
std::string makeServiceInstanceKey(const std::string& host_id,
                                   const std::string& instance_id);

/* @brief 生成无歧义的完整实例身份键（含服务名）
 * @param[in]  identity 服务实例身份
 * @return 可用于 map key 的字符串
 * @note   编码顺序为 name / host_id / instance_id，各自前置长度前缀，保证无碰撞。
 *         实现在 src/core/runtime_connection.cpp。 */
std::string makeServiceIdentityKey(const ServiceIdentity& identity);

struct RuntimeInfo {
    uint32_t pid;
    std::string process_name;
    std::string role;
    uint32_t log_level;
    uint32_t diag_capabilities;
    std::vector<std::string> services;

    // 控制面会话身份/协商字段（MSG_RUNTIME_HELLO）
    uint32_t session_version;
    std::string runtime_id;          // 稳定运行时身份（跨重启不变）
    uint32_t incarnation_id;         // 本次启动代际
    uint32_t heartbeat_interval_ms;
    uint32_t requested_timeout_ms;
    std::string host_id;

    RuntimeInfo()
        : pid(0)
        , role("client")
        , log_level(OMNI_LOG_INFO)
        , diag_capabilities(0)
        , session_version(SESSION_PROTOCOL_VERSION)
        , incarnation_id(0)
        , heartbeat_interval_ms(DEFAULT_HEARTBEAT_INTERVAL)
        , requested_timeout_ms(0) {}
};

// ============================================================
// 数据面会话 Hello / ACK（每条直连一次）
// ============================================================
struct SessionHello {
    uint32_t version;
    std::string runtime_id;
    uint32_t incarnation_id;
    uint32_t heartbeat_interval_ms;
    uint32_t requested_timeout_ms;
    uint32_t capabilities;
    std::string host_id;
    std::string process_name;
    uint32_t pid;

    SessionHello()
        : version(SESSION_PROTOCOL_VERSION)
        , incarnation_id(0)
        , heartbeat_interval_ms(DEFAULT_HEARTBEAT_INTERVAL)
        , requested_timeout_ms(0)
        , capabilities(0)
        , pid(0) {}
};

struct SessionHelloAck {
    uint32_t version;
    uint32_t status;                 // 0=OK, 1=CLAMPED, 2=REJECTED
    uint32_t accepted_interval_ms;
    uint32_t effective_timeout_ms;
    uint32_t peer_incarnation_id;    // 接收方自己的代际

    SessionHelloAck()
        : version(SESSION_PROTOCOL_VERSION)
        , status(0)
        , accepted_interval_ms(DEFAULT_HEARTBEAT_INTERVAL)
        , effective_timeout_ms(DEFAULT_HEARTBEAT_TIMEOUT)
        , peer_incarnation_id(0) {}
};

const uint32_t SESSION_STATUS_OK       = 0;
const uint32_t SESSION_STATUS_CLAMPED  = 1;
const uint32_t SESSION_STATUS_REJECTED = 2;

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
/* @brief FNV-1a 32 位哈希（C 字符串版本，用于生成 interface_id/method_id/topic_id） */
inline uint32_t fnv1a_32(const char* str) {
    uint32_t hash = 0x811c9dc5u;
    while (*str) {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 0x01000193u;
    }
    return hash;
}

/* @brief FNV-1a 32 位哈希（std::string 版本，用于生成 interface_id/method_id/topic_id） */
inline uint32_t fnv1a_32(const std::string& str) {
    return fnv1a_32(str.c_str());
}

} // namespace omnibinder

#endif // OMNIBINDER_TYPES_H
