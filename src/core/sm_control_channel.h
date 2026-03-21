#ifndef OMNIBINDER_CORE_SM_CONTROL_CHANNEL_H
#define OMNIBINDER_CORE_SM_CONTROL_CHANNEL_H

#include "transport/tcp_transport.h"
#include "omnibinder/message.h"
#include "omnibinder/buffer.h"
#include <map>

namespace omnibinder {

class SmControlChannel {
public:
    struct PendingReplySlot {
        bool ready;
        Message message;

        PendingReplySlot() : ready(false), message() {}
    };

    SmControlChannel();
    ~SmControlChannel();

    bool isConnected() const;
    bool sendMessage(const Message& msg);
    int recvSome(uint8_t* data, size_t capacity);
    void appendReceived(const uint8_t* data, size_t length);
    bool tryPopMessage(Message& msg);
    void clearReplies();
    void beginWait(uint32_t seq);
    bool isWaiting(uint32_t seq) const;
    const Message* pendingReply(uint32_t seq) const;
    bool takeReply(uint32_t seq, Message& out);
    void eraseWait(uint32_t seq);
    void storeReply(uint32_t seq, const Message& msg);

    TcpTransport* transport;
    Buffer recv_buffer;

private:
    std::map<uint32_t, PendingReplySlot> pending_replies_;
};

} // namespace omnibinder

#endif
