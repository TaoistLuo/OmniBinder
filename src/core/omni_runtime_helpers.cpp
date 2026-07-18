#include "core/omni_runtime_helpers.h"

#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"
#include "platform/platform.h"

#define LOG_TAG "OmniRuntimeHelpers"

namespace omnibinder {

const uint32_t OMNI_DIAG_IFACE_ID = 0x4F4E4944u;

std::string normalizeAdvertiseHost(const std::string& host) {
    if (host.empty() || host == "0.0.0.0") {
        return "127.0.0.1";
    }
    return host;
}

const char* dataChannelKindName(TransportType type) {
    return type == TransportType::SHM ? "SHM" : "TCP";
}

void diag_serialize_event(Buffer& buf, uint8_t direction, const Message& msg) {
    uint64_t ts_us = static_cast<uint64_t>(platform::currentTimeUs());
    buf.writeUint8(direction);
    uint8_t ts_buf[8];
    for (int i = 7; i >= 0; --i) {
        ts_buf[i] = static_cast<uint8_t>(ts_us);
        ts_us >>= 8;
    }
    buf.writeRaw(ts_buf, 8);
    buf.writeUint16(static_cast<uint16_t>(msg.getType()));
    buf.writeUint32(msg.getSequence());
    buf.writeUint32(static_cast<uint32_t>(msg.payload.size()));
    if (msg.payload.size() > 0) {
        buf.writeRaw(msg.payload.data(), msg.payload.size());
    }
}

bool decodeInvokePayload(const Message& msg, uint32_t& interface_id,
                         uint32_t& idl_hash, uint32_t& method_id, Buffer& request) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!buf.tryReadUint32(interface_id) || !buf.tryReadUint32(idl_hash)
        || !buf.tryReadUint32(method_id) || !buf.tryReadUint32(payload_len)
        || buf.remaining() < payload_len) {
        return false;
    }
    request.clear();
    return payload_len == 0
        || request.writeRaw(buf.data() + buf.readPosition(), payload_len);
}

bool decodeSubscribeBroadcastPayload(const Message& msg, uint32_t& topic_id,
                                     std::string& topic_name) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    return buf.tryReadUint32(topic_id) && buf.tryReadString(topic_name);
}

bool decodeBroadcastPayload(const Message& msg, uint32_t& topic_id, Buffer& payload) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!buf.tryReadUint32(topic_id) || !buf.tryReadUint32(payload_len)
        || buf.remaining() < payload_len) {
        return false;
    }
    payload.clear();
    return payload_len == 0
        || payload.writeRaw(buf.data() + buf.readPosition(), payload_len);
}

bool decodeSingleStringPayload(const Message& msg, std::string& value) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    return buf.tryReadString(value);
}

bool decodeBoolReplyPayload(const Message& msg, bool& value) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    return buf.tryReadBool(value);
}

bool decodeUint32ReplyPayload(const Message& msg, uint32_t& value) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    return buf.tryReadUint32(value);
}

bool decodeInvokeReplyPayload(const Message& msg, int32_t& status, Buffer& response) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!buf.tryReadInt32(status)) {
        return false;
    }
    response.clear();
    if (status != 0) {
        return true;
    }
    if (!buf.tryReadUint32(payload_len) || buf.remaining() < payload_len) {
        return false;
    }
    return payload_len == 0
        || response.writeRaw(buf.data() + buf.readPosition(), payload_len);
}

Message makeInvokeErrorReply(uint32_t seq, ErrorCode error) {
    Message reply(MessageType::MSG_INVOKE_REPLY, seq);
    if (!reply.payload.writeInt32(static_cast<int32_t>(error))
        || !reply.payload.writeUint32(0)) {
        OMNI_LOG_ERROR(LOG_TAG, "invoke_error_reply_serialize_failed seq=%u", seq);
    }
    return reply;
}

Message makeInvokeSuccessReply(uint32_t seq, const Buffer& response) {
    Message reply(MessageType::MSG_INVOKE_REPLY, seq);
    if (!reply.payload.writeInt32(0)
        || !reply.payload.writeUint32(static_cast<uint32_t>(response.size()))
        || (response.size() > 0
            && !reply.payload.writeRaw(response.data(), response.size()))) {
        OMNI_LOG_ERROR(LOG_TAG, "invoke_success_reply_serialize_failed seq=%u", seq);
    }
    return reply;
}

} // namespace omnibinder
