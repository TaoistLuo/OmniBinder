/**************************************************************************************************
 * @file        message_reader.h
 * @brief       统一消息读取器
 * @details     从 IMessageConnection 读出下一条完整协议消息，屏蔽成帧传输（SHM）与字节流
 *              传输（TCP）的差异。core 的所有入站读路径（服务端请求、数据面直连、
 *              SM 控制通道）共用本读取器，不再各自手写组帧循环。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-09-12
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
#ifndef OMNIBINDER_CORE_MESSAGE_READER_H
#define OMNIBINDER_CORE_MESSAGE_READER_H

#include "omnibinder/transport.h"
#include "omnibinder/buffer.h"
#include "omnibinder/message.h"

namespace omnibinder {

/*
 * @brief  读取下一条完整 Message
 * @param[in]     transport   客户端传输（成帧或流式）
 * @param[in,out] recv_buffer 流式传输的接收缓冲（成帧传输不使用）
 * @param[out]    out         解析出的消息（返回 1 时有效）
 * @return 1 成功；0 数据不足（需更多事件）；-1 流损坏/传输错误
 * @note   内部消费传输就绪通知；成帧传输直接按帧解析，不经过 recv_buffer；
 *         返回 -1 时调用方决定重同步或断开（本函数不改动连接状态之外的缓冲）
 */
int readNextMessage(IMessageConnection& transport, Buffer& recv_buffer, Message& out);

} // namespace omnibinder

#endif // OMNIBINDER_CORE_MESSAGE_READER_H
