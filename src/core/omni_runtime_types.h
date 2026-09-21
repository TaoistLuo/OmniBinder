/**************************************************************************************************
 * @file        omni_runtime_types.h
 * @brief       OmniRuntime::Impl 使用的纯数据类型定义
 * @details     从 omni_runtime.h 抽出的只承载数据的类型：本地服务上下文、每服务状态、
 *              重连/心跳配置、入站请求分派结果、诊断状态聚合等。
 *              本头文件仅包含自身所需的公共头，不反向包含 omni_runtime.h，避免循环依赖。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
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
#ifndef OMNIBINDER_CORE_OMNI_RUNTIME_TYPES_H
#define OMNIBINDER_CORE_OMNI_RUNTIME_TYPES_H

#include "omnibinder/runtime.h"
#include "omnibinder/service.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <set>

namespace omnibinder {

enum class InvokeDispatchStatus {
    SUCCESS,
    DECODE_FAILED,
    INTERFACE_MISMATCH,
    IDL_MISMATCH,
    INVOKE_FAILED
};

struct InvokeDispatchResult {
    InvokeDispatchStatus status;
    int error_code;
    Buffer response;
};

/*
 * @brief  本地注册服务的上下文
 * @details endpoints 拥有各传输端点（TCP/SHM），clients 为端点接入的客户端
 *          非拥有视图（生命周期由 endpoint 管理）。
 */
struct LocalServiceEntry {
    Service*            service;
    uint16_t            port;

    std::vector<IServerEndpoint*>    endpoints;
    std::map<int, IMessageConnection*> clients;
    std::map<int, Buffer*>           client_recv_buffers;
    // 已注册到 EventLoop 的端点级 fd（含 TCP 客户端 fd / SHM liveness fd）
    std::set<int>                    endpoint_fds;

    bool     diag_enabled;
    uint32_t diag_topic_id;

    LocalServiceEntry()
        : service(NULL), port(0)
        , diag_enabled(false), diag_topic_id(0) {}

    ~LocalServiceEntry() {
        for (std::map<int, Buffer*>::iterator it = client_recv_buffers.begin();
             it != client_recv_buffers.end(); ++it) {
            delete it->second;
        }
        client_recv_buffers.clear();
        clients.clear();
        for (size_t i = 0; i < endpoints.size(); ++i) {
            endpoints[i]->close();
            delete endpoints[i];
        }
        endpoints.clear();
        endpoint_fds.clear();
    }

private:
    LocalServiceEntry(const LocalServiceEntry&);
    LocalServiceEntry& operator=(const LocalServiceEntry&);
};

struct ReconnectConfig {
    bool     enabled;
    uint32_t interval_ms;
    uint32_t max_retries;
    uint32_t current_retry;
    uint32_t timer_id;
    // 是否由用户显式 connectService 建立：仅由话题订阅建立的条目在最后一个
    // 订阅移除后可整体回收，避免为无人使用的连接无限重试
    bool     explicit_connect;
    // 话题发布者直连恢复状态：SM 下发的最新端点 + 该连接已发送订阅的话题。
    // SM 重连会 closeAll 清空数据面连接，成功重连后按此重放
    // MSG_SUBSCRIBE_BROADCAST（约束 6：恢复必须覆盖数据面）
    bool       has_topic_endpoint;
    ServiceInfo topic_endpoint;
    std::vector<std::string> topic_subscriptions;
    // 显式实例绑定：连接键非服务名时登记原始身份，SM 重连恢复 / 断线重连
    // 时据此经 listServiceInstances 重新解析目标实例（而非按名 lookup）
    bool        has_instance;
    std::string bound_service;
    std::string bound_host_id;
    std::string bound_instance_id;

    ReconnectConfig() : enabled(true), interval_ms(1000),
                        max_retries(0), current_retry(0), timer_id(0),
                        explicit_connect(false), has_topic_endpoint(false),
                        has_instance(false) {}
};

struct HeartbeatState {
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t timer_id;
    int64_t  last_ack_time;
    bool     pending;
    HeartbeatState() : interval_ms(5000), timeout_ms(10000),
                       timer_id(0), last_ack_time(0), pending(false) {}
};

/*
 * @brief  单个远端服务的全部本地状态，取代此前四张并行 map
 *         （service_cache_/death_callbacks_/reconnect_configs_/heartbeat_states_）
 * @note   约束 6：新增每服务状态必须在此统一登记与清理
 */
struct ServiceState {
    ServiceInfo     info;
    bool            has_info;
    DeathCallback   death_cb;
    bool            has_death;
    ReconnectConfig reconnect;
    bool            has_reconnect;
    HeartbeatState  heartbeat;
    bool            has_heartbeat;

    ServiceState()
        : has_info(false), has_death(false), has_reconnect(false), has_heartbeat(false) {}
};

/*
 * @brief  实例级死亡订阅的本地登记
 * @note   约束 6：SM 重连后按此重放实例级死亡订阅（与控制面名字级订阅同等恢复）
 */
struct InstanceDeathSubscription {
    std::string         name;
    std::string         host_id;
    std::string         instance_id;
    DeathSourceCallback callback;

    InstanceDeathSubscription() {}
};

/*
 * @brief  本运行时作为 watcher 的本地登记项
 * @note   约束 6：SM 重连后按此重放（控制面状态恢复）
 */
struct DiagWatcherEntry {
    std::string       topic_name;   // 目标 diag 数据话题名
    bool              active;       // SM 侧关联是否已确认；SM 重连后置 false 待重放
    DiagWatcherEntry() : active(false) {}
};

/*
 * @brief  诊断与统计状态聚合
 * @details 将原散落在 Impl 的诊断成员（统计、进程标识、diag 数据服务/watch 状态、
 *          watcher 登记表）收敛为单一成员，减少 Impl 的成员数量。
 * @note    约束 6：watcher 表是控制面恢复状态，SM 重连后必须重放
 */
struct DiagState {
    RuntimeStats stats;
    int          active_count;     // 已启用诊断的服务数（>0 才触发 BROADCAST 钩子）
    uint32_t     pid;              // 本进程 pid
    std::string  process_name;     // 本进程名（缺失时回退 pid-N）
    bool         watch_active;     // 本运行时作为 watcher 的本地事件发射开关
    Service*     data_service;     // 诊断专用本地服务（可能为 NULL）
    uint32_t     watch_topic_id;   // 本进程 diag 数据话题 id；0 表示未发布
    // SM 重连后按此重放（约束 6 控制面状态恢复）
    std::map<uint32_t, DiagWatcherEntry> watchers;

    DiagState()
        : active_count(0), pid(0), watch_active(false),
          data_service(NULL), watch_topic_id(0) {}
};

} // namespace omnibinder

#endif
