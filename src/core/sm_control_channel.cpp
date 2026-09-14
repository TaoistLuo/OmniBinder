#include "core/sm_control_channel.h"
#include "core/message_reader.h"
#include "core/event_loop.h"

namespace omnibinder {

SmControlChannel::SmControlChannel()
    : transport_(NULL)
    , recv_buffer_() {}

SmControlChannel::~SmControlChannel() {
    clearReplies();
}

bool SmControlChannel::isConnected() const {
    return transport_ && transport_->state() == ConnectionState::CONNECTED;
}

IMessageConnection* SmControlChannel::transport() const {
    return transport_;
}

void SmControlChannel::resetTransport(IMessageConnection* t) {
    transport_ = t;
}

void SmControlChannel::closeTransport(EventLoop& loop) {
    if (!transport_) {
        return;
    }
    // 对象销毁前先从 event-loop 摘除其 fd，避免 fd 号复用后表内残留导致
    // 新注册静默失败（约束 3）
    if (transport_->fd() >= 0) {
        loop.removeFd(transport_->fd());
    }
    transport_->close();
    delete transport_;
    transport_ = NULL;
}

void SmControlChannel::clearReceiveBuffer() {
    recv_buffer_.clear();
}

bool SmControlChannel::sendMessage(const Message& msg) {
    return sendMessageWithinTimeout(msg, DEFAULT_INVOKE_TIMEOUT, NULL);
}

bool SmControlChannel::sendMessageWithinTimeout(const Message& msg, uint32_t timeout_ms, uint32_t* elapsed_ms) {
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }
    if (!isConnected()) {
        return false;
    }

    Buffer buf;
    if (!msg.serialize(buf)) {
        return false;
    }
    return transport_->sendAll(buf.data(), buf.size(), timeout_ms, elapsed_ms) == 0;
}

int SmControlChannel::recvMessage(Message& msg) {
    if (!transport_) {
        return -1;
    }
    return readNextMessage(*transport_, recv_buffer_, msg);
}

void SmControlChannel::clearReplies() {
    // 连接重建后，旧连接上尚未返回的回复不可能再到达，清理控制面等待槽。
    // 数据面槽表独立于本通道（RpcRuntime 持有），SM 重连不得误伤在途数据面 RPC。
    pending_replies_.clear();
}

} // namespace omnibinder
