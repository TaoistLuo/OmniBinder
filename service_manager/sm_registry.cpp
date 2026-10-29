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
    int displaced_fd = -1;
    ServiceHandle handle = registry_.addService(info, new_fd, &displaced_fd);
    if (handle == INVALID_HANDLE) {
        OMNI_LOG_WARN(TAG, "Failed to register service: %s", info.name.c_str());
        sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
        return;
    }

    sendRegisterReply(conn, msg.header.sequence, handle);

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

    // 摘除可能经 notifyServiceDeath 发送失败重入 closeClient(fd)（自订阅场景），
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

void ServiceManagerApp::handleSubscribeService(ClientConnection* conn, const Message& msg) {
    std::string target_service;
    if (!sm_internal::tryReadExactStringArg(msg, target_service, MAX_SERVICE_NAME_LENGTH)) {
        OMNI_LOG_WARN(TAG, "Reject malformed subscribe service request from fd=%d", conn->fd);
        sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, msg.header.sequence, false);
        return;
    }

    bool success = death_notifier_.subscribe(conn->fd, target_service);
    sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, msg.header.sequence, success);
}

void ServiceManagerApp::handleUnsubscribeService(ClientConnection* conn, const Message& msg) {
    std::string target_service;
    if (!sm_internal::tryReadExactStringArg(msg, target_service, MAX_SERVICE_NAME_LENGTH)) {
        OMNI_LOG_WARN(TAG, "Drop malformed unsubscribe service request from fd=%d", conn->fd);
        return;
    }

    death_notifier_.unsubscribe(conn->fd, target_service);
    // unsubscribe 无需回复
}

void ServiceManagerApp::handleSubscribeServiceInstance(ClientConnection* conn,
                                                       const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    ServiceIdentity identity;
    if (!readServiceIdentityPayload(buf, identity.name, identity.host_id, identity.instance_id)) {
        OMNI_LOG_WARN(TAG, "Reject malformed subscribe service instance request from fd=%d",
                       conn->fd);
        sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, msg.header.sequence, false);
        return;
    }

    bool success = death_notifier_.subscribeInstance(conn->fd, identity);
    sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, msg.header.sequence, success);
}

void ServiceManagerApp::handleUnsubscribeServiceInstance(ClientConnection* conn,
                                                         const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    ServiceIdentity identity;
    if (!readServiceIdentityPayload(buf, identity.name, identity.host_id, identity.instance_id)) {
        OMNI_LOG_WARN(TAG, "Drop malformed unsubscribe service instance request from fd=%d",
                       conn->fd);
        return;
    }

    death_notifier_.unsubscribeInstance(conn->fd, identity);
    // unsubscribe 无需回复
}

void ServiceManagerApp::notifyServiceInstanceDeath(const ServiceIdentity& identity) {
    // 名字级订阅只在最后一个实例消失时触发：调用前该实例已从注册表移除，
    // exists() 反映移除后是否仍有同名实例存活
    const bool name_gone = !registry_.exists(identity.name);
    std::vector<int> subscribers =
        death_notifier_.notifyInstanceDeath(identity, name_gone);
    const MessageType type = name_gone ? MessageType::MSG_DEATH_NOTIFY
                                       : MessageType::MSG_DEATH_NOTIFY_INSTANCE;

    for (size_t i = 0; i < subscribers.size(); ++i) {
        // sendDeathNotify → sendMessage 发送失败可能重入 closeClient(fd)，
        // 每轮重新查找订阅者连接（约束 1）
        std::map<int, ClientConnection*>::iterator it = clients_.find(subscribers[i]);
        if (it != clients_.end()) {
            sendDeathNotify(it->second, identity, type);
        }
    }
}

void ServiceManagerApp::notifyServiceRemoved(const ServiceIdentity& identity) {
    notifyServiceInstanceDeath(identity);
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

void ServiceManagerApp::sendDeathNotify(ClientConnection* conn,
                                        const ServiceIdentity& identity,
                                        MessageType type) {
    Message notify(type, nextSMProactiveSequence());
    writeServiceIdentityPayload(notify.payload, identity.name, identity.host_id,
                                identity.instance_id);
    sendMessage(conn, notify);
}

} // namespace omnibinder
