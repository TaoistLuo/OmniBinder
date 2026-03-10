#include "core/omni_runtime.h"
#include "omnibinder/log.h"
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

int OmniRuntime::invoke(const std::string& name, uint32_t iface_id, uint32_t method_id,
                          const Buffer& req, Buffer& resp, uint32_t timeout_ms) {
    return impl_->invoke(name, iface_id, method_id, req, resp, timeout_ms);
}
int OmniRuntime::invokeOneWay(const std::string& name, uint32_t iface_id,
                                uint32_t method_id, const Buffer& req) {
    return impl_->invokeOneWay(name, iface_id, method_id, req);
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
int OmniRuntime::subscribeTopic(const std::string& topic, const TopicCallback& cb) {
    return impl_->subscribeTopic(topic, cb);
}
int OmniRuntime::unsubscribeTopic(const std::string& topic) {
    return impl_->unsubscribeTopic(topic);
}

void OmniRuntime::setHeartbeatInterval(uint32_t ms) { impl_->setHeartbeatInterval(ms); }
void OmniRuntime::setDefaultTimeout(uint32_t ms) { impl_->setDefaultTimeout(ms); }
const std::string& OmniRuntime::hostId() const { return impl_->hostId(); }
int OmniRuntime::getStats(RuntimeStats& stats) { return impl_->getStats(stats); }
void OmniRuntime::resetStats() { impl_->resetStats(); }

// ============================================================
// Impl 构造/析构
// ============================================================

OmniRuntime::Impl::Impl()
    : loop_(NULL)
    , sm_channel_()
    , rpc_runtime_()
    , conn_mgr_(NULL)
    , sm_port_(0)
    , heartbeat_interval_ms_(DEFAULT_HEARTBEAT_INTERVAL)
    , heartbeat_timer_id_(0)
    , running_(false)
    , initialized_(false)
    , owner_(NULL)
    , owner_thread_id_()
    , loop_driver_active_(false)
    , sm_reconnect_needed_(false)
    , run_loop_owned_(false)
{
}

OmniRuntime::Impl::~Impl() {
    stop();
    
    // Cancel heartbeat timer before cleanup
    if (loop_ && heartbeat_timer_id_ > 0) {
        loop_->cancelTimer(heartbeat_timer_id_);
        heartbeat_timer_id_ = 0;
    }
    
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
    run_loop_owned_ = true;
    running_ = true;
    loop_->run();
    run_loop_owned_ = false;
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

// ============================================================
// SM 通信
// ============================================================

bool OmniRuntime::Impl::sendToSM(const Message& msg) {
    if (reconnectServiceManagerIfNeeded() != 0) {
        return false;
    }
    return sm_channel_.sendMessage(msg);
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
        handleSMMessage(msg);
    }
}

void OmniRuntime::Impl::handleSMMessage(const Message& msg) {
    MessageType type = msg.getType();
    uint32_t seq = msg.getSequence();
    
    if (sm_channel_.isWaiting(seq)) {
        storePendingReply(seq, msg);
        return;
    }
    
    switch (type) {
    case MessageType::MSG_HEARTBEAT_ACK:
        break;
    case MessageType::MSG_DEATH_NOTIFY: {
        Buffer buf(msg.payload.data(), msg.payload.size());
        std::string svc_name = buf.readString();
        OMNI_LOG_WARN(LOG_TAG, "Service died: %s", svc_name.c_str());
        std::map<std::string, DeathCallback>::iterator it = death_callbacks_.find(svc_name);
        if (it != death_callbacks_.end() && it->second) {
            it->second(svc_name);
        }
        service_cache_.erase(svc_name);
        conn_mgr_->removeConnection(svc_name);
        break;
    }
    case MessageType::MSG_TOPIC_PUBLISHER_NOTIFY: {
        Buffer buf(msg.payload.data(), msg.payload.size());
        std::string topic = buf.readString();
        ServiceInfo pub_info;
        if (!deserializeServiceInfo(buf, pub_info)) {
            OMNI_LOG_ERROR(LOG_TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
            break;
        }
        OMNI_LOG_INFO(LOG_TAG, "Topic %s publisher at %s:%u (shm=%s)",
                        topic.c_str(), pub_info.host.c_str(), pub_info.port,
                        pub_info.shm_name.empty() ? "none" : pub_info.shm_name.c_str());
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
        [this](int wait_ms) { this->pollOnce(wait_ms); },
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
    if (!initialized_ || !service) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }
    
    const std::string& name = service->name();
    if (local_services_.find(name) != local_services_.end()) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_EXISTS);
    }
    
    LocalServiceEntry* entry = new LocalServiceEntry();
    entry->service = service;

    // 1. 创建 TCP 监听
    entry->server = new TcpTransportServer();
    int port = entry->server->listen("0.0.0.0", 0);
    if (port < 0) {
        delete entry;
        return static_cast<int>(ErrorCode::ERR_LISTEN_FAILED);
    }
    entry->port = static_cast<uint16_t>(port);
    service->setPort(entry->port);
    service->runtime_ = owner_;
    std::string advertise_host = normalizeAdvertiseHost(platform::getSocketAddress(entry->server->fd()));

    // 2. 创建共享内存（服务端侧，多客户端共享）
    entry->shm_name = generateShmName(name);
    ShmConfig shm_config = service->shmConfig();
    size_t req_ring_capacity = shm_config.req_ring_capacity > 0
        ? shm_config.req_ring_capacity
        : SHM_DEFAULT_REQ_RING_CAPACITY;
    size_t resp_ring_capacity = shm_config.resp_ring_capacity > 0
        ? shm_config.resp_ring_capacity
        : SHM_DEFAULT_RESP_RING_CAPACITY;
    entry->shm_server = new ShmTransport(entry->shm_name, true,
                                         req_ring_capacity, resp_ring_capacity);
    if (entry->shm_server->state() != ConnectionState::CONNECTED) {
        OMNI_LOG_WARN(LOG_TAG, "Failed to create SHM for service %s, SHM disabled",
                        name.c_str());
        delete entry->shm_server;
        entry->shm_server = NULL;
        entry->shm_name.clear();
    } else {
        OMNI_LOG_INFO(LOG_TAG, "Created SHM '%s' for service %s",
                        entry->shm_name.c_str(), name.c_str());

        // 注册 req_eventfd 到 EventLoop — 请求到达时触发回调
        if (entry->shm_server->eventfdEnabled() && entry->shm_server->reqEventFd() >= 0) {
            loop_->addFd(entry->shm_server->reqEventFd(), EventLoop::EVENT_READ,
                [this, name](int fd, uint32_t events) {
                    (void)events;
                    platform::eventFdConsume(fd);
                    // 处理该服务的所有 SHM 请求
                    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(name);
                    if (it == local_services_.end() || !it->second->shm_server) return;
                    LocalServiceEntry* e = it->second;
                    uint8_t buf[65536];
                    uint32_t client_id = 0;
                    while (true) {
                        int ret = e->shm_server->serverRecv(buf, sizeof(buf), client_id);
                        if (ret <= 0) break;
                        handleShmRequest(name, e, client_id, buf, static_cast<size_t>(ret));
                    }
                });
        }

        // 注册 uds_listen_fd 到 EventLoop — 接受客户端 eventfd 交换
        if (entry->shm_server->udsListenFd() >= 0) {
            ShmTransport* shm = entry->shm_server;
            loop_->addFd(entry->shm_server->udsListenFd(), EventLoop::EVENT_READ,
                [shm](int fd, uint32_t events) {
                    (void)fd; (void)events;
                    shm->onUdsClientConnect();
                });
        }
    }
    
    loop_->addFd(entry->server->fd(), EventLoop::EVENT_READ,
        [this, name](int fd, uint32_t events) { this->onServiceAccept(name, fd, events); });
    listen_fd_to_service_[entry->server->fd()] = name;
    
    Message msg(MessageType::MSG_REGISTER, allocSequence());
    ServiceInfo svc_info;
    svc_info.name = name;
    svc_info.host = advertise_host;
    svc_info.port = entry->port;
    svc_info.host_id = host_id_;
    svc_info.shm_name = entry->shm_name;
    svc_info.shm_config = ShmConfig(req_ring_capacity, resp_ring_capacity);
    svc_info.interfaces.push_back(service->interfaceInfo());
    serializeServiceInfo(svc_info, msg.payload);
    
    if (!sendToSM(msg)) {
        loop_->removeFd(entry->server->fd());
        listen_fd_to_service_.erase(entry->server->fd());
        delete entry;
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    
    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        loop_->removeFd(entry->server->fd());
        listen_fd_to_service_.erase(entry->server->fd());
        delete entry;
        return ret;
    }
    
    Buffer rbuf(reply.payload.data(), reply.payload.size());
    uint32_t handle = rbuf.readUint32();
    if (handle == INVALID_HANDLE) {
        loop_->removeFd(entry->server->fd());
        listen_fd_to_service_.erase(entry->server->fd());
        delete entry;
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    
    local_services_[name] = entry;
    service->onStart();
    OMNI_LOG_INFO(LOG_TAG, "Registered service %s on port %u (shm=%s)",
                    name.c_str(), entry->port,
                    entry->shm_name.empty() ? "none" : entry->shm_name.c_str());
    return 0;
    });
}

int OmniRuntime::Impl::unregisterService(Service* service) {
    return callSerialized([this, service]() -> int {
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
    loop_->removeFd(entry->server->fd());
    listen_fd_to_service_.erase(entry->server->fd());

    // Remove SHM eventfd/UDS fds from EventLoop
    if (entry->shm_server) {
        if (entry->shm_server->reqEventFd() >= 0) {
            loop_->removeFd(entry->shm_server->reqEventFd());
        }
        if (entry->shm_server->udsListenFd() >= 0) {
            loop_->removeFd(entry->shm_server->udsListenFd());
        }
    }
    
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
    });
}

// ============================================================
// 服务发现
// ============================================================

int OmniRuntime::Impl::lookupService(const std::string& service_name, ServiceInfo& info) {
    return callSerialized([this, &service_name, &info]() -> int {
    return lookupServiceUnlocked(service_name, info);
    });
}

int OmniRuntime::Impl::lookupServiceUnlocked(const std::string& service_name, ServiceInfo& info) {
    std::map<std::string, ServiceInfo>::iterator cit = service_cache_.find(service_name);
    if (cit != service_cache_.end()) {
        info = cit->second;
        return 0;
    }

    Message msg(MessageType::MSG_LOOKUP, allocSequence());
    msg.payload.writeString(service_name);

    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) return ret;

    Buffer rbuf(reply.payload.data(), reply.payload.size());
    bool found = rbuf.readBool();
    if (!found) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }

    if (!deserializeServiceInfo(rbuf, info)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }

    service_cache_[service_name] = info;
    return 0;
}

int OmniRuntime::Impl::listServices(std::vector<ServiceInfo>& services) {
    return callSerialized([this, &services]() -> int {
    Message msg(MessageType::MSG_LIST_SERVICES, allocSequence());
    
    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    
    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) return ret;
    
    Buffer rbuf(reply.payload.data(), reply.payload.size());
    uint32_t count = rbuf.readUint32();
    services.clear();
    services.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i) {
        ServiceInfo info;
        if (!deserializeServiceInfo(rbuf, info)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        services.push_back(info);
    }
    return 0;
    });
}

int OmniRuntime::Impl::queryInterfaces(const std::string& service_name,
                                        std::vector<InterfaceInfo>& interfaces) {
    return callSerialized([this, &service_name, &interfaces]() -> int {
    Message msg(MessageType::MSG_QUERY_INTERFACES, allocSequence());
    msg.payload.writeString(service_name);
    
    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    
    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) return ret;
    
    Buffer rbuf(reply.payload.data(), reply.payload.size());
    bool found = rbuf.readBool();
    if (!found) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    
    uint32_t count = rbuf.readUint32();
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
    });
}

// ============================================================
// 远程调用
// ============================================================

int OmniRuntime::Impl::invoke(const std::string& service_name, uint32_t interface_id,
                               uint32_t method_id, const Buffer& request, Buffer& response,
                               uint32_t timeout_ms) {
    return callSerialized([this, &service_name, interface_id, method_id, &request, &response, timeout_ms]() -> int {
    stats_.total_rpc_calls++;
    ServiceInfo info;
    int ret = lookupServiceUnlocked(service_name, info);
    if (ret != 0) {
        stats_.total_rpc_failures++;
        return ret;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
            service_name, info.host, info.port, info.host_id, info.shm_name, info.shm_config);
        if (!conn) {
            if (attempt == 0 && refreshServiceConnectionUnlocked(service_name, info, conn)) {
                continue;
            }
            stats_.connection_errors++;
            stats_.total_rpc_failures++;
            return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
        }

        uint32_t seq = allocSequence();
        Message msg(MessageType::MSG_INVOKE, seq);
        msg.payload.writeUint32(interface_id);
        msg.payload.writeUint32(method_id);
        msg.payload.writeUint32(static_cast<uint32_t>(request.size()));
        if (request.size() > 0) {
            msg.payload.writeRaw(request.data(), request.size());
        }

        if (!conn_mgr_->sendMessage(service_name, msg)) {
            stats_.connection_errors++;
            OMNI_LOG_ERROR(LOG_TAG,
                           "rpc_send_failed service=%s iface=0x%08x method=0x%08x err=%d attempt=%d",
                           service_name.c_str(), interface_id, method_id,
                           static_cast<int>(ErrorCode::ERR_SEND_FAILED), attempt + 1);
            if (attempt == 0 && refreshServiceConnectionUnlocked(service_name, info, conn)) {
                continue;
            }
            stats_.total_rpc_failures++;
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }

        Message reply;
        ret = waitForReply(seq, effectiveTimeout(timeout_ms), reply);
        if (ret != 0) {
            if (ret == static_cast<int>(ErrorCode::ERR_TIMEOUT)) {
                stats_.total_rpc_timeouts++;
                OMNI_LOG_WARN(LOG_TAG,
                              "rpc_timeout service=%s iface=0x%08x method=0x%08x timeout_ms=%u err=%d",
                              service_name.c_str(), interface_id, method_id,
                              effectiveTimeout(timeout_ms), ret);
            }
            stats_.total_rpc_failures++;
            return ret;
        }

        Buffer rbuf(reply.payload.data(), reply.payload.size());
        int32_t status = rbuf.readInt32();
        if (status != 0) {
            stats_.total_rpc_failures++;
            return status;
        }

        uint32_t payload_len = rbuf.readUint32();
        if (payload_len > 0 && rbuf.remaining() >= payload_len) {
            response.clear();
            response.writeRaw(rbuf.data() + rbuf.readPosition(), payload_len);
        }
        stats_.total_rpc_success++;
        return 0;
    }

    stats_.total_rpc_failures++;
    return static_cast<int>(ErrorCode::ERR_INVOKE_FAILED);
    });
}

int OmniRuntime::Impl::invokeOneWay(const std::string& service_name, uint32_t interface_id,
                                     uint32_t method_id, const Buffer& request) {
    return callSerialized([this, &service_name, interface_id, method_id, &request]() -> int {
    stats_.total_rpc_calls++;
    ServiceInfo info;
    int ret = lookupServiceUnlocked(service_name, info);
    if (ret != 0) {
        stats_.total_rpc_failures++;
        return ret;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
            service_name, info.host, info.port, info.host_id, info.shm_name, info.shm_config);
        if (!conn) {
            if (attempt == 0 && refreshServiceConnectionUnlocked(service_name, info, conn)) {
                continue;
            }
            stats_.connection_errors++;
            stats_.total_rpc_failures++;
            return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
        }

        Message msg(MessageType::MSG_INVOKE_ONEWAY, allocSequence());
        msg.payload.writeUint32(interface_id);
        msg.payload.writeUint32(method_id);
        msg.payload.writeUint32(static_cast<uint32_t>(request.size()));
        if (request.size() > 0) {
            msg.payload.writeRaw(request.data(), request.size());
        }

        if (!conn_mgr_->sendMessage(service_name, msg)) {
            stats_.connection_errors++;
            if (attempt == 0 && refreshServiceConnectionUnlocked(service_name, info, conn)) {
                continue;
            }
            stats_.total_rpc_failures++;
            return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
        }
        stats_.total_rpc_success++;
        return 0;
    }

    stats_.total_rpc_failures++;
    return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    });
}

// ============================================================
// 死亡通知
// ============================================================

int OmniRuntime::Impl::subscribeServiceDeath(const std::string& service_name,
                                               const DeathCallback& callback) {
    return callSerialized([this, &service_name, &callback]() -> int {
    Message msg(MessageType::MSG_SUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(service_name);
    
    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        return ret;
    }

    Buffer rbuf(reply.payload.data(), reply.payload.size());
    if (!rbuf.readBool()) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    death_callbacks_[service_name] = callback;
    return 0;
    });
}

int OmniRuntime::Impl::unsubscribeServiceDeath(const std::string& service_name) {
    return callSerialized([this, &service_name]() -> int {
    Message msg(MessageType::MSG_UNSUBSCRIBE_SERVICE, allocSequence());
    msg.payload.writeString(service_name);
    sendToSM(msg);
    death_callbacks_.erase(service_name);
    return 0;
    });
}

// ============================================================
// 话题
// ============================================================

int OmniRuntime::Impl::publishTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
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
        pub_info.shm_name = entry->shm_name;
        if (entry->service) {
            ShmConfig cfg = entry->service->shmConfig();
            pub_info.shm_config.req_ring_capacity = cfg.req_ring_capacity > 0
                ? cfg.req_ring_capacity : SHM_DEFAULT_REQ_RING_CAPACITY;
            pub_info.shm_config.resp_ring_capacity = cfg.resp_ring_capacity > 0
                ? cfg.resp_ring_capacity : SHM_DEFAULT_RESP_RING_CAPACITY;
        }
    } else {
        OMNI_LOG_WARN(LOG_TAG, "publishTopic requires a registered local service to advertise publisher endpoint");
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    serializeServiceInfo(pub_info, msg.payload);
    
    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        return ret;
    }

    Buffer rbuf(reply.payload.data(), reply.payload.size());
    if (!rbuf.readBool()) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    topic_runtime_.rememberPublishedTopic(topic_name, topic_id, pub_info.name);
    return 0;
    });
}

int OmniRuntime::Impl::broadcast(uint32_t topic_id, const Buffer& data) {
    return callSerialized([this, topic_id, &data]() -> int {
    Message msg(MessageType::MSG_BROADCAST, 0);
    msg.payload.writeUint32(topic_id);
    msg.payload.writeUint32(static_cast<uint32_t>(data.size()));
    if (data.size() > 0) {
        msg.payload.writeRaw(data.data(), data.size());
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

    // Also broadcast via SHM to any SHM-connected subscribers
    std::vector<std::string>* shm_services = topic_runtime_.shmSubscriberServices(topic_id);
    if (shm_services) {
        OMNI_LOG_DEBUG(LOG_TAG, "Broadcast topic 0x%08x over SHM to %zu services",
                        topic_id, shm_services->size());
        for (size_t i = 0; i < shm_services->size(); ++i) {
            const std::string& svc_name = (*shm_services)[i];
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(svc_name);
            if (eit != local_services_.end() && eit->second->shm_server) {
                eit->second->shm_server->serverBroadcast(send_buf.data(), send_buf.size());
            }
        }
    }

    return 0;
    });
}

void OmniRuntime::Impl::handleTopicBroadcastMessage(const Message& msg) {
    Buffer buf(msg.payload.data(), msg.payload.size());
    uint32_t topic_id = buf.readUint32();
    uint32_t payload_len = buf.readUint32();
    if (buf.remaining() < payload_len) {
        OMNI_LOG_WARN(LOG_TAG, "Invalid topic broadcast payload: topic=0x%08x len=%u remaining=%zu",
                        topic_id, payload_len, buf.remaining());
        return;
    }

    Buffer data;
    if (payload_len > 0) {
        data.writeRaw(buf.data() + buf.readPosition(), payload_len);
    }

    topic_runtime_.dispatch(topic_id, data);
}

int OmniRuntime::Impl::subscribeTopic(const std::string& topic_name,
                                       const TopicCallback& callback) {
    return callSerialized([this, &topic_name, &callback]() -> int {
    Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        return ret;
    }

    Buffer rbuf(reply.payload.data(), reply.payload.size());
    if (!rbuf.readBool()) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    topic_runtime_.rememberSubscription(topic_name, callback);
    return 0;
    });
}

int OmniRuntime::Impl::unsubscribeTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
    Message msg(MessageType::MSG_UNSUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    sendToSM(msg);
    topic_runtime_.forgetSubscription(topic_name);
    return 0;
    });
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
            this->handleInvokeRequest(svc, fd, msg);
        },
        [this](const std::string& svc, const Message& msg) {
            this->handleInvokeOneWayRequest(svc, msg);
        },
        [this](int fd, const Message& msg) {
            this->handleSubscribeBroadcast(fd, msg);
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

void OmniRuntime::Impl::handleInvokeRequest(const std::string& service_name,
                                               int client_fd, const Message& msg) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    
    Service* service = it->second->service;
    if (!service) return;
    Buffer req_buf(msg.payload.data(), msg.payload.size());
    uint32_t interface_id = req_buf.readUint32();
    uint32_t method_id = req_buf.readUint32();
    uint32_t payload_len = req_buf.readUint32();
    if (service->interfaceInfo().interface_id != interface_id) {
        Message reply(MessageType::MSG_INVOKE_REPLY, msg.getSequence());
        reply.payload.writeInt32(static_cast<int32_t>(ErrorCode::ERR_INTERFACE_NOT_FOUND));
        reply.payload.writeUint32(0);
        std::map<int, ITransport*>::iterator tit = it->second->client_transports.find(client_fd);
        if (tit != it->second->client_transports.end()) {
            sendOnFd(tit->second, reply);
        }
        return;
    }
    Buffer request;
    if (payload_len > 0 && req_buf.remaining() >= payload_len) {
        request.writeRaw(req_buf.data() + req_buf.readPosition(), payload_len);
    }
    Buffer response;
    service->onInvoke(method_id, request, response);
    Message reply(MessageType::MSG_INVOKE_REPLY, msg.getSequence());
    reply.payload.writeInt32(0);
    reply.payload.writeUint32(static_cast<uint32_t>(response.size()));
    if (response.size() > 0) {
        reply.payload.writeRaw(response.data(), response.size());
    }
    std::map<int, ITransport*>::iterator tit = it->second->client_transports.find(client_fd);
    if (tit != it->second->client_transports.end()) {
        sendOnFd(tit->second, reply);
    }
}

void OmniRuntime::Impl::handleInvokeOneWayRequest(const std::string& service_name,
                                                     const Message& msg) {
    std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.find(service_name);
    if (it == local_services_.end()) return;
    Service* service = it->second->service;
    if (!service) return;
    Buffer req_buf(msg.payload.data(), msg.payload.size());
    uint32_t interface_id = req_buf.readUint32();
    uint32_t method_id = req_buf.readUint32();
    uint32_t payload_len = req_buf.readUint32();
    if (service->interfaceInfo().interface_id != interface_id) {
        return;
    }
    Buffer request;
    if (payload_len > 0 && req_buf.remaining() >= payload_len) {
        request.writeRaw(req_buf.data() + req_buf.readPosition(), payload_len);
    }
    Buffer response;
    service->onInvoke(method_id, request, response);
}

void OmniRuntime::Impl::handleSubscribeBroadcast(int client_fd, const Message& msg) {
    Buffer buf(msg.payload.data(), msg.payload.size());
    uint32_t topic_id = buf.readUint32();
    std::string topic_name = buf.readString();
    
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

    if (sm_channel_.isWaiting(seq)) {
        storePendingReply(seq, msg);
        return;
    }
    
    switch (type) {
    case MessageType::MSG_BROADCAST: {
        Buffer buf(msg.payload.data(), msg.payload.size());
        uint32_t topic_id = buf.readUint32();
        uint32_t data_len = buf.readUint32();
        Buffer data;
        if (data_len > 0 && buf.remaining() >= data_len) {
            data.writeRaw(buf.data() + buf.readPosition(), data_len);
        }
        topic_runtime_.dispatch(topic_id, data);
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
// SHM 服务端请求处理（由 eventfd 回调触发，不再轮询）
// ============================================================

void OmniRuntime::Impl::pollShmServices() {
    // 此函数已被 eventfd 回调替代，保留为空以兼容接口
}

void OmniRuntime::Impl::handleShmRequest(const std::string& service_name,
                                            LocalServiceEntry* entry,
                                            uint32_t client_id,
                                            const uint8_t* data, size_t length) {
    if (!entry || !entry->service) return;

    service_host_runtime_.onShmRequest(
        service_name,
        client_id,
        data,
        length,
        topic_runtime_,
        [entry](const std::string& svc, uint32_t cid, const Message& msg) {
            (void)svc;
            Buffer req_buf(msg.payload.data(), msg.payload.size());
            uint32_t interface_id = req_buf.readUint32();
            uint32_t method_id = req_buf.readUint32();
            uint32_t payload_len = req_buf.readUint32();
            if (entry->service->interfaceInfo().interface_id != interface_id) {
                Message reply(MessageType::MSG_INVOKE_REPLY, msg.getSequence());
                reply.payload.writeInt32(static_cast<int32_t>(ErrorCode::ERR_INTERFACE_NOT_FOUND));
                reply.payload.writeUint32(0);
                Buffer send_buf;
                reply.serialize(send_buf);
                int send_ret = entry->shm_server->serverSend(cid, send_buf.data(), send_buf.size());
                if (send_ret <= 0) {
                    OMNI_LOG_ERROR(LOG_TAG,
                                   "shm_reply_send_failed service=%s client_id=%u seq=%u err=%d",
                                   entry->service->serviceName(), cid, msg.getSequence(),
                                   static_cast<int>(ErrorCode::ERR_SEND_FAILED));
                }
                return;
            }
            Buffer request;
            if (payload_len > 0 && req_buf.remaining() >= payload_len) {
                request.writeRaw(req_buf.data() + req_buf.readPosition(), payload_len);
            }
            Buffer response;
            entry->service->onInvoke(method_id, request, response);
            Message reply(MessageType::MSG_INVOKE_REPLY, msg.getSequence());
            reply.payload.writeInt32(0);
            reply.payload.writeUint32(static_cast<uint32_t>(response.size()));
            if (response.size() > 0) {
                reply.payload.writeRaw(response.data(), response.size());
            }
            Buffer send_buf;
            reply.serialize(send_buf);
            int send_ret = entry->shm_server->serverSend(cid, send_buf.data(), send_buf.size());
            if (send_ret <= 0) {
                OMNI_LOG_ERROR(LOG_TAG,
                               "shm_reply_send_failed service=%s client_id=%u seq=%u err=%d",
                               entry->service->serviceName(), cid, msg.getSequence(),
                               static_cast<int>(ErrorCode::ERR_SEND_FAILED));
            }
        },
        [entry](const std::string& svc, const Message& msg) {
            (void)svc;
            Buffer req_buf(msg.payload.data(), msg.payload.size());
            uint32_t interface_id = req_buf.readUint32();
            uint32_t method_id = req_buf.readUint32();
            uint32_t payload_len = req_buf.readUint32();
            if (entry->service->interfaceInfo().interface_id != interface_id) {
                return;
            }
            Buffer request;
            if (payload_len > 0 && req_buf.remaining() >= payload_len) {
                request.writeRaw(req_buf.data() + req_buf.readPosition(), payload_len);
            }
            Buffer response;
            entry->service->onInvoke(method_id, request, response);
        },
        [this](const std::string& svc, uint32_t cid, const Message& msg) {
            (void)cid;
            Buffer buf2(msg.payload.data(), msg.payload.size());
            uint32_t topic_id = buf2.readUint32();
            std::string topic_name = buf2.readString();
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
        msg.payload.writeString(it->first);
        sendToSM(msg);
    }
}

// ============================================================
// 辅助
// ============================================================

bool OmniRuntime::Impl::sendOnFd(ITransport* transport, const Message& msg) {
    if (!transport) return false;
    Buffer buf;
    msg.serialize(buf);
    size_t sent = 0;
    while (sent < buf.size()) {
        int ret = transport->send(buf.data() + sent, buf.size() - sent);
        if (ret <= 0) return false;
        sent += static_cast<size_t>(ret);
    }
    return true;
}

std::string OmniRuntime::Impl::topicPublisherServiceName(const std::string& topic_name) const {
    return "topic_pub_" + topic_name;
}

bool OmniRuntime::Impl::ensureTopicPublisherConnection(const std::string& topic_name,
                                                      const ServiceInfo& pub_info) {
    std::string pub_name = !pub_info.name.empty() ? pub_info.name : topicPublisherServiceName(topic_name);
    ServiceConnection* conn = conn_mgr_->getOrCreateConnection(
        pub_name, pub_info.host, pub_info.port, pub_info.host_id,
        pub_info.shm_name, pub_info.shm_config);
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

bool OmniRuntime::Impl::refreshServiceConnectionUnlocked(const std::string& service_name,
                                                        ServiceInfo& info,
                                                        ServiceConnection*& conn) {
    if (!conn_mgr_) {
        return false;
    }

    conn_mgr_->removeConnection(service_name);
    service_cache_.erase(service_name);

    int ret = lookupServiceUnlocked(service_name, info);
    if (ret != 0) {
        return false;
    }

    conn = conn_mgr_->getOrCreateConnection(
        service_name, info.host, info.port, info.host_id, info.shm_name, info.shm_config);
    return conn != NULL;
}

void OmniRuntime::Impl::setHeartbeatInterval(uint32_t ms) {
    callSerialized([this, ms]() {
        heartbeat_interval_ms_ = ms;
    });
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
    return owner_thread_id_ != std::thread::id() && owner_thread_id_ == std::this_thread::get_id();
}

void OmniRuntime::Impl::captureOwnerThread() {
    if (owner_thread_id_ == std::thread::id()) {
        owner_thread_id_ = std::this_thread::get_id();
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
        svc_info.host = normalizeAdvertiseHost(platform::getSocketAddress(entry->server->fd()));
        svc_info.port = entry->port;
        svc_info.host_id = host_id_;
        svc_info.shm_name = entry->shm_name;
        if (entry->service) {
            svc_info.shm_config = entry->service->shmConfig();
        }
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
        pub_info.shm_name = sit->second->shm_name;
        pub_info.shm_config = sit->second->service->shmConfig();
        serializeServiceInfo(pub_info, msg.payload);
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
        stats = stats_;
        updateConnectionStats(stats);
        return 0;
    });
}

void OmniRuntime::Impl::resetStats() {
    callSerialized([this]() {
        stats_ = RuntimeStats();
    });
}


} // namespace omnibinder
