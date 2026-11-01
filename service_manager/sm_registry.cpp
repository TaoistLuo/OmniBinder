#include "service_manager_app.h"
#include "sm_parse_helpers.h"
#include "omnibinder/log.h"
#include "platform/platform.h"

#define TAG "ServiceManager"

namespace omnibinder {

void ServiceManagerApp::handleRegister(ClientConnection* conn, const Message& msg) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    ServiceInfo info;

    if (!conn->hello_received) {
        OMNI_LOG_WARN(TAG, "Reject register before session hello from fd=%d", conn->fd);
        sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
        return;
    }

    if (!deserializeServiceInfo(payload, info)) {
        OMNI_LOG_ERROR(TAG, "Failed to deserialize ServiceInfo from fd=%d", conn->fd);
        sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
        return;
    }

    const int new_fd = conn->fd;
    const ServiceIdentity identity(info.name, info.host_id, info.instance_id);
    ServiceEntry old_entry;
    const bool existed = registry_.findServiceByIdentity(identity, old_entry);
    int displaced_fd = -1;
    ServiceHandle handle = registry_.addService(info, new_fd, &displaced_fd);
    if (handle == INVALID_HANDLE) {
        OMNI_LOG_WARN(TAG, "Failed to register service: %s", info.name.c_str());
        sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
        return;
    }

    sendRegisterReply(conn, msg.header.sequence, handle);

    // 同名不同实例：这是新实例上线；同身份 takeover 时端点已变化，按
    // offline + online 通知观察者，便于业务重新初始化数据面资源。
    if (!existed) {
        notifyServiceWatch(identity, true);
    } else if (old_entry.control_fd != new_fd || old_entry.info.port != info.port) {
        notifyServiceWatch(identity, false);
        notifyServiceWatch(identity, true);
    }

    // 后来者已接管该名称：回收被顶替的旧控制连接上残留的发布关系，否则旧 fd
    // 会继续占住话题发布者位置，新 owner 的 publishTopic 被拒。旧连接若已无
    // 其它存活服务则直接关闭，避免其心跳被永久丢弃、连接变僵尸。
    // 此处关闭的是其它 fd，不重入本连接；closeClient 先摘表再清理（约束 1/2）。
    if (displaced_fd >= 0 && displaced_fd != new_fd
        && clients_.find(displaced_fd) != clients_.end()) {
        topic_manager_.removePublishersByService(info.name, displaced_fd);
        if (registry_.listServiceNamesByFd(displaced_fd).empty()) {
            closeClient(displaced_fd);
        }
    }
}

void ServiceManagerApp::sendRegisterReply(ClientConnection* conn, uint32_t seq,
                                      ServiceHandle handle) {
    Message reply(MessageType::MSG_REGISTER_REPLY, seq);
    reply.payload.writeUint32(handle);
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleUnregister(ClientConnection* conn, const Message& msg) {
    std::string name;
    if (!sm_internal::tryReadStringArg(msg, name)) {
        OMNI_LOG_WARN(TAG, "Reject malformed unregister request from fd=%d", conn->fd);
        sendBoolReply(conn, MessageType::MSG_UNREGISTER_REPLY, msg.header.sequence, false);
        return;
    }

    const int fd = conn->fd;
    ServiceEntry entry;
    if (!registry_.findServiceByFdAndName(fd, name, entry)) {
        OMNI_LOG_WARN(TAG, "Reject unregister for %s from non-owner fd=%d",
                       name.c_str(), fd);
        sendBoolReply(conn, MessageType::MSG_UNREGISTER_REPLY, msg.header.sequence, false);
        return;
    }

    // 摘除可能经 watch 通知发送失败重入 closeClient(fd)（自订阅场景），
    // 故使用 conn 前需按 fd 重新校验存活（约束 1）。
    const ServiceIdentity identity(entry.info.name, entry.info.host_id, entry.info.instance_id);
    bool success = removeServiceAndNotify(identity, fd);

    if (success) {
        if (clients_.find(fd) == clients_.end()) {
            return;
        }
        conn = clients_.find(fd)->second;
    }

    sendBoolReply(conn, MessageType::MSG_UNREGISTER_REPLY, msg.header.sequence, success);
}

void ServiceManagerApp::handleControlHeartbeat(ClientConnection* conn, const Message& msg) {
    if (!conn || !conn->hello_received) {
        if (conn) closeClient(conn->fd);
        return;
    }
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t incarnation = 0;
    if (!buf.tryReadUint32(incarnation)) {
        OMNI_LOG_WARN(TAG, "Reject malformed control heartbeat from fd=%d", conn->fd);
        closeClient(conn->fd);
        return;
    }
    if (incarnation != conn->incarnation_id) {
        OMNI_LOG_WARN(TAG, "Control heartbeat incarnation mismatch fd=%d got=%u expected=%u",
                      conn->fd, incarnation, conn->incarnation_id);
        closeClient(conn->fd);
        return;
    }

    conn->last_heartbeat_ms = platform::currentTimeMs();
    armSessionDeadline(conn);
    sendControlHeartbeatAck(conn, msg.header.sequence);
}

void ServiceManagerApp::sendControlHeartbeatAck(ClientConnection* conn, uint32_t seq) {
    Message reply(MessageType::MSG_CONTROL_HEARTBEAT_ACK, seq);
    reply.payload.writeUint32(sm_incarnation_id_);
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleLookup(ClientConnection* conn, const Message& msg) {
    std::string name;
    if (!sm_internal::tryReadStringArg(msg, name)) {
        OMNI_LOG_WARN(TAG, "Reject malformed lookup request from fd=%d", conn->fd);
        sendLookupReply(conn, msg.header.sequence, false, ServiceInfo());
        return;
    }

    ServiceEntry entry;
    bool found = registry_.findService(name, entry);

    sendLookupReply(conn, msg.header.sequence, found, found ? entry.info : ServiceInfo());
}

void ServiceManagerApp::sendLookupReply(ClientConnection* conn, uint32_t seq, bool found,
                                    const ServiceInfo& info) {
    Message reply(MessageType::MSG_LOOKUP_REPLY, seq);
    reply.payload.writeBool(found);
    if (found && !serializeServiceInfo(info, reply.payload)) {
        OMNI_LOG_ERROR(TAG, "Failed to serialize lookup reply for seq=%u", seq);
        return;
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleListInstances(ClientConnection* conn, const Message& msg) {
    std::string name;
    if (!sm_internal::tryReadStringArg(msg, name)) {
        OMNI_LOG_WARN(TAG, "Reject malformed list instances request from fd=%d", conn->fd);
        sendListInstancesReply(conn, msg.header.sequence, false, std::vector<ServiceInfo>());
        return;
    }

    std::vector<ServiceInfo> instances = registry_.listServiceInstances(name);
    sendListInstancesReply(conn, msg.header.sequence, !instances.empty(), instances);
}

void ServiceManagerApp::sendListInstancesReply(ClientConnection* conn, uint32_t seq, bool found,
                                               const std::vector<ServiceInfo>& instances) {
    Message reply(MessageType::MSG_LIST_INSTANCES_REPLY, seq);
    reply.payload.writeBool(found);
    reply.payload.writeUint32(static_cast<uint32_t>(instances.size()));
    if (found) {
        for (size_t i = 0; i < instances.size(); ++i) {
            if (!serializeServiceInfo(instances[i], reply.payload)) {
                OMNI_LOG_ERROR(TAG,
                               "Failed to serialize instance entry %zu/%zu for seq=%u",
                               i, instances.size(), seq);
                return;
            }
        }
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleListServices(ClientConnection* conn, const Message& msg) {
    std::vector<ServiceInfo> all_services = registry_.listServices();
    std::vector<ServiceInfo> services;
    for (size_t i = 0; i < all_services.size(); ++i) {
        if (all_services[i].name.find(DIAG_SERVICE_NAME_PREFIX) == 0) {
            continue;
        }
        services.push_back(all_services[i]);
    }
    sendListServicesReply(conn, msg.header.sequence, services);
}

void ServiceManagerApp::sendListServicesReply(
    ClientConnection* conn, uint32_t seq,
    const std::vector<ServiceInfo>& services) {
    Message reply(MessageType::MSG_LIST_SERVICES_REPLY, seq);
    reply.payload.writeUint32(static_cast<uint32_t>(services.size()));
    for (size_t i = 0; i < services.size(); ++i) {
        if (!serializeServiceInfo(services[i], reply.payload)) {
            OMNI_LOG_ERROR(TAG,
                           "Failed to serialize service list entry %zu/%zu for seq=%u",
                           i, services.size(), seq);
            return;
        }
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleQueryInterfaces(ClientConnection* conn, const Message& msg) {
    std::string name;
    if (!sm_internal::tryReadStringArg(msg, name)) {
        OMNI_LOG_WARN(TAG, "Reject malformed query interfaces request from fd=%d", conn->fd);
        sendQueryInterfacesReply(conn, msg.header.sequence, false, std::vector<InterfaceInfo>());
        return;
    }

    ServiceEntry entry;
    bool found = registry_.findService(name, entry);

    sendQueryInterfacesReply(conn, msg.header.sequence, found,
                             found ? entry.info.interfaces : std::vector<InterfaceInfo>());
}

void ServiceManagerApp::sendQueryInterfacesReply(
    ClientConnection* conn, uint32_t seq, bool found,
    const std::vector<InterfaceInfo>& interfaces) {
    Message reply(MessageType::MSG_QUERY_INTERFACES_REPLY, seq);
    reply.payload.writeBool(found);
    if (found) {
        reply.payload.writeUint32(static_cast<uint32_t>(interfaces.size()));
        for (size_t i = 0; i < interfaces.size(); ++i) {
            if (!serializeInterfaceInfo(interfaces[i], reply.payload)) {
                OMNI_LOG_ERROR(TAG,
                               "Failed to serialize interface entry %zu/%zu for seq=%u",
                               i, interfaces.size(), seq);
                return;
            }
        }
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleSubscribeServiceWatch(ClientConnection* conn,
                                                     const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint64_t watch_id = 0;
    uint32_t count = 0;
    if (!buf.tryReadUint64(watch_id) || !buf.tryReadUint32(count)
        || watch_id == 0 || count == 0 || count > 64) {
        sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_WATCH_REPLY,
                      msg.header.sequence, false);
        return;
    }

    std::vector<ServiceWatchFilter> filters;
    filters.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        std::string host_id;
        std::string instance_id;
        if (!buf.tryReadString(name) || !buf.tryReadString(host_id)
            || !buf.tryReadString(instance_id)
            || name.empty()
            || name.size() > MAX_SERVICE_NAME_LENGTH
            || host_id.size() > MAX_SERVICE_NAME_LENGTH
            || instance_id.size() > MAX_SERVICE_NAME_LENGTH) {
            sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_WATCH_REPLY,
                          msg.header.sequence, false);
            return;
        }
        for (size_t j = 0; j < filters.size(); ++j) {
            if (ServiceWatchNotifier::overlaps(filters[j],
                    ServiceWatchFilter(name, host_id, instance_id))) {
                OMNI_LOG_WARN(TAG,
                    "Reject overlapping service watch from fd=%d: %s/%s/%s",
                    conn->fd, name.c_str(), host_id.c_str(), instance_id.c_str());
                sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_WATCH_REPLY,
                              msg.header.sequence, false);
                return;
            }
        }
        filters.push_back(ServiceWatchFilter(name, host_id, instance_id));
    }

    bool accepted = watch_notifier_.subscribe(conn->fd, watch_id, filters);
    sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_WATCH_REPLY,
                  msg.header.sequence, accepted);
    if (!accepted) {
        return;
    }

    for (size_t i = 0; i < filters.size(); ++i) {
        if (clients_.find(conn->fd) == clients_.end()) {
            return;
        }
        sendInitialServiceWatch(conn, watch_id, filters[i]);
    }
}

void ServiceManagerApp::handleUnsubscribeServiceWatch(ClientConnection* conn,
                                                       const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint64_t watch_id = 0;
    if (!buf.tryReadUint64(watch_id) || watch_id == 0) {
        return;
    }
    watch_notifier_.unsubscribe(conn->fd, watch_id);
}

void ServiceManagerApp::notifyServiceWatch(const ServiceIdentity& identity, bool online) {
    std::vector<ServiceWatchGroup> groups = watch_notifier_.matching(identity);
    for (size_t i = 0; i < groups.size(); ++i) {
        std::map<int, ClientConnection*>::iterator it =
            clients_.find(groups[i].subscriber_fd);
        if (it != clients_.end()) {
            sendServiceWatchNotify(it->second, groups[i].watch_id, identity, online);
        }
    }
}

void ServiceManagerApp::sendServiceWatchNotify(ClientConnection* conn, uint64_t watch_id,
                                               const ServiceIdentity& identity, bool online) {
    Message notify(MessageType::MSG_SERVICE_WATCH_NOTIFY, nextSMProactiveSequence());
    notify.payload.writeUint64(watch_id);
    notify.payload.writeString(identity.name);
    notify.payload.writeString(identity.host_id);
    notify.payload.writeString(identity.instance_id);
    notify.payload.writeBool(online);
    sendMessage(conn, notify);
}

void ServiceManagerApp::sendInitialServiceWatch(ClientConnection* conn, uint64_t watch_id,
                                                const ServiceWatchFilter& filter) {
    const ServiceIdentity filter_identity(filter.service_name, filter.host_id,
                                          filter.instance_id);
    std::vector<ServiceInfo> instances =
        registry_.listServiceInstances(filter.service_name);
    bool found = false;
    for (size_t i = 0; i < instances.size(); ++i) {
        const ServiceIdentity identity(instances[i].name, instances[i].host_id,
                                       instances[i].instance_id);
        if (!ServiceWatchNotifier::matches(filter, identity)) {
            continue;
        }
        found = true;
        if (clients_.find(conn->fd) == clients_.end()) {
            return;
        }
        sendServiceWatchNotify(conn, watch_id, identity, true);
    }
    if (!found && clients_.find(conn->fd) != clients_.end()) {
        sendServiceWatchNotify(conn, watch_id, filter_identity, false);
    }
}

void ServiceManagerApp::notifyServiceRemoved(const ServiceIdentity& identity) {
    notifyServiceWatch(identity, false);
}

bool ServiceManagerApp::removeServiceAndNotify(const ServiceIdentity& identity, int fd) {
    if (!registry_.removeServiceByIdentity(identity)) {
        return false;
    }
    notifyServiceRemoved(identity);
    if (fd >= 0) {
        topic_manager_.removePublishersByService(identity.name, fd);
    }
    return true;
}


} // namespace omnibinder
