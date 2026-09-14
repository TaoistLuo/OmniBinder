#include "core/omni_runtime.h"
#include "transport/transport_selector.h"
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
    return impl_->publishTopic(topic, 0);
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

void OmniRuntime::setRegisterHost(const std::string& host) { impl_->setRegisterHost(host); }
std::string OmniRuntime::getRegisterHost() const { return impl_->getRegisterHost(); }
void OmniRuntime::setHeartbeatInterval(uint32_t ms) { impl_->setHeartbeatInterval(ms); }
void OmniRuntime::setDefaultTimeout(uint32_t ms) { impl_->setDefaultTimeout(ms); }
void OmniRuntime::setReplySendTimeout(uint32_t ms) { impl_->setReplySendTimeout(ms); }
std::string OmniRuntime::hostId() const { return impl_->hostId(); }
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
    , reply_send_timeout_ms_(DEFAULT_REPLY_SEND_TIMEOUT)
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
    
    // 排空 event-loop 中已就绪的 functor（已到期 timer callback 等），
    // 避免 teardown 期间 callback 引用已释放的资源
    if (loop_) {
        loop_->pollOnce(0);
    }
    
    // 清理前先取消心跳定时器
    if (loop_ && heartbeat_timer_id_ > 0) {
        loop_->cancelTimer(heartbeat_timer_id_);
        heartbeat_timer_id_ = 0;
    }

    // 取消所有每服务心跳 / 重连定时器
    for (std::map<std::string, ServiceState>::iterator it = services_.begin();
         it != services_.end(); ++it) {
        if (loop_ && it->second.has_heartbeat && it->second.heartbeat.timer_id > 0) {
            loop_->cancelTimer(it->second.heartbeat.timer_id);
        }
        if (loop_ && it->second.has_reconnect && it->second.reconnect.timer_id > 0) {
            loop_->cancelTimer(it->second.reconnect.timer_id);
        }
    }
    services_.clear();
    
    for (std::map<std::string, LocalServiceEntry*>::iterator it = local_services_.begin();
         it != local_services_.end(); ++it) {
        // 先摘除端点/客户端 fd 再销毁 endpoint（约束 3），并清理路由与话题订阅
        removeServiceEndpointsFromLoop(it->second);
        detachClientsFromEntry(it->first, it->second);
        delete it->second;
    }
    local_services_.clear();
    client_id_to_service_.clear();
    if (diag_data_service_) {
        delete diag_data_service_;
        diag_data_service_ = NULL;
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
    host_id_ = platform::getMachineId();
    sm_host_ = sm_host;
    sm_port_ = sm_port;
    
    loop_ = new EventLoop();
    owner_executor_.bindLoop(loop_);
    
    int connect_err = 0;
    IMessageConnection* control_transport = createControlConnection(sm_host, sm_port, connect_err);
    if (!control_transport) {
        delete loop_;
        loop_ = NULL;
        return connect_err;
    }
    sm_channel_.resetTransport(control_transport);
    
    loop_->addFd(sm_channel_.transport()->fd(), EventLoop::EVENT_READ,
        [this](int fd, uint32_t events) { this->onSMData(fd, events); });
    
    conn_mgr_ = new ConnectionManager(*loop_, host_id_);
    conn_mgr_->setMessageCallback(
        [this](const std::string& svc, Message& msg) { this->onDirectMessage(svc, msg); });
    conn_mgr_->setDisconnectCallback(
        // 参数按值：onDirectDisconnect 内部会 removeConnection 删除 conn，
        // 若传 conn 成员的引用，删除后引用悬垂（重构 failConnection 后暴露的 UAF）
        [this](std::string svc) {
            this->onDirectDisconnect(svc);
        });
    
    heartbeat_timer_id_ = loop_->addTimer(heartbeat_interval_ms_,
        [this]() { this->sendHeartbeat(); }, true);
    
    initialized_ = true;
    sm_reconnect_needed_ = false;
    pid_ = static_cast<uint32_t>(platform::getPid());
    process_name_ = runtimeProcessName();

    // hello 失败不阻塞 init（SM 可能随后恢复），但保留重连标志，
    // 让后续心跳路径重试 hello + 控制面状态恢复
    int hello_ret = sendRuntimeHello();
    if (hello_ret != 0) {
        OMNI_LOG_WARN(LOG_TAG, "sm_hello_failed host=%s port=%u err=%d, will retry via heartbeat",
                      sm_host.c_str(), sm_port, hello_ret);
        sm_reconnect_needed_ = true;
    }

    OMNI_LOG_INFO(LOG_TAG, "Connected to SM at %s:%u, host_id=%s",
                    sm_host.c_str(), sm_port, host_id_.c_str());
    return 0;
}

void OmniRuntime::Impl::run() {
    {
        // owner 认领与 callSerialized 的无 owner 内联判定共用 api_mutex_：
        // 消除 "init 后 run 认领前 API 内联执行" 与 "driver 线程驱动 loop" 并发（TOCTOU）
        std::lock_guard<std::recursive_mutex> lock(api_mutex_);
        if (!initialized_) return;

        // 双驱动守卫：先尝试成为 driver（原子 CAS），成功者才捕获 owner 线程；
        // 已有 driver 且非本线程时拒绝，保持单驱动
        bool expected = false;
        if (loop_driver_active_.compare_exchange_strong(expected, true)) {
            owner_executor_.setOwnerThread(std::this_thread::get_id());
        } else if (!owner_executor_.isOwnerThread()) {
            OMNI_LOG_WARN(LOG_TAG, "run rejected: event-loop already driven by another thread");
            return;
        }

        // stop 是终态：不重新武装 running_/loop_alive_（isRunning 保持 false，
        // 停止后的 API 快速失败）。loop_->run() 仍会调用，用于立即返回并排空
        // 关闭前已入队的 functor。
        if (!loop_->stopRequested()) {
            loop_alive_ = true;
            running_ = true;
        }
    }

    loop_->run();

    // stop() 可能在 reply-wait 期间被调用：waitForReply 的 pollOnceWithoutFunctors
    // 会把 loop_alive_ 重新置 true。run() 退出后必须强制复位，否则后续非 owner
    // 线程的 API 调用会被投递到已停止的 event-loop，永久阻塞（违反停止后快速失败）。
    loop_alive_ = false;
    running_ = false;
    loop_driver_active_ = false;
}

void OmniRuntime::Impl::stop() {
    running_ = false;
    loop_alive_ = false;
    if (loop_) loop_->stop();
}

bool OmniRuntime::Impl::isRunning() const { return running_; }

void OmniRuntime::Impl::pollOnce(int timeout_ms) {
    if (!initialized_) return;

    {
        // 与 run() 一致：owner 认领与 callSerialized 的无 owner 判定共用 api_mutex_，
        // 防止第二个线程驱动 event-loop，同时闭合 TOCTOU
        std::lock_guard<std::recursive_mutex> lock(api_mutex_);
        bool expected = false;
        if (loop_driver_active_.compare_exchange_strong(expected, true)) {
            owner_executor_.setOwnerThread(std::this_thread::get_id());
        } else if (!owner_executor_.isOwnerThread()) {
            OMNI_LOG_WARN(LOG_TAG, "pollOnce rejected: event-loop already driven by another thread");
            return;
        }
    }

    loop_alive_ = true;
    if (loop_) loop_->pollOnce(timeout_ms);
    // SHM 通信现在完全通过 eventfd 事件驱动，不再需要轮询
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
