#include "core/service_host_runtime.h"

#include "core/omni_runtime.h"

#include <string.h>

namespace omnibinder {

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

    loop->addFd(cfd, EventLoop::EVENT_READ,
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
        recv_buf->setReadPosition(pos + total);

        if (msg.getType() == MessageType::MSG_INVOKE) {
            on_invoke(service_name, client_fd, msg);
        } else if (msg.getType() == MessageType::MSG_INVOKE_ONEWAY) {
            on_invoke_oneway(service_name, msg);
        } else if (msg.getType() == MessageType::MSG_SUBSCRIBE_BROADCAST) {
            on_subscribe_broadcast(client_fd, msg);
        } else if (msg.getType() == MessageType::MSG_BROADCAST) {
            Buffer buf2(msg.payload.data(), msg.payload.size());
            uint32_t topic_id = buf2.readUint32();
            uint32_t data_len = buf2.readUint32();
            Buffer data;
            if (data_len > 0 && buf2.remaining() >= data_len) {
                data.writeRaw(buf2.data() + buf2.readPosition(), data_len);
            }
            topic_runtime.dispatch(topic_id, data);
        }
    }

    size_t remaining = recv_buf->size() - recv_buf->readPosition();
    if (remaining > 0 && recv_buf->readPosition() > 0) {
        memmove(recv_buf->mutableData(), recv_buf->data() + recv_buf->readPosition(), remaining);
    }
    recv_buf->setWritePosition(remaining);
    recv_buf->setReadPosition(0);
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
    if (hdr.length > 0 && length >= MESSAGE_HEADER_SIZE + hdr.length) {
        msg.payload.assign(data + MESSAGE_HEADER_SIZE, hdr.length);
    }

    MessageType type = msg.getType();
    if (type == MessageType::MSG_INVOKE) {
        on_invoke(service_name, client_id, msg);
    } else if (type == MessageType::MSG_INVOKE_ONEWAY) {
        on_invoke_oneway(service_name, msg);
    } else if (type == MessageType::MSG_SUBSCRIBE_BROADCAST) {
        on_subscribe_broadcast(service_name, client_id, msg);
    } else if (type == MessageType::MSG_BROADCAST) {
        Buffer buf2(msg.payload.data(), msg.payload.size());
        uint32_t topic_id = buf2.readUint32();
        uint32_t data_len = buf2.readUint32();
        Buffer payload;
        if (data_len > 0 && buf2.remaining() >= data_len) {
            payload.writeRaw(buf2.data() + buf2.readPosition(), data_len);
        }
        topic_runtime.dispatch(topic_id, payload);
    }
}

} // namespace omnibinder
