/**************************************************************************************************
 * @file        service_manager_app.h
 * @brief       ServiceManager 应用类
 * @details     ServiceManager 进程的核心协调类。管理 TCP 监听、客户端连接生命周期、消息分发、心跳检测、服务状态 watch 和话题管理。基于 EventLoop 的单线程事件驱动模型。
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
#ifndef OMNIBINDER_SERVICE_MANAGER_APP_H
#define OMNIBINDER_SERVICE_MANAGER_APP_H

#include "omnibinder/types.h"
#include "omnibinder/log.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include "core/event_loop.h"
#include "transport/transport_selector.h"
#include "service_registry.h"
#include "service_watch_notifier.h"
#include "topic_manager.h"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>

namespace omnibinder {

struct ClientConnection {
    int fd;
    IMessageConnection* transport;  // 非拥有：生命周期由 IServerEndpoint 端点管理
    Buffer recv_buffer;
    Buffer send_buffer;
    size_t send_offset;
    uint32_t pid;
    std::string process_name;
    uint32_t log_level;
    bool runtime_registered;

    // 控制面运行时级会话
    std::string session_key;        // (host_id, runtime_id)
    std::string runtime_id;
    std::string host_id;
    uint32_t incarnation_id;
    uint32_t session_interval_ms;
    uint32_t session_timeout_ms;
    int64_t  last_heartbeat_ms;
    int64_t  accept_time_ms;
    bool     hello_received;
    uint32_t hello_timer_id;
    uint32_t deadline_timer_id;
    uint64_t session_generation;

    ClientConnection()
        : fd(-1)
        , transport(nullptr)
        , recv_buffer(DEFAULT_BUFFER_SIZE)
        , send_buffer(DEFAULT_BUFFER_SIZE)
        , send_offset(0)
        , pid(0)
        , log_level(OMNI_LOG_INFO)
        , runtime_registered(false)
        , incarnation_id(0)
        , session_interval_ms(DEFAULT_HEARTBEAT_INTERVAL)
        , session_timeout_ms(DEFAULT_HEARTBEAT_TIMEOUT)
        , last_heartbeat_ms(0)
        , accept_time_ms(0)
        , hello_received(false)
        , hello_timer_id(0)
        , deadline_timer_id(0)
        , session_generation(0)
    {}
};

class ServiceManagerApp {
public:
    /*
     * @brief  创建 ServiceManager 应用
     * @param[in]  heartbeat_timeout_ms 单次心跳丢失判定超时（毫秒，<=0 使用默认值）
     * @note   心跳检查定时器周期取 min(DEFAULT_HEARTBEAT_INTERVAL, timeout)，
     *         保证短超时配置下能及时发现失联服务
     */
    explicit ServiceManagerApp(uint32_t heartbeat_timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT);
    ~ServiceManagerApp();

    int shutdownFd() const;
    bool init(const std::string& host, uint16_t port);
    void run();
    void stop();
    void cleanup();

private:
    void registerEndpointFd(int fd);
    void onEndpointEvent(int fd, uint32_t events);
    void onClientAccepted(int client_id, IMessageConnection* client);
    void onClientReadable(int client_id);
    void onClientDisconnected(int client_id);
    void dispatchMessage(ClientConnection* conn, const Message& msg);
    void removePidFd(uint32_t pid, int fd);
    /*
     * @brief  遍历指定 pid 关联的全部运行时控制连接
     * @param[in]  pid        目标进程 pid
     * @param[in]  except_fd  跳过该 fd（通常是发起本次诊断请求的连接自身）
     * @param[in]  visitor    对每个存活连接执行；返回 true 表示该连接处理成功
     * @return 是否存在至少一个连接被 visitor 成功处理
     * @note   内部先拷贝 fd 列表再遍历：visitor 中 sendMessage 失败可能触发
     *         closeClient -> removePidFd 修改甚至删除 pid_to_fds_ 条目（约束 2）
     */
    bool forEachPidConn(uint32_t pid, int except_fd,
                        const std::function<bool(int fd, ClientConnection* conn)>& visitor);
    void sendBoolReply(ClientConnection* conn, MessageType type, uint32_t seq, bool ok);
    void handleRuntimeHello(ClientConnection* conn, const Message& msg);
    void handleRuntimeList(ClientConnection* conn, const Message& msg);
    uint32_t nextSMProactiveSequence();
    void handleDiagSetLogLevel(ClientConnection* conn, const Message& msg);
    void handleDiagWatchStart(ClientConnection* conn, const Message& msg);
    void sendDiagWatchStopToPid(uint32_t pid, int except_fd);
    /*
     * @brief  移除 watcher fd 的全部 watch 关系，并在某 pid 不再被任何 watcher 关注时停止其目标连接
     * @param[in]  watcher_fd 断开的 watcher 连接 fd
     * @note   先拷贝 pid 集合再下发 WATCH_STOP：下发过程可能重入 closeClient（约束 2）
     */
    void removeWatcherAndMaybeStopTargets(int watcher_fd);
    /*
     * @brief  被 watch 的目标进程断开时，清除指向该 pid 的全部 watch 关系
     * @param[in]  pid 已无存活控制连接的进程 pid
     * @note   仅摘除 pid_watchers_ 与 watcher_to_pids_ 的反向索引条目，不再向已断开的
     *         目标下发 WATCH_STOP；避免 pid churn 导致两张表无限增长
     */
    void removeTargetWatchers(uint32_t pid);
    /*
     * @brief  移除一条 (watcher_fd, pid) watch 关系
     * @param[in]  watcher_fd watcher 连接 fd
     * @param[in]  pid        被关注的进程 pid
     * @return true 表示该关系此前存在；false 表示未记录（幂等 no-op）
     */
    bool removeWatcherPair(int watcher_fd, uint32_t pid);
    /*
     * @brief  从 pid 的 watcher 列表移除指定 fd；列表清空时向该 pid 的目标连接下发 WATCH_STOP
     * @param[in]  pid        目标进程 pid
     * @param[in]  watcher_fd 要移除的 watcher fd
     */
    void stopTargetIfUnwatched(uint32_t pid, int watcher_fd);
    void handleDiagWatchStop(ClientConnection* conn, const Message& msg);
    void handleRegister(ClientConnection* conn, const Message& msg);
    void sendRegisterReply(ClientConnection* conn, uint32_t seq, ServiceHandle handle);
    void handleUnregister(ClientConnection* conn, const Message& msg);
    void handleControlHeartbeat(ClientConnection* conn, const Message& msg);
    void sendControlHeartbeatAck(ClientConnection* conn, uint32_t seq);
    void armHelloDeadline(ClientConnection* conn);
    void armSessionDeadline(ClientConnection* conn);
    void cancelSessionTimers(ClientConnection* conn);
    void onSessionDeadline(int fd, uint64_t generation);
    void closeClient(int fd, bool notify_service_death = true);
    void handleLookup(ClientConnection* conn, const Message& msg);
    void sendLookupReply(ClientConnection* conn, uint32_t seq, bool found, const ServiceInfo& info);
    void handleListInstances(ClientConnection* conn, const Message& msg);
    void sendListInstancesReply(ClientConnection* conn, uint32_t seq, bool found,
                                const std::vector<ServiceInfo>& instances);
    void handleListServices(ClientConnection* conn, const Message& msg);
    void sendListServicesReply(ClientConnection* conn, uint32_t seq, const std::vector<ServiceInfo>& services);
    void handleQueryInterfaces(ClientConnection* conn, const Message& msg);
    void sendQueryInterfacesReply(ClientConnection* conn, uint32_t seq, bool found, const std::vector<InterfaceInfo>& interfaces);
    void handleQueryPublishedTopics(ClientConnection* conn, const Message& msg);
    void sendQueryPublishedTopicsReply(ClientConnection* conn, uint32_t seq, bool found, const std::vector<std::string>& topics);
    void handleSubscribeServiceWatch(ClientConnection* conn, const Message& msg);
    void handleUnsubscribeServiceWatch(ClientConnection* conn, const Message& msg);
    void notifyServiceWatch(const ServiceIdentity& identity, bool online);
    void sendServiceWatchNotify(ClientConnection* conn, uint64_t watch_id,
                                const ServiceIdentity& identity, bool online);
    void sendInitialServiceWatch(ClientConnection* conn, uint64_t watch_id,
                                 const ServiceWatchFilter& filter);
    void handlePublishTopic(ClientConnection* conn, const Message& msg);
    void handleUnpublishTopic(ClientConnection* conn, const Message& msg);
    void handleSubscribeTopic(ClientConnection* conn, const Message& msg);
    void sendSubscribeTopicReply(ClientConnection* conn, uint32_t seq, bool success, uint32_t idl_hash = 0);
    void handleUnsubscribeTopic(ClientConnection* conn, const Message& msg);
    void sendTopicPublisherNotify(int subscriber_fd, const std::string& topic,
                                  const ServiceInfo& pub_info, uint32_t idl_hash);
    /*
     * @brief  服务实例摘除后的统一收尾：停止该实例心跳跟踪并通知死亡订阅者
     * @param[in]  identity 被摘除的服务实例身份
     * @note   显式注销 / 心跳超时两条摘除路径共用，保证收尾顺序一致；
     *         实例级通知按身份下发，名字级通知仅在最后一个实例消失时下发
     */
    void notifyServiceRemoved(const ServiceIdentity& identity);
    /*
     * @brief  单个服务实例的完整摘除：移除注册表条目 + 统一收尾 + 清理其发布者
     * @param[in]  identity 服务实例身份
     * @param[in]  fd       该实例控制连接 fd；-1 表示未知，仅跳过发布者清理
     * @return true 表示注册表确实移除了该实例；false 表示该实例未注册
     */
    bool removeServiceAndNotify(const ServiceIdentity& identity, int fd);
    void sendMessage(ClientConnection* conn, Message& msg);
    bool flushPendingSends(ClientConnection* conn);
    void enableClientWriteEvents(ClientConnection* conn);
    void disableClientWriteEvents(ClientConnection* conn);

    EventLoop loop_;
    IServerEndpoint* server_;
    std::map<int, ClientConnection*> clients_;
    std::set<int> endpoint_fds_;
    std::map<uint32_t, std::vector<int> > pid_to_fds_;
    // 诊断 watch 关系以 (watcher_fd, pid) 为最小单元，两张表互为反向索引：
    // 一个 watcher 可关注多个 pid，一个 pid 可被多个 watcher 关注
    std::map<uint32_t, std::vector<int> > pid_watchers_;
    std::map<int, std::set<uint32_t> > watcher_to_pids_;
    ServiceRegistry registry_;
    ServiceWatchNotifier watch_notifier_;
    TopicManager topic_manager_;
    // (host_id, runtime_id) -> 控制连接 fd；用于同运行时新会话顶替旧会话
    std::map<std::string, int> runtime_sessions_;
    uint32_t default_session_timeout_ms_;
    uint32_t max_session_timeout_ms_;
    uint32_t sm_incarnation_id_;
    uint32_t sm_seq_counter_;
    int shutdown_fd_;
};

} // namespace omnibinder

#endif
