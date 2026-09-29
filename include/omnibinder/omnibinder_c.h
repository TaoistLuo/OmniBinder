/**************************************************************************************************
 * @file        omnibinder_c.h
 * @brief       OmniBinder C 语言接口封装
 * @details     为纯 C 项目提供 OmniBinder 的常用功能访问能力（客户端、服务端、
 *              Buffer 操作、话题订阅和死亡通知）。C API 是 C++ API 的常用子集，
 *              并非一一对等：listServices/queryInterfaces/watchPid 等发现与诊断接口
 *              目前仅在 C++ API 提供，需要时请在 C++ 侧调用或按需扩展 C 封装。
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

/* 生成的 C 反序列化器与 C++ 侧使用相同的 wire/分配上限 */
#define OMNI_MAX_MESSAGE_SIZE (16u * 1024u * 1024u)
#define OMNI_MAX_ARRAY_ELEMENTS (1024u * 1024u)
#define OMNI_MAX_ZERO_WIRE_ARRAY_ELEMENTS (4096u)

/* C 错误码常量（与 omnibinder::ErrorCode 同值，供 C 用户与生成代码引用） */
#define OMNI_OK                     0
#define OMNI_ERR_UNKNOWN           (-1)
#define OMNI_ERR_INVALID_PARAM     (-2)
#define OMNI_ERR_OUT_OF_MEMORY     (-3)
#define OMNI_ERR_TIMEOUT           (-4)
#define OMNI_ERR_NOT_INITIALIZED   (-5)
#define OMNI_ERR_NOT_RUNNING       (-9)
#define OMNI_ERR_CONNECT_FAILED    (-100)
#define OMNI_ERR_CONNECTION_CLOSED (-101)
#define OMNI_ERR_SEND_FAILED       (-102)
#define OMNI_ERR_RECV_FAILED       (-103)
#define OMNI_ERR_SERVICE_NOT_FOUND (-200)
#define OMNI_ERR_INVOKE_FAILED     (-205)
#define OMNI_ERR_REGISTER_FAILED   (-206)
#define OMNI_ERR_IDL_MISMATCH      (-208)
#define OMNI_ERR_SERIALIZE         (-500)
#define OMNI_ERR_DESERIALIZE       (-501)

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

/* @brief 服务端方法调用回调 */
typedef int (*omni_invoke_callback_t)(uint32_t method_id,
    const omni_buffer_t* request, omni_buffer_t* response, void* user_data);

/* @brief 话题广播接收回调 */
typedef void (*omni_topic_callback_t)(uint32_t topic_id,
    const omni_buffer_t* data, void* user_data);

/* @brief 服务死亡通知回调 */
typedef void (*omni_death_callback_t)(const char* service_name, void* user_data);

/* @brief 服务实例信息（实例发现结果）
 * @note  回调传入的指针仅在本次回调执行期间有效，回调返回后不可保存。 */
typedef struct omni_service_instance_t {
    const char* service_name;   /* 服务名称 */
    const char* host_id;        /* 板级主机身份（同机判断与实例身份的一部分） */
    const char* instance_id;    /* 实例标识；空串表示该 host_id 上该服务的单实例 */
    const char* host;           /* 数据面直连可达地址 */
    uint16_t    port;           /* 数据面直连端口 */
} omni_service_instance_t;

/* @brief 服务实例枚举回调（omni_runtime_list_instances 对每个实例调用一次） */
typedef void (*omni_instance_callback_t)(const omni_service_instance_t* info, void* user_data);

/* ============================================================
 * Buffer 接口
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

/* @brief 读取字符串
 * @param[out] out_len 输出长度（可为 NULL）
 * @return 堆分配字符串，调用者需要用 omni_free() 释放 */
char*    omni_buffer_read_string(omni_buffer_t* buf, uint32_t* out_len);

/* @brief 读取字节数组
 * @param[out] out_len 输出长度（可为 NULL，此时不返回长度）
 * @return 堆分配内存，调用者需要用 omni_free() 释放；失败返回 NULL（读越界或内存不足） */
uint8_t* omni_buffer_read_bytes(omni_buffer_t* buf, uint32_t* out_len);

/* ============================================================
 * Service 接口（服务端）
 * ============================================================ */

/* @brief 创建一个服务实例
 * @param[in] name         服务名称
 * @param[in] interface_id 接口 ID（通常由 omni_fnv1a_32("pkg.ServiceName") 计算）
 * @param[in] callback     方法调用回调函数
 * @param[in] user_data    传递给回调的用户数据指针 */
omni_service_t* omni_service_create(const char* name, uint32_t interface_id,
    omni_invoke_callback_t callback, void* user_data);

/* @brief 销毁服务实例
 * @note  必须先 omni_runtime_unregister_service 注销，再销毁；runtime 内部以裸指针
 *        持有该 Service，注册状态下销毁会留下悬空指针。 */
void omni_service_destroy(omni_service_t* svc);

/* @brief 取回创建服务时传入的 user_data（生成代码据此释放自有的 stub 上下文） */
void* omni_service_get_user_data(omni_service_t* svc);

/* @brief 向服务添加方法信息（用于 omni-cli 查询；idl_hash 按 0 处理，运行时跳过该方法的哈希校验） */
void omni_service_add_method(omni_service_t* svc, uint32_t method_id, const char* method_name);

/* @brief 向服务添加完整方法签名信息（用于 omni-cli 的 JSON/IDL 展示与 IDL 哈希校验）
 * @param[in] svc         服务实例
 * @param[in] method_id   方法 ID（由 omni_fnv1a_32(方法名) 计算）
 * @param[in] method_name 方法名
 * @param[in] param_types 参数类型描述（无参数传空字符串）
 * @param[in] return_type 返回类型描述（无返回值传 "void"）
 * @param[in] idl_hash    方法 IDL 哈希（由 omni-idlc 生成；传 0 表示未声明）
 * @note 客户端请求携带非 0 idl_hash 时，服务端注册的哈希必须与之相等，否则返回 ERR_IDL_MISMATCH */
void omni_service_add_method_ex(omni_service_t* svc, uint32_t method_id, const char* method_name,
    const char* param_types, const char* return_type, uint32_t idl_hash);

/* @brief 获取服务监听端口（注册后有效） */
uint16_t omni_service_port(const omni_service_t* svc);

/* @brief 设置服务注册到 ServiceManager 的可达地址 */
void omni_service_set_register_host(omni_service_t* svc, const char* host);

/* @brief 获取服务注册到 ServiceManager 的可达地址
 * @return 内部字符串指针；指向服务自身持有的字符串，在下次 set 或服务销毁前有效 */
const char* omni_service_get_register_host(const omni_service_t* svc);

/* @brief 设置服务实例标识（同名服务多实例时用于区分）
 * @param[in] svc         服务实例
 * @param[in] instance_id 实例标识；NULL 或空串表示该 host_id 上该服务的默认实例
 * @note  必须在 omni_runtime_register_service 之前调用；实例身份参与注册与 SHM 命名派生。
 *        同一 host_id 上要运行多个同名服务，必须为各服务声明不同的 instance_id，
 *        否则同身份会被视为同一实例而互相顶替（后者接管前者）。 */
void omni_service_set_instance_id(omni_service_t* svc, const char* instance_id);

/* ============================================================
 * Runtime 接口
 * ============================================================ */

omni_runtime_t* omni_runtime_create(void);
void           omni_runtime_destroy(omni_runtime_t* client);

int  omni_runtime_init(omni_runtime_t* client, const char* sm_host, uint16_t sm_port);
void omni_runtime_run(omni_runtime_t* client);
void omni_runtime_poll_once(omni_runtime_t* client, int timeout_ms);
void omni_runtime_stop(omni_runtime_t* client);
int  omni_runtime_is_running(const omni_runtime_t* client);

/* @brief 设置当前 runtime 下服务默认注册到 ServiceManager 的可达地址 */
void omni_runtime_set_register_host(omni_runtime_t* client, const char* host);

/* @brief 获取当前 runtime 默认注册到 ServiceManager 的可达地址
 * @return 内部 thread-local 缓冲指针；指向内容在下次调用本函数前有效 */
const char* omni_runtime_get_register_host(const omni_runtime_t* client);

/* @brief 设置向 ServiceManager 发送心跳的间隔（毫秒） */
void omni_runtime_set_heartbeat_interval(omni_runtime_t* client, uint32_t interval_ms);

/* @brief 设置 RPC 调用的默认超时时间（毫秒） */
void omni_runtime_set_default_timeout(omni_runtime_t* client, uint32_t timeout_ms);

/* @brief 覆盖本机标识符 host_id（同机判断与实例身份）
 * @note  必须在 omni_runtime_init 之前调用；空串表示使用平台默认机器标识。
 *        克隆镜像共享 machine-id 的多板设备应显式设置各设备唯一标识（D8）。 */
void omni_runtime_set_host_id(omni_runtime_t* client, const char* host_id);

/* @brief 获取当前 runtime 的主机标识符
 * @return 内部 thread-local 缓冲指针；指向内容在下次调用本函数前有效 */
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

/* @brief 查询与指定服务实例的数据面连接状态
 * @param[in] client       runtime 实例
 * @param[in] service_name 服务名称
 * @param[in] host_id      目标实例的板级主机身份
 * @param[in] instance_id  目标实例标识
 * @return 1 已连接，0 未连接
 * @note   连接键为 makeServiceConnectionKey(name, host_id, instance_id)，与
 *         omni_runtime_connect_service_instance 配套。按实例建立的连接不能用
 *         omni_runtime_is_service_connected(name) 检测（后者只命中按服务名的连接） */
int  omni_runtime_is_service_instance_connected(const omni_runtime_t* client,
         const char* service_name, const char* host_id, const char* instance_id);

/* @brief 生成数据面实例连接键，供键无关 API（自动重连 / 重连间隔 / 心跳）按实例寻址
 * @param[in] service_name 服务名称
 * @param[in] host_id      目标实例的板级主机身份
 * @param[in] instance_id  目标实例标识
 * @return 指向线程局部缓冲的连接键字符串；在下次调用本函数前有效
 * @note   主要供生成的 C Proxy 在 instance_bound 时把同一套连接 API 作用于实例 */
const char* omni_runtime_service_connection_key(const char* service_name,
         const char* host_id, const char* instance_id);

/* ---- 多实例：实例发现与实例级寻址 ----
 * 同名服务可存在多个实例，实例身份 = (service_name, host_id, instance_id)。
 * 默认（instance_id 为空）时按服务名寻址，行为与历史版本一致。 */

/* @brief 列出指定服务名下的全部实例（跨 host_id / instance_id）
 * @param[in]  client       runtime 实例
 * @param[in]  service_name 服务名称
 * @param[in]  callback     每个实例调用一次（在调用线程执行；info 指针仅回调期间有效）
 * @param[in]  user_data    传给回调的用户数据
 * @return 0 成功；OMNI_ERR_SERVICE_NOT_FOUND 该名称下无任何实例；其它 <0 为错误码
 * @note  回调内不要调用会阻塞等待的 runtime API，避免在同步请求期间重入。 */
int  omni_runtime_list_instances(omni_runtime_t* client, const char* service_name,
         omni_instance_callback_t callback, void* user_data);

/* @brief 建立到指定服务实例 (host_id, instance_id) 的数据面直连
 * @return 0 成功；OMNI_ERR_SERVICE_NOT_FOUND 无匹配实例；网络/协议错误返回对应错误码
 * @note  host_id / instance_id 必须为非 NULL 且非空的显式身份；默认实例请改用
 *        omni_runtime_connect_service(service_name)（按名连接，键即服务名）。
 *        生成的 C Proxy 可通过 <Service>_proxy_connect_instance 使用本能力 */
int  omni_runtime_connect_service_instance(omni_runtime_t* client, const char* service_name,
         const char* host_id, const char* instance_id);

/* @brief 断开到指定服务实例的数据面直连 */
int  omni_runtime_disconnect_service_instance(omni_runtime_t* client, const char* service_name,
         const char* host_id, const char* instance_id);

/* @brief 向指定服务实例发起同步 RPC（需先 connect_service_instance 建立数据面）
 * @param[in]  client       runtime 实例
 * @param[in]  service_name 服务名称
 * @param[in]  host_id      目标实例的板级主机身份
 * @param[in]  instance_id  目标实例标识
 * @param[in]  interface_id 接口 ID（由 IDL 生成）
 * @param[in]  method_id    方法 ID（由 IDL 生成）
 * @param[in]  idl_hash     IDL 哈希（用于接口版本校验）
 * @param[in]  request      请求参数
 * @param[out] response     响应数据
 * @param[in]  timeout_ms   超时时间（毫秒，0 使用默认值）
 * @return 0 成功，<0 失败 */
int  omni_runtime_invoke_instance(omni_runtime_t* client, const char* service_name,
         const char* host_id, const char* instance_id,
         uint32_t interface_id, uint32_t method_id, uint32_t idl_hash,
         const omni_buffer_t* request, omni_buffer_t* response,
         uint32_t timeout_ms);

/* @brief 向指定服务实例发起单向 RPC（需先 connect_service_instance 建立数据面）
 * @return 0 发送成功，<0 失败 */
int  omni_runtime_invoke_oneway_instance(omni_runtime_t* client, const char* service_name,
         const char* host_id, const char* instance_id,
         uint32_t interface_id, uint32_t method_id, uint32_t idl_hash,
         const omni_buffer_t* request);
/* ---- 自动重连 / 重连间隔 / 心跳：name 参数为"数据面连接键" ----
 * 服务级连接直接传服务名；实例级连接先用 omni_runtime_service_connection_key(...)
 * 取得连接键再传入。C API 不单独提供实例版重连/心跳接口，这是有意的子集边界，
 * 生成的 C Proxy 绑定实例后即以实例连接键调用下列接口。 */
void omni_runtime_enable_auto_reconnect(omni_runtime_t* client, const char* service_name, int enable);
void omni_runtime_set_reconnect_interval(omni_runtime_t* client, const char* service_name, uint32_t interval_ms);
void omni_runtime_start_heartbeat(omni_runtime_t* client, const char* service_name, uint32_t interval_ms, uint32_t timeout_ms);
void omni_runtime_stop_heartbeat(omni_runtime_t* client, const char* service_name);

/* 话题 */
/* @brief 声明 topic 发布
 * @param[in] client     runtime 实例
 * @param[in] topic_name topic 名称
 * @param[in] idl_hash   发布者 topic IDL 哈希（由 omni-idlc 生成；传 0 表示不声明）
 * @return 0 成功，<0 失败 */
int  omni_runtime_publish_topic(omni_runtime_t* client, const char* topic_name,
         uint32_t idl_hash);
int  omni_runtime_broadcast(omni_runtime_t* client, uint32_t topic_id,
         const omni_buffer_t* data);
/* @brief 订阅 topic
 * @param[in] client            runtime 实例
 * @param[in] topic_name        topic 名称
 * @param[in] expected_idl_hash 订阅者期望的 topic IDL 哈希（由 omni-idlc 生成；0 表示不校验）
 * @param[in] callback          消息回调
 * @param[in] user_data         用户数据
 * @return 0 成功；发布者哈希与期望不一致时返回 ERR_IDL_MISMATCH 且不投递数据 */
int  omni_runtime_subscribe_topic(omni_runtime_t* client, const char* topic_name,
         uint32_t expected_idl_hash, omni_topic_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_topic(omni_runtime_t* client, const char* topic_name);

/* 死亡通知 */
int  omni_runtime_subscribe_death(omni_runtime_t* client, const char* service_name,
         omni_death_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_death(omni_runtime_t* client, const char* service_name);

/* @brief 订阅指定服务实例的死亡通知
 * @param[in] client       runtime 实例
 * @param[in] service_name 服务名称
 * @param[in] host_id      目标实例的板级主机身份
 * @param[in] instance_id  目标实例标识
 * @param[in] callback     死亡回调（在 owner event-loop 线程执行，携带服务名）
 * @param[in] user_data    用户数据
 * @return 0 成功，<0 失败
 * @note   仅当该 (name, host_id, instance_id) 实例死亡时触发，同名的其它实例死亡不影响 */
int  omni_runtime_subscribe_death_instance(omni_runtime_t* client, const char* service_name,
         const char* host_id, const char* instance_id,
         omni_death_callback_t callback, void* user_data);

/* @brief 取消指定服务实例的死亡通知订阅
 * @return 0 成功，<0 失败 */
int  omni_runtime_unsubscribe_death_instance(omni_runtime_t* client, const char* service_name,
         const char* host_id, const char* instance_id);
int  omni_runtime_get_stats(omni_runtime_t* client, omni_runtime_stats_t* stats);
int  omni_runtime_reset_stats(omni_runtime_t* client);

/* ============================================================
 * 工具函数
 * ============================================================ */

/* @brief FNV-1a 32位哈希（与 C++ 侧 omnibinder::fnv1a_32 一致） */
uint32_t omni_fnv1a_32(const char* str);

/* @brief 自定义内存分配器包装（底层为 ::operator new/delete 的薄封装）
 * @note 想让库内分配使用自定义堆，请在程序中重载全局 operator new/delete（C 与 C++ 一次覆盖） */
void* omni_malloc(size_t size);
void  omni_free(void* ptr);

/* @brief 复制字符串（分配 + 拷贝，等价 strdup）
 * @param[in] str 源字符串
 * @return 以 '\0' 结尾的堆副本；str 为 NULL 或分配失败时返回 NULL
 * @note 返回值需用 omni_free() 释放（不可用 free()）。 */
char* omni_strdup(const char* str);

#ifdef __cplusplus
}
#endif

#endif /* OMNIBINDER_C_H */
