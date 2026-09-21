/**************************************************************************************************
 * @file        platform_common.cpp
 * @brief       平台抽象层的跨平台公共实现
 * @details     收纳不依赖具体操作系统头文件、只调用 platform.h 已声明原语的纯逻辑，
 *              避免 platform_linux.cpp / platform_win.cpp 逐行重复。
 *              Linux 与 Windows 各自只保留系统原语实现。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-09-23
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
#include "platform/platform.h"

namespace omnibinder {
namespace platform {

bool socketSendAll(SocketFd fd, const void* data, size_t length,
                   uint32_t timeout_ms, uint32_t* elapsed_ms) {
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    int64_t start_ms = currentTimeMs();
    int64_t deadline_ms = start_ms + static_cast<int64_t>(timeout_ms);
    size_t sent = 0;
    while (sent < length) {
        int ret = socketSend(fd, bytes + sent, length - sent);
        if (ret < 0) {
            int err = getSocketError();
            if (isWouldBlock(err)) {
                int64_t now_ms = currentTimeMs();
                if (now_ms >= deadline_ms) {
                    break;
                }

                uint32_t remaining_ms = static_cast<uint32_t>(deadline_ms - now_ms);
                if (!waitSocketWritable(fd, remaining_ms)) {
                    break;
                }
                continue;
            }

            if (elapsed_ms) {
                int64_t spent_ms = currentTimeMs() - start_ms;
                *elapsed_ms = spent_ms > 0 ? static_cast<uint32_t>(spent_ms) : 0;
            }
            return false;
        }

        if (ret > 0) {
            sent += static_cast<size_t>(ret);
            continue;
        }

        int64_t now_ms = currentTimeMs();
        if (now_ms >= deadline_ms) {
            break;
        }

        uint32_t remaining_ms = static_cast<uint32_t>(deadline_ms - now_ms);
        if (!waitSocketWritable(fd, remaining_ms)) {
            break;
        }
    }

    if (elapsed_ms) {
        int64_t spent_ms = currentTimeMs() - start_ms;
        *elapsed_ms = spent_ms > 0 ? static_cast<uint32_t>(spent_ms) : 0;
    }

    return sent == length;
}

} // namespace platform
} // namespace omnibinder
