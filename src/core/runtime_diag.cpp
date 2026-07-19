#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"

#include <thread>

#define LOG_TAG "OmniRuntimeDiag"

namespace omnibinder {

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

bool OmniRuntime::Impl::populateInvokeMessage(Message& msg, uint32_t interface_id,
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
        return false;
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
        platform::waitSocketWritable(sm_channel_.transport->fd(), 1000);
        sm_channel_.transport->checkConnectComplete();
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
    int hello_ret = sendRuntimeHello();
    if (hello_ret != 0) {
        return hello_ret;
    }

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
        svc_info.shm_config = entry->service->shmConfig();
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
        uint32_t handle = 0;
        if (!decodeUint32ReplyPayload(reply, handle) || handle == INVALID_HANDLE) {
            OMNI_LOG_ERROR(LOG_TAG, "restore register failed for %s", it->first.c_str());
            return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
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
        bool accepted = false;
        if (!decodeBoolReplyPayload(reply, accepted) || !accepted) {
            OMNI_LOG_WARN(LOG_TAG, "restore subscribe death failed for %s", it->first.c_str());
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
        bool pub_accepted = false;
        if (!decodeBoolReplyPayload(reply, pub_accepted) || !pub_accepted) {
            OMNI_LOG_WARN(LOG_TAG, "restore publish topic failed for %s", it->first.c_str());
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
        bool sub_accepted = false;
        if (!decodeBoolReplyPayload(reply, sub_accepted) || !sub_accepted) {
            OMNI_LOG_WARN(LOG_TAG, "restore subscribe topic failed for %s", it->first.c_str());
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
    callSerialized([this]() {
        service_cache_.clear();
    });
}

void OmniRuntime::Impl::closeAllConnections() {
    callSerialized([this]() {
        if (conn_mgr_) {
            conn_mgr_->closeAll();
        }
    });
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

int OmniRuntime::Impl::setLogLevelByPid(uint32_t pid, uint32_t level) {
    return callSerialized([this, pid, level]() -> int {
        Message msg(MessageType::MSG_DIAG_SET_LOG_LEVEL, allocSequence());
        msg.payload.writeUint32(pid);
        msg.payload.writeUint32(level);
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        BufferView buf(reply.payload.data(), reply.payload.size());
        bool ok = false;
        if (!buf.tryReadBool(ok)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        return ok ? 0 : static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    });
}

int OmniRuntime::Impl::listRuntimes(std::vector<RuntimeInfo>& runtimes) {
    return callSerialized([this, &runtimes]() -> int {
        Message msg(MessageType::MSG_RUNTIME_LIST, allocSequence());
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        BufferView buf(reply.payload.data(), reply.payload.size());
        uint32_t count = 0;
        if (!buf.tryReadUint32(count)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        runtimes.clear();
        for (uint32_t i = 0; i < count; ++i) {
            RuntimeInfo info;
            if (!deserializeRuntimeInfo(buf, info)) {
                return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
            }
            runtimes.push_back(info);
        }
        return 0;
    });
}

int OmniRuntime::Impl::watchPid(uint32_t pid, const DiagEventCallback& callback) {
    return callSerialized([this, pid, &callback]() -> int {
        Message msg(MessageType::MSG_DIAG_WATCH_START, allocSequence());
        msg.payload.writeUint32(pid);
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        BufferView buf(reply.payload.data(), reply.payload.size());
        bool ok = false;
        if (!buf.tryReadBool(ok)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        if (!ok) {
            return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
        }
        std::string topic_name = diagDataServiceName(pid);
        ret = subscribeTopicInternal(topic_name,
            [callback](uint32_t, const Buffer& data) {
                if (callback) {
                    callback(data);
                }
            });
        if (ret != 0) {
            Message stop_msg(MessageType::MSG_DIAG_WATCH_STOP, allocSequence());
            stop_msg.payload.writeUint32(pid);
            Message stop_reply;
            sendSMRequestAndWaitReply(stop_msg, stop_reply);
            return ret;
        }
        return 0;
    });
}

int OmniRuntime::Impl::unwatchPid(uint32_t pid) {
    return callSerialized([this, pid]() -> int {
        unsubscribeTopicInternal(diagDataServiceName(pid));
        Message msg(MessageType::MSG_DIAG_WATCH_STOP, allocSequence());
        msg.payload.writeUint32(pid);
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        BufferView buf(reply.payload.data(), reply.payload.size());
        bool ok = false;
        if (!buf.tryReadBool(ok)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        return ok ? 0 : static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    });
}


} // namespace omnibinder
