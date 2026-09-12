#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "transport/transport_selector.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"
#include <memory>
#include <new>

#define LOG_TAG "OmniRuntimeService"

namespace omnibinder {

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
        shm_config.req_ring_capacity,
        shm_config.resp_ring_capacity);

    ret = registerServiceWithManager(name, service, entry, advertise_host);
    if (ret != 0) {
        cleanupPendingServiceRegistration(name, entry);
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
    IServerTransport* tcp = createServerTransport(service->name(), TransportType::TCP,
                                                  TransportConfig());
    if (!tcp) {
        return static_cast<int>(ErrorCode::ERR_LISTEN_FAILED);
    }
    int port = tcp->start("0.0.0.0", 0, TransportConfig());
    if (port < 0) {
        delete tcp;
        return static_cast<int>(ErrorCode::ERR_LISTEN_FAILED);
    }

    entry->endpoints.push_back(tcp);
    entry->port = static_cast<uint16_t>(port);
    service->setPort(entry->port);
    service->runtime_ = owner_;
    advertise_host = resolveRegisterHost(service, listenerAdvertiseHost());
    wireServiceEndpoint(service->name(), entry, tcp);
    return 0;
}

std::string OmniRuntime::Impl::listenerAdvertiseHost() const {
    // core 固定以 0.0.0.0 绑定 TCP 监听；历史实现经
    // platform::getSocketAddress(listen_fd) 得到的也是通配地址。
    return normalizeAdvertiseHost("0.0.0.0");
}

std::string OmniRuntime::Impl::resolveRegisterHost(Service* service,
                                                     const std::string& listener_host) const {
    if (service && !service->getRegisterHost().empty()) {
        return service->getRegisterHost();
    }
    // register_host_ 由 api_mutex_ 统一保护（setRegisterHost/getRegisterHost 同锁），
    // 本函数可能在 owner 线程执行，也可能在无 driver 的内联路径执行
    std::lock_guard<std::recursive_mutex> lock(api_mutex_);
    if (!register_host_.empty()) {
        return register_host_;
    }
    return normalizeAdvertiseHost(listener_host);
}

void OmniRuntime::Impl::initializeServiceShm(const std::string& name, LocalServiceEntry* entry,
                                              size_t req_ring_capacity, size_t resp_ring_capacity) {
    // SHM 端点：每个客户端创建自己的 SHM，服务端通过握手打开；
    // 容量为 0 时由 SHM 传输使用默认值。
    TransportConfig config(req_ring_capacity, resp_ring_capacity);
    IServerTransport* shm = createServerTransport(name, TransportType::SHM, config);
    if (!shm) {
        OMNI_LOG_WARN(LOG_TAG, "createServerTransport(SHM) failed for service %s", name.c_str());
        return;
    }
    if (shm->start("", 0, config) != 0) {
        OMNI_LOG_WARN(LOG_TAG, "Failed to start SHM endpoint for service %s", name.c_str());
        delete shm;
        return;
    }

    entry->endpoints.push_back(shm);
    wireServiceEndpoint(name, entry, shm);
}

void OmniRuntime::Impl::wireServiceEndpoint(const std::string& name, LocalServiceEntry* entry,
                                            IServerTransport* endpoint) {
    // 回调只捕获 name：entry 可能被用户回调注销释放，回调内必须按名重查（约束 1）
    endpoint->setAcceptCallback([this, name](int client_id, IClientTransport* client) {
        onServiceClientAccepted(name, client_id, client);
    });
    endpoint->setReadableCallback([this, name](int client_id) {
        onServiceClientReadable(name, client_id);
    });
    endpoint->setDisconnectCallback([this, name](int client_id) {
        onServiceClientDisconnected(name, client_id);
    });
    syncEndpointFds(name, entry);
}

void OmniRuntime::Impl::syncEndpointFds(const std::string& name, LocalServiceEntry* entry) {
    if (!entry) return;

    std::vector<int> fds;
    for (size_t i = 0; i < entry->endpoints.size(); ++i) {
        entry->endpoints[i]->pollFds(fds);
    }
    std::set<int> wanted(fds.begin(), fds.end());

    // 摘除已失效的 fd（正常断开路径已先摘除；此处兜底防残留，约束 3）
    std::set<int>::iterator reg = entry->endpoint_fds.begin();
    while (reg != entry->endpoint_fds.end()) {
        if (wanted.find(*reg) == wanted.end()) {
            loop_->removeFd(*reg);
            entry->endpoint_fds.erase(reg++);
        } else {
            ++reg;
        }
    }

    for (size_t i = 0; i < fds.size(); ++i) {
        int fd = fds[i];
        if (entry->endpoint_fds.find(fd) != entry->endpoint_fds.end()) continue;
        loop_->addFd(fd, EventLoop::EVENT_READ | EventLoop::EVENT_ERROR,
            [this, name](int efd, uint32_t ev) { this->onServiceEndpointEvent(name, efd, ev); });
        entry->endpoint_fds.insert(fd);
    }
}

int OmniRuntime::Impl::registerServiceWithManager(const std::string& name, Service* service,
                                                  LocalServiceEntry* entry,
                                                  const std::string& advertise_host) {
    return sendRegisterToManager(name, service, entry->port, advertise_host, NULL);
}

int OmniRuntime::Impl::sendRegisterToManager(const std::string& name, Service* service,
                                             uint16_t port, const std::string& advertise_host,
                                             ServiceHandle* out_handle) {
    Message msg(MessageType::MSG_REGISTER, allocSequence());
    ServiceInfo svc_info;
    svc_info.name = name;
    svc_info.host = advertise_host;
    svc_info.port = port;
    svc_info.host_id = host_id_;
    svc_info.shm_config = service->shmConfig();
    svc_info.interfaces.push_back(service->interfaceInfo());
    if (!serializeServiceInfo(svc_info, msg.payload)) {
        OMNI_LOG_ERROR(LOG_TAG, "serialize_register_payload_failed service=%s", name.c_str());
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

    if (!sendToSM(msg)) {
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    Message reply;
    int ret = waitForReply(msg.getSequence(), effectiveTimeout(0), reply);
    if (ret != 0) {
        return ret;
    }

    ServiceHandle handle = INVALID_HANDLE;
    if (!decodeUint32ReplyPayload(reply, handle)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (handle == INVALID_HANDLE) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    if (out_handle) {
        *out_handle = handle;
    }
    return 0;
}

void OmniRuntime::Impl::detachClientsFromEntry(const std::string& name,
                                               LocalServiceEntry* entry) {
    if (!entry) {
        return;
    }
    for (std::map<int, IClientTransport*>::iterator it = entry->clients.begin();
         it != entry->clients.end(); ++it) {
        client_id_to_service_.erase(it->first);
        topic_runtime_.removeTcpSubscriberFd(it->first);
        topic_runtime_.removeShmSubscriberService(name, it->first);
    }
    entry->clients.clear();
}

void OmniRuntime::Impl::cleanupPendingServiceRegistration(const std::string& name,
                                                          LocalServiceEntry* entry) {
    if (!entry) {
        return;
    }

    removeServiceEndpointsFromLoop(entry);

    // 注册等待 SM reply 期间，端点 fd 已注册，可能有客户端接入。entry 随后会被
    // delete，若不先摘除这些客户端 fd，fd 号被复用时 EventLoop::addFd 会因表内
    // 残留而静默失败（约束 3）。
    detachClientsFromEntry(name, entry);
}

void OmniRuntime::Impl::removeServiceEndpointsFromLoop(LocalServiceEntry* entry) {
    if (!entry) {
        return;
    }

    // 端点 fd（监听/主控/握手）与 per-client fd（TCP client fd、SHM liveness fd）
    // 都必须在 entry/endpoint 销毁前从 event-loop 摘除（约束 3）
    for (std::set<int>::iterator it = entry->endpoint_fds.begin();
         it != entry->endpoint_fds.end(); ++it) {
        loop_->removeFd(*it);
    }
    entry->endpoint_fds.clear();
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
    // 先从 local_services_ 摘除 entry，再执行 onStop()：onStop() 回调内若
    // 重入 unregisterService/registerService，不会 double-free 或操作已释放的
    // entry（各判活路径发现 map 中已无该服务会立即返回）
    local_services_.erase(it);
    removeServiceEndpointsFromLoop(entry);
    
    detachClientsFromEntry(name, entry);
    
    topic_runtime_.forgetPublishedTopicsByOwner(name);
    
    service->onStop();
    service->runtime_ = NULL;
    delete entry;
    
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
    ServiceState* cached = findServiceState(service_name);
    if (cached && cached->has_info) {
        info = cached->info;
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

    ServiceState& svc_state = ensureServiceState(service_name);
    svc_state.info = info;
    svc_state.has_info = true;
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
    // 容错：count 来自网络，必须与剩余 payload 字节匹配（每个 ServiceInfo 至少
    // 含若干固定字段），否则畸形/损坏报文会导致 reserve 超大内存崩溃
    if (count > reply.payload.size() / 32) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    services.clear();
    try {
        services.reserve(count);
    } catch (const std::bad_alloc&) {
        // 内存耗尽：返回错误而非崩溃（无异常传播原则）
        return static_cast<int>(ErrorCode::ERR_OUT_OF_MEMORY);
    }

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
    try {
        interfaces.reserve(count);
    } catch (const std::bad_alloc&) {
        // 内存耗尽：返回错误而非崩溃（无异常传播原则）
        return static_cast<int>(ErrorCode::ERR_OUT_OF_MEMORY);
    }
    
    for (uint32_t i = 0; i < count; ++i) {
        InterfaceInfo info;
        if (!deserializeInterfaceInfo(rbuf, info)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        interfaces.push_back(info);
    }
    return 0;
}

int OmniRuntime::Impl::queryPublishedTopics(const std::string& service_name,
                                            std::vector<std::string>& topics) {
    return queryPublishedTopics(service_name, topics, 0);
}

int OmniRuntime::Impl::queryPublishedTopics(const std::string& service_name,
                                            std::vector<std::string>& topics,
                                            uint32_t timeout_ms) {
    return callSerialized([this, &service_name, &topics, timeout_ms]() -> int {
        return queryPublishedTopicsInternal(service_name, topics, timeout_ms);
    });
}

int OmniRuntime::Impl::queryPublishedTopicsInternal(
    const std::string& service_name, std::vector<std::string>& topics,
    uint32_t timeout_ms) {
    Message msg(MessageType::MSG_QUERY_PUBLISHED_TOPICS, allocSequence());
    if (!msg.payload.writeString(service_name)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply, timeout_ms);
    if (ret != 0) {
        return ret;
    }
    if (reply.getType() != MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY) {
        return static_cast<int>(ErrorCode::ERR_PROTOCOL_ERROR);
    }

    BufferView rbuf(reply.payload.data(), reply.payload.size());
    bool found = false;
    std::vector<std::string> decoded_topics;
    if (!deserializePublishedTopicsReply(rbuf, found, decoded_topics)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!found) {
        topics.clear();
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }

    topics.swap(decoded_topics);
    return 0;
}

} // namespace omnibinder
