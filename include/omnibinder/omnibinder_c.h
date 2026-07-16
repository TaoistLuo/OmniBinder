/**************************************************************************************************
 * @file        omnibinder_c.h
 * @brief       OmniBinder C 语言接口封装
 * @details     为纯 C 项目提供完整的 OmniBinder 功能访问能力，
 *              包括客户端、服务端、Buffer 操作、话题订阅和死亡通知。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-02-28
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
#ifndef OMNIBINDER_C_H
#define OMNIBINDER_C_H

#include <stdint.h>
#include <stddef.h>

/* Generated C deserializers use the same wire/allocation limits as C++. */
#define OMNI_MAX_MESSAGE_SIZE (16u * 1024u * 1024u)
#define OMNI_MAX_ARRAY_ELEMENTS (1024u * 1024u)
#define OMNI_MAX_ZERO_WIRE_ARRAY_ELEMENTS (4096u)

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 不透明句柄
 * ============================================================ */
typedef struct omni_runtime_t  omni_runtime_t;
typedef struct omni_buffer_t  omni_buffer_t;
typedef struct omni_service_t omni_service_t;

typedef void* (*OmniMallocFn)(size_t size);
typedef void  (*OmniFreeFn)(void* ptr);

typedef struct omni_runtime_stats_t {
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
} omni_runtime_stats_t;

/* ============================================================
 * 回调函数类型
 * ============================================================ */

/** 服务端方法调用回调 */
typedef int (*omni_invoke_callback_t)(uint32_t method_id,
    const omni_buffer_t* request, omni_buffer_t* response, void* user_data);

/** 话题广播接收回调 */
typedef void (*omni_topic_callback_t)(uint32_t topic_id,
    const omni_buffer_t* data, void* user_data);

/** 服务死亡通知回调 */
typedef void (*omni_death_callback_t)(const char* service_name, void* user_data);

/* ============================================================
 * Buffer API
 * ============================================================ */

omni_buffer_t* omni_buffer_create(void);
omni_buffer_t* omni_buffer_create_from(const uint8_t* data, size_t len);
void           omni_buffer_destroy(omni_buffer_t* buf);
void           omni_buffer_reset(omni_buffer_t* buf);
const uint8_t* omni_buffer_data(const omni_buffer_t* buf);
size_t         omni_buffer_size(const omni_buffer_t* buf);
size_t         omni_buffer_remaining(const omni_buffer_t* buf);
int            omni_buffer_read_ok(const omni_buffer_t* buf);
void           omni_buffer_clear_error(omni_buffer_t* buf);
void           omni_buffer_mark_error(omni_buffer_t* buf, int32_t error_code);
int32_t        omni_buffer_error(const omni_buffer_t* buf);

/* 写入 */
void omni_buffer_write_bool(omni_buffer_t* buf, uint8_t val);
void omni_buffer_write_int8(omni_buffer_t* buf, int8_t val);
void omni_buffer_write_uint8(omni_buffer_t* buf, uint8_t val);
void omni_buffer_write_int16(omni_buffer_t* buf, int16_t val);
void omni_buffer_write_uint16(omni_buffer_t* buf, uint16_t val);
void omni_buffer_write_int32(omni_buffer_t* buf, int32_t val);
void omni_buffer_write_uint32(omni_buffer_t* buf, uint32_t val);
void omni_buffer_write_int64(omni_buffer_t* buf, int64_t val);
void omni_buffer_write_uint64(omni_buffer_t* buf, uint64_t val);
void omni_buffer_write_float32(omni_buffer_t* buf, float val);
void omni_buffer_write_float64(omni_buffer_t* buf, double val);
void omni_buffer_write_string(omni_buffer_t* buf, const char* val, uint32_t len);
void omni_buffer_write_bytes(omni_buffer_t* buf, const uint8_t* data, uint32_t len);

/* 读取 */
uint8_t  omni_buffer_read_bool(omni_buffer_t* buf);
int8_t   omni_buffer_read_int8(omni_buffer_t* buf);
uint8_t  omni_buffer_read_uint8(omni_buffer_t* buf);
int16_t  omni_buffer_read_int16(omni_buffer_t* buf);
uint16_t omni_buffer_read_uint16(omni_buffer_t* buf);
int32_t  omni_buffer_read_int32(omni_buffer_t* buf);
uint32_t omni_buffer_read_uint32(omni_buffer_t* buf);
int64_t  omni_buffer_read_int64(omni_buffer_t* buf);
uint64_t omni_buffer_read_uint64(omni_buffer_t* buf);
float    omni_buffer_read_float32(omni_buffer_t* buf);
double   omni_buffer_read_float64(omni_buffer_t* buf);

/**
 * 读取字符串。返回的指针为堆分配内存，调用者需要用 omni_free() 释放。
 * out_len 可为 NULL。
 */
char*    omni_buffer_read_string(omni_buffer_t* buf, uint32_t* out_len);

/**
 * 读取字节数组。返回堆分配的内存，调用者需要用 omni_free() 释放。
 * out_len 不可为 NULL。
 */
uint8_t* omni_buffer_read_bytes(omni_buffer_t* buf, uint32_t* out_len);

/* ============================================================
 * Service API（服务端）
 * ============================================================ */

/**
 * 创建一个服务实例。
 * @param name          服务名称
 * @param interface_id  接口 ID（通常由 omni_fnv1a_32("pkg.ServiceName") 计算）
 * @param callback      方法调用回调函数
 * @param user_data     传递给回调的用户数据指针
 */
omni_service_t* omni_service_create(const char* name, uint32_t interface_id,
    omni_invoke_callback_t callback, void* user_data);

void omni_service_destroy(omni_service_t* svc);

/** 向服务添加方法信息（用于 omni-cli 查询） */
void omni_service_add_method(omni_service_t* svc, uint32_t method_id, const char* method_name);

/** 向服务添加完整方法签名信息（用于 omni-cli 的 JSON/IDL 展示） */
void omni_service_add_method_ex(omni_service_t* svc, uint32_t method_id, const char* method_name,
    const char* param_types, const char* return_type);

/** 获取服务监听端口（注册后有效） */
uint16_t omni_service_port(const omni_service_t* svc);

/** 设置服务注册到 ServiceManager 的可达地址 */
void omni_service_set_register_host(omni_service_t* svc, const char* host);

/** 获取服务注册到 ServiceManager 的可达地址。返回内部字符串指针。 */
const char* omni_service_get_register_host(const omni_service_t* svc);

/* ============================================================
 * Runtime API
 * ============================================================ */

omni_runtime_t* omni_runtime_create(void);
void           omni_runtime_destroy(omni_runtime_t* client);

int  omni_runtime_init(omni_runtime_t* client, const char* sm_host, uint16_t sm_port);
void omni_runtime_run(omni_runtime_t* client);
void omni_runtime_poll_once(omni_runtime_t* client, int timeout_ms);
void omni_runtime_stop(omni_runtime_t* client);
int  omni_runtime_is_running(const omni_runtime_t* client);

/** 设置当前 runtime 下服务默认注册到 ServiceManager 的可达地址 */
void omni_runtime_set_register_host(omni_runtime_t* client, const char* host);

/** 获取当前 runtime 默认注册到 ServiceManager 的可达地址。返回内部字符串指针。 */
const char* omni_runtime_get_register_host(const omni_runtime_t* client);

/** 设置向 ServiceManager 发送心跳的间隔（毫秒） */
void omni_runtime_set_heartbeat_interval(omni_runtime_t* client, uint32_t interval_ms);

/** 设置 RPC 调用的默认超时时间（毫秒） */
void omni_runtime_set_default_timeout(omni_runtime_t* client, uint32_t timeout_ms);

/** 获取当前 runtime 的主机标识符。返回内部字符串指针。 */
const char* omni_runtime_host_id(const omni_runtime_t* client);

/* 服务注册/注销 */
int  omni_runtime_register_service(omni_runtime_t* client, omni_service_t* svc);
int  omni_runtime_unregister_service(omni_runtime_t* client, omni_service_t* svc);

/* RPC 调用 */
int  omni_runtime_invoke(omni_runtime_t* client, const char* service_name,
         uint32_t interface_id, uint32_t method_id, uint32_t idl_hash,
         const omni_buffer_t* request, omni_buffer_t* response,
         uint32_t timeout_ms);

int  omni_runtime_invoke_oneway(omni_runtime_t* client, const char* service_name,
         uint32_t interface_id, uint32_t method_id, uint32_t idl_hash,
         const omni_buffer_t* request);

/* 连接管理 */
int  omni_runtime_connect_service(omni_runtime_t* client, const char* service_name);
int  omni_runtime_disconnect_service(omni_runtime_t* client, const char* service_name);
int  omni_runtime_is_service_connected(const omni_runtime_t* client, const char* service_name);
void omni_runtime_enable_auto_reconnect(omni_runtime_t* client, const char* service_name, int enable);
void omni_runtime_set_reconnect_interval(omni_runtime_t* client, const char* service_name, uint32_t interval_ms);
void omni_runtime_start_heartbeat(omni_runtime_t* client, const char* service_name, uint32_t interval_ms, uint32_t timeout_ms);
void omni_runtime_stop_heartbeat(omni_runtime_t* client, const char* service_name);

/* 话题 */
int  omni_runtime_publish_topic(omni_runtime_t* client, const char* topic_name);
int  omni_runtime_broadcast(omni_runtime_t* client, uint32_t topic_id,
         const omni_buffer_t* data);
int  omni_runtime_subscribe_topic(omni_runtime_t* client, const char* topic_name,
         omni_topic_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_topic(omni_runtime_t* client, const char* topic_name);

/* 死亡通知 */
int  omni_runtime_subscribe_death(omni_runtime_t* client, const char* service_name,
         omni_death_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_death(omni_runtime_t* client, const char* service_name);
int  omni_runtime_get_stats(omni_runtime_t* client, omni_runtime_stats_t* stats);
int  omni_runtime_reset_stats(omni_runtime_t* client);

/* ============================================================
 * 工具函数
 * ============================================================ */

/** FNV-1a 32位哈希（与 C++ 侧 omnibinder::fnv1a_32 一致） */
uint32_t omni_fnv1a_32(const char* str);

/* 自定义内存分配器包装（走 omniSetAllocator 注册的分配器，否则回退系统 malloc/free） */
void  omniSetAllocator(OmniMallocFn malloc_fn, OmniFreeFn free_fn);
void* omni_malloc(size_t size);
void  omni_free(void* ptr);
void* omni_realloc_sized(void* ptr, size_t old_size, size_t new_size);
void* omni_realloc(void* ptr, size_t new_size);

#ifdef __cplusplus
}
#endif

#endif /* OMNIBINDER_C_H */
