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
                     Message& reply);

private:
    uint32_t default_timeout_ms_;
    uint32_t sequence_counter_;
    bool in_wait_for_reply_;
    int64_t wait_deadline_ms_;
};

} // namespace omnibinder

#endif
