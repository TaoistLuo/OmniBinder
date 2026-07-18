/**************************************************************************************************
 * @file        omni_runtime.h
 * @brief       OmniRuntime 内部实现（Pimpl）
 * @details     Impl 作为单线程事件循环协调器，聚合控制面（ServiceManager 通信）、
 *              数据面（服务间直连）、入站请求分派、诊断等子系统。
 *              不进一步拆分子组件的原因：嵌入式单线程模型下，协调器需要全局视野；
 *              拆分会引入组件间通信开销，得不偿失。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
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
#include "transport/tcp_transport.h"
#include "transport/shm_transport.h"
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

// ============================================================
// 本地注册服务的上下文
// ============================================================
struct LocalServiceEntry {
    Service*            service;
    TcpTransportServer* server;
    ShmTransport*       shm_transport;
    uint16_t            port;

    std::map<int, ITransport*>  client_transports;
    std::map<int, Buffer*>      client_recv_buffers;
    std::set<int>               shm_client_notify_fds;

    bool     diag_enabled;
    uint32_t diag_topic_id;

    LocalServiceEntry()
        : service(NULL), server(NULL), shm_transport(NULL), port(0)
        , diag_enabled(false), diag_topic_id(0) {}

    ~LocalServiceEntry() {
        if (shm_transport) { shm_transport->close(); delete shm_transport; shm_transport = NULL; }
        for (std::map<int, ITransport*>::iterator it = client_transports.begin();
             it != client_transports.end(); ++it) {
            it->second->close(); delete it->second;
        }
        client_transports.clear();
        for (std::map<int, Buffer*>::iterator it = client_recv_buffers.begin();
             it != client_recv_buffers.end(); ++it) {
            delete it->second;
        }
        client_recv_buffers.clear();
        if (server) { server->close(); delete server; server = NULL; }
    }

private:
    LocalServiceEntry(const LocalServiceEntry&);
    LocalServiceEntry& operator=(const LocalServiceEntry&);
};

// ============================================================
// OmniRuntime::Impl — 单线程事件循环协调器
//
// 职责分层:
//   控制面 → 与 ServiceManager 交互（注册/发现/话题声明/死亡订阅）
//   数据面 → 服务间直连通（连接管理/RPC/心跳/广播）
//   托管   → 入站请求分派（TCP accept/SHM 请求/本地调用执行）
//   诊断   → 运行时可观测性（watch/统计/日志级别）
// ============================================================
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
    int publishTopic(const std::string& topic_name);
    int subscribeTopic(const std::string& topic_name, const TopicCallback& on_msg,
                       const TopicErrorCallback& on_err);
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
    const std::string& getRegisterHost() const;
    void setHeartbeatInterval(uint32_t ms);
    void setDefaultTimeout(uint32_t ms);
    const std::string& hostId() const;

    // ============================================================
    // 内部入口（仅由协调器自身调用，不经过 callSerialized）
    // ============================================================
    int registerServiceInternal(Service* service);
    int unregisterServiceInternal(Service* service);
    int listServicesInternal(std::vector<ServiceInfo>& services);
    int queryInterfacesInternal(const std::string& name, std::vector<InterfaceInfo>& ifaces);
    int queryPublishedTopicsInternal(const std::string& name, std::vector<std::string>& topics,
                                     uint32_t timeout_ms);
    int connectServiceInternal(const std::string& name);
    int disconnectServiceInternal(const std::string& name);
    int invokeInternal(const std::string& name, uint32_t iface_id, uint32_t method_id,
                       uint32_t idl_hash, const Buffer& req, Buffer& resp, uint32_t timeout_ms);
    int invokeOneWayInternal(const std::string& name, uint32_t iface_id, uint32_t method_id,
                             uint32_t idl_hash, const Buffer& req);
    int subscribeServiceDeathInternal(const std::string& name, const DeathCallback& cb);
    int unsubscribeServiceDeathInternal(const std::string& name);
    int publishTopicInternal(const std::string& topic_name);
    int broadcastInternal(uint32_t topic_id, const Buffer& data);
    int subscribeTopicInternal(const std::string& topic_name, const TopicCallback& cb);
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
        ReconnectConfig() : enabled(true), interval_ms(1000),
                            max_retries(0), current_retry(0), timer_id(0) {}
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

    // ============================================================
    // SM 协议 — 控制面底层通信
    // ============================================================
    bool sendToSM(const Message& msg);
    bool sendToSMWithinTimeout(const Message& msg, uint32_t timeout_ms, uint32_t* elapsed_ms);
    int  sendSMRequestAndWaitReply(Message& msg, Message& reply);
    int  sendSMRequestAndWaitReply(Message& msg, Message& reply, uint32_t timeout_ms);
    void onSMData(int fd, uint32_t events);
    void processSMMessages();
    void onSMMessage(const Message& msg);
    int  reconnectServiceManager();
    int  restoreControlPlaneState();
    int  reconnectServiceManagerIfNeeded();
    int  sendRuntimeHello();
    uint32_t allocSequence();
    int  waitForReply(uint32_t seq, uint32_t timeout_ms, Message& reply,
                      const std::function<bool()>& is_alive = std::function<bool()>());
    void storePendingReply(uint32_t seq, const Message& msg);

    // ============================================================
    // 入站请求分派 — TCP accept / SHM 请求 / 本地调用
    // ============================================================
    void onServiceAccept(const std::string& name, int listen_fd, uint32_t events);
    void onServiceClientData(const std::string& name, int client_fd, uint32_t events);
    void processServiceClientMessages(const std::string& name,
                                      LocalServiceEntry* entry, int client_fd);
    void onShmRequest(const std::string& name, LocalServiceEntry* entry,
                      uint32_t client_id, const uint8_t* data, size_t length);
    InvokeDispatchResult dispatchLocalInvoke(Service* service, const Message& msg,
                                              const char* transport, const char* svc_name);
    void onInvokeRequest(const std::string& name, int client_fd, const Message& msg,
                          const char* transport);
    void onInvokeOneWayRequest(const std::string& name, const Message& msg,
                                const char* transport);
    void onSubscribeBroadcast(int client_fd, const Message& msg, const char* transport);
    void onTopicBroadcastMessage(const Message& msg);
    void removeServiceClient(const std::string& name, int client_fd);

    // ============================================================
    // 数据面直连 — ConnectionManager 回调 / 消息发送
    // ============================================================
    void onDirectMessage(const std::string& name, const Message& msg);
    void onDirectDisconnect(const std::string& name);
    void sendHeartbeatToService(const std::string& name);
    void checkHeartbeatTimeout(const std::string& name);
    void sendHeartbeat();
    bool sendOnFd(ITransport* transport, const Message& msg);

    // ============================================================
    // 基础设施 — 线程模型 / 生命周期 / 工具
    // ============================================================
    template<typename F>
    typename std::result_of<F()>::type callSerialized(F func);
    bool isOwnerThread() const;
    void captureOwnerThread();
    uint32_t effectiveTimeout(uint32_t timeout_ms) const;
    void populateInvokeMessage(Message& msg, uint32_t iface_id, uint32_t method_id,
                                uint32_t idl_hash, const Buffer& req) const;
    int  lookupServiceInfo(const std::string& name, ServiceInfo& info);
    int  initializeServiceListener(LocalServiceEntry* entry, Service* service,
                                    std::string& advertise_host);
    void initializeServiceShm(const std::string& name, LocalServiceEntry* entry,
                              size_t req_cap, size_t resp_cap);
    int  registerServiceWithManager(const std::string& name, Service* service,
                                     LocalServiceEntry* entry, const std::string& advertise_host);
    void cleanupPendingServiceRegistration(LocalServiceEntry* entry);
    void removeServiceListenerFromLoop(LocalServiceEntry* entry);
    void removeServiceShmFromLoop(LocalServiceEntry* entry);
    std::string resolveRegisterHost(Service* service, const std::string& listener_host) const;
    std::string topicPublisherServiceName(const std::string& topic) const;
    bool ensureTopicPublisherConnection(const std::string& topic, const ServiceInfo& pub_info);
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

    // ============================================================
    // 成员变量 — 按子系统分组
    // ============================================================

    // 核心引擎
    EventLoop*          loop_;
    OwnerThreadExecutor owner_executor_;
    std::atomic<bool>   running_;
    bool                initialized_;
    OmniRuntime*        owner_;
    std::mutex          api_mutex_;
    std::atomic<bool>   loop_driver_active_;

    // 控制面
    SmControlChannel    sm_channel_;
    RpcRuntime          rpc_runtime_;
    std::atomic<bool>   sm_reconnect_needed_;
    std::string         sm_host_;
    uint16_t            sm_port_;
    std::string         host_id_;
    uint32_t            heartbeat_interval_ms_;
    uint32_t            heartbeat_timer_id_;

    // 数据面
    ConnectionManager*  conn_mgr_;
    TopicRuntime        topic_runtime_;
    std::string         register_host_;
    std::map<std::string, ServiceInfo>           service_cache_;
    std::map<std::string, DeathCallback>         death_callbacks_;
    std::map<std::string, ReconnectConfig>       reconnect_configs_;
    std::map<std::string, HeartbeatState>        heartbeat_states_;

    // 本地服务托管
    std::map<std::string, LocalServiceEntry*>    local_services_;
    std::map<int, std::string>                   listen_fd_to_service_;
    std::map<int, std::string>                   client_fd_to_service_;

    // 诊断
    RuntimeStats stats_;
    int          diag_active_count_;
    uint32_t     pid_;
    std::string  process_name_;
    bool         diag_watch_active_;
    Service*     diag_data_service_;
    uint32_t     diag_watch_topic_id_;

};

template<typename F>
typename std::result_of<F()>::type OmniRuntime::Impl::callSerialized(F func) {
    if (!owner_executor_.hasOwnerThread()) {
        std::lock_guard<std::mutex> lock(api_mutex_);
        return func();
    }
    return owner_executor_.invokeOnOwner(func);
}

} // namespace omnibinder

#endif
