/**************************************************************************************************
 * @file        service_manager_app.h
 * @brief       ServiceManager 应用类
 * @details     ServiceManager 进程的核心协调类。管理 TCP 监听、客户端连接生命周期、消息分发、心跳检测、死亡通知和话题管理。基于 EventLoop 的单线程事件驱动模型。
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
#include "core/event_loop.h"
#include "transport/tcp_transport.h"
#include "service_registry.h"
#include "heartbeat_monitor.h"
#include "death_notifier.h"
#include "topic_manager.h"
#include <string>
#include <map>
#include <vector>

namespace omnibinder {

struct ClientConnection {
    int fd;
    TcpTransport* transport;
    Buffer recv_buffer;
    Buffer send_buffer;
    size_t send_offset;
    std::string registered_service_name;  // Empty if not registered as a service
    std::vector<std::string> registered_services;
    uint32_t pid;
    std::string process_name;
    uint32_t log_level;
    bool runtime_registered;

    ClientConnection()
        : fd(-1)
        , transport(nullptr)
        , recv_buffer(DEFAULT_BUFFER_SIZE)
        , send_buffer(DEFAULT_BUFFER_SIZE)
        , send_offset(0)
        , pid(0)
        , log_level(OMNI_LOG_INFO)
        , runtime_registered(false)
    {}

    ~ClientConnection() {
        if (transport) {
            transport->close();
            delete transport;
            transport = nullptr;
        }
    }
};

class ServiceManagerApp {
public:
    ServiceManagerApp();
    ~ServiceManagerApp();

    int shutdownFd() const;
    bool init(const std::string& host, uint16_t port);
    void run();
    void stop();
    void cleanup();

private:
    void onAccept();
    void onClientEvent(int fd, uint32_t events);
    void onClientRead(ClientConnection* conn);
    void processMessages(ClientConnection* conn);
    void dispatchMessage(ClientConnection* conn, const Message& msg);
    void removePidFd(uint32_t pid, int fd);
    void sendBoolReply(ClientConnection* conn, MessageType type, uint32_t seq, bool ok);
    void handleRuntimeHello(ClientConnection* conn, const Message& msg);
    void handleRuntimeList(ClientConnection* conn, const Message& msg);
    void handleDiagSetLogLevel(ClientConnection* conn, const Message& msg);
    void handleDiagWatchStart(ClientConnection* conn, const Message& msg);
    void sendDiagWatchStopToPid(uint32_t pid, int except_fd);
    void removeWatcherAndMaybeStopTarget(int watcher_fd);
    void handleDiagWatchStop(ClientConnection* conn, const Message& msg);
    void handleRegister(ClientConnection* conn, const Message& msg);
    void sendRegisterReply(ClientConnection* conn, uint32_t seq, ServiceHandle handle);
    void handleUnregister(ClientConnection* conn, const Message& msg);
    void sendUnregisterReply(ClientConnection* conn, uint32_t seq, bool success);
    void handleHeartbeat(ClientConnection* conn, const Message& msg);
    void sendHeartbeatAck(ClientConnection* conn, uint32_t seq);
    void handleLookup(ClientConnection* conn, const Message& msg);
    void sendLookupReply(ClientConnection* conn, uint32_t seq, bool found, const ServiceInfo& info);
    void handleListServices(ClientConnection* conn, const Message& msg);
    void sendListServicesReply(ClientConnection* conn, uint32_t seq, const std::vector<ServiceInfo>& services);
    void handleQueryInterfaces(ClientConnection* conn, const Message& msg);
    void sendQueryInterfacesReply(ClientConnection* conn, uint32_t seq, bool found, const std::vector<InterfaceInfo>& interfaces);
    void handleQueryPublishedTopics(ClientConnection* conn, const Message& msg);
    void sendQueryPublishedTopicsReply(ClientConnection* conn, uint32_t seq, bool found, const std::vector<std::string>& topics);
    void handleSubscribeService(ClientConnection* conn, const Message& msg);
    void sendSubscribeServiceReply(ClientConnection* conn, uint32_t seq, bool success);
    void handleUnsubscribeService(ClientConnection* conn, const Message& msg);
    void handlePublishTopic(ClientConnection* conn, const Message& msg);
    void sendPublishTopicReply(ClientConnection* conn, uint32_t seq, bool success);
    void handleUnpublishTopic(ClientConnection* conn, const Message& msg);
    void handleSubscribeTopic(ClientConnection* conn, const Message& msg);
    void sendSubscribeTopicReply(ClientConnection* conn, uint32_t seq, bool success, uint32_t idl_hash = 0);
    void handleUnsubscribeTopic(ClientConnection* conn, const Message& msg);
    void sendTopicPublisherNotify(int subscriber_fd, const std::string& topic, const ServiceInfo& pub_info);
    void onHeartbeatCheck();
    void notifyServiceDeath(const std::string& service_name);
    void sendDeathNotify(ClientConnection* conn, const std::string& service_name);
    void closeClient(int fd);
    void sendMessage(ClientConnection* conn, Message& msg);
    bool flushPendingSends(ClientConnection* conn);
    void enableClientWriteEvents(ClientConnection* conn);
    void disableClientWriteEvents(ClientConnection* conn);

    EventLoop loop_;
    TcpTransportServer* server_;
    std::map<int, ClientConnection*> clients_;
    std::map<uint32_t, std::vector<int> > pid_to_fds_;
    std::map<uint32_t, std::vector<int> > pid_watchers_;
    std::map<int, uint32_t> watcher_to_pid_;
    ServiceRegistry registry_;
    HeartbeatMonitor heartbeat_;
    DeathNotifier death_notifier_;
    TopicManager topic_manager_;
    uint32_t heartbeat_timer_id_;
    int shutdown_fd_;
};

} // namespace omnibinder

#endif
