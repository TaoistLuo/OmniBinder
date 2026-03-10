/**************************************************************************************************
 * @file        omnibinder_c.h
 * @brief       OmniBinder C 语言接口封装
 * @details     为纯 C 项目提供完整的 OmniBinder 功能访问能力，
 *              包括客户端、服务端、Buffer 操作、话题订阅和死亡通知。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-02-28
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

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 不透明句柄
 * ============================================================ */
typedef struct omni_runtime_t  omni_runtime_t;
typedef struct omni_buffer_t  omni_buffer_t;
typedef struct omni_service_t omni_service_t;

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
typedef void (*omni_invoke_callback_t)(uint32_t method_id,
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
 * 读取字符串。返回的指针指向内部静态缓冲区或堆分配内存，
 * 调用者需要用 free() 释放。out_len 可为 NULL。
 */
char*    omni_buffer_read_string(omni_buffer_t* buf, uint32_t* out_len);

/**
 * 读取字节数组。返回堆分配的内存，调用者需要用 free() 释放。
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

/** 获取服务监听端口（注册后有效） */
uint16_t omni_service_port(const omni_service_t* svc);

/* ============================================================
 * Runtime API
 * ============================================================ */

omni_runtime_t* omni_runtime_create(void);
void           omni_runtime_destroy(omni_runtime_t* client);

int  omni_runtime_init(omni_runtime_t* client, const char* sm_host, uint16_t sm_port);
void omni_runtime_poll_once(omni_runtime_t* client, int timeout_ms);
void omni_runtime_stop(omni_runtime_t* client);

/* 服务注册/注销 */
int  omni_runtime_register_service(omni_runtime_t* client, omni_service_t* svc);
int  omni_runtime_unregister_service(omni_runtime_t* client, omni_service_t* svc);

/* RPC 调用 */
int  omni_runtime_invoke(omni_runtime_t* client, const char* service_name,
         uint32_t interface_id, uint32_t method_id,
         const omni_buffer_t* request, omni_buffer_t* response,
         uint32_t timeout_ms);

int  omni_runtime_invoke_oneway(omni_runtime_t* client, const char* service_name,
         uint32_t interface_id, uint32_t method_id,
         const omni_buffer_t* request);

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
void omni_runtime_reset_stats(omni_runtime_t* client);

/* ============================================================
 * 工具函数
 * ============================================================ */

/** FNV-1a 32位哈希（与 C++ 侧 omnibinder::fnv1a_32 一致） */
uint32_t omni_fnv1a_32(const char* str);

#ifdef __cplusplus
}
#endif

#endif /* OMNIBINDER_C_H */
