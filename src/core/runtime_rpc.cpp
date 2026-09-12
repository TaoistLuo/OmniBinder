#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#define LOG_TAG "OmniRuntimeRpc"

namespace omnibinder {

namespace {

/*
 * @brief  若 seq 已有 ready 的回复则直接消费；否则若正处于等待则存储并消费
 * @return false 表示消息不属于任何等待槽，交由调用方按原类型处理
 * @note   控制面/数据面共用该判定逻辑，但作用于各自独立的槽表
 */
bool consumeReplyIfWaiting(PendingReplyTable& table, uint32_t seq, Message& msg) {
    if (table.pendingReply(seq) != NULL) {
        return true;
    }
    if (table.isWaiting(seq)) {
        table.storeReply(seq, msg);
        return true;
    }
    return false;
}

} // namespace

// ============================================================
// 同步等待
// ============================================================

uint32_t OmniRuntime::Impl::allocSequence() {
    return rpc_runtime_.nextSequence();
}

int OmniRuntime::Impl::waitForReply(uint32_t seq, uint32_t timeout_ms, Message& reply,
                                    const std::function<bool()>& is_alive) {
    // 控制面等待：槽表由 SmControlChannel 持有，SM 重连时只清该表
    return rpc_runtime_.waitForReply(
        seq,
        timeout_ms,
        sm_channel_.pendingReplies(),
        [this](int wait_ms) { this->pollOnceWithoutFunctors(wait_ms); },
        reply,
        is_alive);
}

int OmniRuntime::Impl::waitForDataReply(uint32_t seq, uint32_t timeout_ms, Message& reply,
                                        const std::function<bool()>& is_alive) {
    // 数据面等待：槽表由 RpcRuntime 独立持有，SM 重连不影响在途直连 RPC
    return rpc_runtime_.waitForReply(
        seq,
        timeout_ms,
        rpc_runtime_.dataReplies(),
        [this](int wait_ms) { this->pollOnceWithoutFunctors(wait_ms); },
        reply,
        is_alive);
}

bool OmniRuntime::Impl::storeAndConsumeControlReply(uint32_t seq, Message& msg) {
    return consumeReplyIfWaiting(sm_channel_.pendingReplies(), seq, msg);
}

bool OmniRuntime::Impl::storeAndConsumeDataReply(uint32_t seq, Message& msg) {
    return consumeReplyIfWaiting(rpc_runtime_.dataReplies(), seq, msg);
}

uint32_t OmniRuntime::Impl::effectiveTimeout(uint32_t timeout_ms) const {
    return rpc_runtime_.effectiveTimeout(timeout_ms);
}

// ============================================================
// 远程调用
// ============================================================

int OmniRuntime::Impl::invoke(const std::string& service_name, uint32_t interface_id,
                               uint32_t method_id, uint32_t idl_hash,
                               const Buffer& request, Buffer& response,
                               uint32_t timeout_ms) {
    return callSerialized([this, &service_name, interface_id, method_id, idl_hash, &request, &response, timeout_ms]() -> int {
        return invokeInternal(service_name, interface_id, method_id, idl_hash, request, response, timeout_ms);
    });
}

int OmniRuntime::Impl::invokeInternal(const std::string& service_name, uint32_t interface_id,
                                    uint32_t method_id, uint32_t idl_hash,
                                    const Buffer& request, Buffer& response,
                                    uint32_t timeout_ms) {
    stats_.total_rpc_calls++;
    uint32_t total_timeout_ms = effectiveTimeout(timeout_ms);

    ServiceConnection* conn = conn_mgr_->getConnection(service_name);
    if (!conn) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    uint32_t seq = allocSequence();
    Message msg(MessageType::MSG_INVOKE, seq);
    if (!populateInvokeMessage(msg, interface_id, method_id, idl_hash, request)) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    emitDiagEvent(DIAG_EVENT_REQUEST, msg);

    uint32_t send_elapsed_ms = 0;
    if (!conn_mgr_->sendMessageWithinTimeout(service_name, msg, total_timeout_ms, &send_elapsed_ms)) {
        if (send_elapsed_ms >= total_timeout_ms) {
            stats_.total_rpc_timeouts++;
            stats_.total_rpc_failures++;
            OMNI_LOG_WARN(LOG_TAG,
                          "rpc_send_timeout service=%s iface=0x%08x method=0x%08x timeout_ms=%u",
                          service_name.c_str(), interface_id, method_id, total_timeout_ms);
            return static_cast<int>(ErrorCode::ERR_TIMEOUT);
        }
        stats_.connection_errors++;
        stats_.total_rpc_failures++;
        OMNI_LOG_ERROR(LOG_TAG,
                       "rpc_send_failed service=%s iface=0x%08x method=0x%08x err=%d",
                       service_name.c_str(), interface_id, method_id,
                       static_cast<int>(ErrorCode::ERR_SEND_FAILED));
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }

    uint32_t reply_timeout_ms = total_timeout_ms > send_elapsed_ms
        ? total_timeout_ms - send_elapsed_ms
        : 0;

    Message reply;
    // 数据面等待必须使用独立槽表：SM 重连（控制面 clearReplies）不得使在途直连 RPC 误失败
    int ret = waitForDataReply(seq, reply_timeout_ms, reply,
        [this, &service_name]() -> bool {
            return conn_mgr_ && conn_mgr_->getConnection(service_name) != NULL;
        });
    if (ret != 0) {
        if (ret == static_cast<int>(ErrorCode::ERR_TIMEOUT)) {
            stats_.total_rpc_timeouts++;
            OMNI_LOG_WARN(LOG_TAG,
                          "rpc_timeout service=%s iface=0x%08x method=0x%08x timeout_ms=%u err=%d",
                          service_name.c_str(), interface_id, method_id,
                          total_timeout_ms, ret);
        }
        stats_.total_rpc_failures++;
        return ret;
    }
    int32_t status = 0;
    if (!decodeInvokeReplyPayload(reply, status, response)) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (status != 0) {
        stats_.total_rpc_failures++;
        return status;
    }
    stats_.total_rpc_success++;
    return 0;
}

int OmniRuntime::Impl::invokeOneWay(const std::string& service_name, uint32_t interface_id,
                                     uint32_t method_id, uint32_t idl_hash,
                                     const Buffer& request) {
    return callSerialized([this, &service_name, interface_id, method_id, idl_hash, &request]() -> int {
        return invokeOneWayInternal(service_name, interface_id, method_id, idl_hash, request);
    });
}

int OmniRuntime::Impl::invokeOneWayInternal(const std::string& service_name, uint32_t interface_id,
                                           uint32_t method_id, uint32_t idl_hash,
                                           const Buffer& request) {
    stats_.total_rpc_calls++;

    ServiceConnection* conn = conn_mgr_->getConnection(service_name);
    if (!conn) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_CONNECT_FAILED);
    }

    Message msg(MessageType::MSG_INVOKE_ONEWAY, allocSequence());
    if (!populateInvokeMessage(msg, interface_id, method_id, idl_hash, request)) {
        stats_.total_rpc_failures++;
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    emitDiagEvent(DIAG_EVENT_ONE_WAY, msg);

    if (!conn_mgr_->sendMessage(service_name, msg)) {
        stats_.connection_errors++;
        stats_.total_rpc_failures++;
        OMNI_LOG_ERROR(LOG_TAG,
                       "rpc_send_failed service=%s iface=0x%08x method=0x%08x err=%d",
                       service_name.c_str(), interface_id, method_id,
                       static_cast<int>(ErrorCode::ERR_SEND_FAILED));
        return static_cast<int>(ErrorCode::ERR_SEND_FAILED);
    }
    stats_.total_rpc_success++;
    return 0;
}

} // namespace omnibinder
