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

#include "omnibinder/transport.h"
#include "omnibinder/message.h"
#include "omnibinder/buffer.h"
#include "core/pending_reply_table.h"

namespace omnibinder {

class EventLoop;

class SmControlChannel {
public:
    SmControlChannel();
    ~SmControlChannel();

    bool isConnected() const;
    bool sendMessage(const Message& msg);
    bool sendMessageWithinTimeout(const Message& msg, uint32_t timeout_ms, uint32_t* elapsed_ms = NULL);
    /*
     * @brief  读取下一条完整 SM 消息
     * @param[out] msg 返回的消息
     * @return 1 成功；0 数据不足；-1 流损坏/传输错误
     */
    int recvMessage(Message& msg);

    /*
     * @brief  控制面 pending reply 槽表
     * @return 槽表引用；SM 重连时只清理该表，不影响数据面等待槽
     */
    PendingReplyTable& pendingReplies() { return pending_replies_; }

    /*
     * @brief  连接重建时清理控制面等待槽
     * @note   仅标记/清除控制面槽；数据面槽由 RpcRuntime 独立持有，不受 SM 重连影响
     */
    void clearReplies();

    /*
     * @brief  获取当前控制通道传输对象
     * @return 传输对象指针；未连接时为 NULL
     */
    IMessageConnection* transport() const;

    /*
     * @brief  设置控制通道传输对象（接管所有权，不释放旧对象）
     * @param[in] t 新传输对象，可为 NULL
     */
    void resetTransport(IMessageConnection* t);

    /*
     * @brief  从 event-loop 摘除 fd 后关闭并释放当前传输对象，最后置空
     * @param[in] loop 传输 fd 注册所在的 event-loop
     * @note   必须先 removeFd 再 close/delete（约束 3），否则 fd 号复用后
     *         event-loop 中的残留条目会让新注册静默失败
     */
    void closeTransport(EventLoop& loop);

    /*
     * @brief  清空接收缓冲
     * @note   连接重建时调用，避免旧连接上的半帧字节混入新连接数据流
     */
    void clearReceiveBuffer();

private:
    PendingReplyTable pending_replies_;
    IMessageConnection* transport_;
    Buffer recv_buffer_;
};

} // namespace omnibinder

#endif
