// ServiceManager - The central registry for OmniBinder services
//
// This is the main entry point for the ServiceManager process.
// It handles:
//   - Service registration and discovery
//   - Heartbeat monitoring
//   - Death notifications
//   - Topic management (pub/sub)

#include "service_registry.h"
#include "heartbeat_monitor.h"
#include "death_notifier.h"
#include "topic_manager.h"

#include "omnibinder/types.h"
#include "omnibinder/message.h"
#include "omnibinder/buffer.h"
#include "omnibinder/log.h"
#include "core/event_loop.h"
#include "transport/tcp_transport.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <map>
#include <vector>
#include <memory>

#define TAG "ServiceManager"

namespace omnibinder {

// ============================================================
// ClientConnection - Per-client state including receive buffer
// ============================================================
struct ClientConnection {
    int fd;
    TcpTransport* transport;
    Buffer recv_buffer;
    Buffer send_buffer;
    size_t send_offset;
    std::string registered_service_name;  // Empty if not registered as a service

    ClientConnection()
        : fd(-1)
        , transport(nullptr)
        , recv_buffer(DEFAULT_BUFFER_SIZE)
        , send_buffer(DEFAULT_BUFFER_SIZE)
        , send_offset(0)
    {}

    ~ClientConnection() {
        if (transport) {
            transport->close();
            delete transport;
            transport = nullptr;
        }
    }
};

// ============================================================
// ServiceManagerApp - Main application class
// ============================================================
class ServiceManagerApp {
public:
    ServiceManagerApp()
        : server_(nullptr)
        , heartbeat_timer_id_(0)
        , running_(false)
    {}

    ~ServiceManagerApp() {
        cleanup();
    }

    bool init(const std::string& host, uint16_t port) {
        // Initialize network
        if (!platform::netInit()) {
            OMNI_LOG_ERROR(TAG, "Failed to initialize network");
            return false;
        }

        // Create TCP server
        server_ = new TcpTransportServer();
        int listen_port = server_->listen(host, port);
        if (listen_port < 0) {
            OMNI_LOG_ERROR(TAG, "Failed to listen on %s:%u", host.c_str(), port);
            delete server_;
            server_ = nullptr;
            return false;
        }

        OMNI_LOG_INFO(TAG, "Listening on %s:%d", host.c_str(), listen_port);

        // Register server fd with event loop for accept events
        loop_.addFd(server_->fd(), EventLoop::EVENT_READ,
            [this](int fd, uint32_t events) {
                (void)fd;
                (void)events;
                this->onAccept();
            });

        // Start heartbeat check timer (every 3 seconds)
        heartbeat_timer_id_ = loop_.addTimer(DEFAULT_HEARTBEAT_INTERVAL, [this]() {
            this->onHeartbeatCheck();
        }, true);

        running_ = true;
        return true;
    }

    void run() {
        OMNI_LOG_INFO(TAG, "ServiceManager started");
        loop_.run();
        OMNI_LOG_INFO(TAG, "ServiceManager stopped");
    }

    void stop() {
        running_ = false;
        loop_.stop();
    }

    void cleanup() {
        if (heartbeat_timer_id_ != 0) {
            loop_.cancelTimer(heartbeat_timer_id_);
            heartbeat_timer_id_ = 0;
        }

        // Close all client connections
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            ClientConnection* conn = it->second;
            if (conn) {
                loop_.removeFd(conn->fd);
                delete conn;
            }
        }
        clients_.clear();

        // Close server
        if (server_) {
            loop_.removeFd(server_->fd());
            server_->close();
            delete server_;
            server_ = nullptr;
        }

        platform::netCleanup();
    }

private:
    // ============================================================
    // Accept new connections
    // ============================================================
    void onAccept() {
        while (true) {
            ITransport* transport = server_->accept();
            if (!transport) {
                break;  // No more pending connections
            }

            TcpTransport* tcp = static_cast<TcpTransport*>(transport);
            int fd = tcp->fd();

            ClientConnection* conn = new ClientConnection();
            conn->fd = fd;
            conn->transport = tcp;
            clients_[fd] = conn;

            OMNI_LOG_INFO(TAG, "Client connected: fd=%d", fd);

            // Register for read events
            loop_.addFd(fd, EventLoop::EVENT_READ,
                [this, fd](int, uint32_t events) {
                    this->onClientEvent(fd, events);
                });
        }
    }

    // ============================================================
    // Handle client events (read/error)
    // ============================================================
    void onClientEvent(int fd, uint32_t events) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }

        ClientConnection* conn = it->second;

        if (events & EventLoop::EVENT_ERROR) {
            OMNI_LOG_WARN(TAG, "Client error: fd=%d", fd);
            closeClient(fd);
            return;
        }

        if (events & EventLoop::EVENT_WRITE) {
            if (!flushPendingSends(conn)) {
                closeClient(fd);
                return;
            }
        }

        if (events & EventLoop::EVENT_READ) {
            onClientRead(conn);
        }
    }

    // ============================================================
    // Read data from client and process messages
    // ============================================================
    void onClientRead(ClientConnection* conn) {
        // Read available data into the receive buffer
        uint8_t temp[4096];
        int n = conn->transport->recv(temp, sizeof(temp));

        if (n < 0) {
            // Error or connection closed
            OMNI_LOG_INFO(TAG, "Client disconnected: fd=%d", conn->fd);
            closeClient(conn->fd);
            return;
        }

        if (n == 0) {
            // No data available (would block)
            return;
        }

        // Append to receive buffer
        size_t old_size = conn->recv_buffer.size();
        conn->recv_buffer.resize(old_size + n);
        memcpy(conn->recv_buffer.mutableData() + old_size, temp, n);

        // Process complete messages
        processMessages(conn);
    }

    // ============================================================
    // Message framing: parse complete messages from buffer
    // ============================================================
    void processMessages(ClientConnection* conn) {
        while (true) {
            size_t buf_size = conn->recv_buffer.size();
            const uint8_t* data = conn->recv_buffer.data();

            // Need at least a header
            if (buf_size < MESSAGE_HEADER_SIZE) {
                break;
            }

            // Parse header
            MessageHeader header;
            if (!Message::parseHeader(data, buf_size, header)) {
                OMNI_LOG_ERROR(TAG, "Invalid message header from fd=%d", conn->fd);
                closeClient(conn->fd);
                return;
            }

            // Validate header
            if (!Message::validateHeader(header)) {
                OMNI_LOG_ERROR(TAG, "Invalid message header from fd=%d (magic/version)", conn->fd);
                closeClient(conn->fd);
                return;
            }

            // Check payload size limit
            if (header.length > MAX_MESSAGE_SIZE) {
                OMNI_LOG_ERROR(TAG, "Message too large from fd=%d: %u bytes", conn->fd, header.length);
                closeClient(conn->fd);
                return;
            }

            // Check if we have the full message
            size_t total_size = MESSAGE_HEADER_SIZE + header.length;
            if (buf_size < total_size) {
                break;  // Wait for more data
            }

            // Extract the message
            Message msg;
            msg.header = header;
            if (header.length > 0) {
                msg.payload.assign(data + MESSAGE_HEADER_SIZE, header.length);
            }

            // Remove processed data from buffer
            size_t remaining = buf_size - total_size;
            if (remaining > 0) {
                memmove(conn->recv_buffer.mutableData(),
                       data + total_size, remaining);
            }
            conn->recv_buffer.resize(remaining);

            // Dispatch the message
            dispatchMessage(conn, msg);
        }
    }

    // ============================================================
    // Dispatch message to appropriate handler
    // ============================================================
    void dispatchMessage(ClientConnection* conn, const Message& msg) {
        MessageType type = msg.getType();

        OMNI_LOG_DEBUG(TAG, "Received %s from fd=%d (seq=%u, len=%u)",
                        messageTypeToString(type), conn->fd,
                        msg.header.sequence, msg.header.length);

        switch (type) {
            case MessageType::MSG_REGISTER:
                handleRegister(conn, msg);
                break;
            case MessageType::MSG_UNREGISTER:
                handleUnregister(conn, msg);
                break;
            case MessageType::MSG_HEARTBEAT:
                handleHeartbeat(conn, msg);
                break;
            case MessageType::MSG_LOOKUP:
                handleLookup(conn, msg);
                break;
            case MessageType::MSG_LIST_SERVICES:
                handleListServices(conn, msg);
                break;
            case MessageType::MSG_QUERY_INTERFACES:
                handleQueryInterfaces(conn, msg);
                break;
            case MessageType::MSG_SUBSCRIBE_SERVICE:
                handleSubscribeService(conn, msg);
                break;
            case MessageType::MSG_UNSUBSCRIBE_SERVICE:
                handleUnsubscribeService(conn, msg);
                break;
            case MessageType::MSG_PUBLISH_TOPIC:
                handlePublishTopic(conn, msg);
                break;
            case MessageType::MSG_SUBSCRIBE_TOPIC:
                handleSubscribeTopic(conn, msg);
                break;
            case MessageType::MSG_UNSUBSCRIBE_TOPIC:
                handleUnsubscribeTopic(conn, msg);
                break;
            case MessageType::MSG_UNPUBLISH_TOPIC:
                handleUnpublishTopic(conn, msg);
                break;
            default:
                OMNI_LOG_WARN(TAG, "Unknown message type 0x%04x from fd=%d",
                               static_cast<uint16_t>(type), conn->fd);
                break;
        }
    }

    // ============================================================
    // MSG_REGISTER: Register a new service
    // ============================================================
    void handleRegister(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        ServiceInfo info;

        if (!deserializeServiceInfo(payload, info)) {
            OMNI_LOG_ERROR(TAG, "Failed to deserialize ServiceInfo from fd=%d", conn->fd);
            sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
            return;
        }

        ServiceHandle handle = registry_.addService(info, conn->fd);
        if (handle == INVALID_HANDLE) {
            OMNI_LOG_WARN(TAG, "Failed to register service: %s", info.name.c_str());
            sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
            return;
        }

        // Track which service this connection registered
        conn->registered_service_name = info.name;

        // Start heartbeat tracking
        heartbeat_.startTracking(info.name);

        sendRegisterReply(conn, msg.header.sequence, handle);
    }

    void sendRegisterReply(ClientConnection* conn, uint32_t seq, ServiceHandle handle) {
        Message reply(MessageType::MSG_REGISTER_REPLY, seq);
        reply.payload.writeUint32(handle);
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_UNREGISTER: Unregister a service
    // ============================================================
    void handleUnregister(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string name = payload.readString();

        if (!registry_.ownsService(conn->fd, name)) {
            OMNI_LOG_WARN(TAG, "Reject unregister for %s from non-owner fd=%d",
                           name.c_str(), conn->fd);
            sendUnregisterReply(conn, msg.header.sequence, false);
            return;
        }

        bool success = registry_.removeService(name);
        if (success) {
            heartbeat_.stopTracking(name);

            // Notify death subscribers
            notifyServiceDeath(name);

            // Clean up topics
            topic_manager_.removeByFd(conn->fd);

            if (conn->registered_service_name == name) {
                conn->registered_service_name.clear();
            }
        }

        sendUnregisterReply(conn, msg.header.sequence, success);
    }

    void sendUnregisterReply(ClientConnection* conn, uint32_t seq, bool success) {
        Message reply(MessageType::MSG_UNREGISTER_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_HEARTBEAT: Update heartbeat timestamp
    // ============================================================
    void handleHeartbeat(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string name = payload.readString();

        if (!registry_.ownsService(conn->fd, name)) {
            OMNI_LOG_WARN(TAG, "Reject heartbeat for %s from non-owner fd=%d",
                           name.c_str(), conn->fd);
            closeClient(conn->fd);
            return;
        }

        heartbeat_.updateHeartbeat(name);
        sendHeartbeatAck(conn, msg.header.sequence);
    }

    void sendHeartbeatAck(ClientConnection* conn, uint32_t seq) {
        Message reply(MessageType::MSG_HEARTBEAT_ACK, seq);
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_LOOKUP: Find a service by name
    // ============================================================
    void handleLookup(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string name = payload.readString();

        ServiceEntry entry;
        bool found = registry_.findService(name, entry);

        sendLookupReply(conn, msg.header.sequence, found, found ? entry.info : ServiceInfo());
    }

    void sendLookupReply(ClientConnection* conn, uint32_t seq, bool found, const ServiceInfo& info) {
        Message reply(MessageType::MSG_LOOKUP_REPLY, seq);
        reply.payload.writeBool(found);
        if (found) {
            serializeServiceInfo(info, reply.payload);
        }
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_LIST_SERVICES: List all registered services
    // ============================================================
    void handleListServices(ClientConnection* conn, const Message& msg) {
        std::vector<ServiceInfo> services = registry_.listServices();
        sendListServicesReply(conn, msg.header.sequence, services);
    }

    void sendListServicesReply(ClientConnection* conn, uint32_t seq,
                               const std::vector<ServiceInfo>& services) {
        Message reply(MessageType::MSG_LIST_SERVICES_REPLY, seq);
        reply.payload.writeUint32(static_cast<uint32_t>(services.size()));
        for (size_t i = 0; i < services.size(); ++i) {
            serializeServiceInfo(services[i], reply.payload);
        }
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_QUERY_INTERFACES: Query interfaces of a service
    // ============================================================
    void handleQueryInterfaces(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string name = payload.readString();

        ServiceEntry entry;
        bool found = registry_.findService(name, entry);

        sendQueryInterfacesReply(conn, msg.header.sequence, found,
                                 found ? entry.info.interfaces : std::vector<InterfaceInfo>());
    }

    void sendQueryInterfacesReply(ClientConnection* conn, uint32_t seq, bool found,
                                  const std::vector<InterfaceInfo>& interfaces) {
        Message reply(MessageType::MSG_QUERY_INTERFACES_REPLY, seq);
        reply.payload.writeBool(found);
        if (found) {
            reply.payload.writeUint32(static_cast<uint32_t>(interfaces.size()));
            for (size_t i = 0; i < interfaces.size(); ++i) {
                serializeInterfaceInfo(interfaces[i], reply.payload);
            }
        }
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_SUBSCRIBE_SERVICE: Subscribe to death notifications
    // ============================================================
    void handleSubscribeService(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string target_service = payload.readString();

        bool success = death_notifier_.subscribe(conn->fd, target_service);
        sendSubscribeServiceReply(conn, msg.header.sequence, success);
    }

    void sendSubscribeServiceReply(ClientConnection* conn, uint32_t seq, bool success) {
        Message reply(MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_UNSUBSCRIBE_SERVICE: Unsubscribe from death notifications
    // ============================================================
    void handleUnsubscribeService(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string target_service = payload.readString();

        death_notifier_.unsubscribe(conn->fd, target_service);
        // No reply needed for unsubscribe
    }

    // ============================================================
    // MSG_PUBLISH_TOPIC: Register as a topic publisher
    // ============================================================
    void handlePublishTopic(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string topic = payload.readString();
        ServiceInfo pub_info;
        if (!deserializeServiceInfo(payload, pub_info)) {
            OMNI_LOG_ERROR(TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }

        if (!registry_.ownsService(conn->fd, pub_info.name)) {
            OMNI_LOG_WARN(TAG, "Reject publish topic %s from fd=%d for non-owned service %s",
                           topic.c_str(), conn->fd, pub_info.name.c_str());
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }

        bool success = topic_manager_.registerPublisher(topic, pub_info, conn->fd);
        sendPublishTopicReply(conn, msg.header.sequence, success);

        if (success) {
            // Notify existing subscribers about the new publisher
            std::vector<int> subscribers = topic_manager_.getSubscribers(topic);
            for (size_t i = 0; i < subscribers.size(); ++i) {
                sendTopicPublisherNotify(subscribers[i], topic, pub_info);
            }
        }
    }

    void sendPublishTopicReply(ClientConnection* conn, uint32_t seq, bool success) {
        Message reply(MessageType::MSG_PUBLISH_TOPIC_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_UNPUBLISH_TOPIC: Unregister as a topic publisher
    // ============================================================
    void handleUnpublishTopic(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string topic = payload.readString();

        if (!topic_manager_.removePublisher(topic, conn->fd)) {
            OMNI_LOG_WARN(TAG, "Reject unpublish topic %s from non-owner fd=%d",
                           topic.c_str(), conn->fd);
        }
        // No reply needed
    }

    // ============================================================
    // MSG_SUBSCRIBE_TOPIC: Subscribe to a topic
    // ============================================================
    void handleSubscribeTopic(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string topic = payload.readString();

        bool added = topic_manager_.addSubscriber(topic, conn->fd);
        sendSubscribeTopicReply(conn, msg.header.sequence, added);

        // If there's already a publisher, notify the subscriber
        ServiceInfo pub_info;
        if (topic_manager_.getPublisher(topic, pub_info)) {
            sendTopicPublisherNotify(conn->fd, topic, pub_info);
        }
    }

    void sendSubscribeTopicReply(ClientConnection* conn, uint32_t seq, bool success) {
        Message reply(MessageType::MSG_SUBSCRIBE_TOPIC_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
    }

    // ============================================================
    // MSG_UNSUBSCRIBE_TOPIC: Unsubscribe from a topic
    // ============================================================
    void handleUnsubscribeTopic(ClientConnection* conn, const Message& msg) {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string topic = payload.readString();

        topic_manager_.removeSubscriber(topic, conn->fd);
        // No reply needed
    }

    // ============================================================
    // Send topic publisher notification to a subscriber
    // ============================================================
    void sendTopicPublisherNotify(int subscriber_fd, const std::string& topic,
                                  const ServiceInfo& pub_info) {
        auto it = clients_.find(subscriber_fd);
        if (it == clients_.end()) {
            return;
        }

        Message notify(MessageType::MSG_TOPIC_PUBLISHER_NOTIFY, nextSequenceNumber());
        notify.payload.writeString(topic);
        serializeServiceInfo(pub_info, notify.payload);
        sendMessage(it->second, notify);
    }

    // ============================================================
    // Heartbeat check timer callback
    // ============================================================
    void onHeartbeatCheck() {
        std::vector<std::string> timed_out = heartbeat_.checkTimeouts();

        for (size_t i = 0; i < timed_out.size(); ++i) {
            const std::string& name = timed_out[i];
            OMNI_LOG_WARN(TAG, "Service timed out: %s", name.c_str());

            // Get the control fd before removing
            int fd = registry_.getControlFd(name);

            // Remove from registry
            registry_.removeService(name);

            // Notify death subscribers
            notifyServiceDeath(name);

            // Clean up topics for this service
            if (fd >= 0) {
                topic_manager_.removeByFd(fd);
            }

            // Close the client connection
            if (fd >= 0) {
                closeClient(fd);
            }
        }
    }

    // ============================================================
    // Notify subscribers about a service death
    // ============================================================
    void notifyServiceDeath(const std::string& service_name) {
        std::vector<int> subscribers = death_notifier_.notify(service_name);

        for (size_t i = 0; i < subscribers.size(); ++i) {
            int fd = subscribers[i];
            auto it = clients_.find(fd);
            if (it != clients_.end()) {
                sendDeathNotify(it->second, service_name);
            }
        }
    }

    void sendDeathNotify(ClientConnection* conn, const std::string& service_name) {
        Message notify(MessageType::MSG_DEATH_NOTIFY, nextSequenceNumber());
        notify.payload.writeString(service_name);
        sendMessage(conn, notify);
    }

    // ============================================================
    // Close a client connection and clean up
    // ============================================================
    void closeClient(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }

        ClientConnection* conn = it->second;

        // Remove from event loop
        loop_.removeFd(fd);

        // Clean up registered service
        if (!conn->registered_service_name.empty()) {
            std::string name = conn->registered_service_name;

            registry_.removeService(name);
            heartbeat_.stopTracking(name);
            notifyServiceDeath(name);
        }

        // Clean up any services registered by this fd
        std::vector<std::string> removed = registry_.removeByFd(fd);
        for (size_t i = 0; i < removed.size(); ++i) {
            heartbeat_.stopTracking(removed[i]);
            notifyServiceDeath(removed[i]);
        }

        // Clean up death notification subscriptions
        death_notifier_.removeSubscriber(fd);

        // Clean up topic subscriptions and publications
        topic_manager_.removeByFd(fd);

        // Delete the connection
        delete conn;
        clients_.erase(it);

        OMNI_LOG_INFO(TAG, "Client closed: fd=%d", fd);
    }

    // ============================================================
    // Send a message to a client
    // ============================================================
    void sendMessage(ClientConnection* conn, Message& msg) {
        Buffer output;
        if (!msg.serialize(output)) {
            OMNI_LOG_ERROR(TAG, "Failed to serialize message");
            return;
        }

        if (conn->send_offset < conn->send_buffer.size()) {
            conn->send_buffer.writeRaw(output.data(), output.size());
            enableClientWriteEvents(conn);
            return;
        }

        int sent = conn->transport->send(output.data(), output.size());
        if (sent < 0) {
            OMNI_LOG_ERROR(TAG, "Failed to send message to fd=%d", conn->fd);
            closeClient(conn->fd);
            return;
        }

        if (static_cast<size_t>(sent) < output.size()) {
            conn->send_buffer.assign(output.data() + sent, output.size() - static_cast<size_t>(sent));
            conn->send_offset = 0;
            enableClientWriteEvents(conn);
        }
    }

    bool flushPendingSends(ClientConnection* conn) {
        while (conn->send_offset < conn->send_buffer.size()) {
            int sent = conn->transport->send(conn->send_buffer.data() + conn->send_offset,
                                             conn->send_buffer.size() - conn->send_offset);
            if (sent < 0) {
                OMNI_LOG_ERROR(TAG, "Failed to flush pending send to fd=%d", conn->fd);
                return false;
            }
            if (sent == 0) {
                enableClientWriteEvents(conn);
                return true;
            }
            conn->send_offset += static_cast<size_t>(sent);
        }

        conn->send_buffer.clear();
        conn->send_offset = 0;
        disableClientWriteEvents(conn);
        return true;
    }

    void enableClientWriteEvents(ClientConnection* conn) {
        loop_.modifyFd(conn->fd, EventLoop::EVENT_READ | EventLoop::EVENT_WRITE);
    }

    void disableClientWriteEvents(ClientConnection* conn) {
        loop_.modifyFd(conn->fd, EventLoop::EVENT_READ);
    }

    // ============================================================
    // Member variables
    // ============================================================
    EventLoop loop_;
    TcpTransportServer* server_;
    std::map<int, ClientConnection*> clients_;

    ServiceRegistry registry_;
    HeartbeatMonitor heartbeat_;
    DeathNotifier death_notifier_;
    TopicManager topic_manager_;

    uint32_t heartbeat_timer_id_;
    volatile bool running_;
};

// ============================================================
// Global app instance for signal handling
// ============================================================
static ServiceManagerApp* g_app = nullptr;

static void signalHandler(int sig) {
    OMNI_LOG_INFO(TAG, "Received signal %d, shutting down...", sig);
    if (g_app) {
        g_app->stop();
    }
}

} // namespace omnibinder

// ============================================================
// Command line argument parsing
// ============================================================
static void printUsage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --host <addr>     Listen address (default: 0.0.0.0)\n");
    fprintf(stderr, "  --port <port>     Listen port (default: 9900)\n");
    fprintf(stderr, "  --log-level <n>   Log level: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR (default: 1)\n");
    fprintf(stderr, "  --help            Show this help\n");
}

int main(int argc, char* argv[]) {
    std::string host = "0.0.0.0";
    uint16_t port = omnibinder::DEFAULT_SM_PORT;
    int log_level = 1;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            log_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    // Set log level
    if (log_level >= 0 && log_level <= 4) {
        omnibinder::setLogLevel(static_cast<omnibinder::LogLevel>(log_level));
    }

    OMNI_LOG_INFO(TAG, "OmniBinder ServiceManager starting...");
    OMNI_LOG_INFO(TAG, "Host: %s, Port: %u, LogLevel: %d", host.c_str(), port, log_level);

    // Create and initialize the application
    omnibinder::ServiceManagerApp app;
    omnibinder::g_app = &app;

    // Set up signal handlers
    signal(SIGINT, omnibinder::signalHandler);
    signal(SIGTERM, omnibinder::signalHandler);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE for socket writes
#endif

    if (!app.init(host, port)) {
        OMNI_LOG_ERROR(TAG, "Failed to initialize ServiceManager");
        return 1;
    }

    // Run the event loop
    app.run();

    omnibinder::g_app = nullptr;
    OMNI_LOG_INFO(TAG, "ServiceManager exited cleanly");
    return 0;
}
