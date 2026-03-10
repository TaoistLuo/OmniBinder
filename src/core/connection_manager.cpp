#include "core/connection_manager.h"
#include "core/event_loop.h"
#include "transport/transport_factory.h"
#include "transport/tcp_transport.h"
#include "transport/shm_transport.h"
#include "platform/platform.h"
#include "omnibinder/error.h"
#include "omnibinder/log.h"
#include <cstring>

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
    const std::string& shm_name,
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

    // 明确 transport policy：同机优先 SHM，缺少 shm_name 或连接失败则降级 TCP
    bool use_shm = false;
    TransportType preferred = TransportFactory::selectPreferredTransport(local_host_id_, host_id, shm_name);
    if (preferred == TransportType::SHM && !shm_name.empty()) {
        size_t req_ring_capacity = shm_config.req_ring_capacity > 0
            ? shm_config.req_ring_capacity
            : SHM_DEFAULT_REQ_RING_CAPACITY;
        size_t resp_ring_capacity = shm_config.resp_ring_capacity > 0
            ? shm_config.resp_ring_capacity
            : SHM_DEFAULT_RESP_RING_CAPACITY;
        ShmTransport* shm = new ShmTransport(shm_name, false,
                                             req_ring_capacity, resp_ring_capacity);
        int ret = shm->connect("", 0);
        if (ret == 0) {
            conn->transport = shm;
            use_shm = true;
            OMNI_LOG_INFO(LOG_TAG, "Connected to %s via SHM '%s' (same machine)",
                            service_name.c_str(), shm_name.c_str());
        } else {
            OMNI_LOG_WARN(LOG_TAG,
                          "data_connect_fallback service=%s transport=SHM shm=%s fallback=TCP reason=connect_failed",
                          service_name.c_str(), shm_name.c_str());
            delete shm;
        }
    } else if (preferred == TransportType::SHM) {
        OMNI_LOG_INFO(LOG_TAG,
                        "Same-machine service %s has no shm_name, using TCP fallback",
                        service_name.c_str());
    }

    // 跨机或 SHM 失败：用 TCP
    if (!use_shm) {
        conn->transport = new TcpTransport();
        int ret = conn->transport->connect(host, port);
        if (ret < 0) {
            OMNI_LOG_ERROR(LOG_TAG,
                           "data_connect_failed service=%s transport=TCP host=%s port=%u err=%d",
                           service_name.c_str(), host.c_str(), port,
                           static_cast<int>(ErrorCode::ERR_CONNECT_FAILED));
            delete conn;
            return NULL;
        }

        if (ret == 1) {
            TcpTransport* tcp = static_cast<TcpTransport*>(conn->transport);
            for (int i = 0; i < 100; ++i) {
                if (tcp->checkConnectComplete()) {
                    break;
                }
                platform::sleepMs(10);
            }
            if (tcp->state() != ConnectionState::CONNECTED) {
                OMNI_LOG_ERROR(LOG_TAG,
                               "data_connect_timeout service=%s transport=TCP host=%s port=%u err=%d",
                               service_name.c_str(), host.c_str(), port,
                               static_cast<int>(ErrorCode::ERR_TIMEOUT));
                delete conn;
                return NULL;
            }
        }

        OMNI_LOG_INFO(LOG_TAG, "Connected to %s via TCP at %s:%u (fd=%d)",
                        service_name.c_str(), host.c_str(), port, conn->transport->fd());
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

bool ConnectionManager::sendRaw(ServiceConnection* conn, const uint8_t* data, size_t length) {
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
            // EAGAIN - 应该等待可写事件，这里简化处理
            break;
        }
        sent += static_cast<size_t>(ret);
    }
    return sent == length;
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
        uint8_t buf[65536];
        while (true) {
            int ret = conn->transport->recv(buf, sizeof(buf));
            if (ret <= 0) {
                if (ret < 0) {
                    OMNI_LOG_WARN(LOG_TAG, "SHM connection to %s error",
                                     service_name.c_str());
                    conn->connected = false;
                    loop_.removeFd(conn->transport->fd());
                    if (disconnect_cb_) {
                        disconnect_cb_(service_name);
                    }
                }
                break;
            }

            static const size_t MAX_RECV_BUFFER = 16 * 1024 * 1024;
            if (conn->recv_buffer.size() + static_cast<size_t>(ret) > MAX_RECV_BUFFER) {
                OMNI_LOG_ERROR(LOG_TAG, "SHM recv_buffer overflow for %s (>%zuMB)",
                                 service_name.c_str(), MAX_RECV_BUFFER / (1024*1024));
                conn->connected = false;
                loop_.removeFd(conn->transport->fd());
                if (disconnect_cb_) {
                    disconnect_cb_(service_name);
                }
                break;
            }
            conn->recv_buffer.writeRaw(buf, static_cast<size_t>(ret));
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
        return;
    }

    if (ret == 0) {
        // EAGAIN，无数据
        return;
    }

    // 追加到接收缓冲区 (with size limit to prevent OOM - fix #17)
    static const size_t MAX_RECV_BUFFER = 16 * 1024 * 1024; // 16MB
    if (conn->recv_buffer.size() + static_cast<size_t>(ret) > MAX_RECV_BUFFER) {
        OMNI_LOG_ERROR(LOG_TAG, "recv_buffer overflow for %s (>%zuMB), disconnecting",
                         service_name.c_str(), MAX_RECV_BUFFER / (1024*1024));
        conn->connected = false;
        loop_.removeFd(conn->transport->fd());
        if (disconnect_cb_) {
            disconnect_cb_(service_name);
        }
        return;
    }
    conn->recv_buffer.writeRaw(buf, static_cast<size_t>(ret));

    // 处理完整消息
    processMessages(conn);
}

void ConnectionManager::processMessages(ServiceConnection* conn) {
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
        conn->recv_buffer.setReadPosition(saved_pos + total_size);

        // 回调处理消息
        if (message_cb_) {
            message_cb_(conn->service_name, msg);
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
    conn->recv_buffer.setReadPosition(0);
}

} // namespace omnibinder
