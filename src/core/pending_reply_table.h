/**************************************************************************************************
 * @file        pending_reply_table.h
 * @brief       请求-响应等待槽表
 * @details     按序列号索引的 pending reply 槽。控制面（SmControlChannel）与数据面
 *              （RpcRuntime）各自持有独立实例：SM 重连只清理控制面槽，不再误伤正在
 *              直连通道上等待的 RPC（控制面/数据面回复存储隔离）。
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
#ifndef OMNIBINDER_CORE_PENDING_REPLY_TABLE_H
#define OMNIBINDER_CORE_PENDING_REPLY_TABLE_H

#include "omnibinder/message.h"
#include <map>
#include <stdint.h>

namespace omnibinder {

/*
 * @brief  按序列号索引的请求-响应等待槽表
 * @details 每个平面（控制面/数据面）持有独立实例，回复只允许进入本平面的表；
 *          连接失效时由所属平面调用 clear() 标记等待槽 failed，让等待方快速失败。
 */
class PendingReplyTable {
public:
    struct Slot {
        bool ready;
        bool failed;   // 连接重建后旧连接的回复不可能到达，标记失败让等待方快速退出
        Message message;

        Slot() : ready(false), failed(false), message() {}
    };

    PendingReplyTable();

    /*
     * @brief  连接重建时清理槽表
     * @details 已就绪（ready）的槽直接清除；正在等待的槽保留并标记 failed，
     *          让外层 waitForReply 快速失败而非空转至超时
     */
    void clear();

    /*
     * @brief  登记一个待回复槽（不存在则创建）
     * @param[in] seq 请求序列号
     */
    void beginWait(uint32_t seq);

    /*
     * @brief  查询序列号是否处于等待中（未就绪且未失败）
     */
    bool isWaiting(uint32_t seq) const;

    /*
     * @brief  获取已就绪的回复
     * @param[in] seq 请求序列号
     * @return 回复指针；不存在或未就绪时为 NULL
     */
    const Message* pendingReply(uint32_t seq) const;

    /*
     * @brief  查询序列号对应的等待槽是否已被标记失败
     */
    bool isFailed(uint32_t seq) const;

    /*
     * @brief  取出已就绪的回复并从槽表移除
     * @param[in]  seq 请求序列号
     * @param[out] out 返回的回复
     * @return true 成功；false 槽不存在或未就绪
     */
    bool takeReply(uint32_t seq, Message& out);

    /*
     * @brief  移除等待槽（超时/失败路径回收）
     */
    void eraseWait(uint32_t seq);

    /*
     * @brief  存储回复到等待中的槽（移动语义，避免回复载荷多拷贝一次）
     * @param[in] seq 请求序列号
     * @param[in] msg 回复消息；成功后其 payload 被移出
     * @note   只为 beginWait() 创建过的槽存储，避免无人等待的回复无限增长槽表
     */
    void storeReply(uint32_t seq, Message& msg);

private:
    std::map<uint32_t, Slot> replies_;

    PendingReplyTable(const PendingReplyTable&);
    PendingReplyTable& operator=(const PendingReplyTable&);
};

} // namespace omnibinder

#endif
