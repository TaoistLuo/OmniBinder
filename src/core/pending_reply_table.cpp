#include "core/pending_reply_table.h"

#include <utility>

namespace omnibinder {

PendingReplyTable::PendingReplyTable()
    : replies_() {}

void PendingReplyTable::clear() {
    // 连接重建后，旧连接上尚未返回的回复不可能再到达。
    // 正在等待的槽保留并标记 failed，让外层 waitForReply 快速失败而非空转至超时；
    // 已就绪（ready）的槽直接清除。
    for (std::map<uint32_t, Slot>::iterator it = replies_.begin(); it != replies_.end();) {
        if (it->second.ready) {
            replies_.erase(it++);
        } else {
            it->second.failed = true;
            ++it;
        }
    }
}

void PendingReplyTable::beginWait(uint32_t seq) {
    std::map<uint32_t, Slot>::iterator it = replies_.find(seq);
    if (it == replies_.end()) {
        replies_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(seq),
                         std::forward_as_tuple());
        return;
    }
    if (it->second.ready) {
        return;
    }
    it->second.ready = false;
    it->second.failed = false;
    it->second.message.payload.clear();
}

bool PendingReplyTable::isWaiting(uint32_t seq) const {
    std::map<uint32_t, Slot>::const_iterator it = replies_.find(seq);
    return it != replies_.end() && !it->second.ready && !it->second.failed;
}

const Message* PendingReplyTable::pendingReply(uint32_t seq) const {
    std::map<uint32_t, Slot>::const_iterator it = replies_.find(seq);
    if (it == replies_.end()) {
        return NULL;
    }
    if (!it->second.ready) {
        return NULL;
    }
    return &it->second.message;
}

bool PendingReplyTable::isFailed(uint32_t seq) const {
    std::map<uint32_t, Slot>::const_iterator it = replies_.find(seq);
    return it != replies_.end() && it->second.failed;
}

bool PendingReplyTable::takeReply(uint32_t seq, Message& out) {
    std::map<uint32_t, Slot>::iterator it = replies_.find(seq);
    if (it == replies_.end()) {
        return false;
    }
    if (!it->second.ready) {
        return false;
    }
    out.header = it->second.message.header;
    out.payload = std::move(it->second.message.payload);
    replies_.erase(it);
    return true;
}

void PendingReplyTable::eraseWait(uint32_t seq) {
    replies_.erase(seq);
}

void PendingReplyTable::storeReply(uint32_t seq, Message& msg) {
    std::map<uint32_t, Slot>::iterator it = replies_.find(seq);
    if (it == replies_.end()) {
        // 丢弃无人等待的回复：槽只能由 beginWait() 创建，
        // 未经请求的回复无法无限撑大槽表
        return;
    }
    if (it->second.ready) {
        return;
    }

    it->second.message.header = msg.header;
    it->second.message.payload = std::move(msg.payload);
    it->second.ready = true;
}

} // namespace omnibinder
