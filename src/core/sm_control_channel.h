#ifndef OMNIBINDER_CORE_SM_CONTROL_CHANNEL_H
#define OMNIBINDER_CORE_SM_CONTROL_CHANNEL_H

#include "transport/tcp_transport.h"
#include "omnibinder/message.h"
#include "omnibinder/buffer.h"
#include <map>

namespace omnibinder {

class SmControlChannel {
public:
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
    Message* pendingReply(uint32_t seq) const;
    void eraseWait(uint32_t seq);
    void storeReply(uint32_t seq, const Message& msg);

    TcpTransport* transport;
    Buffer recv_buffer;

private:
    std::map<uint32_t, Message*> pending_replies_;
};

} // namespace omnibinder

#endif
