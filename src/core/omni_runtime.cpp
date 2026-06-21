#include "core/omni_runtime.h"
#include "omnibinder/log.h"
#include "omnibinder/buffer_view.h"
#include "transport/transport_factory.h"
#include <cstring>
#include <algorithm>

#define LOG_TAG "OmniRuntime"

namespace omnibinder {

namespace {

std::string normalizeAdvertiseHost(const std::string& host) {
    if (host.empty() || host == "0.0.0.0") {
        return "127.0.0.1";
    }
    return host;
}

const char* dataChannelKindName(TransportType type) {
    return type == TransportType::SHM ? "SHM" : "TCP";
}

const uint32_t OMNI_DIAG_IFACE_ID = 0x4F4E4944;

static void diag_serialize_event(Buffer& buf, uint8_t direction, const Message& msg) {
    uint64_t ts_us = static_cast<uint64_t>(platform::currentTimeUs());
    buf.writeUint8(direction);
    uint8_t ts_buf[8];
    for (int i = 7; i >= 0; --i) {
        ts_buf[i] = static_cast<uint8_t>(ts_us); 
        ts_us >>= 8; 
    }
    buf.writeRaw(ts_buf, 8);
    buf.writeUint16(static_cast<uint16_t>(msg.getType()));
    buf.writeUint32(msg.getSequence());
    buf.writeUint32(static_cast<uint32_t>(msg.payload.size()));
    if (msg.payload.size() > 0) buf.writeRaw(msg.payload.data(), msg.payload.size());
}

bool decodeInvokePayload(const Message& msg,
                         uint32_t& interface_id,
                         uint32_t& idl_hash,
                         uint32_t& method_id,
                         Buffer& request) {
    BufferView req_buf(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!req_buf.tryReadUint32(interface_id)
        || !req_buf.tryReadUint32(idl_hash)
        || !req_buf.tryReadUint32(method_id)
        || !req_buf.tryReadUint32(payload_len)) {
        return false;
    }
    if (req_buf.remaining() < payload_len) {
        return false;
    }

    request.clear();
    if (payload_len > 0) {
        if (!request.writeRaw(req_buf.data() + req_buf.readPosition(), payload_len)) {
            return false;
        }
    }
    return true;
}

bool decodeSubscribeBroadcastPayload(const Message& msg,
                                     uint32_t& topic_id,
                                     std::string& topic_name) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    if (!buf.tryReadUint32(topic_id) || !buf.tryReadString(topic_name)) {
        return false;
    }
    return true;
}

bool decodeBroadcastPayload(const Message& msg,
                            uint32_t& topic_id,
                            Buffer& payload) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!buf.tryReadUint32(topic_id) || !buf.tryReadUint32(payload_len)) {
        return false;
    }
    if (buf.remaining() < payload_len) {
        return false;
    }

    payload.clear();
    if (payload_len > 0) {
        if (!payload.writeRaw(buf.data() + buf.readPosition(), payload_len)) {
            return false;
        }
    }
    return true;
}

bool decodeSingleStringPayload(const Message& msg, std::string& value) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    if (!buf.tryReadString(value)) {
        return false;
    }
    return true;
}

bool decodeBoolReplyPayload(const Message& msg, bool& value) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    if (!buf.tryReadBool(value)) {
        return false;
    }
    return true;
}

bool decodeUint32ReplyPayload(const Message& msg, uint32_t& value) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    if (!buf.tryReadUint32(value)) {
        return false;
    }
    return true;
}

bool decodeInvokeReplyPayload(const Message& msg, int32_t& status, Buffer& response) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!buf.tryReadInt32(status)) {
        return false;
    }
    if (status != 0) {
        response.clear();
        return true;
    }

    if (!buf.tryReadUint32(payload_len)) {
        return false;
    }
    if (buf.remaining() < payload_len) {
        return false;
    }

    response.clear();
    if (payload_len > 0) {
        if (!response.writeRaw(buf.data() + buf.readPosition(), payload_len)) {
            return false;
        }
    }
    return true;
}

bool writeInvokeErrorReply(Buffer& payload, ErrorCode error) {
    return payload.writeInt32(static_cast<int32_t>(error))
        && payload.writeUint32(0);
}

Message makeInvokeErrorReply(uint32_t seq, ErrorCode error) {
    Message reply(MessageType::MSG_INVOKE_REPLY, seq);
    (void)writeInvokeErrorReply(reply.payload, error);
    return reply;
}

Message makeInvokeSuccessReply(uint32_t seq, const Buffer& response) {
    Message reply(MessageType::MSG_INVOKE_REPLY, seq);
    reply.payload.writeInt32(0);
    reply.payload.writeUint32(static_cast<uint32_t>(response.size()));
    if (response.size() > 0) {
        reply.payload.writeRaw(response.data(), response.size());
    }
    return reply;
}

} // namespace

OmniRuntime::OmniRuntime() : impl_(new Impl()) {
    impl_->setOwner(this);
}
OmniRuntime::~OmniRuntime() { delete impl_; }

int OmniRuntime::init(const std::string& sm_host, uint16_t sm_port) {
    return impl_->init(sm_host, sm_port);
}
void OmniRuntime::run() { impl_->run(); }
void OmniRuntime::stop() { impl_->stop(); }
bool OmniRuntime::isRunning() const { return impl_->isRunning(); }
void OmniRuntime::pollOnce(int timeout_ms) { impl_->pollOnce(timeout_ms); }

int OmniRuntime::registerService(Service* service) {
    return impl_->registerService(service);
}
int OmniRuntime::unregisterService(Service* service) {
    return impl_->unregisterService(service);
}

int OmniRuntime::lookupService(const std::string& name, ServiceInfo& info) {
    return impl_->lookupService(name, info);
}
int OmniRuntime::listServices(std::vector<ServiceInfo>& services) {
    return impl_->listServices(services);
}
int OmniRuntime::queryInterfaces(const std::string& name, std::vector<InterfaceInfo>& ifaces) {
    return impl_->queryInterfaces(name, ifaces);
}

int OmniRuntime::connectService(const std::string& name) {
    return impl_->connectService(name);
}
int OmniRuntime::disconnectService(const std::string& name) {
    return impl_->disconnectService(name);
}
bool OmniRuntime::isServiceConnected(const std::string& name) const {
    return impl_->isServiceConnected(name);
}
void OmniRuntime::enableAutoReconnect(const std::string& name, bool enable) {
    impl_->enableAutoReconnect(name, enable);
}
void OmniRuntime::setReconnectInterval(const std::string& name, uint32_t interval_ms) {
    impl_->setReconnectInterval(name, interval_ms);
}
void OmniRuntime::startHeartbeat(const std::string& name, uint32_t interval_ms, uint32_t timeout_ms) {
    impl_->startHeartbeat(name, interval_ms, timeout_ms);
}
void OmniRuntime::stopHeartbeat(const std::string& name) {
    impl_->stopHeartbeat(name);
}

int OmniRuntime::invoke(const std::string& name, uint32_t iface_id, uint32_t method_id,
                          uint32_t idl_hash, const Buffer& req, Buffer& resp,
                          uint32_t timeout_ms) {
    return impl_->invoke(name, iface_id, method_id, idl_hash, req, resp, timeout_ms);
}
int OmniRuntime::invokeOneWay(const std::string& name, uint32_t iface_id,
                                uint32_t method_id, uint32_t idl_hash,
                                const Buffer& req) {
    return impl_->invokeOneWay(name, iface_id, method_id, idl_hash, req);
}

int OmniRuntime::subscribeServiceDeath(const std::string& name, const DeathCallback& cb) {
    return impl_->subscribeServiceDeath(name, cb);
}
int OmniRuntime::unsubscribeServiceDeath(const std::string& name) {
    return impl_->unsubscribeServiceDeath(name);
}

int OmniRuntime::publishTopic(const std::string& topic) {
    return impl_->publishTopic(topic);
}
int OmniRuntime::broadcast(uint32_t topic_id, const Buffer& data) {
    return impl_->broadcast(topic_id, data);
}
int OmniRuntime::subscribeTopic(const std::string& topic, const TopicCallback& on_msg,
                                 const TopicErrorCallback& on_err) {
    return impl_->subscribeTopic(topic, on_msg, on_err);
}
int OmniRuntime::unsubscribeTopic(const std::string& topic) {
    return impl_->unsubscribeTopic(topic);
}

void OmniRuntime::setRegisterHost(const std::string& host) { impl_->setRegisterHost(host); }
const std::string& OmniRuntime::getRegisterHost() const { return impl_->getRegisterHost(); }
void OmniRuntime::setHeartbeatInterval(uint32_t ms) { impl_->setHeartbeatInterval(ms); }
void OmniRuntime::setDefaultTimeout(uint32_t ms) { impl_->setDefaultTimeout(ms); }
const std::string& OmniRuntime::hostId() const { return impl_->hostId(); }
int OmniRuntime::getStats(RuntimeStats& stats) { return impl_->getStats(stats); }
int OmniRuntime::resetStats() { return impl_->resetStats(); }
void OmniRuntime::clearServiceCache() { impl_->clearServiceCache(); }
void OmniRuntime::closeAllConnections() { impl_->closeAllConnections(); }
int OmniRuntime::enableDiagnostic(const std::string& service_name) { return impl_->enableDiagnostic(service_name); }
int OmniRuntime::disableDiagnostic(const std::string& service_name) { return impl_->disableDiagnostic(service_name); }

// ============================================================
// Impl 构造/析构
// ============================================================

OmniRuntime::Impl::Impl()
    : loop_(NULL)
    , sm_channel_()
    , rpc_runtime_()
    , conn_mgr_(NULL)
    , register_host_()
    , sm_port_(0)
    , heartbeat_interval_ms_(DEFAULT_HEARTBEAT_INTERVAL)
    , heartbeat_timer_id_(0)
    , running_(false)
    , initialized_(false)
    , owner_(NULL)
    , loop_driver_active_(false)
    , sm_reconnect_needed_(false)
    , diag_active_count_(0)
{
}

OmniRuntime::Impl::~Impl() {
    stop();
    
    // Cancel heartbeat timer before cleanup
    if (loop_ && heartbeat_timer_id_ > 0) {
        loop_->cancelTimer(heartbeat_timer_id_);
        heartbeat_timer_id_ = 0;
    }

    // Cancel all per-service heartbeat timers
    for (std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.begin();
         it != heartbeat_states_.end(); ++it) {
        if (loop_ && it->second.timer_id > 0) {
            loop_->cancelTimer(it->second.timer_id);
        }
    }
    heartbeat_states_.clear();

    // Cancel all per-service reconnect timers
    for (std::map<std::string, ReconnectConfig>::iterator it = reconnect_configs_.begin();
         it != reconnect_configs_.end(); ++it) {
        if (loop_ && it->second.timer_id > 0) {
            loop_->cancelTimer(it->second.timer_id);
        }
    }
    reconnect_configs_.clear();
    
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        delete it->second;
    }
    local_services_.clear();
    
    sm_channel_.clearReplies();
    
    delete conn_mgr_;
    conn_mgr_ = NULL;
    
    if (sm_channel_.transport) {
        sm_channel_.transport->close();
        delete sm_channel_.transport;
        sm_channel_.transport = NULL;
    }
    
    delete loop_;
    loop_ = NULL;
    
    if (initialized_) {
        platform::netCleanup();
    }
}

// ============================================================
// 初始化
// ============================================================

int OmniRuntime::Impl::init(const std::string& sm_host, uint16_t sm_port) {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (initialized_) {
        return static_cast<int>(ErrorCode::ERR_ALREADY_INITIALIZED);
    }
    
    platform::netInit();
    host_id_ = platform::getMachineId();
    sm_host_ = sm_host;
    sm_port_ = sm_port;
    
    loop_ = new EventLoop();
    owner_executor_.bindLoop(loop_);
    
    sm_channel_.transport = new TcpTransport();
    int ret = sm_channel_.transport->connect(sm_host, sm_port);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "sm_connect_failed host=%s port=%u err=%d",
                       sm_host.c_str(), sm_port, static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE));
        delete sm_channel_.transport;
        sm_channel_.transport = NULL;
        delete loop_;
        loop_ = NULL;
        return static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
    }
    
    if (ret == 1) {
        for (int i = 0; i < 100 && sm_channel_.transport->state() == ConnectionState::CONNECTING; ++i) {
            sm_channel_.transport->checkConnectComplete();
            platform::sleepMs(10);
        }
        if (sm_channel_.transport->state() != ConnectionState::CONNECTED) {
            OMNI_LOG_ERROR(LOG_TAG, "sm_connect_timeout host=%s port=%u timeout_ms=%u err=%d",
                           sm_host.c_str(), sm_port, 1000u, static_cast<int>(ErrorCode::ERR_TIMEOUT));
            delete sm_channel_.transport;
            sm_channel_.transport = NULL;
            delete loop_;
            loop_ = NULL;
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }
    }
    
    loop_->addFd(sm_channel_.transport->fd(), EventLoop::EVENT_READ,
        [this](int fd, uint32_t events) { this->onSMData(fd, events); });
    
    conn_mgr_ = new ConnectionManager(*loop_, host_id_);
    conn_mgr_->setMessageCallback(
        [this](const std::string& svc, const Message& msg) { this->onDirectMessage(svc, msg); });
    conn_mgr_->setDisconnectCallback(
        [this](const std::string& svc) { this->onDirectDisconnect(svc); });
    
    heartbeat_timer_id_ = loop_->addTimer(heartbeat_interval_ms_,
        [this]() { this->sendHeartbeat(); }, true);
    
    initialized_ = true;
    sm_reconnect_needed_ = false;
    OMNI_LOG_INFO(LOG_TAG, "Connected to SM at %s:%u, host_id=%s",
                    sm_host.c_str(), sm_port, host_id_.c_str());
    return 0;
}

void OmniRuntime::Impl::run() {
    if (!initialized_) return;
    captureOwnerThread();
    loop_driver_active_ = true;
    owner_executor_.setLoopOwned(true);
    running_ = true;
    loop_->run();
    owner_executor_.setLoopOwned(false);
    loop_driver_active_ = false;
}

void OmniRuntime::Impl::stop() {
    running_ = false;
    if (loop_) loop_->stop();
}

bool OmniRuntime::Impl::isRunning() const { return running_; }

void OmniRuntime::Impl::pollOnce(int timeout_ms) {
    captureOwnerThread();
    if (loop_) loop_->pollOnce(timeout_ms);
    // SHM 通信现在完全通过 eventfd 事件驱动，不再需要轮询
}

void OmniRuntime::Impl::pollOnceWithoutFunctors(int timeout_ms) {
    if (loop_) loop_->pollOnceWithoutFunctors(timeout_ms);
}

// ============================================================
// SM 通信
// ============================================================

bool OmniRuntime::Impl::sendToSM(const Message& msg) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        return false;
    }
    return sm_channel_.sendMessage(msg);
}

bool OmniRuntime::Impl::sendToSMWithinTimeout(const Message& msg, uint32_t timeout_ms,
                                              uint32_t* elapsed_ms) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        if (elapsed_ms) {
            *elapsed_ms = 0;
        }
        return false;
    }
    return sm_channel_.sendMessageWithinTimeout(msg, timeout_ms, elapsed_ms);
}

int OmniRuntime::Impl::sendSMRequestAndWaitReply(Message& msg, Message& reply) {
    uint32_t total_timeout_ms = effectiveTimeout(0);
    uint32_t send_elapsed_ms = 0;
    if (!sendToSMWithinTimeout(msg, total_timeout_ms, &send_elapsed_ms)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    uint32_t reply_timeout_ms = total_timeout_ms > send_elapsed_ms
        ? total_timeout_ms - send_elapsed_ms : 0;
    return waitForReply(msg.getSequence(), reply_timeout_ms, reply);
}

void OmniRuntime::Impl::onSMData(int fd, uint32_t events) {
    (void)fd; (void)events;
    uint8_t buf[4096];
    int ret = sm_channel_.recvSome(buf, sizeof(buf));
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "sm_connection_lost host=%s port=%u err=%d",
                       sm_host_.c_str(), sm_port_, static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED));
        sm_reconnect_needed_ = true;
        if (sm_channel_.transport && sm_channel_.transport->fd() >= 0) {
            loop_->removeFd(sm_channel_.transport->fd());
        }
        if (sm_channel_.transport) {
            sm_channel_.transport->close();
            delete sm_channel_.transport;
            sm_channel_.transport = NULL;
        }
        return;
    }
    if (ret == 0) return;
    sm_channel_.appendReceived(buf, static_cast<size_t>(ret));
    processSMMessages();
}

void OmniRuntime::Impl::processSMMessages() {
    Message msg;
    while (sm_channel_.tryPopMessage(msg)) {
        onSMMessage(msg);
    }
}

void OmniRuntime::Impl::onSMMessage(const Message& msg) {
    MessageType type = msg.getType();
    uint32_t seq = msg.getSequence();

    if (type == MessageType::MSG_LOOKUP_REPLY
        || type == MessageType::MSG_LIST_SERVICES_REPLY
        || type == MessageType::MSG_QUERY_INTERFACES_REPLY
        || type == MessageType::MSG_SUBSCRIBE_SERVICE_REPLY
        || type == MessageType::MSG_REGISTER_REPLY
        || type == MessageType::MSG_UNREGISTER_REPLY
        || type == MessageType::MSG_PUBLISH_TOPIC_REPLY
        || type == MessageType::MSG_SUBSCRIBE_TOPIC_REPLY) {
        storePendingReply(seq, msg);
        if (sm_channel_.pendingReply(seq) != NULL) {
            return;
        }
    }
    
    if (sm_channel_.isWaiting(seq)) {
        storePendingReply(seq, msg);
        return;
    }
    
    switch (type) {
    case MessageType::MSG_HEARTBEAT_ACK:
        break;
    case MessageType::MSG_DEATH_NOTIFY: {
        std::string svc_name;
        if (!decodeSingleStringPayload(msg, svc_name)) {
            OMNI_LOG_WARN(LOG_TAG, "malformed_death_notify seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        OMNI_LOG_WARN(LOG_TAG, "Service died: %s", svc_name.c_str());
        std::map<std::string, DeathCallback>::iterator it = death_callbacks_.find(svc_name);
        if (it != death_callbacks_.end() && it->second) {
            it->second(svc_name);
        }
        service_cache_.erase(svc_name);
        conn_mgr_->removeConnection(svc_name);

        stopHeartbeat(svc_name);

        std::map<std::string, ReconnectConfig>::iterator rc_it = reconnect_configs_.find(svc_name);
        if (rc_it != reconnect_configs_.end() && rc_it->second.enabled) {
            rc_it->second.current_retry = 0;
            scheduleReconnect(svc_name, rc_it->second.interval_ms);
        }
        break;
    }
    case MessageType::MSG_TOPIC_PUBLISHER_NOTIFY: {
        BufferView buf(msg.payload.data(), msg.payload.size());
        std::string topic;
        if (!buf.tryReadString(topic)) {
            OMNI_LOG_WARN(LOG_TAG, "malformed_topic_publisher_notify seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        ServiceInfo pub_info;
        if (!deserializeServiceInfo(buf, pub_info)) {
            OMNI_LOG_ERROR(LOG_TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
            break;
        }
        OMNI_LOG_INFO(LOG_TAG, "Topic %s publisher at %s:%u",
                        topic.c_str(), pub_info.host.c_str(), pub_info.port);
        if (!ensureTopicPublisherConnection(topic, pub_info)) {
            OMNI_LOG_WARN(LOG_TAG, "Failed to establish topic publisher path for %s", topic.c_str());
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled SM message: %s", messageTypeToString(type));
        break;
    }
}

// ============================================================
// 同步等待
// ============================================================

uint32_t OmniRuntime::Impl::allocSequence() {
    return rpc_runtime_.nextSequence();
}

int OmniRuntime::Impl::waitForReply(uint32_t seq, uint32_t timeout_ms, Message& reply) {
    return rpc_runtime_.waitForReply(
        seq,
        timeout_ms,
        sm_channel_,
        [this](int wait_ms) { this->pollOnceWithoutFunctors(wait_ms); },
        reply);
}

void OmniRuntime::Impl::storePendingReply(uint32_t seq, const Message& msg) {
    sm_channel_.storeReply(seq, msg);
}

uint32_t OmniRuntime::Impl::effectiveTimeout(uint32_t timeout_ms) const {
    return rpc_runtime_.effectiveTimeout(timeout_ms);
}

// ============================================================
// 服务注册
// ============================================================

int OmniRuntime::Impl::registerService(Service* service) {
    return callSerialized([this, service]() -> int {
        return registerServiceInternal(service);
    });
}

int OmniRuntime::Impl::registerServiceInternal(Service* service) {
    if (!initialized_ || !service) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }
    
    const std::string& name = service->name();
    if (local_services_.find(name) != local_services_.end()) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_EXISTS);
    }
    
    LocalServiceEntry* entry = new LocalServiceEntry();
    entry->service = service;

    std::string advertise_host;
    int     ret = initializeServiceListener(entry, service, advertise_host);
    if (ret != 0) {
        delete entry;
        return ret;
    }

    ShmConfig shm_config = service->shmConfig();
    initializeServiceShm(name, entry,
        shm_config.req_ring_capacity > 0 ? shm_config.req_ring_capacity : SHM_DEFAULT_REQ_RING_CAPACITY,
        shm_config.resp_ring_capacity > 0 ? shm_config.resp_ring_capacity : SHM_DEFAULT_RESP_RING_CAPACITY);

    ret = registerServiceWithManager(name, service, entry, advertise_host);
    if (ret != 0) {
        cleanupPendingServiceRegistration(entry);
        delete entry;
        return ret;
    }

    local_services_[name] = entry;
    service->onStart();
    OMNI_LOG_INFO(LOG_TAG, "Registered service %s on port %u",
                    name.c_str(), entry->port);
    return 0;
}

int OmniRuntime::Impl::initializeServiceListener(LocalServiceEntry* entry, Service* service,
                                                 std::string& advertise_host) {
    entry->server = new TcpTransportServer();
    int port = entry->server->listen("0.0.0.0", 0);
    if (port < 0) {
        return static_cast<int>(ErrorCode::ERR_LISTEN_FAILED);
    }

    entry->port = static_cast<uint16_t>(port);
    service->setPort(entry->port);
    service->runtime_ = owner_;
    advertise_host = resolveRegisterHost(service,
                                         platform::getSocketAddress(entry->server->fd()));
    return 0;
}

std::string OmniRuntime::Impl::resolveRegisterHost(Service* service,
                                                     const std::string& listener_host) const {
    if (service && !service->getRegisterHost().empty()) {
        return service->getRegisterHost();
    }
    if (!register_host_.empty()) {
        return register_host_;
    }
    return normalizeAdvertiseHost(listener_host);
}

void OmniRuntime::Impl::initializeServiceShm(const std::string& name, LocalServiceEntry* entry,
                                              size_t req_ring_capacity, size_t resp_ring_capacity) {
    // Create server-side ShmTransport for UDS listening + per-client context management.
    // Each connecting client creates its own SHM; the server opens it via UDS handshake.
    entry->shm_transport = new ShmTransport(generateShmName(name), true,
                                             req_ring_capacity, resp_ring_capacity);
    if (entry->shm_transport->state() != ConnectionState::CONNECTED) {
        OMNI_LOG_WARN(LOG_TAG, "Failed to create SHM UDS listener for service %s", name.c_str());
        delete entry->shm_transport;
        entry->shm_transport = NULL;
        return;
    }

    // Register UDS listen fd with EventLoop for client handshake
    if (entry->shm_transport->eventfdEnabled()) {
        loop_->addFd(entry->shm_transport->reqEventFd(), EventLoop::EVENT_READ,
            [this, entry, name](int fd, uint32_t) {
                platform::eventFdConsume(fd);
                if (!entry->shm_transport) return;
                uint8_t buf[65536];
                uint32_t from_id = 0;
                while (true) {
                    int ret = entry->shm_transport->serverRecv(buf, sizeof(buf), from_id);
                    if (ret <= 0) break;
                    onShmRequest(name, entry, from_id, buf, static_cast<size_t>(ret));
                }
            });
    }

    loop_->addFd(entry->shm_transport->udsListenFd(), EventLoop::EVENT_READ,
        [this, entry](int, uint32_t) {
            if (entry->shm_transport) {
                entry->shm_transport->onUdsClientConnect();
            }
        });

    // Register callback for per-client notification fds.
    // On Linux: udsSendServerResponse never creates new fds, callback is a no-op.
    // On Windows: each new client gets a per-client pipe, registered here.
    entry->shm_transport->setOnNewClientNotifyFd(
        [this, entry, name](int fd) {
            loop_->addFd(fd, EventLoop::EVENT_READ,
                [this, entry, name](int efd, uint32_t) {
                    platform::eventFdConsume(efd);
                    if (!entry->shm_transport) return;
                    uint8_t buf[65536];
                    uint32_t from_id = 0;
                    while (true) {
                        int ret = entry->shm_transport->serverRecv(buf, sizeof(buf), from_id);
                        if (ret <= 0) break;
                        onShmRequest(name, entry, from_id, buf, static_cast<size_t>(ret));
                    }
                });
        });
}

int OmniRuntime::Impl::registerServiceWithManager(const std::string& name, Service* service,
                                                  LocalServiceEntry* entry,
                                                  const std::string& advertise_host) {
    loop_->addFd(entry->server->fd(), EventLoop::EVENT_READ,
        [this, name](int fd, uint32_t events) { this->onServiceAccept(name, fd, events); });
    listen_fd_to_service_[entry->server->fd()] = name;

    Message msg(MessageType::MSG_REGISTER, allocSequence());
    ServiceInfo svc_info;
    svc_info.name = name;
    svc_info.host = advertise_host;
    svc_info.port = entry->port;
    svc_info.host_id = host_id_;
    svc_info.shm_config = service->shmConfig();
    svc_info.interfaces.push_back(service->interfaceInfo());
    serializeServiceInfo(svc_info, msg.payload);

    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        return ret;
    }

    uint32_t handle = INVALID_HANDLE;
    if (!decodeUint32ReplyPayload(reply, handle)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (handle == INVALID_HANDLE) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    return 0;
}

void OmniRuntime::Impl::cleanupPendingServiceRegistration(LocalServiceEntry* entry) {
    if (!entry || !entry->server) {
        return;
    }

    removeServiceListenerFromLoop(entry);
    removeServiceShmFromLoop(entry);
}

void OmniRuntime::Impl::removeServiceListenerFromLoop(LocalServiceEntry* entry) {
    if (!entry || !entry->server) {
        return;
    }

    loop_->removeFd(entry->server->fd());
    listen_fd_to_service_.erase(entry->server->fd());
}

void OmniRuntime::Impl::removeServiceShmFromLoop(LocalServiceEntry* entry) {
    if (!entry || !entry->shm_transport) {
        return;
    }

    if (entry->shm_transport->reqEventFd() >= 0) {
        loop_->removeFd(entry->shm_transport->reqEventFd());
    }
    if (entry->shm_transport->udsListenFd() >= 0) {
        loop_->removeFd(entry->shm_transport->udsListenFd());
    }
}

int OmniRuntime::Impl::unregisterService(Service* service) {
    return callSerialized([this, service]() -> int {
        return unregisterServiceInternal(service);
    });
}

int OmniRuntime::Impl::unregisterServiceInternal(Service* service) {
    if (!service) return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    
    const std::string& name = service->name();
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
    if (it == local_services_.end()) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    
    Message msg(MessageType::MSG_UNREGISTER, allocSequence());
    msg.payload.writeString(name);
    sendToSM(msg);
    
    LocalServiceEntry* entry = it->second;
    removeServiceListenerFromLoop(entry);
    removeServiceShmFromLoop(entry);
    
    for (std::map<int, ITransport*>::iterator cit = entry->client_transports.begin();
         cit != entry->client_transports.end(); ++cit) {
        loop_->removeFd(cit->first);
        client_fd_to_service_.erase(cit->first);
        topic_runtime_.removeTcpSubscriberFd(cit->first);
    }
    
    topic_runtime_.forgetPublishedTopicsByOwner(name);
    
    service->onStop();
    service->runtime_ = NULL;
    delete entry;
    local_services_.erase(it);
    
    OMNI_LOG_INFO(LOG_TAG, "Unregistered service %s", name.c_str());
    return 0;
}

// ============================================================
// 服务发现
// ============================================================

int OmniRuntime::Impl::lookupService(const std::string& service_name, ServiceInfo& info) {
    return callSerialized([this, &service_name, &info]() -> int {
        return lookupServiceInfo(service_name, info);
    });
}

int OmniRuntime::Impl::lookupServiceInfo(const std::string& service_name, ServiceInfo& info) {
    std::map<std::string, ServiceInfo>::iterator cit = service_cache_.find(service_name);
    if (cit != service_cache_.end()) {
        info = cit->second;
        return 0;
    }

    Message msg(MessageType::MSG_LOOKUP, allocSequence());
    msg.payload.writeString(service_name);

    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    bool found = false;
    if (!decodeBoolReplyPayload(reply, found)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!found) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }

    BufferView rbuf(reply.payload.data(), reply.payload.size());
    bool ignored_found = false;
    if (!rbuf.tryReadBool(ignored_found)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!deserializeServiceInfo(rbuf, info)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }

    service_cache_[service_name] = info;
    return 0;
}

int OmniRuntime::Impl::listServices(std::vector<ServiceInfo>& services) {
    return callSerialized([this, &services]() -> int {
        return listServicesInternal(services);
    });
}

int OmniRuntime::Impl::listServicesInternal(std::vector<ServiceInfo>& services) {
    Message msg(MessageType::MSG_LIST_SERVICES, allocSequence());
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;
    
    uint32_t count = 0;
    if (!decodeUint32ReplyPayload(reply, count)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    services.clear();
    services.reserve(count);

    BufferView rbuf(reply.payload.data(), reply.payload.size());
    uint32_t ignored_count = 0;
    if (!rbuf.tryReadUint32(ignored_count)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    
    for (uint32_t i = 0; i < count; ++i) {
        ServiceInfo info;
        if (!deserializeServiceInfo(rbuf, info)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        services.push_back(info);
    }
    return 0;
}

int OmniRuntime::Impl::queryInterfaces(const std::string& service_name,
                                        std::vector<InterfaceInfo>& interfaces) {
    return callSerialized([this, &service_name, &interfaces]() -> int {
        return queryInterfacesInternal(service_name, interfaces);
    });
}

int OmniRuntime::Impl::queryInterfacesInternal(const std::string& service_name,
                                              std::vector<InterfaceInfo>& interfaces) {
    Message msg(MessageType::MSG_QUERY_INTERFACES, allocSequence());
    msg.payload.writeString(service_name);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;
    
    bool found = false;
    if (!decodeBoolReplyPayload(reply, found)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!found) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }

    BufferView rbuf(reply.payload.data(), reply.payload.size());
    bool ignored_found = false;
    if (!rbuf.tryReadBool(ignored_found)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    uint32_t count = 0;
    if (!rbuf.tryReadUint32(count)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    interfaces.clear();
    interfaces.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i) {
        InterfaceInfo info;
        if (!deserializeInterfaceInfo(rbuf, info)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        interfaces.push_back(info);
    }
    return 0;
}

// ============================================================
// 连接管理
// ============================================================

int OmniRuntime::Impl::connectService(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
        return connectServiceInternal(service_name);
    });
}

int OmniRuntime::Impl::disconnectService(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
        return disconnectServiceInternal(service_name);
    });
}

bool OmniRuntime::Impl::isServiceConnected(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> bool {
        if (!conn_mgr_) return false;
        return conn_mgr_->getConnection(service_name) != NULL;
    });
}

int OmniRuntime::Impl::connectServiceInternal(const std::string& service_name) {
    ServiceInfo info;
    int ret = lookupServiceInfo(service_name, info);
    if (ret != 0) return ret;

    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        service_name, info.host, info.port, info.host_id, info.shm_config);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "connectService failed for %s", service_name.c_str());
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    OMNI_LOG_INFO(LOG_TAG, "connectService success for %s", service_name.c_str());
    return 0;
}

int OmniRuntime::Impl::disconnectServiceInternal(const std::string& service_name) {
    if (conn_mgr_) {
        conn_mgr_->removeConnection(service_name);
    }
    service_cache_.erase(service_name);
    reconnect_configs_.erase(service_name);
    OMNI_LOG_INFO(LOG_TAG, "disconnectService for %s", service_name.c_str());
    return 0;
}

void OmniRuntime::Impl::enableAutoReconnect(const std::string& service_name, bool enable) {
    callSerialized([this, &service_name, enable]() {
        ReconnectConfig& config = reconnect_configs_[service_name];
        config.enabled = enable;
        config.current_retry = 0;
        if (!enable && config.timer_id > 0) {
            loop_->cancelTimer(config.timer_id);
            config.timer_id = 0;
        }
        OMNI_LOG_INFO(LOG_TAG, "AutoReconnect %s for %s",
                       enable ? "enabled" : "disabled", service_name.c_str());
    });
}

void OmniRuntime::Impl::setReconnectInterval(const std::string& service_name, uint32_t interval_ms) {
    callSerialized([this, &service_name, interval_ms]() {
        ReconnectConfig& config = reconnect_configs_[service_name];
        config.interval_ms = interval_ms > 0 ? interval_ms : 1000;
    });
}

void OmniRuntime::Impl::tryReconnectService(const std::string& service_name) {
    std::map<std::string, ReconnectConfig>::iterator it = reconnect_configs_.find(service_name);
    if (it == reconnect_configs_.end() || !it->second.enabled) {
        return;
    }

    ReconnectConfig& config = it->second;
    OMNI_LOG_INFO(LOG_TAG, "Trying to reconnect to %s (attempt %u)",
                   service_name.c_str(), config.current_retry + 1);

    int ret = connectServiceInternal(service_name);
    if (ret == 0) {
        OMNI_LOG_INFO(LOG_TAG, "Successfully reconnected to %s", service_name.c_str());
        config.current_retry = 0;
        config.timer_id = 0;
        // Reset heartbeat state so the periodic timer doesn't false-timeout
        std::map<std::string, HeartbeatState>::iterator hb_it = heartbeat_states_.find(service_name);
        if (hb_it != heartbeat_states_.end()) {
            hb_it->second.last_ack_time = platform::currentTimeMs();
            hb_it->second.pending = false;
        }
    } else {
        config.current_retry++;
        if (config.max_retries > 0 && config.current_retry >= config.max_retries) {
            OMNI_LOG_WARN(LOG_TAG, "Max reconnect attempts reached for %s", service_name.c_str());
            config.enabled = false;
            config.timer_id = 0;
        } else {
            uint32_t delay_ms = config.interval_ms * (1u << (config.current_retry < 5 ? config.current_retry : 5));
            scheduleReconnect(service_name, delay_ms);
        }
    }
}

static uint32_t addReconnectJitter(uint32_t delay_ms) {
    if (delay_ms <= 10) return delay_ms;
    // ±25% jitter using low bits of current time as entropy source.
    // Prevents thundering herd when multiple clients reconnect simultaneously.
    uint32_t r = static_cast<uint32_t>(platform::currentTimeMs()) % 51;
    int32_t jitter_pct = static_cast<int32_t>(r) - 25;
    uint32_t result = static_cast<uint32_t>(
        static_cast<int64_t>(delay_ms) * (100 + jitter_pct) / 100);
    return result > 0 ? result : 1;
}

void OmniRuntime::Impl::scheduleReconnect(const std::string& service_name, uint32_t delay_ms) {
    std::map<std::string, ReconnectConfig>::iterator it = reconnect_configs_.find(service_name);
    if (it == reconnect_configs_.end() || !it->second.enabled) {
        return;
    }

    ReconnectConfig& config = it->second;
    if (config.timer_id > 0) {
        loop_->cancelTimer(config.timer_id);
    }

    uint32_t jittered_ms = addReconnectJitter(delay_ms);
    config.timer_id = loop_->addTimer(jittered_ms, [this, service_name]() {
        tryReconnectService(service_name);
    }, false);

    OMNI_LOG_DEBUG(LOG_TAG, "Scheduled reconnect for %s in %u ms (base=%u ms)",
                     service_name.c_str(), jittered_ms, delay_ms);
}

void OmniRuntime::Impl::startHeartbeat(const std::string& service_name, uint32_t interval_ms, uint32_t timeout_ms) {
    callSerialized([this, &service_name, interval_ms, timeout_ms]() {
        HeartbeatState& state = heartbeat_states_[service_name];
        state.interval_ms = interval_ms > 0 ? interval_ms : 5000;
        state.timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;
        state.last_ack_time = platform::currentTimeMs();
        state.pending = false;

        if (state.timer_id > 0) {
            loop_->cancelTimer(state.timer_id);
        }

        state.timer_id = loop_->addTimer(state.interval_ms, [this, service_name]() {
            sendHeartbeatToService(service_name);
            checkHeartbeatTimeout(service_name);
        }, true);

        OMNI_LOG_INFO(LOG_TAG, "Started heartbeat for %s (interval=%u ms, timeout=%u ms)",
                       service_name.c_str(), state.interval_ms, state.timeout_ms);
    });
}

void OmniRuntime::Impl::stopHeartbeat(const std::string& service_name) {
    callSerialized([this, &service_name]() {
        std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
        if (it != heartbeat_states_.end()) {
            if (it->second.timer_id > 0) {
                loop_->cancelTimer(it->second.timer_id);
            }
            heartbeat_states_.erase(it);
            OMNI_LOG_INFO(LOG_TAG, "Stopped heartbeat for %s", service_name.c_str());
        }
    });
}

void OmniRuntime::Impl::sendHeartbeatToService(const std::string& service_name) {
    std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
    if (it == heartbeat_states_.end()) {
        return;
    }

    HeartbeatState& state = it->second;
    if (state.pending) {
        return;
    }

    Message msg(MessageType::MSG_HEARTBEAT, allocSequence());
    if (conn_mgr_->sendMessage(service_name, msg)) {
        state.pending = true;
        OMNI_LOG_DEBUG(LOG_TAG, "Sent heartbeat to %s", service_name.c_str());
    }
}

void OmniRuntime::Impl::checkHeartbeatTimeout(const std::string& service_name) {
    std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
    if (it == heartbeat_states_.end()) {
        return;
    }

    HeartbeatState& state = it->second;
    if (!state.pending) {
        return;
    }

    int64_t now = platform::currentTimeMs();
    int64_t elapsed = now - state.last_ack_time;
    if (elapsed > static_cast<int64_t>(state.timeout_ms)) {
        OMNI_LOG_WARN(LOG_TAG, "Heartbeat timeout for %s (elapsed=%lld ms)", service_name.c_str(), elapsed);

        service_cache_.erase(service_name);
        if (conn_mgr_) {
            conn_mgr_->removeConnection(service_name);
        }

        std::map<std::string, ReconnectConfig>::iterator rc_it = reconnect_configs_.find(service_name);
        if (rc_it != reconnect_configs_.end() && rc_it->second.enabled) {
            rc_it->second.current_retry = 0;
            scheduleReconnect(service_name, rc_it->second.interval_ms);
        }

        stopHeartbeat(service_name);
    }
}

// ============================================================
// 远程调用
// ============================================================

int OmniRuntime::Impl::invoke(const std::string& service_name, uint32_t interface_id,
                               uint32_t method_id, uint32_t idl_hash,
                               const Buffer& request, Buffer& response,
                               uint32_t timeout_ms) {
    return callSerialized([this, &service_name, interface_id, method_id, idl_hash, &request, &response, timeout_ms]() -> int {
        return invokeInternal(service_name, interface_id, method_id, idl_hash, request, response, timeout_ms);
    });
}

int OmniRuntime::Impl::invokeInternal(const std::string& service_name, uint32_t interface_id,
                                    uint32_t method_id, uint32_t idl_hash,
                                    const Buffer& request, Buffer& response,
                                    uint32_t timeout_ms) {
    stats_.total_rpc_calls++;
    uint32_t total_timeout_ms = effectiveTimeout(timeout_ms);

    ServiceConnection* conn = conn_mgr_->getConnection(service_name);
    if (!conn) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    uint32_t seq = allocSequence();
    Message msg(MessageType::MSG_INVOKE, seq);
    populateInvokeMessage(msg, interface_id, method_id, idl_hash, request);

    uint32_t send_elapsed_ms = 0;
    if (!conn_mgr_->sendMessageWithinTimeout(service_name, msg, total_timeout_ms, &send_elapsed_ms)) {
        if (send_elapsed_ms >= total_timeout_ms) {
            stats_.total_rpc_timeouts++;
            stats_.total_rpc_failures++;
            OMNI_LOG_WARN(LOG_TAG,
                          "rpc_send_timeout service=%s iface=0x%08x method=0x%08x timeout_ms=%u",
                          service_name.c_str(), interface_id, method_id, total_timeout_ms);
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }
        stats_.connection_errors++;
        stats_.total_rpc_failures++;
        OMNI_LOG_ERROR(LOG_TAG,
                       "rpc_send_failed service=%s iface=0x%08x method=0x%08x err=%d",
                       service_name.c_str(), interface_id, method_id,
                       static_cast<int>(ErrorCode::ERR_SEND_FAILED));
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    uint32_t reply_timeout_ms = total_timeout_ms > send_elapsed_ms
        ? total_timeout_ms - send_elapsed_ms
        : 0;

    Message reply;
    int ret = waitForReply(seq, reply_timeout_ms, reply);
    if (ret != 0) {
        if (ret == static_cast<int>(ErrorCode::ERR_TIMEOUT)) {
            stats_.total_rpc_timeouts++;
            OMNI_LOG_WARN(LOG_TAG,
                          "rpc_timeout service=%s iface=0x%08x method=0x%08x timeout_ms=%u err=%d",
                          service_name.c_str(), interface_id, method_id,
                          total_timeout_ms, ret);
        }
        stats_.total_rpc_failures++;
        return ret;
    }

    int32_t status = 0;
    if (!decodeInvokeReplyPayload(reply, status, response)) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (status != 0) {
        stats_.total_rpc_failures++;
        return status;
    }
    stats_.total_rpc_success++;
    return 0;
}

int OmniRuntime::Impl::invokeOneWay(const std::string& service_name, uint32_t interface_id,
                                     uint32_t method_id, uint32_t idl_hash,
                                     const Buffer& request) {
    return callSerialized([this, &service_name, interface_id, method_id, idl_hash, &request]() -> int {
        return invokeOneWayInternal(service_name, interface_id, method_id, idl_hash, request);
    });
}

int OmniRuntime::Impl::invokeOneWayInternal(const std::string& service_name, uint32_t interface_id,
                                           uint32_t method_id, uint32_t idl_hash,
                                           const Buffer& request) {
    stats_.total_rpc_calls++;

    ServiceConnection* conn = conn_mgr_->getConnection(service_name);
    if (!conn) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    Message msg(MessageType::MSG_INVOKE_ONEWAY, allocSequence());
    populateInvokeMessage(msg, interface_id, method_id, idl_hash, request);

    if (!conn_mgr_->sendMessage(service_name, msg)) {
        stats_.connection_errors++;
        stats_.total_rpc_failures++;
        OMNI_LOG_ERROR(LOG_TAG,
                       "rpc_send_failed service=%s iface=0x%08x method=0x%08x err=%d",
                       service_name.c_str(), interface_id, method_id,
                       static_cast<int>(ErrorCode::ERR_SEND_FAILED));
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    stats_.total_rpc_success++;
    return 0;
}

// ============================================================
// 死亡通知
// ============================================================

int OmniRuntime::Impl::subscribeServiceDeath(const std::string& service_name,
                                               const DeathCallback& callback) {
    return callSerialized([this, &service_name, &callback]() -> int {
        return subscribeServiceDeathInternal(service_name, callback);
    });
}

int OmniRuntime::Impl::subscribeServiceDeathInternal(const std::string& service_name,
                                                    const DeathCallback& callback) {
    Message msg(MessageType::MSG_SUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(service_name);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    death_callbacks_[service_name] = callback;
    return 0;
}

int OmniRuntime::Impl::unsubscribeServiceDeath(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
        return unsubscribeServiceDeathInternal(service_name);
    });
}

int OmniRuntime::Impl::unsubscribeServiceDeathInternal(const std::string& service_name) {
    Message msg(MessageType::MSG_UNSUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(service_name);
    sendToSM(msg);
    death_callbacks_.erase(service_name);
    return 0;
}

// ============================================================
// 话题
// ============================================================

int OmniRuntime::Impl::publishTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
        return publishTopicInternal(topic_name);
    });
}

int OmniRuntime::Impl::publishTopicInternal(const std::string& topic_name) {
    if (!initialized_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }

    Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    // Build publisher ServiceInfo so SM can tell subscribers where to connect.
    // Use the first registered local service's listen port. If no local service
    // is registered, create a dedicated publisher listener.
    ServiceInfo pub_info;
    pub_info.host_id = host_id_;
    
    if (!local_services_.empty()) {
        LocalServiceEntry* entry = local_services_.begin()->second;
        pub_info.name = local_services_.begin()->first;
        pub_info.host = normalizeAdvertiseHost(platform::getSocketAddress(entry->server->fd()));
        pub_info.port = entry->port;
        if (entry->service) {
            pub_info.shm_config = entry->service->shmConfig();
        }
    } else {
        OMNI_LOG_WARN(LOG_TAG, "publishTopic requires a registered local service to advertise publisher endpoint");
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    serializeServiceInfo(pub_info, msg.payload);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    topic_runtime_.rememberPublishedTopic(topic_name, topic_id, pub_info.name);
    return 0;
}

int OmniRuntime::Impl::broadcast(uint32_t topic_id, const Buffer& data) {
    return callSerialized([this, topic_id, &data]() -> int {
        return broadcastInternal(topic_id, data);
    });
}

int OmniRuntime::Impl::broadcastInternal(uint32_t topic_id, const Buffer& data) {
    Message msg(MessageType::MSG_BROADCAST, 0);
    msg.payload.writeUint32(topic_id);
    msg.payload.writeUint32(static_cast<uint32_t>(data.size()));
    if (data.size() > 0) {
        msg.payload.writeRaw(data.data(), data.size());
    }

    // Diagnostic BROADCAST hook (skip for diag topics to prevent recursion)
    if (diag_active_count_ > 0) {
        bool is_diag_topic = false;
        for (auto& kv : local_services_) {
            if (kv.second->diag_topic_id == topic_id) { is_diag_topic = true; break; }
        }
        if (!is_diag_topic) {
            for (auto& kv : local_services_) {
                if (kv.second->diag_enabled && kv.second->diag_topic_id != 0) {
                    Buffer diag_buf;
                    diag_serialize_event(diag_buf, 4, msg);
                    broadcastInternal(kv.second->diag_topic_id, diag_buf);
                }
            }
        }
    }

    Buffer send_buf;
    msg.serialize(send_buf);
    
    std::vector<int>* tcp_subscribers = topic_runtime_.tcpSubscribers(topic_id);
    if (tcp_subscribers) {
        OMNI_LOG_DEBUG(LOG_TAG, "Broadcast topic 0x%08x over TCP to %zu subscribers",
                        topic_id, tcp_subscribers->size());
        std::vector<int>& fds = *tcp_subscribers;
        // Iterate in reverse so we can safely erase stale entries
        for (int i = static_cast<int>(fds.size()) - 1; i >= 0; --i) {
            std::map<int, std::string>::iterator sit = client_fd_to_service_.find(fds[i]);
            if (sit == client_fd_to_service_.end()) {
                // Stale fd — remove it (fix #4: broadcast_subscribers_ cleanup)
                fds.erase(fds.begin() + i);
                continue;
            }
            std::string svc = sit->second;
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(svc);
            if (eit != local_services_.end()) {
                std::map<int, ITransport*>::iterator tit = eit->second->client_transports.find(fds[i]);
                if (tit != eit->second->client_transports.end()) {
                    // Use sendOnFd for proper partial-write handling (fix #1)
                    Message broadcast_msg;
                    broadcast_msg.header = msg.header;
                    broadcast_msg.payload.assign(msg.payload.data(), msg.payload.size());
                    if (!sendOnFd(tit->second, broadcast_msg)) {
                        OMNI_LOG_WARN(LOG_TAG, "broadcast send failed to fd=%d", fds[i]);
                    }
                }
            }
        }
    }

    // Broadcast via SHM to all SHM-connected subscribers
    {
        std::vector<std::string>* shm_services = topic_runtime_.shmSubscriberServices(topic_id);
        if (shm_services) {
            for (size_t i = 0; i < shm_services->size(); ++i) {
                const std::string& svc_name = (*shm_services)[i];
                std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(svc_name);
                if (eit != local_services_.end() && eit->second->shm_transport) {
                    eit->second->shm_transport->serverBroadcast(send_buf.data(), send_buf.size());
                }
            }
        }
    }

    return 0;
}

InvokeDispatchResult OmniRuntime::Impl::dispatchLocalInvoke(Service* service, const Message& msg,
                                                            const char* transport_label,
                                                            const char* service_name) {
    // Diagnostic intercept
    {
        uint32_t diag_iface_id = 0;
        uint32_t diag_method_id = 0;
        uint32_t diag_payload_len = 0;
        BufferView check_buf(msg.payload.data(), msg.payload.size());
        if (check_buf.tryReadUint32(diag_iface_id) && check_buf.tryReadUint32(diag_method_id)
            && check_buf.tryReadUint32(diag_payload_len)) {
            if (diag_iface_id == OMNI_DIAG_IFACE_ID) {
                Buffer request;
                if (diag_payload_len > 0) {
                    if (!request.writeRaw(check_buf.data() + check_buf.readPosition(), diag_payload_len)) {
                        InvokeDispatchResult result;
                        result.status = InvokeDispatchStatus::DECODE_FAILED;
                        result.error_code = static_cast<int>(ErrorCode::ERR_DESERIALIZE);
                        return result;
                    }
                }
                InvokeDispatchResult result;
                result.status = InvokeDispatchStatus::SUCCESS;
                result.error_code = 0;
                LocalServiceEntry* entry = nullptr;
                for (auto& kv : local_services_) { if (kv.second->service == service) { entry = kv.second; break; } }
                if (!entry) {
                    result.status = InvokeDispatchStatus::INVOKE_FAILED;
                    result.error_code = static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
                    return result;
                }
                bool enable = (request.size() >= 1 && request.data()[0] != 0);
                if (!enable && diag_active_count_ == 0) {
                    result.response.writeUint8(0);
                    return result;
                }
                if (enable && !entry->diag_enabled) {
                    std::string diag_topic = "__diag__" + std::string(service_name);
                    auto tid_it = topic_runtime_.published_topics.find(diag_topic);
                    if (tid_it == topic_runtime_.published_topics.end()) {
                        int pub_ret = publishTopicInternal(diag_topic);
                        if (pub_ret != 0) {
                            result.status = InvokeDispatchStatus::INVOKE_FAILED;
                            result.error_code = pub_ret;
                        } else {
                            tid_it = topic_runtime_.published_topics.find(diag_topic);
                        }
                    }
                    if (tid_it != topic_runtime_.published_topics.end()) {
                        entry->diag_topic_id = tid_it->second;
                        entry->diag_enabled = true;
                        diag_active_count_++;
                        OMNI_LOG_INFO(LOG_TAG, "diag_enabled service=%s topic=%s topic_id=0x%08x",
                                      service_name, diag_topic.c_str(), entry->diag_topic_id);
                        result.response.writeUint8(0);
                    } else {
                        result.status = InvokeDispatchStatus::INVOKE_FAILED;
                        result.error_code = -1;
                    }
                } else if (!enable) {
                    entry->diag_enabled = false;
                    entry->diag_topic_id = 0;
                    OMNI_LOG_INFO(LOG_TAG, "diag_disabled service=%s", service_name);
                    result.response.writeUint8(0);
                } else {
                    result.response.writeUint8(0);
                }
                return result;
            }
        }
    }

    InvokeDispatchResult result;
    result.error_code = 0;

    uint32_t interface_id = 0;
    uint32_t client_idl_hash = 0;
    uint32_t method_id = 0;
    Buffer request;
    if (!decodeInvokePayload(msg, interface_id, client_idl_hash, method_id, request)) {
        OMNI_LOG_WARN(LOG_TAG,
                      "malformed_invoke_payload service=%s transport=%s seq=%u err=%d",
                      service_name, transport_label, msg.getSequence(),
                      static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        result.status = InvokeDispatchStatus::DECODE_FAILED;
        result.error_code = static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        return result;
    }
    if (service->interfaceInfo().interface_id != interface_id) {
        result.status = InvokeDispatchStatus::INTERFACE_MISMATCH;
        result.error_code = static_cast<int>(ErrorCode::ERR_INTERFACE_NOT_FOUND);
        return result;
    }

    if (client_idl_hash != 0) {
        uint32_t server_idl_hash = 0;
        const InterfaceInfo& iface = service->interfaceInfo();
        for (size_t i = 0; i < iface.methods.size(); ++i) {
            if (iface.methods[i].method_id == method_id) {
                server_idl_hash = iface.methods[i].idl_hash;
                break;
            }
        }
        if (server_idl_hash != 0 && client_idl_hash != server_idl_hash) {
            OMNI_LOG_WARN(LOG_TAG,
                          "idl_mismatch service=%s transport=%s method=0x%08x "
                          "client_hash=0x%08x server_hash=0x%08x",
                          service_name, transport_label, method_id,
                          client_idl_hash, server_idl_hash);
            result.status = InvokeDispatchStatus::IDL_MISMATCH;
            result.error_code = static_cast<int>(ErrorCode::ERR_IDL_MISMATCH);
            return result;
        }
    }

    int invoke_status = service->onInvoke(method_id, request, result.response);
    if (invoke_status != 0) {
        OMNI_LOG_WARN(LOG_TAG,
                      "invoke_failed service=%s transport=%s seq=%u method=0x%08x err=%d",
                      service_name, transport_label, msg.getSequence(), method_id,
                      invoke_status);
        result.status = InvokeDispatchStatus::INVOKE_FAILED;
        result.error_code = invoke_status;
        return result;
    }

    result.status = InvokeDispatchStatus::SUCCESS;
    return result;
}

void OmniRuntime::Impl::onTopicBroadcastMessage(const Message& msg) {
    uint32_t topic_id = 0;
    Buffer data;
    if (!decodeBroadcastPayload(msg, topic_id, data)) {
        OMNI_LOG_WARN(LOG_TAG, "Invalid topic broadcast payload: topic=0x%08x len=%u remaining=%zu",
                        topic_id, msg.payload.size(), static_cast<size_t>(0));
        return;
    }

    topic_runtime_.dispatch(topic_id, data);
}

int OmniRuntime::Impl::subscribeTopic(const std::string& topic_name,
                                       const TopicCallback& on_message,
                                       const TopicErrorCallback& on_error) {
    return callSerialized([this, &topic_name, &on_message, &on_error]() -> int {
        int ret = subscribeTopicInternal(topic_name, on_message);
        if (ret == 0) {
            topic_runtime_.setErrorCallback(topic_name, on_error);
        }
        return ret;
    });
}

int OmniRuntime::Impl::subscribeTopicInternal(const std::string& topic_name,
                                             const TopicCallback& callback) {
    Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    Buffer reply_payload(reply.payload.data(), reply.payload.size());
    bool accepted = false;
    if (!reply_payload.tryReadBool(accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    uint32_t publisher_idl_hash = 0;
    reply_payload.tryReadUint32(publisher_idl_hash);
    (void)publisher_idl_hash;

    topic_runtime_.rememberSubscription(topic_name, callback);
    return 0;
}

int OmniRuntime::Impl::unsubscribeTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
        return unsubscribeTopicInternal(topic_name);
    });
}

int OmniRuntime::Impl::unsubscribeTopicInternal(const std::string& topic_name) {
    Message msg(MessageType::MSG_UNSUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    sendToSM(msg);
    topic_runtime_.forgetSubscription(topic_name);
    return 0;
}

// ============================================================
// 本地服务 accept 和请求处理
// ============================================================

void OmniRuntime::Impl::onServiceAccept(const std::string& service_name,
                                          int listen_fd, uint32_t events) {
    (void)listen_fd; (void)events;
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;

    service_host_runtime_.onServiceAccept(
        service_name,
        it->second,
        loop_,
        client_fd_to_service_,
        [this](const std::string& svc, int fd, uint32_t ev) {
            this->onServiceClientData(svc, fd, ev);
        },
        [](LocalServiceEntry* entry, int cfd) {
            if (entry && entry->service) {
                entry->service->onClientConnected("fd=" + std::to_string(cfd));
            }
        });
}

void OmniRuntime::Impl::onServiceClientData(const std::string& service_name,
                                              int client_fd, uint32_t events) {
    (void)events;
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    service_host_runtime_.onServiceClientData(
        service_name,
        it->second,
        client_fd,
        topic_runtime_,
        loop_,
        client_fd_to_service_,
        [this](const std::string& svc, int fd, const Message& msg) {
            this->onInvokeRequest(svc, fd, msg, "TCP");
        },
        [this](const std::string& svc, const Message& msg) {
            this->onInvokeOneWayRequest(svc, msg, "TCP");
        },
        [this](int fd, const Message& msg) {
            this->onSubscribeBroadcast(fd, msg, "TCP");
        },
        [this](const std::string& svc, int fd) {
            this->removeServiceClient(svc, fd);
        },
        [](LocalServiceEntry* entry, int client_fd) {
            if (entry && entry->service) {
                entry->service->onClientDisconnected("fd=" + std::to_string(client_fd));
            }
        });
}

void OmniRuntime::Impl::onInvokeRequest(const std::string& service_name,
                                        int client_fd, const Message& msg,
                                        const char* transport_label) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;

    Service* service = it->second->service;
    if (!service) return;

    if (it->second->diag_enabled && it->second->diag_topic_id != 0) {
        Buffer diag_buf;
        diag_serialize_event(diag_buf, 0, msg);
        broadcastInternal(it->second->diag_topic_id, diag_buf);
    }

    std::map<int, ITransport*>::iterator transport_it = it->second->client_transports.find(client_fd);
    ITransport* transport = (transport_it != it->second->client_transports.end()) ? transport_it->second : nullptr;

    InvokeDispatchResult result = dispatchLocalInvoke(service, msg, transport_label, service_name.c_str());
    if (result.status == InvokeDispatchStatus::SUCCESS) {
        Message reply = makeInvokeSuccessReply(msg.getSequence(), result.response);
        if (it->second->diag_enabled && it->second->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, 1, reply);
            broadcastInternal(it->second->diag_topic_id, diag_buf);
        }
        if (transport && !sendOnFd(transport, reply)) {
            OMNI_LOG_WARN(LOG_TAG, "invoke_reply_send_failed service=%s fd=%d seq=%u status=0",
                          service_name.c_str(), client_fd, msg.getSequence());
        }
    } else {
        Message reply = makeInvokeErrorReply(msg.getSequence(), static_cast<ErrorCode>(result.error_code));
        if (it->second->diag_enabled && it->second->diag_topic_id != 0) {
            Buffer diag_buf;
            diag_serialize_event(diag_buf, 1, reply);
            broadcastInternal(it->second->diag_topic_id, diag_buf);
        }
        if (transport && !sendOnFd(transport, reply)) {
            OMNI_LOG_WARN(LOG_TAG, "invoke_reply_send_failed service=%s fd=%d seq=%u status=%d",
                          service_name.c_str(), client_fd, msg.getSequence(), result.error_code);
        }
    }
}

void OmniRuntime::Impl::onInvokeOneWayRequest(const std::string& service_name,
                                                      const Message& msg,
                                                      const char* transport_label) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    Service* service = it->second->service;
    if (!service) return;
    if (it->second->diag_enabled && it->second->diag_topic_id != 0) {
        Buffer diag_buf;
        diag_serialize_event(diag_buf, 2, msg);
        broadcastInternal(it->second->diag_topic_id, diag_buf);
    }

    InvokeDispatchResult result = dispatchLocalInvoke(service, msg, transport_label, service_name.c_str());
    if (result.status == InvokeDispatchStatus::IDL_MISMATCH) {
        OMNI_LOG_ERROR(LOG_TAG,
                       "oneway_idl_mismatch service=%s method=0x%08x err=%d — message discarded",
                       service_name.c_str(), 0, result.error_code);
    }
}

void OmniRuntime::Impl::onSubscribeBroadcast(int client_fd, const Message& msg,
                                                 const char* transport_label) {
    uint32_t topic_id = 0;
    std::string topic_name;
    if (!decodeSubscribeBroadcastPayload(msg, topic_id, topic_name)) {
        OMNI_LOG_WARN(LOG_TAG,
                      "malformed_subscribe_broadcast transport=%s seq=%u err=%d",
                      transport_label,
                      msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
        return;
    }
    {
        std::map<int, std::string>::iterator fd_it = client_fd_to_service_.find(client_fd);
        if (fd_it != client_fd_to_service_.end()) {
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(fd_it->second);
            if (eit != local_services_.end() && eit->second->diag_enabled && eit->second->diag_topic_id != 0) {
                Buffer diag_buf;
                diag_serialize_event(diag_buf, 3, msg);
                broadcastInternal(eit->second->diag_topic_id, diag_buf);
            }
        }
    }
    topic_runtime_.addTcpSubscriber(topic_id, client_fd);
    OMNI_LOG_INFO(LOG_TAG, "Broadcast subscriber fd=%d added for topic %s (id=0x%08x)",
                    client_fd, topic_name.c_str(), topic_id);
}

void OmniRuntime::Impl::removeServiceClient(const std::string& service_name, int client_fd) {
    loop_->removeFd(client_fd);
    client_fd_to_service_.erase(client_fd);
    
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it != local_services_.end()) {
        LocalServiceEntry* entry = it->second;
        topic_runtime_.removeTcpSubscriberFd(client_fd);
        std::map<int, ITransport*>::iterator tit = entry->client_transports.find(client_fd);
        if (tit != entry->client_transports.end()) {
            tit->second->close();
            delete tit->second;
            entry->client_transports.erase(tit);
        }
        std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_fd);
        if (bit != entry->client_recv_buffers.end()) {
            delete bit->second;
            entry->client_recv_buffers.erase(bit);
        }
        if (entry->service) {
            entry->service->onClientDisconnected("fd=" + std::to_string(client_fd));
        }
    }
}

// ============================================================
// ConnectionManager 回调
// ============================================================

void OmniRuntime::Impl::onDirectMessage(const std::string& service_name, const Message& msg) {
    MessageType type = msg.getType();
    uint32_t seq = msg.getSequence();

    if (type == MessageType::MSG_INVOKE_REPLY) {
        storePendingReply(seq, msg);
        if (sm_channel_.pendingReply(seq) != NULL) {
            return;
        }
    }

    if (sm_channel_.isWaiting(seq)) {
        storePendingReply(seq, msg);
        return;
    }
    
    switch (type) {
    case MessageType::MSG_BROADCAST: {
        uint32_t topic_id = 0;
        Buffer data;
        if (!decodeBroadcastPayload(msg, topic_id, data)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "malformed_broadcast transport=direct seq=%u err=%d",
                          msg.getSequence(), static_cast<int>(ErrorCode::ERR_DESERIALIZE));
            break;
        }
        topic_runtime_.dispatch(topic_id, data);
        break;
    }
    case MessageType::MSG_HEARTBEAT_ACK: {
        std::map<std::string, HeartbeatState>::iterator it = heartbeat_states_.find(service_name);
        if (it != heartbeat_states_.end()) {
            it->second.last_ack_time = platform::currentTimeMs();
            it->second.pending = false;
        }
        break;
    }
    default:
        OMNI_LOG_DEBUG(LOG_TAG, "Unhandled direct message from %s: %s",
                         service_name.c_str(), messageTypeToString(type));
        break;
    }
}

void OmniRuntime::Impl::onDirectDisconnect(const std::string& service_name) {
    OMNI_LOG_WARN(LOG_TAG, "Direct connection to %s lost", service_name.c_str());
    stats_.connection_errors++;
    service_cache_.erase(service_name);
    if (conn_mgr_) {
        conn_mgr_->removeConnection(service_name);
    }
    stopHeartbeat(service_name);
    
    // Trigger reconnect if auto-reconnect is enabled for this service
    std::map<std::string, ReconnectConfig>::iterator rc_it = reconnect_configs_.find(service_name);
    if (rc_it != reconnect_configs_.end() && rc_it->second.enabled) {
        rc_it->second.current_retry = 0;
        scheduleReconnect(service_name, rc_it->second.interval_ms);
    }
    
    // If this was a topic publisher connection, clean up subscription state (fix #12)
    static const std::string prefix = "topic_pub_";
    if (service_name.compare(0, prefix.size(), prefix) == 0) {
        std::string topic = service_name.substr(prefix.size());
        topic_runtime_.forgetSubscription(topic);
        OMNI_LOG_WARN(LOG_TAG, "Topic publisher for '%s' disconnected, subscription cleared",
                        topic.c_str());
    }
}

// ============================================================
// SHM 服务端请求处理（由 eventfd 回调触发）
// ============================================================

void OmniRuntime::Impl::onShmRequest(const std::string& service_name,
                                            LocalServiceEntry* entry,
                                            uint32_t client_id,
                                            const uint8_t* data, size_t length) {
    if (!entry || !entry->service) return;

    // Handle MSG_HEARTBEAT on the SHM path — the TCP path handles this
    // in ServiceHostRuntime::handleClientData(), but the SHM callback
    // path has no equivalent handler, so we must send the ACK here.
    if (length >= MESSAGE_HEADER_SIZE) {
        MessageHeader hdr;
        if (Message::parseHeader(data, length, hdr)
            && Message::validateHeader(hdr)
            && hdr.type == static_cast<uint16_t>(MessageType::MSG_HEARTBEAT)) {
            Message ack(MessageType::MSG_HEARTBEAT_ACK, hdr.sequence);
            Buffer ack_buf;
            if (ack.serialize(ack_buf) && entry->shm_transport) {
                entry->shm_transport->serverSend(client_id, ack_buf.data(), ack_buf.size());
            }
            return;
        }
    }

    service_host_runtime_.onShmRequest(
        service_name,
        client_id,
        data,
        length,
        topic_runtime_,
        [this, entry](const std::string& svc, uint32_t cid, const Message& msg) {
            (void)svc;
            if (entry->diag_enabled && entry->diag_topic_id != 0) {
                Buffer diag_buf;
                diag_serialize_event(diag_buf, 0, msg);
                broadcastInternal(entry->diag_topic_id, diag_buf);
            }
            InvokeDispatchResult result = dispatchLocalInvoke(entry->service, msg, "SHM",
                                                               entry->service->serviceName());
            Message reply_msg;
            if (result.status == InvokeDispatchStatus::SUCCESS) {
                reply_msg = makeInvokeSuccessReply(msg.getSequence(), result.response);
            } else {
                reply_msg = makeInvokeErrorReply(msg.getSequence(), static_cast<ErrorCode>(result.error_code));
            }
            if (entry->diag_enabled && entry->diag_topic_id != 0) {
                Buffer diag_buf;
                diag_serialize_event(diag_buf, 1, reply_msg);
                broadcastInternal(entry->diag_topic_id, diag_buf);
            }
            Buffer send_buf;
            if (!reply_msg.serialize(send_buf)) {
                OMNI_LOG_ERROR(LOG_TAG, "shm_reply_serialize_failed service=%s client_id=%u seq=%u",
                               entry->service->serviceName(), cid, msg.getSequence());
                return;
            }
            if (entry->shm_transport) {
                int send_ret = entry->shm_transport->serverSend(cid, send_buf.data(), send_buf.size());
                if (send_ret <= 0) {
                    OMNI_LOG_ERROR(LOG_TAG, "shm_reply_send_failed service=%s client_id=%u seq=%u ret=%d",
                                   entry->service->serviceName(), cid, msg.getSequence(), send_ret);
                }
            }
        },
        [this, entry](const std::string& svc, const Message& msg) {
            (void)svc;
            if (entry->diag_enabled && entry->diag_topic_id != 0) {
                Buffer diag_buf;
                diag_serialize_event(diag_buf, 2, msg);
                broadcastInternal(entry->diag_topic_id, diag_buf);
            }
            (void)dispatchLocalInvoke(entry->service, msg, "SHM", entry->service->serviceName());
        },
        [this](const std::string& svc, uint32_t cid, const Message& msg) {
            (void)cid;
            uint32_t topic_id = 0;
            std::string topic_name;
            if (!decodeSubscribeBroadcastPayload(msg, topic_id, topic_name)) {
                OMNI_LOG_WARN(LOG_TAG,
                              "malformed_subscribe_broadcast transport=SHM seq=%u service=%s err=%d",
                              msg.getSequence(), svc.c_str(),
                              static_cast<int>(ErrorCode::ERR_DESERIALIZE));
                return;
            }
            {
                std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(svc);
                if (eit != local_services_.end() && eit->second->diag_enabled && eit->second->diag_topic_id != 0) {
                    Buffer diag_buf;
                    diag_serialize_event(diag_buf, 3, msg);
                    broadcastInternal(eit->second->diag_topic_id, diag_buf);
                }
            }
            topic_runtime_.addShmSubscriberService(topic_id, svc);
            OMNI_LOG_INFO(LOG_TAG, "SHM broadcast subscriber for topic %s (0x%08x) on %s",
                            topic_name.c_str(), topic_id, svc.c_str());
        });
}

// ============================================================
// 心跳
// ============================================================

void OmniRuntime::Impl::sendHeartbeat() {
    // Send heartbeat for each registered local service
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        Message msg(MessageType::MSG_HEARTBEAT, 0);
        if (!msg.payload.writeString(it->first)) {
            OMNI_LOG_ERROR(LOG_TAG, "heartbeat_serialize_failed service=%s", it->first.c_str());
            continue;
        }
        sendToSM(msg);
    }
}

// ============================================================
// 辅助
// ============================================================

bool OmniRuntime::Impl::sendOnFd(ITransport* transport, const Message& msg) {
    if (!transport) return false;
    Buffer buf;
    if (!msg.serialize(buf)) {
        return false;
    }
    return platform::socketSendAll(transport->fd(), buf.data(), buf.size(), effectiveTimeout(0), NULL);
}

void OmniRuntime::Impl::populateInvokeMessage(Message& msg, uint32_t interface_id,
                                              uint32_t method_id, uint32_t idl_hash,
                                              const Buffer& request) const {
    if (!msg.payload.writeUint32(interface_id)
        || !msg.payload.writeUint32(idl_hash)
        || !msg.payload.writeUint32(method_id)
        || !msg.payload.writeUint32(static_cast<uint32_t>(request.size()))
        || (request.size() > 0 && !msg.payload.writeRaw(request.data(), request.size()))) {
        OMNI_LOG_ERROR(LOG_TAG,
                       "invoke_payload_serialize_failed iface=0x%08x method=0x%08x err=%d",
                       interface_id, method_id, static_cast<int>(ErrorCode::ERR_SERIALIZE));
    }
}

std::string OmniRuntime::Impl::topicPublisherServiceName(const std::string& topic_name) const {
    return "topic_pub_" + topic_name;
}

bool OmniRuntime::Impl::ensureTopicPublisherConnection(const std::string& topic_name,
                                                      const ServiceInfo& pub_info) {
    std::string pub_name = !pub_info.name.empty() ? pub_info.name : topicPublisherServiceName(topic_name);
    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        pub_name, pub_info.host, pub_info.port, pub_info.host_id, pub_info.shm_config);
    if (!conn) {
        return false;
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    Message sub_msg(MessageType::MSG_SUBSCRIBE_BROADCAST, 0);
    sub_msg.payload.writeUint32(topic_id);
    sub_msg.payload.writeString(topic_name);

    OMNI_LOG_INFO(LOG_TAG, "Topic %s uses %s data path to publisher %s",
                    topic_name.c_str(), dataChannelKindName(conn->transport->type()),
                    pub_name.c_str());
    return conn_mgr_->sendMessage(pub_name, sub_msg);
}

void OmniRuntime::Impl::setHeartbeatInterval(uint32_t ms) {
    callSerialized([this, ms]() {
        heartbeat_interval_ms_ = ms;
    });
}

void OmniRuntime::Impl::setRegisterHost(const std::string& host) {
    callSerialized([this, host]() {
        register_host_ = host;
    });
}

const std::string& OmniRuntime::Impl::getRegisterHost() const {
    return register_host_;
}

void OmniRuntime::Impl::setDefaultTimeout(uint32_t ms) {
    callSerialized([this, ms]() {
        rpc_runtime_.setDefaultTimeout(ms);
    });
}

const std::string& OmniRuntime::Impl::hostId() const {
    return host_id_;
}

bool OmniRuntime::Impl::isOwnerThread() const {
    return owner_executor_.isOwnerThread();
}

void OmniRuntime::Impl::captureOwnerThread() {
    if (!owner_executor_.hasOwnerThread()) {
        owner_executor_.setOwnerThread(std::this_thread::get_id());
    }
}

int OmniRuntime::Impl::reconnectServiceManager() {
    stats_.sm_reconnect_attempts++;
    OMNI_LOG_WARN(LOG_TAG, "sm_reconnect_begin host=%s port=%u attempt=%lu",
                  sm_host_.c_str(), sm_port_,
                  static_cast<unsigned long>(stats_.sm_reconnect_attempts));
    if (!loop_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }

    if (sm_channel_.transport && sm_channel_.transport->fd() >= 0) {
        loop_->removeFd(sm_channel_.transport->fd());
    }
    if (sm_channel_.transport) {
        sm_channel_.transport->close();
        delete sm_channel_.transport;
        sm_channel_.transport = NULL;
    }
    sm_channel_.clearReplies();

    sm_channel_.transport = new TcpTransport();
    int ret = sm_channel_.transport->connect(sm_host_, sm_port_);
    if (ret < 0) {
        delete sm_channel_.transport;
        sm_channel_.transport = NULL;
        return static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
    }

    if (ret == 1) {
        for (int i = 0; i < 100 && sm_channel_.transport->state() == ConnectionState::CONNECTING; ++i) {
            sm_channel_.transport->checkConnectComplete();
            platform::sleepMs(10);
        }
        if (sm_channel_.transport->state() != ConnectionState::CONNECTED) {
            sm_channel_.transport->close();
            delete sm_channel_.transport;
            sm_channel_.transport = NULL;
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }
    }

    loop_->addFd(sm_channel_.transport->fd(), EventLoop::EVENT_READ,
        [this](int fd, uint32_t events) { this->onSMData(fd, events); });
    sm_reconnect_needed_ = false;
    stats_.sm_reconnect_successes++;
    service_cache_.clear();
    if (conn_mgr_) {
        conn_mgr_->closeAll();
    }
    OMNI_LOG_INFO(LOG_TAG, "sm_reconnect_success host=%s port=%u success_count=%lu",
                  sm_host_.c_str(), sm_port_,
                  static_cast<unsigned long>(stats_.sm_reconnect_successes));
    return 0;
}

int OmniRuntime::Impl::restoreControlPlaneState() {
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        LocalServiceEntry* entry = it->second;
        if (!entry || !entry->service) {
            continue;
        }

        Message msg(MessageType::MSG_REGISTER, allocSequence());
        ServiceInfo svc_info;
        svc_info.name = it->first;
        svc_info.host = resolveRegisterHost(entry->service,
                                            platform::getSocketAddress(entry->server->fd()));
        svc_info.port = entry->port;
        svc_info.host_id = host_id_;
        svc_info.interfaces.push_back(entry->service->interfaceInfo());
        serializeServiceInfo(svc_info, msg.payload);
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
    }

    for (std::map<std::string, DeathCallback>::iterator it = death_callbacks_.begin();
         it != death_callbacks_.end(); ++it) {
        Message msg(MessageType::MSG_SUBSCRIBE_SERVICE, allocSequence());
        msg.payload.writeString(it->first);
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
    }

    std::map<std::string, std::string> published_owners = topic_runtime_.publishedTopicOwners();
    for (std::map<std::string, std::string>::iterator it = published_owners.begin();
         it != published_owners.end(); ++it) {
        std::map<std::string, LocalServiceEntry*>::iterator sit = local_services_.find(it->second);
        if (sit == local_services_.end() || !sit->second || !sit->second->service) {
            continue;
        }
        Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
        msg.payload.writeString(it->first);
        ServiceInfo pub_info;
        pub_info.name = it->second;
        pub_info.host = normalizeAdvertiseHost(platform::getSocketAddress(sit->second->server->fd()));
        pub_info.port = sit->second->port;
        pub_info.host_id = host_id_;
        pub_info.shm_config = sit->second->service->shmConfig();
    serializeServiceInfo(pub_info, msg.payload);
    msg.payload.writeUint32(0);  // idl_hash placeholder, generated proxy will overwrite
    
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
    }

    std::map<std::string, TopicCallback> subscriptions = topic_runtime_.subscriptions();
    for (std::map<std::string, TopicCallback>::iterator it = subscriptions.begin();
         it != subscriptions.end(); ++it) {
        Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
        msg.payload.writeString(it->first);
        if (!sendToSM(msg)) {
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        Message reply;
        int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

int OmniRuntime::Impl::reconnectServiceManagerIfNeeded() {
    if (!initialized_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }
    if (!sm_reconnect_needed_.load() && sm_channel_.isConnected()) {
        return 0;
    }
    int ret = reconnectServiceManager();
    if (ret != 0) {
        return ret;
    }
    return restoreControlPlaneState();
}

void OmniRuntime::Impl::updateConnectionStats(RuntimeStats& stats) const {
    if (!conn_mgr_) {
        stats.active_connections = 0;
        stats.tcp_connections = 0;
        stats.shm_connections = 0;
        return;
    }
    stats.active_connections = conn_mgr_->activeConnectionCount();
    stats.tcp_connections = conn_mgr_->tcpConnectionCount();
    stats.shm_connections = conn_mgr_->shmConnectionCount();
}

int OmniRuntime::Impl::getStats(RuntimeStats& stats) {
    return callSerialized([this, &stats]() -> int {
        return getStatsInternal(stats);
    });
}

int OmniRuntime::Impl::getStatsInternal(RuntimeStats& stats) {
    stats = stats_;
    updateConnectionStats(stats);
    return 0;
}

int OmniRuntime::Impl::resetStats() {
    return callSerialized([this]() -> int {
        resetStatsInternal();
        return 0;
    });
}

int OmniRuntime::Impl::resetStatsInternal() {
    stats_ = RuntimeStats();
    return 0;
}

void OmniRuntime::Impl::clearServiceCache() {
    service_cache_.clear();
}

void OmniRuntime::Impl::closeAllConnections() {
    if (conn_mgr_) {
        conn_mgr_->closeAll();
    }
}

int OmniRuntime::Impl::enableDiagnostic(const std::string& service_name) {
    Buffer req, resp;
    req.writeUint8(1);
    int ret = invoke(service_name, OMNI_DIAG_IFACE_ID, 1, 0, req, resp, 3000);
    if (ret != 0) return ret;
    if (resp.size() >= 1) return resp.data()[0];
    return -1;
}

int OmniRuntime::Impl::disableDiagnostic(const std::string& service_name) {
    Buffer req, resp;
    req.writeUint8(0);
    int ret = invoke(service_name, OMNI_DIAG_IFACE_ID, 1, 0, req, resp, 3000);
    if (ret != 0) return ret;
    if (resp.size() >= 1) return resp.data()[0];
    return -1;
}


} // namespace omnibinder
