/**************************************************************************************************
 * @file        sm_control_channel.h
 * @brief       ServiceManager 控制通道
 * @details     封装与 ServiceManager 之间的 TCP 控制面通信。负责消息收发缓冲、
 *              连接状态管理、异步请求-响应匹配。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
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
    bool sendMessageWithinTimeout(const Message& msg, uint32_t timeout_ms, uint32_t* elapsed_ms = NULL);
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
