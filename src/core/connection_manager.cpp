#include "core/connection_manager.h"
#include "core/event_loop.h"
#include "core/message_reader.h"
#include "core/runtime_helpers.h"
#include "transport/transport_selector.h"
#include "platform/platform.h"
#include "omnibinder/error.h"
#include "omnibinder/log.h"

#define LOG_TAG "ConnMgr"

namespace omnibinder {

ConnectionManager::ConnectionManager(EventLoop& loop, const std::string& local_host_id)
    : loop_(loop)
    , local_host_id_(local_host_id)
{
}

ConnectionManager::~ConnectionManager() {
    closeAll();
}

ServiceConnection* ConnectionManager::getOrCreateConnection(
    const std::string& service_name,
    const std::string& host,
    uint16_t port,
    const std::string& host_id,
    const ShmConfig& shm_config)
{
    // 检查是否已有连接
    std::map<std::string, ServiceConnection*>::iterator it = connections_.find(service_name);
    if (it != connections_.end()) {
        if (it->second->connected) {
            return it->second;
        }
        // 已断开连接：先注销其 fd 再释放，避免 EventLoop 中残留悬垂 fd
        // （部分错误路径只置 connected=false 而未 removeFd）
        if (it->second->transport && it->second->transport->fd() >= 0) {
            loop_.removeFd(it->second->transport->fd());
        }
        delete it->second;
        connections_.erase(it);
    }

    ServiceConnection* conn = new ServiceConnection();
    conn->service_name = service_name;

    conn->transport = createClientConnection(service_name, host, port,
                                      local_host_id_, host_id, shm_config);
    if (!conn->transport) {
        delete conn;
        return NULL;
    }

    // 无效 fd 的传输不可用：不保留无效连接，避免无事件注册的僵尸连接
    if (conn->transport->fd() < 0) {
        OMNI_LOG_WARN(LOG_TAG, "Invalid transport fd for %s, connection rejected",
                      service_name.c_str());
        conn->transport->close();
        delete conn->transport;
        conn->transport = NULL;
        delete conn;
        return NULL;
    }

    conn->connected = true;
    connections_[service_name] = conn;

    // 注册到 EventLoop
    int fd = conn->transport->fd();
    loop_.addFd(fd, EventLoop::EVENT_READ,
        [this, service_name](int fd, uint32_t events) {
            (void)events;
            this->onConnectionData(service_name, fd);
        });

    OMNI_LOG_INFO(LOG_TAG, "Connected to %s via %s (fd=%d)",
                    service_name.c_str(),
                    dataChannelKindName(conn->transport->type()),
                    conn->transport->fd());

    return conn;
}

ServiceConnection* ConnectionManager::getConnection(const std::string& service_name) {
    std::map<std::string, ServiceConnection*>::iterator it = connections_.find(service_name);
    if (it != connections_.end() && it->second->connected) {
        return it->second;
    }
    return NULL;
}

void ConnectionManager::removeConnection(const std::string& service_name) {
    std::map<std::string, ServiceConnection*>::iterator it = connections_.find(service_name);
    if (it != connections_.end()) {
        ServiceConnection* conn = it->second;
        if (conn->transport && conn->transport->fd() >= 0) {
            loop_.removeFd(conn->transport->fd());
        }
        delete conn;
        connections_.erase(it);
        OMNI_LOG_INFO(LOG_TAG, "Removed connection to %s", service_name.c_str());
    }
}

bool ConnectionManager::sendMessage(const std::string& service_name, Message& msg) {
    ServiceConnection* conn = getConnection(service_name);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "No connection to %s", service_name.c_str());
        return false;
    }

    if (!msg.serializeInPlace()) {
        OMNI_LOG_ERROR(LOG_TAG, "message_serialize_failed service=%s type=%s",
                       service_name.c_str(), messageTypeToString(msg.getType()));
        return false;
    }
    return sendRaw(conn, msg.payload.data(), msg.payload.size());
}

bool ConnectionManager::sendMessageWithinTimeout(const std::string& service_name,
                                                Message& msg,
                                                uint32_t timeout_ms,
                                                uint32_t* elapsed_ms) {
    ServiceConnection* conn = getConnection(service_name);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "No connection to %s", service_name.c_str());
        return false;
    }

    if (!msg.serializeInPlace()) {
        OMNI_LOG_ERROR(LOG_TAG, "message_serialize_failed service=%s type=%s timeout_ms=%u",
                       service_name.c_str(), messageTypeToString(msg.getType()), timeout_ms);
        return false;
    }
    return sendRawWithinTimeout(conn, msg.payload.data(), msg.payload.size(), timeout_ms, elapsed_ms);
}

bool ConnectionManager::sendRaw(ServiceConnection* conn, const uint8_t* data, size_t length) {
    return sendRawWithDeadline(conn, data, length, 0);
}

bool ConnectionManager::sendRawWithDeadline(ServiceConnection* conn, const uint8_t* data,
                                            size_t length, uint32_t deadline_ms) {
    if (!conn || !conn->transport || !conn->connected) {
        return false;
    }

    uint32_t timeout_ms = 0;
    if (deadline_ms > 0) {
        int64_t now = platform::currentTimeMs();
        if (now >= static_cast<int64_t>(deadline_ms)) {
            OMNI_LOG_WARN(LOG_TAG,
                          "data_send_deadline_expired service=%s bytes=%zu",
                          conn->service_name.c_str(), length);
            return false;
        }
        timeout_ms = static_cast<uint32_t>(deadline_ms - now);
    }

    if (conn->transport->sendAll(data, length, timeout_ms, NULL) != 0) {
        OMNI_LOG_WARN(LOG_TAG,
                      "data_send_incomplete service=%s transport=%s bytes=%zu timeout_ms=%u",
                      conn->service_name.c_str(),
                      dataChannelKindName(conn->transport->type()),
                      length, timeout_ms);
        return false;
    }
    return true;
}

bool ConnectionManager::sendRawWithinTimeout(ServiceConnection* conn,
                                            const uint8_t* data,
                                            size_t length,
                                            uint32_t timeout_ms,
                                            uint32_t* elapsed_ms) {
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }

    if (!conn || !conn->transport || !conn->connected) {
        return false;
    }

    uint32_t elapsed = 0;
    int ret = conn->transport->sendAll(data, length, timeout_ms, &elapsed);
    if (elapsed_ms) {
        *elapsed_ms = elapsed;
    }
    if (ret != 0) {
        OMNI_LOG_WARN(LOG_TAG,
                      "data_send_incomplete service=%s transport=%s bytes=%zu timeout_ms=%u elapsed_ms=%u",
                      conn->service_name.c_str(),
                      dataChannelKindName(conn->transport->type()),
                      length, timeout_ms, elapsed);
        return false;
    }
    return true;
}

void ConnectionManager::setMessageCallback(const MessageCallback& cb) {
    message_cb_ = cb;
}

void ConnectionManager::setDisconnectCallback(const DisconnectCallback& cb) {
    disconnect_cb_ = cb;
}

void ConnectionManager::closeAll() {
    for (std::map<std::string, ServiceConnection*>::iterator it = connections_.begin();
         it != connections_.end(); ++it) {
        ServiceConnection* conn = it->second;
        if (conn->transport && conn->transport->fd() >= 0) {
            loop_.removeFd(conn->transport->fd());
        }
        delete conn;
    }
    connections_.clear();
}

void ConnectionManager::connectionCounts(uint32_t& active, uint32_t& tcp, uint32_t& shm) const {
    active = 0;
    tcp = 0;
    shm = 0;
    for (std::map<std::string, ServiceConnection*>::const_iterator it = connections_.begin();
         it != connections_.end(); ++it) {
        ServiceConnection* conn = it->second;
        if (!conn->connected) {
            continue;
        }
        ++active;
        if (!conn->transport) {
            continue;
        }
        if (conn->transport->type() == TransportType::TCP) {
            ++tcp;
        } else if (conn->transport->type() == TransportType::SHM) {
            ++shm;
        }
    }
}

bool ConnectionManager::failConnection(ServiceConnection* conn) {
    conn->connected = false;
    if (conn->transport && conn->transport->fd() >= 0) {
        loop_.removeFd(conn->transport->fd());
    }
    // 回调前保存 service_name 副本：onDirectDisconnect 内部会 removeConnection
    // 删除 conn，回调后 conn 已释放，不能再解引用其成员（约束 1）
    std::string svc = conn->service_name;
    if (disconnect_cb_) {
        disconnect_cb_(svc);
    }
    // 回调链可能 removeConnection 删除该连接：重新判活（约束 1）
    std::map<std::string, ServiceConnection*>::iterator current =
        connections_.find(svc);
    return current != connections_.end() && current->second == conn;
}

void ConnectionManager::onConnectionData(const std::string& service_name, int fd) {
    (void)fd;
    std::map<std::string, ServiceConnection*>::iterator it = connections_.find(service_name);
    if (it == connections_.end()) {
        return;
    }

    ServiceConnection* conn = it->second;
    if (!conn->transport) {
        return;
    }

    Message msg;
    while (true) {
        int ret = readNextMessage(*conn->transport, conn->recv_buffer, msg);
        if (ret == 0) {
            return;
        }
        if (ret < 0) {
            // 传输错误/流损坏：统一走 failConnection（摘 fd + 断开回调，约束 3）
            OMNI_LOG_WARN(LOG_TAG, "data_connection_lost service=%s transport=%s err=%d",
                          service_name.c_str(),
                          dataChannelKindName(conn->transport->type()),
                          static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED));
            if (!failConnection(conn)) return;
            return;
        }

        if (message_cb_) {
            message_cb_(conn->service_name, msg);
            // 回调可能 removeConnection 删除该连接：重新判活（约束 1）
            std::map<std::string, ServiceConnection*>::iterator current =
                connections_.find(service_name);
            if (current == connections_.end() || current->second != conn) {
                return;
            }
        }
    }
}

} // namespace omnibinder
