#include "core/omni_runtime.h"
#include "transport/transport_selector.h"
#include "omnibinder/proxy_base.h"
#include <algorithm>
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntime"

namespace omnibinder {

namespace {
// 同进程内多个 OmniRuntime 的默认 runtime_id 序号。单 runtime 进程重启后
// 序号仍为 #1，因此默认身份跨重启稳定；多 runtime 进程如需跨重启稳定，
// 应由业务显式 setRuntimeId()。
std::atomic<uint32_t> g_runtime_instance_seq(0);

// 进程代际：同一进程内稳定，重启后变化。用于区分"同进程重连"与"新进程"。
uint32_t makeIncarnationId() {
    uint64_t x = static_cast<uint64_t>(platform::currentTimeMs());
    x ^= static_cast<uint64_t>(platform::getPid()) << 32;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    uint32_t v = static_cast<uint32_t>(x);
    return v != 0 ? v : 1u;
}
} // namespace

OmniRuntime::OmniRuntime() : impl_(new Impl()) {
    impl_->setOwner(this);
}
OmniRuntime::~OmniRuntime() { delete impl_; }

int OmniRuntime::init(const std::string& sm_host, uint16_t sm_port) {
    return impl_->init(sm_host, sm_port);
}
int OmniRuntime::start() { return impl_->start(); }
int OmniRuntime::start(int poll_timeout_ms, const LoopTickCallback& on_tick) {
    return impl_->start(poll_timeout_ms, on_tick);
}
void OmniRuntime::registerProxy(ServiceProxyBase* proxy) { impl_->registerProxy(proxy); }
void OmniRuntime::unregisterProxy(ServiceProxyBase* proxy) { impl_->unregisterProxy(proxy); }
int OmniRuntime::connectProxy(ServiceProxyBase* proxy) {
    return proxy ? proxy->connect() : static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
}
void OmniRuntime::stop() { impl_->stop(); }
bool OmniRuntime::isRunning() const { return impl_->isRunning(); }
bool OmniRuntime::isReady() const { return impl_->isReady(); }

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
int OmniRuntime::listServiceInstances(const std::string& name, std::vector<ServiceInfo>& out) {
    return impl_->listServiceInstances(name, out);
}
int OmniRuntime::queryInterfaces(const std::string& name, std::vector<InterfaceInfo>& ifaces) {
    return impl_->queryInterfaces(name, ifaces);
}
int OmniRuntime::queryPublishedTopics(const std::string& name,
                                      std::vector<std::string>& topics,
                                      uint32_t timeout_ms) {
    return impl_->queryPublishedTopics(name, topics, timeout_ms);
}

int OmniRuntime::connectService(const std::string& name) {
    return impl_->connectService(name);
}
int OmniRuntime::connectServiceInstance(const std::string& name, const std::string& host_id,
                                        const std::string& instance_id) {
    return impl_->connectServiceInstance(name, host_id, instance_id);
}
int OmniRuntime::disconnectService(const std::string& name) {
    return impl_->disconnectService(name);
}
int OmniRuntime::disconnectServiceInstance(const std::string& name, const std::string& host_id,
                                           const std::string& instance_id) {
    return impl_->disconnectServiceInstance(name, host_id, instance_id);
}
bool OmniRuntime::isServiceConnected(const std::string& name) const {
    return impl_->isServiceConnected(name);
}
bool OmniRuntime::isServiceInstanceConnected(const std::string& name, const std::string& host_id,
                                             const std::string& instance_id) const {
    return impl_->isServiceInstanceConnected(name, host_id, instance_id);
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
int OmniRuntime::subscribeServiceInstanceDeath(const std::string& name,
                                               const std::string& host_id,
                                               const std::string& instance_id,
                                               const DeathSourceCallback& cb) {
    return impl_->subscribeServiceInstanceDeath(name, host_id, instance_id, cb);
}
int OmniRuntime::unsubscribeServiceInstanceDeath(const std::string& name,
                                                 const std::string& host_id,
                                                 const std::string& instance_id) {
    return impl_->unsubscribeServiceInstanceDeath(name, host_id, instance_id);
}

int OmniRuntime::publishTopic(const std::string& topic, uint32_t idl_hash) {
    return impl_->publishTopic(topic, idl_hash);
}
int OmniRuntime::broadcast(uint32_t topic_id, const Buffer& data) {
    return impl_->broadcast(topic_id, data);
}
int OmniRuntime::subscribeTopic(const std::string& topic, const TopicCallback& on_msg,
                                 const TopicErrorCallback& on_err) {
    return impl_->subscribeTopic(topic, 0, on_msg, on_err);
}
int OmniRuntime::subscribeTopic(const std::string& topic, uint32_t expected_idl_hash,
                                 const TopicCallback& on_msg, const TopicErrorCallback& on_err) {
    return impl_->subscribeTopic(topic, expected_idl_hash, on_msg, on_err);
}
int OmniRuntime::unsubscribeTopic(const std::string& topic) {
    return impl_->unsubscribeTopic(topic);
}
int OmniRuntime::subscribeTopic(const TopicSubscriptionSelector& selector,
                                const std::string& topic, uint32_t expected_idl_hash,
                                const TopicSourceCallback& on_msg,
                                const TopicErrorCallback& on_err) {
    return impl_->subscribeTopic(selector, topic, expected_idl_hash, on_msg, on_err);
}
int OmniRuntime::unsubscribeTopic(const TopicSubscriptionSelector& selector,
                                  const std::string& topic) {
    return impl_->unsubscribeTopic(selector, topic);
}

void OmniRuntime::setRegisterHost(const std::string& host) { impl_->setRegisterHost(host); }
std::string OmniRuntime::getRegisterHost() const { return impl_->getRegisterHost(); }
void OmniRuntime::setHeartbeatPolicy(uint32_t interval_ms, uint32_t timeout_ms) {
    impl_->setHeartbeatPolicy(interval_ms, timeout_ms);
}
void OmniRuntime::setDefaultTimeout(uint32_t ms) { impl_->setDefaultTimeout(ms); }
void OmniRuntime::setReplySendTimeout(uint32_t ms) { impl_->setReplySendTimeout(ms); }
void OmniRuntime::setHostId(const std::string& host_id) { impl_->setHostId(host_id); }
std::string OmniRuntime::hostId() const { return impl_->hostId(); }
void OmniRuntime::setRuntimeId(const std::string& runtime_id) { impl_->setRuntimeId(runtime_id); }
std::string OmniRuntime::runtimeId() const { return impl_->runtimeId(); }
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
    , loop_alive_(false)
    , initialized_(false)
    , started_(false)
    , ready_(false)
    , shutdown_done_(false)
    , start_owned_(false)
    , owner_(NULL)
    , loop_driver_active_(false)
    , sm_channel_()
    , rpc_runtime_()
    , sm_reconnect_needed_(false)
    , sm_port_(0)
    , runtime_id_()
    , runtime_id_override_()
    , incarnation_id_(0)
    , heartbeat_interval_ms_(DEFAULT_HEARTBEAT_INTERVAL)
    , heartbeat_timer_id_(0)
    , control_timeout_ms_(DEFAULT_HEARTBEAT_TIMEOUT)
    , control_requested_timeout_ms_(0)
    , control_ack_timer_id_(0)
    , control_ack_pending_(false)
    , sm_incarnation_id_(0)
    , conn_mgr_(NULL)
    , register_host_()
    , reply_send_timeout_ms_(DEFAULT_REPLY_SEND_TIMEOUT)
    , proxy_retry_timer_id_(0)
    , client_session_generation_(0)
    , data_max_session_timeout_ms_(0)
    , diag_()
{
}

OmniRuntime::Impl::~Impl() {
    stop();
    shutdownInternal();
}

void OmniRuntime::Impl::shutdownInternal() {
    if (shutdown_done_) {
        return;
    }
    shutdown_done_ = true;

    if (loop_) {
        loop_->pollOnce(0);
    }

    if (loop_ && heartbeat_timer_id_ > 0) {
        loop_->cancelTimer(heartbeat_timer_id_);
        heartbeat_timer_id_ = 0;
    }
    if (loop_ && proxy_retry_timer_id_ > 0) {
        loop_->cancelTimer(proxy_retry_timer_id_);
        proxy_retry_timer_id_ = 0;
    }
    if (loop_ && control_ack_timer_id_ > 0) {
        loop_->cancelTimer(control_ack_timer_id_);
        control_ack_timer_id_ = 0;
    }

    for (std::map<std::string, ServiceState>::iterator it = services_.begin();
         it != services_.end(); ++it) {
        if (loop_ && it->second.has_heartbeat && it->second.heartbeat.timer_id > 0) {
            loop_->cancelTimer(it->second.heartbeat.timer_id);
        }
        if (loop_ && it->second.has_heartbeat && it->second.heartbeat.deadline_timer_id > 0) {
            loop_->cancelTimer(it->second.heartbeat.deadline_timer_id);
        }
        if (loop_ && it->second.has_reconnect && it->second.reconnect.timer_id > 0) {
            loop_->cancelTimer(it->second.reconnect.timer_id);
        }
    }
    services_.clear();

    // 先从容器摘除，再执行 onStop：onStop 是用户重入点，可能调用
    // unregisterService/registerService，不能持有本地服务 map 的迭代器（约束 1/2）。
    std::vector<std::pair<std::string, LocalServiceEntry*> > shutdown_services;
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        removeServiceEndpointsFromLoop(it->second);
        detachClientsFromEntry(it->first, it->second);
        shutdown_services.push_back(std::make_pair(it->first, it->second));
    }
    local_services_.clear();
    for (size_t i = 0; i < shutdown_services.size(); ++i) {
        Service* service = shutdown_services[i].second ? shutdown_services[i].second->service : NULL;
        // start() 驱动的生命周期中，调用方必须在 start() 返回（stop 完成）前保持
        // Service 存活；这里触发 onStop。手动 pollOnce 旧路径由 unregisterService 触发。
        if (service && start_owned_) {
            service->onStop();
            service->runtime_ = NULL;
        }
        delete shutdown_services[i].second;
    }
    declared_services_.clear();
    declared_proxies_.clear();
    client_id_to_service_.clear();
    if (diag_.data_service) {
        delete diag_.data_service;
        diag_.data_service = NULL;
    }

    sm_channel_.clearReplies();
    delete conn_mgr_;
    conn_mgr_ = NULL;
    if (loop_) {
        sm_channel_.closeTransport(*loop_);
    }
    delete loop_;
    loop_ = NULL;

    if (initialized_) {
        platform::netCleanup();
    }
    initialized_ = false;
    started_ = false;
    ready_ = false;
    start_owned_ = false;
}

// ============================================================
// 初始化
// ============================================================

int OmniRuntime::Impl::init(const std::string& sm_host, uint16_t sm_port) {
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    if (initialized_) {
        return static_cast<int>(ErrorCode::ERR_ALREADY_INITIALIZED);
    }

    platform::netInit();
    host_id_ = host_id_override_.empty() ? platform::getMachineId() : host_id_override_;
    sm_host_ = sm_host;
    sm_port_ = sm_port;

    loop_ = new EventLoop();
    if (!loop_->valid()) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to initialize event-loop backend");
        delete loop_;
        loop_ = NULL;
        platform::netCleanup();
        return static_cast<int>(ErrorCode::ERR_INTERNAL);
    }
    owner_executor_.bindLoop(loop_);

    conn_mgr_ = new ConnectionManager(*loop_, host_id_);
    conn_mgr_->setMessageCallback(
        [this](const std::string& svc, Message& msg) { this->onDirectMessage(svc, msg); });
    conn_mgr_->setDisconnectCallback(
        [this](std::string svc) {
            this->onDirectDisconnect(svc);
        });

    initialized_ = true;
    started_ = false;
    ready_ = false;
    // 启动前不连接 SM；由 start()/首次兼容 API 调用进入激活流程。
    sm_reconnect_needed_ = true;
    diag_.pid = static_cast<uint32_t>(platform::getPid());
    diag_.process_name = runtimeProcessName();
    if (runtime_id_override_.empty()) {
        const uint32_t seq = ++g_runtime_instance_seq;
        const std::string base = diag_.process_name.empty()
            ? "runtime" : diag_.process_name;
        runtime_id_ = base + "#" + std::to_string(diag_.pid) + "-" + std::to_string(seq);
    } else {
        runtime_id_ = runtime_id_override_;
    }
    incarnation_id_ = makeIncarnationId();
    control_timeout_ms_ = DEFAULT_HEARTBEAT_TIMEOUT;
    control_ack_pending_ = false;
    sm_incarnation_id_ = 0;

    OMNI_LOG_INFO(LOG_TAG, "Runtime initialized locally, SM target %s:%u will be connected on start",
                  sm_host_.c_str(), sm_port_);
    return 0;
}

int OmniRuntime::Impl::connectControlPlaneInternal() {
    if (sm_channel_.isConnected()) {
        return 0;
    }

    int connect_err = 0;
    IMessageConnection* control_transport = createControlConnection(sm_host_, sm_port_, connect_err);
    if (!control_transport) {
        sm_reconnect_needed_ = true;
        ready_ = false;
        OMNI_LOG_WARN(LOG_TAG, "sm_connect_deferred host=%s port=%u err=%d",
                      sm_host_.c_str(), sm_port_, connect_err);
        return connect_err;
    }

    sm_channel_.resetTransport(control_transport);
    loop_->addFd(sm_channel_.transport()->fd(), EventLoop::EVENT_READ,
        [this](int fd, uint32_t events) { this->onSMData(fd, events); });
    // 新连接已就绪，避免 sendRuntimeHello 内部再次触发 reconnect。
    sm_reconnect_needed_ = false;

    int hello_ret = sendRuntimeHello();
    if (hello_ret != 0) {
        OMNI_LOG_WARN(LOG_TAG, "sm_hello_failed host=%s port=%u err=%d, will retry via heartbeat",
                      sm_host_.c_str(), sm_port_, hello_ret);
        sm_reconnect_needed_ = true;
        ready_ = false;
        return hello_ret;
    }
    sm_reconnect_needed_ = false;
    ready_ = true;
    return 0;
}

int OmniRuntime::Impl::activateDeclaredServices() {
    std::vector<std::string> names;
    names.reserve(declared_services_.size());
    for (std::map<std::string, Service*>::iterator it = declared_services_.begin();
         it != declared_services_.end(); ++it) {
        names.push_back(it->first);
    }

    for (size_t i = 0; i < names.size(); ++i) {
        std::map<std::string, Service*>::iterator it = declared_services_.find(names[i]);
        if (it == declared_services_.end()) {
            continue;
        }
        Service* service = it->second;
        int ret = registerServiceInternal(service, true);
        if (ret == 0) {
            declared_services_.erase(names[i]);
        } else {
            OMNI_LOG_WARN(LOG_TAG, "activate service %s failed err=%d", names[i].c_str(), ret);
        }
    }
    return 0;
}

int OmniRuntime::Impl::beginRuntimeInternal() {
    if (!initialized_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }
    if (started_) {
        return 0;
    }

    started_ = true;

    if (loop_ && heartbeat_timer_id_ == 0) {
        heartbeat_timer_id_ = loop_->addTimer(heartbeat_interval_ms_,
            [this]() { this->sendHeartbeat(); }, true);
    }

    connectControlPlaneInternal();
    activateDeclaredServices();
    return 0;
}

int OmniRuntime::Impl::start() {
    return start(-1, LoopTickCallback());
}

int OmniRuntime::Impl::start(int poll_timeout_ms, const LoopTickCallback& on_tick) {
    if (poll_timeout_ms < -1) {
        return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    }
    if (poll_timeout_ms == -1 && on_tick) {
        return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(api_mutex_);
        if (!initialized_) {
            return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
        }
        if (loop_ && loop_->stopRequested()) {
            return static_cast<int>(ErrorCode::ERR_NOT_RUNNING);
        }

        bool expected = false;
        if (loop_driver_active_.compare_exchange_strong(expected, true)) {
            owner_executor_.setSpinWait(poll_timeout_ms == 0);
            owner_executor_.setOwnerThread(std::this_thread::get_id());
        } else {
            return static_cast<int>(ErrorCode::ERR_ALREADY_RUNNING);
        }

        running_ = true;
        loop_alive_ = true;
        start_owned_ = true;
        beginRuntimeInternal();
        connectDeclaredProxiesInternal();
        ensureProxyRetryTimerInternal();
    }

    loop_->run(poll_timeout_ms, on_tick);

    loop_alive_ = false;
    running_ = false;
    loop_driver_active_ = false;
    shutdownInternal();
    return 0;
}

void OmniRuntime::Impl::registerProxy(ServiceProxyBase* proxy) {
    if (!proxy) return;
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    for (size_t i = 0; i < declared_proxies_.size(); ++i) {
        if (declared_proxies_[i] == proxy) return;
    }
    declared_proxies_.push_back(proxy);
    ensureProxyRetryTimerInternal();
}

void OmniRuntime::Impl::unregisterProxy(ServiceProxyBase* proxy) {
    if (!proxy) return;
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    declared_proxies_.erase(
        std::remove(declared_proxies_.begin(), declared_proxies_.end(), proxy),
        declared_proxies_.end());
}

int OmniRuntime::Impl::connectDeclaredProxiesInternal() {
    if (!sm_channel_.isConnected()) {
        return static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
    }

    std::vector<ServiceProxyBase*> proxies;
    {
        std::lock_guard<std::recursive_mutex> lock(api_mutex_);
        proxies = declared_proxies_;
    }

    int last_error = 0;
    for (size_t i = 0; i < proxies.size(); ++i) {
        ServiceProxyBase* proxy = proxies[i];
        if (!proxy || !proxy->shouldAutoConnect() || proxy->isConnected()) {
            continue;
        }
        // 已成功连接过的 Proxy 由数据面自动重连机制负责；这里仅重试“首次等待服务”
        // 的 Proxy，避免用户显式 disconnectService 后被自动拉回。
        if (proxy->hasEverConnected()) {
            continue;
        }
        int ret = owner_->connectProxy(proxy);
        if (ret != 0) {
            last_error = ret;
        }
    }
    return last_error;
}

void OmniRuntime::Impl::ensureProxyRetryTimerInternal() {
    if (!loop_ || !started_ || proxy_retry_timer_id_ != 0) {
        return;
    }
    proxy_retry_timer_id_ = loop_->addTimer(1000, [this]() {
        this->connectDeclaredProxiesInternal();
    }, true);
}

void OmniRuntime::Impl::stop() {
    running_ = false;
    loop_alive_ = false;
    if (loop_) loop_->stop();
}

bool OmniRuntime::Impl::isRunning() const { return running_; }

bool OmniRuntime::Impl::isReady() const {
    return started_.load() && ready_.load();
}


void OmniRuntime::Impl::pollOnceWithoutFunctors(int timeout_ms) {
    // 同步 reply wait 期间的内部驱动：只处理 fd/timer，不执行 functor。
    // 不捕获 owner 线程、不争夺 driver 身份——真正的 driver 由 run()/pollOnce()
    // 决定；否则 init 阶段的 hello 等待会把主线程误记为 driver，
    // 导致真正的驱动线程（服务线程）被 pollOnce 守卫拒绝。
    if (!initialized_) return;
    loop_alive_ = true;
    if (loop_) loop_->pollOnceWithoutFunctors(timeout_ms);
}

} // namespace omnibinder
