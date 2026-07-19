/**************************************************************************************************
 * @file        rpc_runtime.h
 * @brief       RPC 运行时
 * @details     管理客户端侧 RPC 调用状态。包括序列号分配、pending reply 存储
 *              与超时检测。与 SmControlChannel 协同完成同步 RPC 调用。
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
#ifndef OMNIBINDER_CORE_RPC_RUNTIME_H
#define OMNIBINDER_CORE_RPC_RUNTIME_H

#include "core/sm_control_channel.h"
#include <functional>
#include <stdint.h>

namespace omnibinder {

class RpcRuntime {
public:
    RpcRuntime();

    uint32_t nextSequence();
    uint32_t effectiveTimeout(uint32_t timeout_ms) const;
    void setDefaultTimeout(uint32_t timeout_ms);
    bool beginWait(uint32_t timeout_ms);
    void endWait();
    int64_t remainingWaitMs() const;
    bool isTimedOut() const;
    int waitForReply(uint32_t seq, uint32_t timeout_ms,
                     SmControlChannel& channel,
                     const std::function<void(int)>& poll_once,
                     Message& reply,
                     const std::function<bool()>& is_alive = std::function<bool()>());

private:
    uint32_t default_timeout_ms_;
    uint32_t sequence_counter_;
    bool in_wait_for_reply_;
    int64_t wait_deadline_ms_;
};

} // namespace omnibinder

#endif
