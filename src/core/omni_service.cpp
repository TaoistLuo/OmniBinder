#include "core/omni_runtime.h"

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
                ShmTransport* transport = entry->shm_transport;
                if (!transport) return;
                uint8_t buf[65536];
                uint32_t from_id = 0;
                while (true) {
                    int ret = transport->serverRecv(buf, sizeof(buf), from_id);
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
            entry->shm_client_notify_fds.insert(fd);
            loop_->addFd(fd, EventLoop::EVENT_READ,
                [this, entry, name](int efd, uint32_t) {
                    platform::eventFdConsume(efd);
                    ShmTransport* transport = entry->shm_transport;
                    if (!transport) return;
                    uint8_t buf[65536];
                    uint32_t from_id = 0;
                    while (true) {
                        int ret = transport->serverRecv(buf, sizeof(buf), from_id);
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

    // Set shm_transport to NULL first so any in-flight callback on the
    // owner thread sees the cleared pointer and returns early, avoiding
    // use-after-free when unregisterService is called re-entrantly from
    // a service callback.
    ShmTransport* transport = entry->shm_transport;
    entry->shm_transport = NULL;

    if (transport->reqEventFd() >= 0) {
        loop_->removeFd(transport->reqEventFd());
    }
    if (transport->udsListenFd() >= 0) {
        loop_->removeFd(transport->udsListenFd());
    }

    for (std::set<int>::iterator it = entry->shm_client_notify_fds.begin();
         it != entry->shm_client_notify_fds.end(); ++it) {
        loop_->removeFd(*it);
    }
    entry->shm_client_notify_fds.clear();

    // Restore so the caller (which may wrap this in its own cleanup) can
    // still close and delete the transport through the entry.
    entry->shm_transport = transport;
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

