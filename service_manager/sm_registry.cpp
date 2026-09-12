#include "service_manager_app.h"
#include "sm_parse_helpers.h"
#include "omnibinder/log.h"

#define TAG "ServiceManager"

namespace omnibinder {

void ServiceManagerApp::handleRegister(ClientConnection* conn, const Message& msg) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    ServiceInfo info;

    if (!deserializeServiceInfo(payload, info)) {
        OMNI_LOG_ERROR(TAG, "Failed to deserialize ServiceInfo from fd=%d", conn->fd);
        sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
        return;
    }

    ServiceHandle handle = registry_.addService(info, conn->fd);
    if (handle == INVALID_HANDLE) {
        OMNI_LOG_WARN(TAG, "Failed to register service: %s", info.name.c_str());
        sendRegisterReply(conn, msg.header.sequence, INVALID_HANDLE);
        return;
    }

    heartbeat_.startTracking(info.name);

    sendRegisterReply(conn, msg.header.sequence, handle);
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
    if (!registry_.ownsService(fd, name)) {
        OMNI_LOG_WARN(TAG, "Reject unregister for %s from non-owner fd=%d",
                       name.c_str(), fd);
        sendBoolReply(conn, MessageType::MSG_UNREGISTER_REPLY, msg.header.sequence, false);
        return;
    }

    bool success = registry_.removeService(name);
    if (success) {
        heartbeat_.stopTracking(name);

        // 先通知死亡订阅者，再做发布者清理（与心跳超时路径顺序一致）。
        // notifyServiceDeath -> sendDeathNotify -> sendMessage 在发送失败时
        // 可能重入 closeClient(fd)（自订阅场景），因此后续使用前必须按 fd
        // 重新校验 conn 存活（约束 1）。
        notifyServiceDeath(name);

        if (clients_.find(fd) == clients_.end()) {
            // closeClient(fd) 已在其自身的 topic_manager_.removeByFd(fd)
            // 调用中清理了该 fd 的发布者。
            return;
        }
        conn = clients_.find(fd)->second;

        // 只清理该服务的发布者。同一 runtime 控制连接上的订阅关系
        // 和同级服务保持有效。
        topic_manager_.removePublishersByService(name, fd);
    }

    sendBoolReply(conn, MessageType::MSG_UNREGISTER_REPLY, msg.header.sequence, success);
}

void ServiceManagerApp::handleHeartbeat(ClientConnection* conn, const Message& msg) {
    std::string name;
    if (!sm_internal::tryReadExactStringArg(msg, name, MAX_SERVICE_NAME_LENGTH)) {
        OMNI_LOG_WARN(TAG, "Reject malformed heartbeat from fd=%d", conn->fd);
        return;
    }

    if (!registry_.ownsService(conn->fd, name)) {
        OMNI_LOG_WARN(TAG, "Drop stale or non-owner heartbeat for %s from fd=%d",
                       name.c_str(), conn->fd);
        return;
    }

    heartbeat_.updateHeartbeat(name);
    sendHeartbeatAck(conn, msg.header.sequence);
}

void ServiceManagerApp::sendHeartbeatAck(ClientConnection* conn, uint32_t seq) {
    Message reply(MessageType::MSG_HEARTBEAT_ACK, seq);
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
        OMNI_LOG_ERROR(TAG, "Failed to serialize lookup reply for seq=%u, degrade to not-found", seq);
        Message fallback(MessageType::MSG_LOOKUP_REPLY, seq);
        fallback.payload.writeBool(false);
        sendMessage(conn, fallback);
        return;
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
                           "Failed to serialize service list entry %zu/%zu for seq=%u, degrade to empty list",
                           i, services.size(), seq);
            Message fallback(MessageType::MSG_LIST_SERVICES_REPLY, seq);
            fallback.payload.writeUint32(0);
            sendMessage(conn, fallback);
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
                               "Failed to serialize interface entry %zu/%zu for seq=%u, degrade to not-found",
                               i, interfaces.size(), seq);
                Message fallback(MessageType::MSG_QUERY_INTERFACES_REPLY, seq);
                fallback.payload.writeBool(false);
                sendMessage(conn, fallback);
                return;
            }
        }
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleSubscribeService(ClientConnection* conn, const Message& msg) {
    std::string target_service;
    if (!sm_internal::tryReadStringArg(msg, target_service)) {
        OMNI_LOG_WARN(TAG, "Reject malformed subscribe service request from fd=%d", conn->fd);
        sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, msg.header.sequence, false);
        return;
    }

    bool success = death_notifier_.subscribe(conn->fd, target_service);
    sendBoolReply(conn, MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, msg.header.sequence, success);
}

void ServiceManagerApp::handleUnsubscribeService(ClientConnection* conn, const Message& msg) {
    std::string target_service;
    if (!sm_internal::tryReadStringArg(msg, target_service)) {
        OMNI_LOG_WARN(TAG, "Drop malformed unsubscribe service request from fd=%d", conn->fd);
        return;
    }

    death_notifier_.unsubscribe(conn->fd, target_service);
    // unsubscribe 无需回复
}

void ServiceManagerApp::onHeartbeatCheck() {
    std::vector<std::string> timed_out = heartbeat_.checkTimeouts();

    // 本轮确有服务因心跳超时被摘除的控制连接。全部超时服务处理完后，再统一
    // 判定这些连接是否已无任何存活注册；纯客户端（从未注册过任何服务）不会
    // 进入该集合，因此永远不会被本规则关闭。
    std::set<int> affected_fds;

    for (size_t i = 0; i < timed_out.size(); ++i) {
        const std::string& name = timed_out[i];
        OMNI_LOG_WARN(TAG, "Service timed out: %s", name.c_str());

        // 移除前先获取控制连接 fd
        int fd = registry_.getControlFd(name);

        if (!registry_.removeService(name)) {
            continue;
        }

        if (fd >= 0) {
            affected_fds.insert(fd);
        }

        // 保持既定顺序：先移除注册表条目，再通知死亡，
        // 最后只清理该超时服务的发布者。
        notifyServiceDeath(name);
        if (fd >= 0) {
            topic_manager_.removePublishersByService(name, fd);
        }
    }

    // 连接级失联处理（C2）：心跳超时后若该控制连接已无任何存活注册服务，
    // 则按连接失联关闭，让 runtime 走既有的 disconnect → reconnect →
    // restoreControlPlaneState 路径重新注册（约束 6）。若同连接仍有健康服务，
    // 仅摘除超时服务，不断开连接，避免误伤同一 runtime 的其它服务。
    for (std::set<int>::iterator it = affected_fds.begin(); it != affected_fds.end(); ++it) {
        int fd = *it;

        // notifyServiceDeath → sendDeathNotify → sendMessage 发送失败可能重入
        // closeClient(fd)，必须在每轮重新判活（约束 1/2）
        if (clients_.find(fd) == clients_.end()) {
            continue;
        }
        if (!registry_.listServiceNamesByFd(fd).empty()) {
            continue;
        }

        OMNI_LOG_WARN(TAG, "Control connection fd=%d has no live service after heartbeat "
                           "timeout, closing to trigger runtime re-registration", fd);
        closeClient(fd);
    }
}

void ServiceManagerApp::notifyServiceDeath(const std::string& service_name) {
    std::vector<int> subscribers = death_notifier_.notify(service_name);

    for (size_t i = 0; i < subscribers.size(); ++i) {
        int fd = subscribers[i];
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            sendDeathNotify(it->second, service_name);
        }
    }
}

void ServiceManagerApp::sendDeathNotify(ClientConnection* conn,
                                    const std::string& service_name) {
    Message notify(MessageType::MSG_DEATH_NOTIFY, nextSMProactiveSequence());
    notify.payload.writeString(service_name);
    sendMessage(conn, notify);
}

} // namespace omnibinder
