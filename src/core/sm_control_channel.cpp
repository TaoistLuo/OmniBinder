#include "core/sm_control_channel.h"
#include "platform/platform.h"
#include <chrono>
#include <string.h>

namespace omnibinder {

SmControlChannel::SmControlChannel()
    : transport(NULL)
    , recv_buffer() {}

SmControlChannel::~SmControlChannel() {
    clearReplies();
}

bool SmControlChannel::isConnected() const {
    return transport && transport->state() == ConnectionState::CONNECTED;
}

bool SmControlChannel::sendMessage(const Message& msg) {
    if (!isConnected()) {
        return false;
    }

    Buffer buf;
    msg.serialize(buf);
    size_t sent = 0;
    while (sent < buf.size()) {
        int ret = transport->send(buf.data() + sent, buf.size() - sent);
        if (ret <= 0) {
            return false;
        }
        sent += static_cast<size_t>(ret);
    }
    return true;
}

int SmControlChannel::recvSome(uint8_t* data, size_t capacity) {
    if (!transport) {
        return -1;
    }
    return transport->recv(data, capacity);
}

void SmControlChannel::appendReceived(const uint8_t* data, size_t length) {
    recv_buffer.writeRaw(data, length);
}

bool SmControlChannel::tryPopMessage(Message& msg) {
    size_t avail = recv_buffer.size() - recv_buffer.readPosition();
    if (avail < MESSAGE_HEADER_SIZE) {
        return false;
    }

    size_t pos = recv_buffer.readPosition();
    MessageHeader hdr;
    if (!Message::parseHeader(recv_buffer.data() + pos, avail, hdr)) {
        return false;
    }
    if (!Message::validateHeader(hdr)) {
        recv_buffer.setWritePosition(0);
        recv_buffer.setReadPosition(0);
        return false;
    }

    size_t total = MESSAGE_HEADER_SIZE + hdr.length;
    if (avail < total) {
        return false;
    }

    msg.header = hdr;
    msg.payload.clear();
    if (hdr.length > 0) {
        msg.payload.assign(recv_buffer.data() + pos + MESSAGE_HEADER_SIZE, hdr.length);
    }
    recv_buffer.setReadPosition(pos + total);

    size_t remaining = recv_buffer.size() - recv_buffer.readPosition();
    if (remaining > 0 && recv_buffer.readPosition() > 0) {
        memmove(recv_buffer.mutableData(), recv_buffer.data() + recv_buffer.readPosition(), remaining);
    }
    recv_buffer.setWritePosition(remaining);
    recv_buffer.setReadPosition(0);
    return true;
}

void SmControlChannel::clearReplies() {
    pending_replies_.clear();
}

void SmControlChannel::beginWait(uint32_t seq) {
    std::map<uint32_t, PendingReplySlot>::iterator it = pending_replies_.find(seq);
    if (it == pending_replies_.end()) {
        pending_replies_.emplace(std::piecewise_construct,
                                 std::forward_as_tuple(seq),
                                 std::forward_as_tuple());
        return;
    }
    it->second.ready = false;
    it->second.message.payload.clear();
}

bool SmControlChannel::isWaiting(uint32_t seq) const {
    std::map<uint32_t, PendingReplySlot>::const_iterator it = pending_replies_.find(seq);
    return it != pending_replies_.end() && !it->second.ready;
}

const Message* SmControlChannel::pendingReply(uint32_t seq) const {
    std::map<uint32_t, PendingReplySlot>::const_iterator it = pending_replies_.find(seq);
    if (it == pending_replies_.end()) {
        return NULL;
    }
    if (!it->second.ready) {
        return NULL;
    }
    return &it->second.message;
}

bool SmControlChannel::takeReply(uint32_t seq, Message& out) {
    std::map<uint32_t, PendingReplySlot>::iterator it = pending_replies_.find(seq);
    if (it == pending_replies_.end()) {
        return false;
    }
    if (!it->second.ready) {
        return false;
    }
    out.header = it->second.message.header;
    out.payload = std::move(it->second.message.payload);
    pending_replies_.erase(it);
    return true;
}

void SmControlChannel::eraseWait(uint32_t seq) {
    pending_replies_.erase(seq);
}

void SmControlChannel::storeReply(uint32_t seq, const Message& msg) {
    std::map<uint32_t, PendingReplySlot>::iterator it = pending_replies_.find(seq);
    if (it == pending_replies_.end() || it->second.ready) {
        return;
    }

    it->second.message.header = msg.header;
    it->second.message.payload.assign(msg.payload.data(), msg.payload.size());
    it->second.ready = true;
}

} // namespace omnibinder
