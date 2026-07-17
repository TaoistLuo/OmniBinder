#include "core/omni_runtime.h"
#include "omnibinder/log.h"
#include "omnibinder/buffer_view.h"

#include "core/omni_runtime.h"
#include "omnibinder/log.h"
#include "omnibinder/buffer_view.h"
#include "transport/transport_factory.h"
#include <cstring>
#include <algorithm>
#include <sstream>

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

class RuntimeDiagService : public Service {
public:
    explicit RuntimeDiagService(const std::string& name) : Service(name) {
        iface_.interface_id = OMNI_DIAG_IFACE_ID;
        iface_.name = name;
    }

    virtual const char* serviceName() const { return name().c_str(); }
    virtual const InterfaceInfo& interfaceInfo() const { return iface_; }

protected:
    virtual int onInvoke(uint32_t, const Buffer&, Buffer&) {
        return static_cast<int>(ErrorCode::ERR_NOT_SUPPORTED);
    }

private:
    InterfaceInfo iface_;
};

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
    if (!writeInvokeErrorReply(reply.payload, error)) {
        OMNI_LOG_ERROR(LOG_TAG, "makeInvokeErrorReply serialization failed seq=%u", seq);
    }
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

bool decodeBroadcastPayload(const Message& msg, uint32_t& topic_id, Buffer& payload) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t data_len = 0;
    if (!buf.tryReadUint32(topic_id) || !buf.tryReadUint32(data_len)) {
        return false;
    }
    if (buf.remaining() < data_len) {
        return false;
    }
    payload.clear();
    if (data_len > 0) {
        payload.writeRaw(buf.data() + buf.readPosition(), data_len);
    }
    return true;
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

