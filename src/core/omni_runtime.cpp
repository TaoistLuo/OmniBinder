#include "core/omni_runtime.h"
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntime"

namespace omnibinder {

namespace {

// 确保 omni_allocator.o 被链接器拉入（.a 静态库场景）。
// 该编译单元包含 operator new/delete 弱符号重载，
// 必须有外部符号引用才会被链接器选中。
extern "C" void omni_ensure_allocator_linked();
struct ForceLinkAllocator {
    ForceLinkAllocator() { omni_ensure_allocator_linked(); }
} force_link_allocator;

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
int OmniRuntime::queryPublishedTopics(const std::string& name,
                                      std::vector<std::string>& topics) {
    return impl_->queryPublishedTopics(name, topics);
}
int OmniRuntime::queryPublishedTopics(const std::string& name,
                                      std::vector<std::string>& topics,
                                      uint32_t timeout_ms) {
    return impl_->queryPublishedTopics(name, topics, timeout_ms);
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
int OmniRuntime::setLogLevelByPid(uint32_t pid, uint32_t level) { return impl_->setLogLevelByPid(pid, level); }
int OmniRuntime::listRuntimes(std::vector<RuntimeInfo>& runtimes) { return impl_->listRuntimes(runtimes); }
int OmniRuntime::watchPid(uint32_t pid, const DiagEventCallback& callback) { return impl_->watchPid(pid, callback); }
int OmniRuntime::unwatchPid(uint32_t pid) { return impl_->unwatchPid(pid); }

// ============================================================
// Impl 构造/析构
// ============================================================

OmniRuntime::Impl::Impl()
    : loop_(NULL)
    , running_(false)
    , initialized_(false)
    , owner_(NULL)
    , loop_driver_active_(false)
    , sm_channel_()
    , rpc_runtime_()
    , sm_reconnect_needed_(false)
    , sm_port_(0)
    , heartbeat_interval_ms_(DEFAULT_HEARTBEAT_INTERVAL)
    , heartbeat_timer_id_(0)
    , conn_mgr_(NULL)
    , register_host_()
    , diag_active_count_(0)
    , pid_(0)
    , process_name_()
    , diag_watch_active_(false)
    , diag_data_service_(NULL)
    , diag_watch_topic_id_(0)
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
    if (diag_data_service_) {
        delete diag_data_service_;
        diag_data_service_ = NULL;
    }
    
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
        platform::waitSocketWritable(sm_channel_.transport->fd(), 1000);
        sm_channel_.transport->checkConnectComplete();
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
    pid_ = static_cast<uint32_t>(platform::getPid());
    process_name_ = runtimeProcessName();
    sendRuntimeHello();
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

} // namespace omnibinder
