#include "core/connection_manager.h"
#include "core/event_loop.h"
#include "transport/transport_selector.h"
#include "transport/shm_transport.h"
#include "platform/platform.h"
#include "omnibinder/error.h"
#include "omnibinder/log.h"
#include <cstring>
#include <memory>
#include <new>

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
        delete it->second;
        connections_.erase(it);
    }

    ServiceConnection* conn = new ServiceConnection();
    conn->service_name = service_name;
    conn->host = host;
    conn->port = port;
    conn->host_id = host_id;

    conn->transport = selectTransport(service_name, host, port,
                                      local_host_id_, host_id, shm_config);
    if (!conn->transport) {
        delete conn;
        return NULL;
    }

    conn->connected = true;
    connections_[service_name] = conn;

    // 注册到 EventLoop
    int fd = conn->transport->fd();
    if (fd >= 0) {
        loop_.addFd(fd, EventLoop::EVENT_READ,
            [this, service_name](int fd, uint32_t events) {
                (void)events;
                this->onConnectionData(service_name, fd);
            });
    }

    OMNI_LOG_INFO(LOG_TAG, "Connected to %s via %s (fd=%d)",
                    service_name.c_str(),
                    conn->transport->type() == TransportType::SHM ? "SHM" : "TCP",
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

bool ConnectionManager::sendMessage(const std::string& service_name, const Message& msg) {
    ServiceConnection* conn = getConnection(service_name);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "No connection to %s", service_name.c_str());
        return false;
    }

    Buffer buf;
    msg.serialize(buf);
    return sendRaw(conn, buf.data(), buf.size());
}

bool ConnectionManager::sendMessageWithinTimeout(const std::string& service_name,
                                                const Message& msg,
                                                uint32_t timeout_ms,
                                                uint32_t* elapsed_ms) {
    ServiceConnection* conn = getConnection(service_name);
    if (!conn) {
        OMNI_LOG_ERROR(LOG_TAG, "No connection to %s", service_name.c_str());
        return false;
    }

    Buffer buf;
    msg.serialize(buf);
    return sendRawWithinTimeout(conn, buf.data(), buf.size(), timeout_ms, elapsed_ms);
}

bool ConnectionManager::sendRaw(ServiceConnection* conn, const uint8_t* data, size_t length) {
    return sendRawWithDeadline(conn, data, length, 0);
}

bool ConnectionManager::sendRawWithDeadline(ServiceConnection* conn, const uint8_t* data,
                                            size_t length, uint32_t deadline_ms) {
    if (!conn || !conn->transport || !conn->connected) {
        return false;
    }

    size_t sent = 0;
    while (sent < length) {
        int ret = conn->transport->send(data + sent, length - sent);
        if (ret < 0) {
            OMNI_LOG_ERROR(LOG_TAG,
                           "data_send_failed service=%s transport=%s err=%d",
                           conn->service_name.c_str(),
                           conn->transport->type() == TransportType::SHM ? "SHM" : "TCP",
                           static_cast<int>(ErrorCode::ERR_SEND_FAILED));
            conn->connected = false;
            return false;
        }
        if (ret == 0) {
            // SHM ring full: busy-wait with deadline to avoid starving the
            // owner event loop indefinitely.
            if (conn->transport->type() == TransportType::SHM && deadline_ms > 0) {
                uint64_t now = platform::currentTimeMs();
                if (now >= deadline_ms) {
                    OMNI_LOG_WARN(LOG_TAG,
                                  "data_send_shm_ring_full service=%s sent=%zu/%zu timeout",
                                  conn->service_name.c_str(), sent, length);
                    return false;
                }
                // Spin briefly (up to 1 ms) then recheck deadline to avoid
                // blocking the event loop for too long.
                platform::sleepMs(1);
                continue;
            } else if (conn->transport->type() == TransportType::SHM) {
                // No deadline provided: fall through to break to avoid
                // unbounded blocking.
                break;
            }
            // TCP EAGAIN without deadline: break
            break;
        }
        sent += static_cast<size_t>(ret);
    }
    return sent == length;
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

    if (conn->transport->type() != TransportType::TCP) {
        uint64_t deadline_ms = platform::currentTimeMs() + timeout_ms;
        bool ok = sendRawWithDeadline(conn, data, length,
                                      static_cast<uint32_t>(deadline_ms));
        if (elapsed_ms) {
            *elapsed_ms = 0;
        }
        return ok;
    }

    bool ok = platform::socketSendAll(conn->transport->fd(), data, length, timeout_ms, elapsed_ms);
    if (!ok) {
        int err = platform::getSocketError();
        uint32_t spent_ms = elapsed_ms ? *elapsed_ms : 0;
        OMNI_LOG_WARN(LOG_TAG,
                      "data_send_incomplete service=%s fd=%d bytes=%zu timeout_ms=%u elapsed_ms=%u sock_err=%d",
                      conn->service_name.c_str(), conn->transport->fd(), length, timeout_ms,
                      spent_ms, err);
        if (!platform::isWouldBlock(err)) {
            OMNI_LOG_ERROR(LOG_TAG,
                           "data_send_failed service=%s transport=%s err=%d",
                           conn->service_name.c_str(),
                           conn->transport->type() == TransportType::SHM ? "SHM" : "TCP",
                           static_cast<int>(ErrorCode::ERR_SEND_FAILED));
            conn->connected = false;
        }
    }
    return ok;
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

std::vector<std::string> ConnectionManager::connectedServices() const {
    std::vector<std::string> result;
    for (std::map<std::string, ServiceConnection*>::const_iterator it = connections_.begin();
         it != connections_.end(); ++it) {
        if (it->second->connected) {
            result.push_back(it->first);
        }
    }
    return result;
}

uint32_t ConnectionManager::activeConnectionCount() const {
    uint32_t count = 0;
    for (std::map<std::string, ServiceConnection*>::const_iterator it = connections_.begin();
         it != connections_.end(); ++it) {
        if (it->second->connected) {
            ++count;
        }
    }
    return count;
}

uint32_t ConnectionManager::tcpConnectionCount() const {
    uint32_t count = 0;
    for (std::map<std::string, ServiceConnection*>::const_iterator it = connections_.begin();
         it != connections_.end(); ++it) {
        if (it->second->connected && it->second->transport &&
            it->second->transport->type() == TransportType::TCP) {
            ++count;
        }
    }
    return count;
}

uint32_t ConnectionManager::shmConnectionCount() const {
    uint32_t count = 0;
    for (std::map<std::string, ServiceConnection*>::const_iterator it = connections_.begin();
         it != connections_.end(); ++it) {
        if (it->second->connected && it->second->transport &&
            it->second->transport->type() == TransportType::SHM) {
            ++count;
        }
    }
    return count;
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

    // SHM 连接: eventfd 触发，先消费 eventfd 再读取数据
    if (conn->transport->type() == TransportType::SHM) {
        platform::eventFdConsume(conn->transport->fd());

        // 循环读取所有可用数据（可能有多条消息）
        while (true) {
            ShmTransport* shm = static_cast<ShmTransport*>(conn->transport);
            size_t frame_size = 0;
            int ready = shm->nextRecvSize(frame_size);
            if (ready <= 0) {
                if (ready < 0) {
                    OMNI_LOG_WARN(LOG_TAG, "SHM connection to %s has invalid frame metadata",
                                  service_name.c_str());
                    conn->connected = false;
                    loop_.removeFd(conn->transport->fd());
                    if (disconnect_cb_) disconnect_cb_(service_name);
                    std::map<std::string, ServiceConnection*>::iterator current =
                        connections_.find(service_name);
                    if (current == connections_.end() || current->second != conn) return;
                }
                break;
            }
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[frame_size]);
            if (!buf) {
                OMNI_LOG_ERROR(LOG_TAG, "SHM receive allocation failed for %s (%zu bytes)",
                               service_name.c_str(), frame_size);
                conn->connected = false;
                loop_.removeFd(conn->transport->fd());
                if (disconnect_cb_) disconnect_cb_(service_name);
                std::map<std::string, ServiceConnection*>::iterator current =
                    connections_.find(service_name);
                if (current == connections_.end() || current->second != conn) return;
                break;
            }
            int ret = conn->transport->recv(buf.get(), frame_size);
            if (ret <= 0) {
                if (ret < 0) {
                    OMNI_LOG_WARN(LOG_TAG, "SHM connection to %s error",
                                     service_name.c_str());
                    conn->connected = false;
                    loop_.removeFd(conn->transport->fd());
                    if (disconnect_cb_) {
                        disconnect_cb_(service_name);
                    }
                    std::map<std::string, ServiceConnection*>::iterator current = connections_.find(service_name);
                    if (current == connections_.end() || current->second != conn) {
                        return;
                    }
                }
                break;
            }

            static const size_t MAX_RECV_BUFFER = MAX_MESSAGE_SIZE;
            if (conn->recv_buffer.size() + static_cast<size_t>(ret) > MAX_RECV_BUFFER) {
                OMNI_LOG_ERROR(LOG_TAG, "SHM recv_buffer overflow for %s (>%zuMB)",
                                 service_name.c_str(), MAX_RECV_BUFFER / (1024*1024));
                conn->connected = false;
                loop_.removeFd(conn->transport->fd());
                if (disconnect_cb_) {
                    disconnect_cb_(service_name);
                }
                std::map<std::string, ServiceConnection*>::iterator current = connections_.find(service_name);
                if (current == connections_.end() || current->second != conn) {
                    return;
                }
                break;
            }
            if (!conn->recv_buffer.writeRaw(buf.get(), static_cast<size_t>(ret))) {
                OMNI_LOG_ERROR(LOG_TAG, "SHM recv_buffer allocation failed for %s",
                               service_name.c_str());
                conn->connected = false;
                loop_.removeFd(conn->transport->fd());
                if (disconnect_cb_) disconnect_cb_(service_name);
                std::map<std::string, ServiceConnection*>::iterator current =
                    connections_.find(service_name);
                if (current == connections_.end() || current->second != conn) return;
                break;
            }
        }

        processMessages(conn);
        return;
    }

    // TCP 连接: 原有逻辑
    uint8_t buf[4096];
    int ret = conn->transport->recv(buf, sizeof(buf));

    if (ret < 0) {
        // 连接断开
        OMNI_LOG_WARN(LOG_TAG,
                      "data_connection_lost service=%s transport=TCP err=%d",
                      service_name.c_str(), static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED));
        conn->connected = false;
        loop_.removeFd(conn->transport->fd());
        if (disconnect_cb_) {
            disconnect_cb_(service_name);
        }
        std::map<std::string, ServiceConnection*>::iterator current = connections_.find(service_name);
        if (current == connections_.end() || current->second != conn) {
            return;
        }
        return;
    }

    if (ret == 0) {
        // EAGAIN，无数据
        return;
    }

    // 追加到接收缓冲区 (with size limit to prevent OOM - fix #17)
    static const size_t MAX_RECV_BUFFER = MAX_MESSAGE_SIZE;
    if (conn->recv_buffer.size() + static_cast<size_t>(ret) > MAX_RECV_BUFFER) {
        OMNI_LOG_ERROR(LOG_TAG, "recv_buffer overflow for %s (>%zuMB), disconnecting",
                         service_name.c_str(), MAX_RECV_BUFFER / (1024*1024));
        conn->connected = false;
        loop_.removeFd(conn->transport->fd());
        if (disconnect_cb_) {
            disconnect_cb_(service_name);
        }
        std::map<std::string, ServiceConnection*>::iterator current = connections_.find(service_name);
        if (current == connections_.end() || current->second != conn) {
            return;
        }
        return;
    }
    conn->recv_buffer.writeRaw(buf, static_cast<size_t>(ret));

    // 处理完整消息
    processMessages(conn);
}

void ConnectionManager::processMessages(ServiceConnection* conn) {
    const std::string service_name = conn->service_name;
    while (true) {
        // 检查是否有完整的消息头
        if (conn->recv_buffer.size() - conn->recv_buffer.readPosition() < MESSAGE_HEADER_SIZE) {
            break;
        }

        // 解析消息头
        size_t saved_pos = conn->recv_buffer.readPosition();
        MessageHeader header;
        if (!Message::parseHeader(
                conn->recv_buffer.data() + saved_pos,
                conn->recv_buffer.size() - saved_pos,
                header)) {
            break;
        }

        // 验证消息头
        if (!Message::validateHeader(header)) {
            OMNI_LOG_ERROR(LOG_TAG, "Invalid message header from %s",
                             conn->service_name.c_str());
            conn->connected = false;
            return;
        }

        // 检查是否有完整的载荷
        size_t total_size = MESSAGE_HEADER_SIZE + header.length;
        if (conn->recv_buffer.size() - saved_pos < total_size) {
            break;
        }

        // 构造完整消息
        Message msg;
        msg.header = header;
        if (header.length > 0) {
            msg.payload.assign(
                conn->recv_buffer.data() + saved_pos + MESSAGE_HEADER_SIZE,
                header.length);
        }

        // 移动读取位置
        if (!conn->recv_buffer.trySetReadPosition(saved_pos + total_size)) {
            conn->connected = false;
            return;
        }

        // 回调处理消息
        if (message_cb_) {
            message_cb_(conn->service_name, msg);
            std::map<std::string, ServiceConnection*>::iterator it = connections_.find(service_name);
            if (it == connections_.end() || it->second != conn) {
                return;
            }
        }
    }

    // 压缩缓冲区（移除已处理的数据）
    size_t remaining = conn->recv_buffer.size() - conn->recv_buffer.readPosition();
    if (remaining > 0 && conn->recv_buffer.readPosition() > 0) {
        memmove(conn->recv_buffer.mutableData(),
                conn->recv_buffer.data() + conn->recv_buffer.readPosition(),
                remaining);
    }
    conn->recv_buffer.setWritePosition(remaining);
    if (!conn->recv_buffer.trySetReadPosition(0)) {
        conn->connected = false;
    }
}

} // namespace omnibinder
