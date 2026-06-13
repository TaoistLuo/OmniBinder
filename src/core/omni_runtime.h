/**************************************************************************************************
 * @file        omni_runtime.h
 * @brief       OmniRuntime 内部实现
 * @details     OmniRuntime::Impl 的完整定义（Pimpl 模式），包含与 ServiceManager 的
 *              控制连接、本地服务管理（LocalServiceEntry）、同步 RPC 等待机制、
 *              服务信息缓存、死亡回调、话题回调以及 SHM 事件驱动通信相关状态和逻辑。
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
#include "core/service_host_runtime.h"
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
    ShmTransport*       shm_server;   // 共享内存传输（服务端侧，多客户端共享）
    std::string         shm_name;     // SHM 名称
    uint16_t            port;

    // fd -> accepted client transport (incoming invoke connections via TCP)
    std::map<int, ITransport*>  client_transports;
    // fd -> recv buffer for each accepted client
    std::map<int, Buffer*>      client_recv_buffers;

    bool     diag_enabled;
    uint32_t diag_topic_id;

    LocalServiceEntry()
        : service(NULL), server(NULL), shm_server(NULL), port(0)
        , diag_enabled(false), diag_topic_id(0)
        {}

    ~LocalServiceEntry() {
        for (std::map<int, ITransport*>::iterator it = client_transports.begin();
             it != client_transports.end(); ++it) {
            it->second->close();
            delete it->second;
        }
        client_transports.clear();

        for (std::map<int, Buffer*>::iterator it = client_recv_buffers.begin();
             it != client_recv_buffers.end(); ++it) {
            delete it->second;
        }
        client_recv_buffers.clear();

        if (server) {
            server->close();
            delete server;
            server = NULL;
        }

        if (shm_server) {
            shm_server->close();
            delete shm_server;
            shm_server = NULL;
        }
    }

private:
    LocalServiceEntry(const LocalServiceEntry&);
    LocalServiceEntry& operator=(const LocalServiceEntry&);
};

class OmniRuntime::Impl {
public:
    Impl();
    ~Impl();

    // --- 核心生命周期 ---
    int init(const std::string& sm_host, uint16_t sm_port);
    void run();
    void stop();
    bool isRunning() const;
    void pollOnce(int timeout_ms);
    void pollOnceWithoutFunctors(int timeout_ms);

    // --- 服务注册 ---
    int registerService(Service* service);
    int unregisterService(Service* service);
    int registerServiceInternal(Service* service);
    int unregisterServiceInternal(Service* service);

    // --- 服务发现 ---
    int lookupService(const std::string& service_name, ServiceInfo& info);
    int listServices(std::vector<ServiceInfo>& services);
    int queryInterfaces(const std::string& service_name,
                        std::vector<InterfaceInfo>& interfaces);
    int listServicesInternal(std::vector<ServiceInfo>& services);
    int queryInterfacesInternal(const std::string& service_name,
                                std::vector<InterfaceInfo>& interfaces);

    // --- 连接管理 ---
    int connectService(const std::string& service_name);
    int disconnectService(const std::string& service_name);
    bool isServiceConnected(const std::string& service_name);
    void enableAutoReconnect(const std::string& service_name, bool enable);
    void setReconnectInterval(const std::string& service_name, uint32_t interval_ms);
    void startHeartbeat(const std::string& service_name, uint32_t interval_ms, uint32_t timeout_ms);
    void stopHeartbeat(const std::string& service_name);
    int connectServiceInternal(const std::string& service_name);
    int disconnectServiceInternal(const std::string& service_name);
    void tryReconnectService(const std::string& service_name);
    void scheduleReconnect(const std::string& service_name, uint32_t delay_ms);
    void sendHeartbeatToService(const std::string& service_name);
    void checkHeartbeatTimeout(const std::string& service_name);

    // --- 远程调用 ---
    int invoke(const std::string& service_name, uint32_t interface_id,
               uint32_t method_id, uint32_t idl_hash, const Buffer& request,
               Buffer& response, uint32_t timeout_ms);
    int invokeOneWay(const std::string& service_name, uint32_t interface_id,
                     uint32_t method_id, uint32_t idl_hash, const Buffer& request);
    int invokeInternal(const std::string& service_name, uint32_t interface_id,
                       uint32_t method_id, uint32_t idl_hash, const Buffer& request,
                       Buffer& response, uint32_t timeout_ms);
    int invokeOneWayInternal(const std::string& service_name, uint32_t interface_id,
                             uint32_t method_id, uint32_t idl_hash, const Buffer& request);

    // --- 死亡通知 ---
    int subscribeServiceDeath(const std::string& service_name,
                              const DeathCallback& callback);
    int unsubscribeServiceDeath(const std::string& service_name);
    int subscribeServiceDeathInternal(const std::string& service_name,
                                      const DeathCallback& callback);
    int unsubscribeServiceDeathInternal(const std::string& service_name);

    // --- 话题 ---
    int publishTopic(const std::string& topic_name);
    int broadcast(uint32_t topic_id, const Buffer& data);
    int subscribeTopic(const std::string& topic_name, const TopicCallback& on_message,
                       const TopicErrorCallback& on_error);
    int unsubscribeTopic(const std::string& topic_name);
    int publishTopicInternal(const std::string& topic_name);
    int broadcastInternal(uint32_t topic_id, const Buffer& data);
    int subscribeTopicInternal(const std::string& topic_name,
                               const TopicCallback& callback);
    int unsubscribeTopicInternal(const std::string& topic_name);

    // --- 配置 ---
    void setRegisterHost(const std::string& host);
    const std::string& getRegisterHost() const;
    void setHeartbeatInterval(uint32_t interval_ms);
    void setDefaultTimeout(uint32_t timeout_ms);
    const std::string& hostId() const;
    int getStats(RuntimeStats& stats);
    int resetStats();
    int getStatsInternal(RuntimeStats& stats);
    int resetStatsInternal();
    void clearServiceCache();
    void closeAllConnections();
    void setOwner(OmniRuntime* owner) { owner_ = owner; }

    int enableDiagnostic(const std::string& service_name);
    int disableDiagnostic(const std::string& service_name);

private:
    Impl(const Impl&);
    Impl& operator=(const Impl&);

    // --- SM 连接 ---
    bool sendToSM(const Message& msg);
    bool sendToSMWithinTimeout(const Message& msg, uint32_t timeout_ms,
                               uint32_t* elapsed_ms);
    int sendSMRequestAndWaitReply(Message& msg, Message& reply);
    void onSMData(int fd, uint32_t events);
    void processSMMessages();
    void onSMMessage(const Message& msg);

    // --- 同步等待 ---
    uint32_t allocSequence();
    int waitForReply(uint32_t seq, uint32_t timeout_ms, Message& reply);
    void storePendingReply(uint32_t seq, const Message& msg);

    // --- 本地服务 accept ---
    void onServiceAccept(const std::string& service_name, int listen_fd,
                         uint32_t events);
    void onServiceClientData(const std::string& service_name, int client_fd,
                             uint32_t events);
    void onInvokeRequest(const std::string& service_name, int client_fd,
                          const Message& msg, const char* transport_label);
    void onInvokeOneWayRequest(const std::string& service_name,
                                const Message& msg, const char* transport_label);
    void onSubscribeBroadcast(int client_fd, const Message& msg,
                               const char* transport_label);
    void onTopicBroadcastMessage(const Message& msg);
    void removeServiceClient(const std::string& service_name, int client_fd);

    InvokeDispatchResult dispatchLocalInvoke(Service* service, const Message& msg,
                                             const char* transport_label,
                                             const char* service_name);

    void onShmRequest(const std::string& service_name, LocalServiceEntry* entry,
                          uint32_t client_id, const uint8_t* data, size_t length);

    // --- ConnectionManager 回调 ---
    void onDirectMessage(const std::string& service_name, const Message& msg);
    void onDirectDisconnect(const std::string& service_name);

    // --- 心跳 ---
    void sendHeartbeat();

    // --- 辅助 ---
    bool isOwnerThread() const;
    void captureOwnerThread();
    template<typename F>
    typename std::result_of<F()>::type callSerialized(F func);
    bool sendOnFd(ITransport* transport, const Message& msg);
    uint32_t effectiveTimeout(uint32_t timeout_ms) const;
    int initializeServiceListener(LocalServiceEntry* entry, Service* service,
                                  std::string& advertise_host);
    std::string resolveRegisterHost(Service* service,
                                    const std::string& listener_host) const;
    void initializeServiceShm(const std::string& name, LocalServiceEntry* entry,
                              size_t req_ring_capacity, size_t resp_ring_capacity);
    int registerServiceWithManager(const std::string& name, Service* service,
                                   LocalServiceEntry* entry, const std::string& advertise_host,
                                   size_t req_ring_capacity, size_t resp_ring_capacity);
    void cleanupPendingServiceRegistration(LocalServiceEntry* entry);
    void removeServiceListenerFromLoop(LocalServiceEntry* entry);
    void removeServiceShmFromLoop(LocalServiceEntry* entry);
    void populateInvokeMessage(Message& msg, uint32_t interface_id, uint32_t method_id,
                                uint32_t idl_hash, const Buffer& request) const;
    int lookupServiceInfo(const std::string& service_name, ServiceInfo& info);
    std::string topicPublisherServiceName(const std::string& topic_name) const;
    bool ensureTopicPublisherConnection(const std::string& topic_name, const ServiceInfo& pub_info);
    int reconnectServiceManager();
    int restoreControlPlaneState();
    int reconnectServiceManagerIfNeeded();
    void updateConnectionStats(RuntimeStats& stats) const;

    // ============================================================
    // 成员变量
    // ============================================================

    EventLoop*          loop_;
    SmControlChannel    sm_channel_;
    RpcRuntime          rpc_runtime_;
    ServiceHostRuntime  service_host_runtime_;
    ConnectionManager*  conn_mgr_;

    // 本地注册的服务: service_name -> LocalServiceEntry*
    std::map<std::string, LocalServiceEntry*> local_services_;

    // fd -> service_name 反向映射（用于 accept 和 client data 回调）
    std::map<int, std::string> listen_fd_to_service_;
    std::map<int, std::string> client_fd_to_service_;

    // 服务信息缓存: service_name -> ServiceInfo
    std::map<std::string, ServiceInfo> service_cache_;

    // 死亡回调: service_name -> DeathCallback
    std::map<std::string, DeathCallback> death_callbacks_;

    // 自动重连配置
    struct ReconnectConfig {
        bool enabled;
        uint32_t interval_ms;
        uint32_t max_retries;
        uint32_t current_retry;
        uint32_t timer_id;
        
        ReconnectConfig() : enabled(true), interval_ms(1000), 
                           max_retries(0), current_retry(0), timer_id(0) {}
    };
    std::map<std::string, ReconnectConfig> reconnect_configs_;

    struct HeartbeatState {
        uint32_t interval_ms;
        uint32_t timeout_ms;
        uint32_t timer_id;
        int64_t last_ack_time;
        bool pending;
        
        HeartbeatState() : interval_ms(5000), timeout_ms(10000), 
                          timer_id(0), last_ack_time(0), pending(false) {}
    };
    std::map<std::string, HeartbeatState> heartbeat_states_;

    TopicRuntime     topic_runtime_;

    std::string     host_id_;
    std::string     sm_host_;
    std::string     register_host_;
    uint16_t        sm_port_;
    uint32_t        heartbeat_interval_ms_;
    uint32_t        heartbeat_timer_id_;
    std::atomic<bool> running_;
    bool            initialized_;
    OmniRuntime* owner_;
    std::mutex      api_mutex_;
    std::atomic<bool> loop_driver_active_;
    std::atomic<bool> sm_reconnect_needed_;
    RuntimeStats stats_;
    OwnerThreadExecutor owner_executor_;
    int diag_active_count_;
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

#endif // OMNIBINDER_BINDER_CLIENT_IMPL_H
