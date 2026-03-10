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
#include <future>
#include <mutex>
#include <memory>
#include <type_traits>
#include <thread>
#include <sys/time.h>

namespace omnibinder {

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

    LocalServiceEntry()
        : service(NULL), server(NULL), shm_server(NULL), port(0) {}

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

    // --- 服务注册 ---
    int registerService(Service* service);
    int unregisterService(Service* service);

    // --- 服务发现 ---
    int lookupService(const std::string& service_name, ServiceInfo& info);
    int listServices(std::vector<ServiceInfo>& services);
    int queryInterfaces(const std::string& service_name,
                        std::vector<InterfaceInfo>& interfaces);

    // --- 远程调用 ---
    int invoke(const std::string& service_name, uint32_t interface_id,
               uint32_t method_id, const Buffer& request, Buffer& response,
               uint32_t timeout_ms);
    int invokeOneWay(const std::string& service_name, uint32_t interface_id,
                     uint32_t method_id, const Buffer& request);

    // --- 死亡通知 ---
    int subscribeServiceDeath(const std::string& service_name,
                              const DeathCallback& callback);
    int unsubscribeServiceDeath(const std::string& service_name);

    // --- 话题 ---
    int publishTopic(const std::string& topic_name);
    int broadcast(uint32_t topic_id, const Buffer& data);
    int subscribeTopic(const std::string& topic_name,
                       const TopicCallback& callback);
    int unsubscribeTopic(const std::string& topic_name);

    // --- 配置 ---
    void setHeartbeatInterval(uint32_t interval_ms);
    void setDefaultTimeout(uint32_t timeout_ms);
    const std::string& hostId() const;
    int getStats(RuntimeStats& stats);
    void resetStats();
    void setOwner(OmniRuntime* owner) { owner_ = owner; }

private:
    Impl(const Impl&);
    Impl& operator=(const Impl&);

    // --- SM 连接 ---
    bool sendToSM(const Message& msg);
    void onSMData(int fd, uint32_t events);
    void processSMMessages();
    void handleSMMessage(const Message& msg);

    // --- 同步等待 ---
    uint32_t allocSequence();
    int waitForReply(uint32_t seq, uint32_t timeout_ms, Message& reply);
    void storePendingReply(uint32_t seq, const Message& msg);

    // --- 本地服务 accept ---
    void onServiceAccept(const std::string& service_name, int listen_fd,
                         uint32_t events);
    void onServiceClientData(const std::string& service_name, int client_fd,
                             uint32_t events);
    void handleInvokeRequest(const std::string& service_name, int client_fd,
                             const Message& msg);
    void handleInvokeOneWayRequest(const std::string& service_name,
                                   const Message& msg);
    void handleSubscribeBroadcast(int client_fd, const Message& msg);
    void handleTopicBroadcastMessage(const Message& msg);
    void removeServiceClient(const std::string& service_name, int client_fd);

    // --- SHM 服务端处理 ---
    void pollShmServices();
    void handleShmRequest(const std::string& service_name, LocalServiceEntry* entry,
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
    template<typename F, typename Result>
    Result callSerializedImpl(F func, std::false_type);
    template<typename F, typename Result>
    void callSerializedImpl(F func, std::true_type);
    bool sendOnFd(ITransport* transport, const Message& msg);
    uint32_t effectiveTimeout(uint32_t timeout_ms) const;
    int lookupServiceUnlocked(const std::string& service_name, ServiceInfo& info);
    std::string topicPublisherServiceName(const std::string& topic_name) const;
    bool ensureTopicPublisherConnection(const std::string& topic_name, const ServiceInfo& pub_info);
    bool refreshServiceConnectionUnlocked(const std::string& service_name, ServiceInfo& info,
                                          ServiceConnection*& conn);
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

    TopicRuntime     topic_runtime_;

    std::string     host_id_;
    std::string     sm_host_;
    uint16_t        sm_port_;
    uint32_t        heartbeat_interval_ms_;
    uint32_t        heartbeat_timer_id_;
    std::atomic<bool> running_;
    bool            initialized_;
    OmniRuntime* owner_;
    std::thread::id owner_thread_id_;
    std::mutex      lifecycle_mutex_;
    std::mutex      api_mutex_;
    std::atomic<bool> loop_driver_active_;
    std::atomic<bool> sm_reconnect_needed_;
    RuntimeStats stats_;
    std::atomic<bool> run_loop_owned_;
};

template<typename F>
typename std::result_of<F()>::type OmniRuntime::Impl::callSerialized(F func) {
    typedef typename std::result_of<F()>::type Result;
    return callSerializedImpl<F, Result>(func, typename std::is_void<Result>::type());
}

template<typename F, typename Result>
Result OmniRuntime::Impl::callSerializedImpl(F func, std::false_type) {
    if (!loop_ || !run_loop_owned_.load() || isOwnerThread()) {
        std::lock_guard<std::mutex> lock(api_mutex_);
        return func();
    }

    std::shared_ptr<std::packaged_task<Result()> > task(
        new std::packaged_task<Result()>(func));
    std::future<Result> future = task->get_future();
    loop_->post([task]() {
        (*task)();
    });
    return future.get();
}

template<typename F, typename Result>
void OmniRuntime::Impl::callSerializedImpl(F func, std::true_type) {
    if (!loop_ || !run_loop_owned_.load() || isOwnerThread()) {
        std::lock_guard<std::mutex> lock(api_mutex_);
        func();
        return;
    }

    std::shared_ptr<std::packaged_task<void()> > task(
        new std::packaged_task<void()>(func));
    std::future<void> future = task->get_future();
    loop_->post([task]() {
        (*task)();
    });
    future.get();
}

} // namespace omnibinder

#endif // OMNIBINDER_BINDER_CLIENT_IMPL_H
