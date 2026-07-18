#include "service_manager_app.h"
#include "omnibinder/log.h"
#include "platform/platform.h"
#include <cstring>

#define TAG "ServiceManager"

namespace omnibinder {

ServiceManagerApp::ServiceManagerApp()
    : server_(nullptr)
    , heartbeat_timer_id_(0)
    , shutdown_fd_(-1) {
}

ServiceManagerApp::~ServiceManagerApp() {
    cleanup();
}

int ServiceManagerApp::shutdownFd() const {
    return shutdown_fd_;
}

bool ServiceManagerApp::init(const std::string& host, uint16_t port) {
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

        shutdown_fd_ = platform::createEventFd();
        if (shutdown_fd_ >= 0) {
            loop_.addFd(shutdown_fd_, EventLoop::EVENT_READ,
                [this](int fd, uint32_t) {
                    platform::eventFdConsume(fd);
                    OMNI_LOG_INFO(TAG, "Shutdown requested, stopping...");
                    this->stop();
                });
        }

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

        return true;
}

void ServiceManagerApp::run() {
        OMNI_LOG_INFO(TAG, "ServiceManager started");
        loop_.run();
        OMNI_LOG_INFO(TAG, "ServiceManager stopped");
}

void ServiceManagerApp::stop() {
        loop_.stop();
}

void ServiceManagerApp::cleanup() {
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

        if (shutdown_fd_ >= 0) {
            loop_.removeFd(shutdown_fd_);
            platform::closeEventFd(shutdown_fd_);
            shutdown_fd_ = -1;
        }

        platform::netCleanup();
}

void ServiceManagerApp::onAccept() {
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

void ServiceManagerApp::onClientEvent(int fd, uint32_t events) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }

        ClientConnection* conn = it->second;

        // Process READ first: a normal disconnect triggers EPOLLHUP|EPOLLIN
        // (Linux) or POLLHUP (Windows), both of which set EVENT_READ.
        // Handle via onClientRead which detects recv==0 / recv<0 as
        // disconnect, avoiding spurious "Client error" logs on Linux.
        if (events & EventLoop::EVENT_READ) {
            onClientRead(conn);
            // onClientRead may close the client on error/disconnect
            if (clients_.find(fd) == clients_.end()) return;
        }

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
}

void ServiceManagerApp::onClientRead(ClientConnection* conn) {
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

        // Append to receive buffer (with size limit to prevent OOM)
        static const size_t MAX_RECV_BUFFER = MAX_MESSAGE_SIZE;
        size_t old_size = conn->recv_buffer.size();
        if (old_size + static_cast<size_t>(n) > MAX_RECV_BUFFER) {
            OMNI_LOG_ERROR(TAG, "recv_buffer overflow for fd=%d (>%zuMB), disconnecting",
                           conn->fd, MAX_RECV_BUFFER / (1024*1024));
            closeClient(conn->fd);
            return;
        }
        conn->recv_buffer.resize(old_size + n);
        memcpy(conn->recv_buffer.mutableData() + old_size, temp, n);

        // Process complete messages
        processMessages(conn);
}

void ServiceManagerApp::processMessages(ClientConnection* conn) {
        int fd = conn->fd;
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
            if (clients_.find(fd) == clients_.end()) {
                return;
            }
        }
}

void ServiceManagerApp::dispatchMessage(ClientConnection* conn, const Message& msg) {
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
            case MessageType::MSG_QUERY_PUBLISHED_TOPICS:
                handleQueryPublishedTopics(conn, msg);
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
            case MessageType::MSG_RUNTIME_HELLO:
                handleRuntimeHello(conn, msg);
                break;
            case MessageType::MSG_RUNTIME_LIST:
                handleRuntimeList(conn, msg);
                break;
            case MessageType::MSG_DIAG_SET_LOG_LEVEL:
                handleDiagSetLogLevel(conn, msg);
                break;
            case MessageType::MSG_DIAG_WATCH_START:
                handleDiagWatchStart(conn, msg);
                break;
            case MessageType::MSG_DIAG_WATCH_STOP:
                handleDiagWatchStop(conn, msg);
                break;
            case MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY:
            case MessageType::MSG_DIAG_WATCH_START_REPLY:
            case MessageType::MSG_DIAG_WATCH_STOP_REPLY:
                break;
            default:
                OMNI_LOG_WARN(TAG, "Unknown message type 0x%04x from fd=%d",
                               static_cast<uint16_t>(type), conn->fd);
                break;
        }
}

void ServiceManagerApp::sendBoolReply(ClientConnection* conn, MessageType type,
                                      uint32_t seq, bool ok) {
        Message reply(type, seq);
        reply.payload.writeBool(ok);
        sendMessage(conn, reply);
}

void ServiceManagerApp::closeClient(int fd) {
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

            if (registry_.removeService(name)) {
                heartbeat_.stopTracking(name);
                notifyServiceDeath(name);
            }
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

        if (conn->runtime_registered) {
            removePidFd(conn->pid, fd);
        }
        removeWatcherAndMaybeStopTarget(fd);

        // Delete the connection
        delete conn;
        clients_.erase(it);

        OMNI_LOG_INFO(TAG, "Client closed: fd=%d", fd);
}

void ServiceManagerApp::sendMessage(ClientConnection* conn, Message& msg) {
        Buffer output;
        if (!msg.serialize(output)) {
            OMNI_LOG_ERROR(TAG, "Failed to serialize message");
            return;
        }

        if (conn->send_offset < conn->send_buffer.size()) {
            static const size_t MAX_SEND_BUFFER = MAX_MESSAGE_SIZE;
            if (conn->send_buffer.size() + output.size() > MAX_SEND_BUFFER) {
                OMNI_LOG_ERROR(TAG, "send_buffer overflow for fd=%d (>%zuMB), disconnecting",
                               conn->fd, MAX_SEND_BUFFER / (1024*1024));
                closeClient(conn->fd);
                return;
            }
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

bool ServiceManagerApp::flushPendingSends(ClientConnection* conn) {
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

void ServiceManagerApp::enableClientWriteEvents(ClientConnection* conn) {
        loop_.modifyFd(conn->fd, EventLoop::EVENT_READ | EventLoop::EVENT_WRITE);
}

void ServiceManagerApp::disableClientWriteEvents(ClientConnection* conn) {
        loop_.modifyFd(conn->fd, EventLoop::EVENT_READ);
}

} // namespace omnibinder
