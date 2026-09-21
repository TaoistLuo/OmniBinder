#include "core/rpc_runtime.h"

#include "omnibinder/error.h"
#include "platform/platform.h"
#include "core/omni_runtime.h"
#include "omnibinder/log.h"

namespace omnibinder {

static const char* LOG_TAG_RPC = "RpcRuntime";

RpcRuntime::RpcRuntime()
    : default_timeout_ms_(DEFAULT_INVOKE_TIMEOUT)
    , sequence_counter_(0)
    , in_wait_for_reply_(false)
    , wait_deadline_ms_(0)
    , data_replies_() {}

uint32_t RpcRuntime::nextSequence() {
    uint32_t seq = ++sequence_counter_;
    if (seq == 0) {
        seq = ++sequence_counter_;
    }
    return seq;
}

uint32_t RpcRuntime::effectiveTimeout(uint32_t timeout_ms) const {
    return timeout_ms > 0 ? timeout_ms : default_timeout_ms_;
}

void RpcRuntime::setDefaultTimeout(uint32_t timeout_ms) {
    // 0 会让 effectiveTimeout 返回 0，所有 RPC/注册等待零预算立即超时；
    // 视为非法入参忽略，保持既有默认值
    if (timeout_ms == 0) {
        return;
    }
    default_timeout_ms_ = timeout_ms;
}

bool RpcRuntime::beginWait(uint32_t timeout_ms) {
    bool reentrant = in_wait_for_reply_;
    in_wait_for_reply_ = true;
    wait_deadline_ms_ = platform::currentTimeMs() + timeout_ms;
    return !reentrant;
}

int64_t RpcRuntime::remainingWaitMs() const {
    return wait_deadline_ms_ - platform::currentTimeMs();
}

bool RpcRuntime::isTimedOut() const {
    return remainingWaitMs() <= 0;
}

int RpcRuntime::waitForReply(uint32_t seq, uint32_t timeout_ms,
                             PendingReplyTable& table,
                             const std::function<void(int)>& poll_once,
                             Message& reply,
                             const std::function<bool()>& is_alive) {
    // RAII 恢复等待状态：无论从哪个出口返回都复位，消除 5 处重复恢复
    struct WaitStateGuard {
        RpcRuntime& self;
        bool prev_in_wait;
        int64_t prev_deadline;
        WaitStateGuard(RpcRuntime& s, bool in_wait, int64_t deadline)
            : self(s), prev_in_wait(in_wait), prev_deadline(deadline) {}
        ~WaitStateGuard() {
            self.in_wait_for_reply_ = prev_in_wait;
            self.wait_deadline_ms_ = prev_deadline;
        }
    } guard(*this, in_wait_for_reply_, wait_deadline_ms_);

    if (!beginWait(timeout_ms)) {
        OMNI_LOG_WARN(LOG_TAG_RPC, "Re-entrant waitForReply detected, this may cause issues");
    }

    table.beginWait(seq);
    while (table.pendingReply(seq) == NULL) {
        if (isTimedOut()) {
            table.eraseWait(seq);
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }

        // 所属平面连接重建（clear 标记失败）后旧连接的回复不可能到达，快速失败，
        // 避免空转至超时
        if (table.isFailed(seq)) {
            table.eraseWait(seq);
            return static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED);
        }

        int64_t remaining = remainingWaitMs();
        poll_once(static_cast<int>(remaining));

        if (table.pendingReply(seq) == NULL && is_alive && !is_alive()) {
            table.eraseWait(seq);
            return static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED);
        }
    }

    if (!table.takeReply(seq, reply)) {
        return static_cast<int>(ErrorCode::ERR_CONNECTION_CLOSED);
    }
    return 0;
}

} // namespace omnibinder
