#include "core/service_host_runtime.h"

#include "core/omni_runtime.h"

#include <string.h>

#include "omnibinder/log.h"

#define LOG_TAG "ServiceHostRuntime"

namespace omnibinder {

namespace {

bool decodeBroadcastPayload(const Message& msg, uint32_t& topic_id, Buffer& payload) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t data_len = 0;
    if (!buf.tryReadUint32(topic_id) || !buf.tryReadUint32(data_len)) {
        return false;
    }
    if (buf.remaining() < data_len) {
        return false;
    }

    payload.clear();
    if (data_len > 0) {
        payload.writeRaw(buf.data() + buf.readPosition(), data_len);
    }
    return true;
}

}

void ServiceHostRuntime::onServiceAccept(const std::string& service_name,
                                         LocalServiceEntry* entry,
                                         EventLoop* loop,
                                         std::map<int, std::string>& client_fd_to_service,
                                         const ClientDataHandler& on_client_data,
                                         const ClientConnectedHook& on_client_connected) const {
    if (!entry || !entry->server || !loop) {
        return;
    }

    ITransport* client = entry->server->accept();
    if (!client) {
        return;
    }

    int cfd = client->fd();
    entry->client_transports[cfd] = client;
    entry->client_recv_buffers[cfd] = new Buffer();
    client_fd_to_service[cfd] = service_name;

    loop->addFd(cfd, EventLoop::EVENT_READ | EventLoop::EVENT_ERROR,
        [on_client_data, service_name, cfd](int fd, uint32_t ev) {
            (void)fd;
            on_client_data(service_name, cfd, ev);
        });

    on_client_connected(entry, cfd);
}

void ServiceHostRuntime::onServiceClientData(const std::string& service_name,
                                             LocalServiceEntry* entry,
                                             int client_fd,
                                             TopicRuntime& topic_runtime,
                                             EventLoop* loop,
                                             std::map<int, std::string>& client_fd_to_service,
                                             const InvokeHandler& on_invoke,
                                             const InvokeOneWayHandler& on_invoke_oneway,
                                             const SubscribeBroadcastHandler& on_subscribe_broadcast,
                                             const DisconnectHandler& on_disconnect,
                                             const ClientDisconnectedHook& on_client_disconnected) const {
    (void)loop;
    if (!entry) {
        return;
    }

    std::map<int, ITransport*>::iterator tit = entry->client_transports.find(client_fd);
    if (tit == entry->client_transports.end()) {
        return;
    }

    uint8_t buf[4096];
    int ret = tit->second->recv(buf, sizeof(buf));
    if (ret < 0) {
        client_fd_to_service.erase(client_fd);
        topic_runtime.removeTcpSubscriberFd(client_fd);
        on_client_disconnected(entry, client_fd);
        on_disconnect(service_name, client_fd);
        return;
    }
    if (ret == 0) {
        return;
    }

    std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_fd);
    if (bit == entry->client_recv_buffers.end()) {
        return;
    }
    static const size_t MAX_CLIENT_RECV_BUFFER = 16 * 1024 * 1024; // 16MB
    if (bit->second->size() + static_cast<size_t>(ret) > MAX_CLIENT_RECV_BUFFER) {
        OMNI_LOG_ERROR(LOG_TAG, "client_recv_buffer overflow for fd=%d (>%zuMB)",
                       client_fd, MAX_CLIENT_RECV_BUFFER / (1024*1024));
        topic_runtime.removeTcpSubscriberFd(client_fd);
        on_client_disconnected(entry, client_fd);
        on_disconnect(service_name, client_fd);
        return;
    }
    bit->second->writeRaw(buf, static_cast<size_t>(ret));

    processServiceClientMessages(service_name, entry, client_fd, topic_runtime,
                                 on_invoke, on_invoke_oneway, on_subscribe_broadcast);
}

void ServiceHostRuntime::processServiceClientMessages(const std::string& service_name,
                                                      LocalServiceEntry* entry,
                                                      int client_fd,
                                                      TopicRuntime& topic_runtime,
                                                      const InvokeHandler& on_invoke,
                                                      const InvokeOneWayHandler& on_invoke_oneway,
                                                      const SubscribeBroadcastHandler& on_subscribe_broadcast) const {
    std::map<int, Buffer*>::iterator bit = entry->client_recv_buffers.find(client_fd);
    if (bit == entry->client_recv_buffers.end()) {
        return;
    }

    Buffer* recv_buf = bit->second;
    while (true) {
        size_t avail = recv_buf->size() - recv_buf->readPosition();
        if (avail < MESSAGE_HEADER_SIZE) {
            break;
        }

        size_t pos = recv_buf->readPosition();
        MessageHeader hdr;
        if (!Message::parseHeader(recv_buf->data() + pos, avail, hdr)) {
            break;
        }
        if (!Message::validateHeader(hdr)) {
            // Corrupted header — skip one byte forward to resume on next poll,
            // same recovery strategy as SmControlChannel::tryPopMessage.
            recv_buf->trySetReadPosition(pos + 1);
            size_t remaining = recv_buf->size() - recv_buf->readPosition();
            if (remaining > 0 && recv_buf->readPosition() > 0) {
                memmove(recv_buf->mutableData(),
                        recv_buf->data() + recv_buf->readPosition(), remaining);
            }
            recv_buf->setWritePosition(remaining);
            recv_buf->trySetReadPosition(0);
            return;
        }

        size_t total = MESSAGE_HEADER_SIZE + hdr.length;
        if (avail < total) {
            break;
        }

        Message msg;
        msg.header = hdr;
        if (hdr.length > 0) {
            msg.payload.assign(recv_buf->data() + pos + MESSAGE_HEADER_SIZE, hdr.length);
        }
        if (!recv_buf->trySetReadPosition(pos + total)) {
            return;
        }

        if (msg.getType() == MessageType::MSG_INVOKE) {
            on_invoke(service_name, client_fd, msg);
        } else if (msg.getType() == MessageType::MSG_INVOKE_ONEWAY) {
            on_invoke_oneway(service_name, msg);
        } else if (msg.getType() == MessageType::MSG_HEARTBEAT) {
            Message ack(MessageType::MSG_HEARTBEAT_ACK, msg.getSequence());
            Buffer buf;
            if (ack.serialize(buf)) {
                ITransport* transport = nullptr;
                std::map<int, ITransport*>::iterator tit = entry->client_transports.find(client_fd);
                if (tit != entry->client_transports.end()) {
                    transport = tit->second;
                }
                if (transport) {
                    transport->send(buf.data(), buf.size());
                }
            }
        } else if (msg.getType() == MessageType::MSG_SUBSCRIBE_BROADCAST) {
            on_subscribe_broadcast(client_fd, msg);
        } else if (msg.getType() == MessageType::MSG_BROADCAST) {
            uint32_t topic_id = 0;
            Buffer data;
            if (!decodeBroadcastPayload(msg, topic_id, data)) {
                continue;
            }
            topic_runtime.dispatch(topic_id, data);
        }
    }

    size_t remaining = recv_buf->size() - recv_buf->readPosition();
    if (remaining > 0 && recv_buf->readPosition() > 0) {
        memmove(recv_buf->mutableData(), recv_buf->data() + recv_buf->readPosition(), remaining);
    }
    recv_buf->setWritePosition(remaining);
    if (!recv_buf->trySetReadPosition(0)) {
        return;
    }
}

void ServiceHostRuntime::onShmRequest(const std::string& service_name,
                                      uint32_t client_id,
                                      const uint8_t* data,
                                      size_t length,
                                      TopicRuntime& topic_runtime,
                                      const ShmInvokeHandler& on_invoke,
                                      const ShmInvokeOneWayHandler& on_invoke_oneway,
                                      const ShmSubscribeBroadcastHandler& on_subscribe_broadcast) const {
    if (length < MESSAGE_HEADER_SIZE) {
        return;
    }

    MessageHeader hdr;
    if (!Message::parseHeader(data, length, hdr) || !Message::validateHeader(hdr)) {
        return;
    }

    Message msg;
    msg.header = hdr;
    if (length < MESSAGE_HEADER_SIZE + hdr.length) {
        OMNI_LOG_WARN(LOG_TAG, "incomplete SHM message for service=%s: need=%zu have=%zu",
                      service_name.c_str(), MESSAGE_HEADER_SIZE + static_cast<size_t>(hdr.length), length);
        return;
    }
    if (hdr.length > 0) {
        msg.payload.assign(data + MESSAGE_HEADER_SIZE, hdr.length);
    }

    MessageType type = msg.getType();
    if (type == MessageType::MSG_INVOKE) {
        on_invoke(service_name, client_id, msg);
    } else if (type == MessageType::MSG_INVOKE_ONEWAY) {
        on_invoke_oneway(service_name, msg);
    } else if (type == MessageType::MSG_HEARTBEAT) {
    } else if (type == MessageType::MSG_SUBSCRIBE_BROADCAST) {
        on_subscribe_broadcast(service_name, client_id, msg);
    } else if (type == MessageType::MSG_BROADCAST) {
        uint32_t topic_id = 0;
        Buffer payload;
        if (!decodeBroadcastPayload(msg, topic_id, payload)) {
            return;
        }
        topic_runtime.dispatch(topic_id, payload);
    }
}

} // namespace omnibinder
