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
    , wait_deadline_ms_(0) {}

uint32_t RpcRuntime::nextSequence() {
    return ++sequence_counter_;
}

uint32_t RpcRuntime::effectiveTimeout(uint32_t timeout_ms) const {
    return timeout_ms > 0 ? timeout_ms : default_timeout_ms_;
}

void RpcRuntime::setDefaultTimeout(uint32_t timeout_ms) {
    default_timeout_ms_ = timeout_ms;
}

bool RpcRuntime::beginWait(uint32_t timeout_ms) {
    bool reentrant = in_wait_for_reply_;
    in_wait_for_reply_ = true;
    wait_deadline_ms_ = platform::currentTimeMs() + timeout_ms;
    return !reentrant;
}

void RpcRuntime::endWait() {
    in_wait_for_reply_ = false;
    wait_deadline_ms_ = 0;
}

int64_t RpcRuntime::remainingWaitMs() const {
    return wait_deadline_ms_ - platform::currentTimeMs();
}

bool RpcRuntime::isTimedOut() const {
    return remainingWaitMs() <= 0;
}

int RpcRuntime::waitForReply(uint32_t seq, uint32_t timeout_ms,
                             SmControlChannel& channel,
                             const std::function<void(int)>& poll_once,
                             Message& reply) {
    if (!beginWait(timeout_ms)) {
        OMNI_LOG_WARN(LOG_TAG_RPC, "Re-entrant waitForReply detected, this may cause issues");
    }

    channel.beginWait(seq);
    while (channel.pendingReply(seq) == NULL) {
        if (isTimedOut()) {
            channel.eraseWait(seq);
            endWait();
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }

        int64_t remaining = remainingWaitMs();
        poll_once(static_cast<int>(std::min<int64_t>(remaining, 100)));
    }

    Message* pending = channel.pendingReply(seq);
    reply.header = pending->header;
    reply.payload.assign(pending->payload.data(), pending->payload.size());
    channel.eraseWait(seq);
    endWait();
    return 0;
}

} // namespace omnibinder
