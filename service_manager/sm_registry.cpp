#include "service_manager_app.h"
#include "sm_parse_helpers.h"
#include "omnibinder/log.h"
#include <algorithm>

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

        // Track which service this connection registered
        conn->registered_service_name = info.name;
        if (std::find(conn->registered_services.begin(), conn->registered_services.end(), info.name)
            == conn->registered_services.end()) {
            conn->registered_services.push_back(info.name);
        }

        // Start heartbeat tracking
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
            sendUnregisterReply(conn, msg.header.sequence, false);
            return;
        }

        if (!registry_.ownsService(conn->fd, name)) {
            OMNI_LOG_WARN(TAG, "Reject unregister for %s from non-owner fd=%d",
                           name.c_str(), conn->fd);
            sendUnregisterReply(conn, msg.header.sequence, false);
            return;
        }

        bool success = registry_.removeService(name);
        if (success) {
            heartbeat_.stopTracking(name);

            // Notify death subscribers
            notifyServiceDeath(name);

            // Clean up only this service's publishers. Subscriptions and sibling
            // services on the same runtime control connection remain active.
            topic_manager_.removePublishersByService(name, conn->fd);

            if (conn->registered_service_name == name) {
                conn->registered_service_name.clear();
            }
            conn->registered_services.erase(std::remove(conn->registered_services.begin(),
                                                        conn->registered_services.end(), name),
                                            conn->registered_services.end());
        }

        sendUnregisterReply(conn, msg.header.sequence, success);
}

void ServiceManagerApp::sendUnregisterReply(ClientConnection* conn, uint32_t seq,
                                            bool success) {
        Message reply(MessageType::MSG_UNREGISTER_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
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
        if (found) {
            serializeServiceInfo(info, reply.payload);
        }
        sendMessage(conn, reply);
}

void ServiceManagerApp::handleListServices(ClientConnection* conn, const Message& msg) {
        std::vector<ServiceInfo> all_services = registry_.listServices();
        std::vector<ServiceInfo> services;
        for (size_t i = 0; i < all_services.size(); ++i) {
            if (all_services[i].name.find("__diag_pid_") == 0) {
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
            serializeServiceInfo(services[i], reply.payload);
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
                serializeInterfaceInfo(interfaces[i], reply.payload);
            }
        }
        sendMessage(conn, reply);
}

void ServiceManagerApp::handleSubscribeService(ClientConnection* conn, const Message& msg) {
        std::string target_service;
        if (!sm_internal::tryReadStringArg(msg, target_service)) {
            OMNI_LOG_WARN(TAG, "Reject malformed subscribe service request from fd=%d", conn->fd);
            sendSubscribeServiceReply(conn, msg.header.sequence, false);
            return;
        }

        bool success = death_notifier_.subscribe(conn->fd, target_service);
        sendSubscribeServiceReply(conn, msg.header.sequence, success);
}

void ServiceManagerApp::sendSubscribeServiceReply(ClientConnection* conn, uint32_t seq,
                                                  bool success) {
        Message reply(MessageType::MSG_SUBSCRIBE_SERVICE_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
}

void ServiceManagerApp::handleUnsubscribeService(ClientConnection* conn, const Message& msg) {
        std::string target_service;
        if (!sm_internal::tryReadStringArg(msg, target_service)) {
            OMNI_LOG_WARN(TAG, "Drop malformed unsubscribe service request from fd=%d", conn->fd);
            return;
        }

        death_notifier_.unsubscribe(conn->fd, target_service);
        // No reply needed for unsubscribe
}

void ServiceManagerApp::onHeartbeatCheck() {
        std::vector<std::string> timed_out = heartbeat_.checkTimeouts();

        for (size_t i = 0; i < timed_out.size(); ++i) {
            const std::string& name = timed_out[i];
            OMNI_LOG_WARN(TAG, "Service timed out: %s", name.c_str());

            // Get the control fd before removing
            int fd = registry_.getControlFd(name);

            if (!registry_.removeService(name)) {
                continue;
            }

            // Preserve established ordering: registry removal, death notification,
            // then publisher cleanup for the expired service only.
            notifyServiceDeath(name);
            if (fd >= 0) {
                topic_manager_.removePublishersByService(name, fd);
                auto client_it = clients_.find(fd);
                if (client_it != clients_.end()) {
                    ClientConnection* conn = client_it->second;
                    if (conn->registered_service_name == name) {
                        conn->registered_service_name.clear();
                    }
                    conn->registered_services.erase(
                        std::remove(conn->registered_services.begin(),
                                    conn->registered_services.end(), name),
                        conn->registered_services.end());
                }
            }
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
        Message notify(MessageType::MSG_DEATH_NOTIFY, nextSequenceNumber());
        notify.payload.writeString(service_name);
        sendMessage(conn, notify);
}

} // namespace omnibinder
