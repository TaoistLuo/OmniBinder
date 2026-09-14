/**************************************************************************************************
 * @file        omni_runtime.h
 * @brief       OmniRuntime 内部实现（Pimpl）
 * @details     Impl 作为单线程事件循环协调器，聚合控制面（ServiceManager 通信）、
 *              数据面（服务间直连）、入站请求分派、诊断等子系统。
 *              不拆分为独立组件对象（嵌入式单线程模型下协调器需要全局视野）；
 *              实现体按域拆分到 runtime_*.cpp，成员与状态仍集中在 Impl。
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
#ifndef OMNIBINDER_BINDER_CLIENT_IMPL_H
#define OMNIBINDER_BINDER_CLIENT_IMPL_H

#include "omnibinder/runtime.h"
#include "omnibinder/service.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include "core/event_loop.h"
#include "core/owner_thread_executor.h"
#include "core/connection_manager.h"
#include "core/sm_control_channel.h"
#include "core/rpc_runtime.h"
#include "core/topic_runtime.h"
#include "platform/platform.h"

#include <string>
#include <map>
#include <vector>
#include <cstring>
#include <functional>
#include <atomic>
#include <mutex>
#include <memory>
#include <type_traits>
#include <thread>
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

/*
 * @brief  OmniRuntime::Impl — 单线程事件循环协调器
 * @details 职责分层：
 *            控制面 → 与 ServiceManager 交互（注册/发现/话题声明/死亡订阅）
 *            数据面 → 服务间直连通（连接管理/RPC/心跳/广播）
 *            托管   → 入站请求分派（TCP accept/SHM 请求/本地调用执行）
 *            诊断   → 运行时可观测性（watch/统计/日志级别）
 */
class OmniRuntime::Impl {
public:
    Impl();
    ~Impl();

    // ============================================================
    // 生命周期
    // ============================================================
    int  init(const std::string& sm_host, uint16_t sm_port);
    void run();
    void stop();
    bool isRunning() const;
    void pollOnce(int timeout_ms);
    void pollOnceWithoutFunctors(int timeout_ms);
    void setOwner(OmniRuntime* owner) { owner_ = owner; }

    // ============================================================
    // 控制面 — 与 ServiceManager 交互
    // ============================================================

    // 服务注册/注销
    int registerService(Service* service);
    int unregisterService(Service* service);

    // 服务发现
    int lookupService(const std::string& name, ServiceInfo& info);
    int listServices(std::vector<ServiceInfo>& services);
    int queryInterfaces(const std::string& name, std::vector<InterfaceInfo>& ifaces);
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics);
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics,
                             uint32_t timeout_ms);

    // 话题声明（向 SM 注册发布/订阅关系）
    int publishTopic(const std::string& topic_name, uint32_t idl_hash);
    int subscribeTopic(const std::string& topic_name, uint32_t expected_idl_hash,
                       const TopicCallback& on_msg, const TopicErrorCallback& on_err);
    int unsubscribeTopic(const std::string& topic_name);

    // 死亡通知
    int subscribeServiceDeath(const std::string& name, const DeathCallback& cb);
    int unsubscribeServiceDeath(const std::string& name);

    // ============================================================
    // 数据面 — 服务间直连通信
    // ============================================================

    // 连接管理
    int  connectService(const std::string& name);
    int  disconnectService(const std::string& name);
    bool isServiceConnected(const std::string& name);
    void enableAutoReconnect(const std::string& name, bool enable);
    void setReconnectInterval(const std::string& name, uint32_t interval_ms);

    // 心跳
    void startHeartbeat(const std::string& name, uint32_t interval_ms, uint32_t timeout_ms);
    void stopHeartbeat(const std::string& name);
    void pauseHeartbeat(const std::string& name);
    void resumeHeartbeat(const std::string& name);

    // RPC 调用
    int invoke(const std::string& name, uint32_t iface_id, uint32_t method_id,
               uint32_t idl_hash, const Buffer& req, Buffer& resp, uint32_t timeout_ms);
    int invokeOneWay(const std::string& name, uint32_t iface_id, uint32_t method_id,
                     uint32_t idl_hash, const Buffer& req);

    // 话题广播（数据面直连，不经过 SM）
    int broadcast(uint32_t topic_id, const Buffer& data);

    // ============================================================
    // 诊断 — 运行时可观测性
    // ============================================================
    int  enableDiagnostic(const std::string& service_name);
    int  disableDiagnostic(const std::string& service_name);
    int  setLogLevelByPid(uint32_t pid, uint32_t level);
    int  listRuntimes(std::vector<RuntimeInfo>& runtimes);
    int  watchPid(uint32_t pid, const DiagEventCallback& callback);
    int  unwatchPid(uint32_t pid);
    int  getStats(RuntimeStats& stats);
    int  resetStats();
    void clearServiceCache();
    void closeAllConnections();

    // ============================================================
    // 配置
    // ============================================================
    void setRegisterHost(const std::string& host);
    std::string getRegisterHost() const;
    void setHeartbeatInterval(uint32_t ms);
    void setDefaultTimeout(uint32_t ms);
    void setReplySendTimeout(uint32_t ms);
    std::string hostId() const;

private:
    // ============================================================
    // 内部入口（仅由协调器自身调用，不经过 callSerialized）
    // ============================================================
    int registerServiceInternal(Service* service);
    int unregisterServiceInternal(Service* service);
    int listServicesInternal(std::vector<ServiceInfo>& services);
    int queryInterfacesInternal(const std::string& name, std::vector<InterfaceInfo>& ifaces);
    int queryPublishedTopicsInternal(const std::string& name, std::vector<std::string>& topics,
                                     uint32_t timeout_ms);
    int connectServiceInternal(const std::string& name, bool explicit_connect = true);
    int disconnectServiceInternal(const std::string& name);
    int invokeInternal(const std::string& name, uint32_t iface_id, uint32_t method_id,
                       uint32_t idl_hash, const Buffer& req, Buffer& resp, uint32_t timeout_ms);
    int invokeOneWayInternal(const std::string& name, uint32_t iface_id, uint32_t method_id,
                             uint32_t idl_hash, const Buffer& req);
    int subscribeServiceDeathInternal(const std::string& name, const DeathCallback& cb);
    int unsubscribeServiceDeathInternal(const std::string& name);
    int publishTopicInternal(const std::string& topic_name, uint32_t idl_hash);
    int broadcastInternal(uint32_t topic_id, const Buffer& data);
    int subscribeTopicInternal(const std::string& topic_name, uint32_t expected_idl_hash,
                               const TopicCallback& cb);
    int unsubscribeTopicInternal(const std::string& topic_name);
    int getStatsInternal(RuntimeStats& stats);
    int resetStatsInternal();

private:
    Impl(const Impl&);
    Impl& operator=(const Impl&);

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

        ReconnectConfig() : enabled(true), interval_ms(1000),
                            max_retries(0), current_retry(0), timer_id(0),
                            explicit_connect(false), has_topic_endpoint(false) {}
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

    ServiceState* findServiceState(const std::string& name);
    ServiceState& ensureServiceState(const std::string& name);
    void eraseServiceStateIfUnused(const std::string& name);

    // ============================================================
    // SM 协议 — 控制面底层通信
    // ============================================================
    bool sendToSM(const Message& msg);
    bool sendToSMWithinTimeout(const Message& msg, uint32_t timeout_ms, uint32_t* elapsed_ms);
    int  sendSMRequestAndWaitReply(Message& msg, Message& reply);
    int  sendSMRequestAndWaitReply(Message& msg, Message& reply, uint32_t timeout_ms);
    void onSMData(int fd, uint32_t events);
    void onSMMessage(Message& msg);
    void handleDeathNotify(const Message& msg);
    void handleTopicPublisherNotify(const Message& msg);
    int  reconnectServiceManager();
    void dropServiceManagerConnection();
    int  restoreControlPlaneState();
    int  restoreRegisterService(const std::string& name);
    int  restoreSubscribeDeath(const std::string& name);
    int  restorePublishTopic(const std::string& topic, const std::string& owner,
                             uint32_t idl_hash);
    int  restoreSubscribeTopic(const std::string& topic, uint32_t expected_idl_hash);
    int  reconnectServiceManagerIfNeeded();
    int  sendRuntimeHello();
    uint32_t allocSequence();
    /*
     * @brief  控制面等待：使用 SmControlChannel 的 pending reply 槽表
     */
    int  waitForReply(uint32_t seq, uint32_t timeout_ms, Message& reply,
                      const std::function<bool()>& is_alive = std::function<bool()>());
    /*
     * @brief  数据面等待：使用 RpcRuntime 独立槽表，SM 重连不影响在途 RPC
     */
    int  waitForDataReply(uint32_t seq, uint32_t timeout_ms, Message& reply,
                          const std::function<bool()>& is_alive = std::function<bool()>());
    bool storeAndConsumeControlReply(uint32_t seq, Message& msg);
    bool storeAndConsumeDataReply(uint32_t seq, Message& msg);

    /*
     * @brief  判活：服务是否仍注册且 entry 未被回调链释放（约束 1 的通用检查）
     */
    bool isEntryAlive(const std::string& service_name, LocalServiceEntry* entry) const;

    // ============================================================
    // 入站请求分派 — 端点事件 / TCP 流 / SHM 帧 / 本地调用
    // ============================================================
    void wireServiceEndpoint(const std::string& name, LocalServiceEntry* entry,
                             IServerEndpoint* endpoint);
    void syncEndpointFds(const std::string& name, LocalServiceEntry* entry);
    void onServiceEndpointEvent(const std::string& name, int fd, uint32_t events);
    void onServiceClientAccepted(const std::string& name, int client_id,
                                 IMessageConnection* client);
    void onServiceClientReadable(const std::string& name, int client_id);
    void onServiceClientDisconnected(const std::string& name, int client_id);
    void handleServiceClientMessage(const std::string& name, int client_id,
                                    const Message& msg);
    /*
     * @brief  广播消息解码 + 诊断事件 + 本地回调分发；客户端直连路径与服务端防御
     *         分支共用，保证两条路径行为一致
     * @return false 表示 payload 解码失败
     */
    bool dispatchBroadcastMessage(const Message& msg);
    InvokeDispatchResult dispatchLocalInvoke(Service* service, const Message& msg,
                                              const char* transport, const char* svc_name);
    InvokeDispatchResult dispatchDiagInvoke(Service* service, const char* service_name,
                                            const Buffer& request);
    void onInvokeRequest(const std::string& name, int client_id, const Message& msg,
                          const char* transport);
    void onInvokeOneWayRequest(const std::string& name, const Message& msg,
                                const char* transport);

    // ============================================================
    // 数据面直连 — ConnectionManager 回调 / 消息发送
    // ============================================================
    void onDirectMessage(const std::string& name, Message& msg);
    void onDirectDisconnect(const std::string& name);
    bool handleServiceLost(const std::string& name);
    void sendHeartbeatToService(const std::string& name);
    void checkHeartbeatTimeout(const std::string& name);
    void sendHeartbeat();
    bool sendOnFd(IMessageConnection* transport, Message& msg);
    bool sendRawOnFd(IMessageConnection* transport, const uint8_t* data, size_t size);
    /*
     * @brief  发送可丢弃报文（如心跳 ACK）
     * @note   timeout=0 只尝试一次，不占用 owner 发送预算
     */
    bool sendOnFdBestEffort(IMessageConnection* transport, Message& msg);

    // ============================================================
    // 基础设施 — 线程模型 / 生命周期 / 工具
    // ============================================================
    template<typename F>
    typename std::result_of<F()>::type callSerialized(F func);
    /*
     * @brief  callSerialized 的 owner 投递段：post 被拒绝（loop 已停止）时快速失败
     * @note   返回 FailFastResult（int → ERR_NOT_RUNNING；bool → false；其余默认构造）
     */
    template<typename F>
    typename std::enable_if<!std::is_void<typename std::result_of<F()>::type>::value,
                            typename std::result_of<F()>::type>::type
    invokeSerializedOnOwner(F func);
    template<typename F>
    typename std::enable_if<std::is_void<typename std::result_of<F()>::type>::value, void>::type
    invokeSerializedOnOwner(F func);
    uint32_t effectiveTimeout(uint32_t timeout_ms) const;
    bool populateInvokeMessage(Message& msg, uint32_t iface_id, uint32_t method_id,
                                uint32_t idl_hash, const Buffer& req) const;
    int  lookupServiceInfo(const std::string& name, ServiceInfo& info);
    int  initializeServiceListener(LocalServiceEntry* entry, Service* service,
                                    std::string& advertise_host);
    void initializeServiceShm(const std::string& name, LocalServiceEntry* entry,
                              size_t req_cap, size_t resp_cap);
    int  registerServiceWithManager(const std::string& name, Service* service,
                                     LocalServiceEntry* entry, const std::string& advertise_host);
    /*
     * @brief  向 ServiceManager 发送注册请求并等待应答
     * @param[in]  name           服务名
     * @param[in]  service        服务对象（读取 shm 配置与接口信息）
     * @param[in]  port           服务数据面监听端口
     * @param[in]  advertise_host 注册上报地址（其它服务据此直连）
     * @param[out] out_handle     注册成功时回填 SM 分配的句柄，可为 NULL
     * @return 0 成功；否则为 ERR_SEND_FAILED/ERR_TIMEOUT/ERR_DESERIALIZE/ERR_REGISTER_FAILED 等
     * @note   首次注册与重连恢复共用同一实现，避免控制面语义漂移（约束 6）
     */
    int  sendRegisterToManager(const std::string& name, Service* service, uint16_t port,
                               const std::string& advertise_host, ServiceHandle* out_handle);
    void cleanupPendingServiceRegistration(const std::string& name, LocalServiceEntry* entry);
    /*
     * @brief  将 entry 下所有已接入客户端从运行时路由与话题订阅中摘除
     * @param[in] name  服务名（SHM 订阅按 服务名 + client id 索引）
     * @param[in] entry 本地服务上下文，可为 NULL
     * @note   服务注销、注册失败清理、析构三条路径共用；只清路由，
     *         客户端 fd 的 event-loop 摘除由调用方 removeServiceEndpointsFromLoop 保证（约束 3）
     */
    void detachClientsFromEntry(const std::string& name, LocalServiceEntry* entry);
    void removeServiceEndpointsFromLoop(LocalServiceEntry* entry);
    std::string resolveRegisterHost(Service* service, const std::string& listener_host) const;
    /*
     * @brief  本地服务 TCP 监听对外公布的地址（端点抽象不暴露监听 fd，按通配绑定规范化）
     */
    std::string listenerAdvertiseHost() const;
    std::string topicPublisherServiceName(const std::string& topic) const;
    bool ensureTopicPublisherConnection(const std::string& topic, const ServiceInfo& pub_info);
    /*
     * @brief  发送单个话题订阅（MSG_SUBSCRIBE_BROADCAST）；初始订阅与重连重放共用
     */
    bool sendTopicBroadcastSubscription(const std::string& pub_name,
                                        const std::string& topic_name);
    /*
     * @brief  重连成功后按绑定列表重放订阅，避免发布者已移除订阅者导致广播永久丢失
     */
    bool resubscribeTopicPublisher(const std::string& pub_name,
                                   const std::vector<std::string>& topics);
    /*
     * @brief  取消本地订阅时同步移除连接绑定；仅话题建立的条目在清空后整体回收
     * @param[in] keep_pub_name 非空时保留该发布者上的绑定（发布者切换场景）
     */
    void removeTopicSubscriptionBinding(const std::string& topic_name,
                                        const std::string& keep_pub_name = std::string());
    void tryReconnectService(const std::string& name);
    void scheduleReconnect(const std::string& name, uint32_t delay_ms);
    void updateConnectionStats(RuntimeStats& stats) const;

    // ============================================================
    // 诊断辅助
    // ============================================================
    std::string runtimeProcessName() const;
    std::string diagDataServiceName(uint32_t pid) const;
    bool initDiagDataService();
    void destroyDiagDataService();
    bool isDiagDataTopic(uint32_t topic_id) const;
    void emitDiagEvent(uint8_t direction, const Message& msg);
    /*
     * @brief  按服务转发诊断事件到其专属 diag topic（entry 已判活时调用）
     */
    void emitDiagHook(LocalServiceEntry* entry, uint8_t direction, const Message& msg);

    /*
     * @brief  向 SM 发起对目标 pid 的 MSG_DIAG_WATCH_START 并等待应答
     * @note   首次 watch 与 SM 重连重放共用
     */
    int  requestDiagWatch(uint32_t pid);
    /*
     * @brief  SM 重连恢复：重放所有 active==false 的 watcher（幂等）
     */
    int  restoreDiagWatchers();
    /*
     * @brief  心跳周期兜底重放：目标 pid 重连晚于 watcher 时，watch 重放可能先失败
     */
    void retryInactiveDiagWatchers();

    // ============================================================
    // 成员变量 — 按子系统分组
    // ============================================================

    // 核心引擎
    EventLoop*          loop_;
    OwnerThreadExecutor owner_executor_;
    std::atomic<bool>   running_;
    std::atomic<bool>   loop_alive_;      // event-loop 是否仍会处理 functor（stop 后置 false）
    bool                initialized_;
    OmniRuntime*        owner_;
    mutable std::recursive_mutex api_mutex_;  // recursive：回调链内重入 API 需可重入加锁
    std::atomic<bool>   loop_driver_active_;

    // 控制面
    SmControlChannel    sm_channel_;
    RpcRuntime          rpc_runtime_;
    std::atomic<bool>   sm_reconnect_needed_;
    bool                restoring_ = false;   // 重连恢复流程重入保护
    std::string         sm_host_;
    uint16_t            sm_port_;
    std::string         host_id_;
    uint32_t            heartbeat_interval_ms_;
    uint32_t            heartbeat_timer_id_;

    // 数据面
    ConnectionManager*  conn_mgr_;
    TopicRuntime        topic_runtime_;
    std::string         register_host_;
    std::map<std::string, ServiceState>          services_;
    uint32_t                                     reply_send_timeout_ms_;

    // 本地服务托管
    std::map<std::string, LocalServiceEntry*>    local_services_;
    std::map<int, std::string>                   client_id_to_service_;

    // 诊断
    struct DiagWatcherEntry {
        DiagEventCallback callback;     // 用户回调（本地订阅已持有副本，此处用于登记）
        std::string       topic_name;   // 目标 diag 数据话题名
        bool              active;       // SM 侧关联是否已确认；SM 重连后置 false 待重放
        DiagWatcherEntry() : active(false) {}
    };

    RuntimeStats stats_;
    int          diag_active_count_;
    uint32_t     pid_;
    std::string  process_name_;
    bool         diag_watch_active_;
    Service*     diag_data_service_;
    uint32_t     diag_watch_topic_id_;
    // 本运行时作为 watcher 的本地登记：SM 重连后按此重放（约束 6 控制面状态恢复）
    std::map<uint32_t, DiagWatcherEntry> diag_watchers_;

};

// ============================================================
// callSerialized — 串行化入口
// ============================================================

/*
 * @brief  loop 已停止时 callSerialized 的快速失败返回值
 * @details int → ERR_NOT_RUNNING；bool → false；其余类型 → 默认构造
 */
template<typename T>
struct FailFastResult {
    static T value() { return T(); }
};

template<>
struct FailFastResult<int> {
    static int value() { return static_cast<int>(ErrorCode::ERR_NOT_RUNNING); }
};

template<>
struct FailFastResult<bool> {
    static bool value() { return false; }
};

template<typename F>
typename std::result_of<F()>::type OmniRuntime::Impl::callSerialized(F func) {
    typedef typename std::result_of<F()>::type Result;

    // stop 为终态：即使在 owner 认领前（init 后、run/pollOnce 前）也不能内联
    // 执行业务逻辑，必须快速失败（约束 5）。
    if (loop_ != NULL && loop_->stopRequested()) {
        return FailFastResult<Result>::value();
    }

    if (!owner_executor_.hasOwnerThread()) {
        // 无 owner 时的内联执行路径（含无 driver 的多线程模式）。
        // 与 run()/pollOnce() 的 owner 认领共用 api_mutex_：
        // 等待锁期间若 driver 线程已认领 owner，则改为投递，避免
        // "判定无 owner → 内联执行" 与 "另一线程认领 owner 并驱动 loop" 并发（TOCTOU）
        std::lock_guard<std::recursive_mutex> lock(api_mutex_);
        if (!owner_executor_.hasOwnerThread()) {
            return func();
        }
        // owner 已出现：释放锁后走 owner 投递路径
    }

    // 非 owner 线程且 loop 已停止（stop 之后）：投递到 event-loop 的 functor
    // 永远不会被处理，等待会永久阻塞。这里快速失败返回错误码（int）/默认值（bool）。
    if (!loop_alive_.load()) {
        OMNI_LOG_WARN("OmniRuntime", "callSerialized rejected: event-loop not running");
        return FailFastResult<Result>::value();
    }

    // EventLoop::post 在 stop 关闭后拒绝投递，且关闭动作先于 run() 退出排空，
    // 关闭 check 与 post 之间 stop 完成的窗口不会造成永久阻塞
    return invokeSerializedOnOwner<F>(func);
}

template<typename F>
typename std::enable_if<!std::is_void<typename std::result_of<F()>::type>::value,
                        typename std::result_of<F()>::type>::type
OmniRuntime::Impl::invokeSerializedOnOwner(F func) {
    typedef typename std::result_of<F()>::type Result;
    Result out = Result();
    if (owner_executor_.tryInvokeOnOwner(func, out)) {
        return out;
    }
    OMNI_LOG_WARN("OmniRuntime", "callSerialized post rejected: event-loop stopped");
    return FailFastResult<Result>::value();
}

template<typename F>
typename std::enable_if<std::is_void<typename std::result_of<F()>::type>::value, void>::type
OmniRuntime::Impl::invokeSerializedOnOwner(F func) {
    if (!owner_executor_.tryInvokeOnOwner(func)) {
        OMNI_LOG_WARN("OmniRuntime", "callSerialized post rejected: event-loop stopped");
    }
}

} // namespace omnibinder

#endif
