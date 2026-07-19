/**************************************************************************************************
 * @file        runtime_helpers.h
 * @brief       运行时辅助函数
 * @details     OmniRuntime::Impl 拆分后共享的辅助声明。包括 advertise host 规范化、
 *              data channel 种类名称、诊断事件序列化、各类 payload 解码。
 *              仅由 Impl 内部模块引用。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-07-18
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
#ifndef OMNIBINDER_RUNTIME_HELPERS_H
#define OMNIBINDER_RUNTIME_HELPERS_H

#include "omnibinder/message.h"
#include "omnibinder/error.h"
#include "omnibinder/transport.h"

#include <string>

namespace omnibinder {

extern const uint32_t OMNI_DIAG_IFACE_ID;

std::string normalizeAdvertiseHost(const std::string& host);
const char* dataChannelKindName(TransportType type);
void diag_serialize_event(Buffer& buf, uint8_t direction, const Message& msg);
bool decodeInvokePayload(const Message& msg, uint32_t& interface_id,
                         uint32_t& idl_hash, uint32_t& method_id, Buffer& request);
bool decodeSubscribeBroadcastPayload(const Message& msg, uint32_t& topic_id,
                                     std::string& topic_name);
bool decodeBroadcastPayload(const Message& msg, uint32_t& topic_id, Buffer& payload);
bool decodeSingleStringPayload(const Message& msg, std::string& value);
bool decodeBoolReplyPayload(const Message& msg, bool& value);
bool decodeUint32ReplyPayload(const Message& msg, uint32_t& value);
bool decodeInvokeReplyPayload(const Message& msg, int32_t& status, Buffer& response);
Message makeInvokeErrorReply(uint32_t seq, ErrorCode error);
Message makeInvokeSuccessReply(uint32_t seq, const Buffer& response);

} // namespace omnibinder

#endif
