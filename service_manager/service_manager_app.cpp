#include "service_manager_app.h"
#include "core/message_reader.h"
#include "omnibinder/log.h"
#include "platform/platform.h"

#define TAG "ServiceManager"

namespace omnibinder {

// SM 主动下发消息（MSG_DEATH_NOTIFY、MSG_TOPIC_PUBLISHER_NOTIFY、MSG_DIAG_* 控制消息）
// 使用的序列号基础值。客户端请求的 seq 由其 RpcRuntime 从小整数（1,2,3...）递增分配，
// SM 主动消息若继续使用独立的小整数序列号，可能恰好命中客户端正在等待的请求 seq，
// 导致客户端把主动消息误当作请求应答（onSMMessage 的 isWaiting() 分支）。从大值起点
// 分配可彻底隔离两个序列号域。
static const uint32_t SM_PROACTIVE_SEQ_BASE = 0x40000000u;

ServiceManagerApp::ServiceManagerApp(uint32_t heartbeat_timeout_ms)
    : server_(nullptr)
    , heartbeat_(heartbeat_timeout_ms > 0 ? heartbeat_timeout_ms : DEFAULT_HEARTBEAT_TIMEOUT)
    , heartbeat_check_interval_ms_(DEFAULT_HEARTBEAT_INTERVAL)
    , heartbeat_timer_id_(0)
    , sm_seq_counter_(SM_PROACTIVE_SEQ_BASE)
    , shutdown_fd_(-1) {
    if (heartbeat_timeout_ms > 0 && heartbeat_timeout_ms < DEFAULT_HEARTBEAT_INTERVAL) {
        heartbeat_check_interval_ms_ = heartbeat_timeout_ms;
    }
}

uint32_t ServiceManagerApp::nextSMProactiveSequence() {
    uint32_t seq = sm_seq_counter_++;
    if (sm_seq_counter_ < SM_PROACTIVE_SEQ_BASE) {
        sm_seq_counter_ = SM_PROACTIVE_SEQ_BASE;
    }
    return seq;
}

ServiceManagerApp::~ServiceManagerApp() {
    cleanup();
}

int ServiceManagerApp::shutdownFd() const {
    return shutdown_fd_;
}

bool ServiceManagerApp::init(const std::string& host, uint16_t port) {
    // 初始化网络
    if (!platform::netInit()) {
        OMNI_LOG_ERROR(TAG, "Failed to initialize network");
        return false;
    }

    // 创建 TCP 服务端端点（统一契约：IServerTransport）
    server_ = createServerTransport("service_manager", TransportType::TCP, TransportConfig());
    if (!server_) {
        OMNI_LOG_ERROR(TAG, "Failed to create server transport");
        return false;
    }

    int listen_port = server_->start(host, port, TransportConfig());
    if (listen_port < 0) {
        OMNI_LOG_ERROR(TAG, "Failed to listen on %s:%u", host.c_str(), port);
        delete server_;
        server_ = nullptr;
        return false;
    }

    // 端点回调：接入/可读/断开统一由端点上报；client transport 由端点持有（非拥有）
    server_->setAcceptCallback([this](int client_id, IClientTransport* client) {
        this->onClientAccepted(client_id, client);
    });
    server_->setReadableCallback([this](int client_id) {
        this->onClientReadable(client_id);
    });
    server_->setDisconnectCallback([this](int client_id) {
        this->onClientDisconnected(client_id);
    });

    // 注册端点级 fd（TCP 为监听 socket）；客户端 fd 在接入回调中注册
    std::vector<int> poll_fds;
    server_->pollFds(poll_fds);
    for (size_t i = 0; i < poll_fds.size(); ++i) {
        registerEndpointFd(poll_fds[i]);
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

    // 启动心跳检查定时器（默认每 3 秒；超时配置较小时相应缩短）
    heartbeat_timer_id_ = loop_.addTimer(heartbeat_check_interval_ms_, [this]() {
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

    // 关闭全部客户端连接
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        ClientConnection* conn = it->second;
        if (conn) {
            loop_.removeFd(conn->fd);
            delete conn;
        }
    }
    clients_.clear();

    // 摘除全部端点 fd 后再关闭端点（约束 3：fd 从 event-loop 摘除先于资源释放）
    for (std::set<int>::iterator it = endpoint_fds_.begin(); it != endpoint_fds_.end(); ++it) {
        loop_.removeFd(*it);
    }
    endpoint_fds_.clear();

    // 端点持有所有 client transport，close() 统一关闭并释放
    if (server_) {
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

void ServiceManagerApp::registerEndpointFd(int fd) {
    if (fd < 0 || endpoint_fds_.find(fd) != endpoint_fds_.end()) {
        return;
    }
    endpoint_fds_.insert(fd);
    loop_.addFd(fd, EventLoop::EVENT_READ | EventLoop::EVENT_ERROR,
        [this, fd](int, uint32_t events) {
            this->onEndpointEvent(fd, events);
        });
}

void ServiceManagerApp::onClientAccepted(int client_id, IClientTransport* client) {
    if (!client || clients_.find(client_id) != clients_.end()) {
        return;
    }

    ClientConnection* conn = new ClientConnection();
    conn->fd = client_id;
    conn->transport = client;
    clients_[client_id] = conn;
    registerEndpointFd(client_id);

    OMNI_LOG_INFO(TAG, "Client connected: fd=%d", client_id);
}

void ServiceManagerApp::onEndpointEvent(int fd, uint32_t events) {
    // SM 自有的写背压：EVENT_WRITE 只用于 flush 待发缓冲，不改变端点读写语义
    if (events & EventLoop::EVENT_WRITE) {
        std::map<int, ClientConnection*>::iterator it = clients_.find(fd);
        if (it != clients_.end()) {
            if (!flushPendingSends(it->second)) {
                closeClient(fd);
                return;
            }
            // flush 可能因发送失败重入 closeClient
            if (clients_.find(fd) == clients_.end()) {
                return;
            }
        }
    }

    // 读/断开/接入事件交给端点分派（readable_cb_ / disconnect_cb_ / accept_cb_）
    if (server_) {
        server_->onPollEvent(fd, events);
    }
}

void ServiceManagerApp::onClientReadable(int client_id) {
    Message msg;
    while (true) {
        std::map<int, ClientConnection*>::iterator it = clients_.find(client_id);
        if (it == clients_.end()) {
            return;
        }
        ClientConnection* conn = it->second;

        int ret = readNextMessage(*conn->transport, conn->recv_buffer, msg);
        if (ret == 0) {
            return;
        }
        if (ret < 0) {
            OMNI_LOG_INFO(TAG, "Client disconnected: fd=%d", client_id);
            onClientDisconnected(client_id);
            return;
        }

        dispatchMessage(conn, msg);

        // 分发可能因发送失败关闭本连接（约束 1/2）：按 fd 重新确认存活
        if (clients_.find(client_id) == clients_.end()) {
            return;
        }
    }
}

void ServiceManagerApp::onClientDisconnected(int client_id) {
    // closeClient 末尾调用 server_->removeClient()，由端点释放 transport
    closeClient(client_id);
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
    // 先从表中摘除：notifyServiceDeath → sendDeathNotify → sendMessage 发送失败时
    // 可能再次 closeClient(fd)，早摘除可让重入在开头直接返回，避免 conn 被二次删除
    clients_.erase(it);

    // 从 event-loop 中摘除
    loop_.removeFd(fd);
    endpoint_fds_.erase(fd);

    // 清理该 fd 拥有的服务。registry fd 索引是唯一的归属权威来源：
    // 若某名称已被更新的连接重新注册，则不再索引到该 fd 下，
    // 其存活注册保持不动。
    std::vector<std::string> removed = registry_.removeByFd(fd);
    for (size_t i = 0; i < removed.size(); ++i) {
        notifyServiceRemoved(removed[i]);
    }

    // 清理死亡通知订阅
    death_notifier_.removeSubscriber(fd);

    // 清理话题订阅与发布关系
    topic_manager_.removeByFd(fd);

    if (conn->runtime_registered) {
        removePidFd(conn->pid, fd);
    }
    removeWatcherAndMaybeStopTargets(fd);

    // 删除连接
    delete conn;

    OMNI_LOG_INFO(TAG, "Client closed: fd=%d", fd);

    // 最后通知端点释放 per-client transport（非拥有；约束 3：fd 已先从 event-loop 摘除）
    if (server_) {
        server_->removeClient(fd);
    }
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
