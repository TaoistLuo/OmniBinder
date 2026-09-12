/**************************************************************************************************
 * @file        rpc_runtime.h
 * @brief       RPC 运行时
 * @details     管理客户端侧 RPC 调用状态，包括序列号分配、等待状态与超时检测。
 *              控制面等待槽由 SmControlChannel 持有；数据面等待槽由本类独立持有，
 *              SM 重连只清理控制面槽，在途数据面 RPC 不受影响。序列号域两平面共享，
 *              但回复只允许进入各自平面的槽表（协议序列号空间隔离）。
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

#include "core/pending_reply_table.h"
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
    int64_t remainingWaitMs() const;
    bool isTimedOut() const;

    /*
     * @brief  等待请求回复
     * @param[in]  seq        请求序列号
     * @param[in]  timeout_ms 超时时间（ms）
     * @param[in]  table      该请求所属平面的等待槽表（控制面/数据面）
     * @param[in]  poll_once  等待期间的事件轮询回调
     * @param[out] reply      返回的回复
     * @param[in]  is_alive   连接存活判定回调，可为空
     * @return 0 成功；ERR_TIMEOUT 超时；ERR_CONNECTION_CLOSED 连接失效/槽被标记失败
     */
    int waitForReply(uint32_t seq, uint32_t timeout_ms,
                     PendingReplyTable& table,
                     const std::function<void(int)>& poll_once,
                     Message& reply,
                     const std::function<bool()>& is_alive = std::function<bool()>());

    /*
     * @brief  数据面 pending reply 槽表（独立于 SmControlChannel）
     */
    PendingReplyTable& dataReplies() { return data_replies_; }

private:
    uint32_t default_timeout_ms_;
    uint32_t sequence_counter_;
    bool in_wait_for_reply_;
    int64_t wait_deadline_ms_;
    PendingReplyTable data_replies_;
};

} // namespace omnibinder

#endif
