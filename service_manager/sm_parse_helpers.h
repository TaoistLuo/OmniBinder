/**************************************************************************************************
 * @file        sm_parse_helpers.h
 * @brief       SM 消息解析辅助
 * @details     ServiceManager 通用的消息参数解析内联辅助函数。提供 tryReadStringArg、tryReadExactStringArg、tryReadExactTopicArg 等工具函数。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-10-15
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
#ifndef OMNIBINDER_SM_PARSE_HELPERS_H
#define OMNIBINDER_SM_PARSE_HELPERS_H

#include "omnibinder/message.h"
#include "omnibinder/types.h"

#include <string>

namespace omnibinder {
namespace sm_internal {

inline bool tryReadStringArg(const Message& msg, std::string& value) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    return payload.tryReadString(value);
}

inline bool tryReadExactStringArg(const Message& msg, std::string& value,
                                  size_t max_length) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    return payload.tryReadString(value)
        && !value.empty()
        && value.size() <= max_length
        && payload.remaining() == 0;
}

inline bool tryReadExactTopicArg(const Message& msg, std::string& topic) {
    return tryReadExactStringArg(msg, topic, MAX_TOPIC_NAME_LENGTH);
}

} // namespace sm_internal
} // namespace omnibinder

#endif
