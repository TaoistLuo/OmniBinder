#ifndef OMNIBINDER_SERVICE_MANAGER_APP_H
#define OMNIBINDER_SERVICE_MANAGER_APP_H

#include "omnibinder/types.h"
#include "omnibinder/log.h"
#include "omnibinder/message.h"
#include "core/event_loop.h"
#include "platform/platform.h"
#include "service_registry.h"
#include "heartbeat_monitor.h"
#include "death_notifier.h"
#include "topic_manager.h"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <set>

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
    int shutdownFd() const;
    bool init(const std::string& host, uint16_t port);
    void run();
    void stop();
    void cleanup();
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

private:
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

#endif
