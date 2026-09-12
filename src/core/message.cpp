#include "omnibinder/message.h"
#include <cstring>
#include <limits>

namespace omnibinder {

namespace {

void putUint16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void putUint32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

} // namespace

// ============================================================
// Message 实现
// ============================================================

Message::Message() {
    memset(&header, 0, sizeof(header));
    header.magic = OMNI_MAGIC;
    header.version = OMNI_VERSION;
}

Message::Message(MessageType type, uint32_t seq) {
    memset(&header, 0, sizeof(header));
    header.magic = OMNI_MAGIC;
    header.version = OMNI_VERSION;
    header.type = static_cast<uint16_t>(type);
    header.sequence = seq;
}

MessageType Message::getType() const {
    return static_cast<MessageType>(header.type);
}

uint32_t Message::getSequence() const {
    return header.sequence;
}

bool Message::serialize(Buffer& output) const {
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    // 更新 length 字段
    MessageHeader h = header;
    h.length = static_cast<uint32_t>(payload.size());

    // 写入头部（小端序）
    if (!output.writeUint32(h.magic)
        || !output.writeUint16(h.version)
        || !output.writeUint16(h.type)
        || !output.writeUint32(h.sequence)
        || !output.writeUint32(h.length)) {
        return false;
    }

    // 写入载荷
    if (h.length > 0) {
        if (!output.writeRaw(payload.data(), h.length)) {
            return false;
        }
    }

    return true;
}

bool Message::serializeInPlace() {
    const size_t len = payload.size();
    if (len > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    payload.reserve(MESSAGE_HEADER_SIZE + len);
    if (payload.capacity() < MESSAGE_HEADER_SIZE + len) {
        return false;
    }

    uint8_t* base = payload.mutableData();
    std::memmove(base + MESSAGE_HEADER_SIZE, base, len);
    putUint32LE(base, header.magic);
    putUint16LE(base + 4, header.version);
    putUint16LE(base + 6, header.type);
    putUint32LE(base + 8, header.sequence);
    putUint32LE(base + 12, static_cast<uint32_t>(len));
    payload.setWritePosition(MESSAGE_HEADER_SIZE + len);
    return true;
}

bool Message::parseHeader(const uint8_t* data, size_t length, MessageHeader& hdr) {
    if (!data || length < MESSAGE_HEADER_SIZE) {
        return false;
    }

    // 小端序直接按偏移读取，避开每帧构造 Buffer 的堆分配
    hdr.magic    = static_cast<uint32_t>(data[0])
                 | (static_cast<uint32_t>(data[1]) << 8)
                 | (static_cast<uint32_t>(data[2]) << 16)
                 | (static_cast<uint32_t>(data[3]) << 24);
    hdr.version  = static_cast<uint16_t>(data[4] | (data[5] << 8));
    hdr.type     = static_cast<uint16_t>(data[6] | (data[7] << 8));
    hdr.sequence = static_cast<uint32_t>(data[8])
                 | (static_cast<uint32_t>(data[9]) << 8)
                 | (static_cast<uint32_t>(data[10]) << 16)
                 | (static_cast<uint32_t>(data[11]) << 24);
    hdr.length   = static_cast<uint32_t>(data[12])
                 | (static_cast<uint32_t>(data[13]) << 8)
                 | (static_cast<uint32_t>(data[14]) << 16)
                 | (static_cast<uint32_t>(data[15]) << 24);
    return true;
}

bool Message::validateHeader(const MessageHeader& hdr) {
    if (hdr.magic != OMNI_MAGIC) {
        return false;
    }
    if (hdr.length > MAX_MESSAGE_SIZE) {
        return false;
    }
    return true;
}

FrameStatus tryExtractFrame(const uint8_t* data, size_t avail,
                            MessageHeader& header, size_t& frame_size) {
    if (!data || avail < MESSAGE_HEADER_SIZE) {
        return FrameStatus::NeedMore;
    }
    if (!Message::parseHeader(data, avail, header)) {
        return FrameStatus::NeedMore;
    }
    if (!Message::validateHeader(header)) {
        return FrameStatus::Corrupt;
    }
    size_t total = MESSAGE_HEADER_SIZE + header.length;
    if (avail < total) {
        return FrameStatus::NeedMore;
    }
    frame_size = total;
    return FrameStatus::Complete;
}

// ============================================================
// 消息类型转字符串
// ============================================================
const char* messageTypeToString(MessageType type) {
    switch (type) {
    case MessageType::MSG_REGISTER:               return "REGISTER";
    case MessageType::MSG_REGISTER_REPLY:          return "REGISTER_REPLY";
    case MessageType::MSG_UNREGISTER:              return "UNREGISTER";
    case MessageType::MSG_UNREGISTER_REPLY:        return "UNREGISTER_REPLY";
    case MessageType::MSG_HEARTBEAT:               return "HEARTBEAT";
    case MessageType::MSG_HEARTBEAT_ACK:           return "HEARTBEAT_ACK";
    case MessageType::MSG_LOOKUP:                  return "LOOKUP";
    case MessageType::MSG_LOOKUP_REPLY:            return "LOOKUP_REPLY";
    case MessageType::MSG_LIST_SERVICES:           return "LIST_SERVICES";
    case MessageType::MSG_LIST_SERVICES_REPLY:     return "LIST_SERVICES_REPLY";
    case MessageType::MSG_QUERY_INTERFACES:        return "QUERY_INTERFACES";
    case MessageType::MSG_QUERY_INTERFACES_REPLY:  return "QUERY_INTERFACES_REPLY";
    case MessageType::MSG_SUBSCRIBE_SERVICE:       return "SUBSCRIBE_SERVICE";
    case MessageType::MSG_SUBSCRIBE_SERVICE_REPLY: return "SUBSCRIBE_SERVICE_REPLY";
    case MessageType::MSG_UNSUBSCRIBE_SERVICE:     return "UNSUBSCRIBE_SERVICE";
    case MessageType::MSG_DEATH_NOTIFY:            return "DEATH_NOTIFY";
    case MessageType::MSG_PUBLISH_TOPIC:           return "PUBLISH_TOPIC";
    case MessageType::MSG_PUBLISH_TOPIC_REPLY:     return "PUBLISH_TOPIC_REPLY";
    case MessageType::MSG_SUBSCRIBE_TOPIC:         return "SUBSCRIBE_TOPIC";
    case MessageType::MSG_SUBSCRIBE_TOPIC_REPLY:   return "SUBSCRIBE_TOPIC_REPLY";
    case MessageType::MSG_TOPIC_PUBLISHER_NOTIFY:  return "TOPIC_PUBLISHER_NOTIFY";
    case MessageType::MSG_UNPUBLISH_TOPIC:         return "UNPUBLISH_TOPIC";
    case MessageType::MSG_UNSUBSCRIBE_TOPIC:       return "UNSUBSCRIBE_TOPIC";
    case MessageType::MSG_QUERY_PUBLISHED_TOPICS:  return "QUERY_PUBLISHED_TOPICS";
    case MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY: return "QUERY_PUBLISHED_TOPICS_REPLY";
    case MessageType::MSG_RUNTIME_HELLO:           return "RUNTIME_HELLO";
    case MessageType::MSG_RUNTIME_HELLO_REPLY:     return "RUNTIME_HELLO_REPLY";
    case MessageType::MSG_DIAG_SET_LOG_LEVEL:      return "DIAG_SET_LOG_LEVEL";
    case MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY:return "DIAG_SET_LOG_LEVEL_REPLY";
    case MessageType::MSG_DIAG_WATCH_START:        return "DIAG_WATCH_START";
    case MessageType::MSG_DIAG_WATCH_START_REPLY:  return "DIAG_WATCH_START_REPLY";
    case MessageType::MSG_DIAG_WATCH_STOP:         return "DIAG_WATCH_STOP";
    case MessageType::MSG_DIAG_WATCH_STOP_REPLY:   return "DIAG_WATCH_STOP_REPLY";
    case MessageType::MSG_RUNTIME_LIST:            return "RUNTIME_LIST";
    case MessageType::MSG_RUNTIME_LIST_REPLY:      return "RUNTIME_LIST_REPLY";
    case MessageType::MSG_INVOKE:                  return "INVOKE";
    case MessageType::MSG_INVOKE_REPLY:            return "INVOKE_REPLY";
    case MessageType::MSG_BROADCAST:               return "BROADCAST";
    default:                                       return "UNKNOWN";
    }
}

bool serializeRuntimeInfo(const RuntimeInfo& info, Buffer& buf) {
    if (!buf.writeUint32(info.pid)
        || !buf.writeString(info.process_name)
        || !buf.writeString(info.role)
        || !buf.writeUint32(info.log_level)
        || !buf.writeUint32(info.diag_capabilities)) {
        return false;
    }
    uint16_t service_count = static_cast<uint16_t>(info.services.size());
    if (!buf.writeUint16(service_count)) {
        return false;
    }
    for (uint16_t i = 0; i < service_count; ++i) {
        if (!buf.writeString(info.services[i])) {
            return false;
        }
    }
    return true;
}

// ============================================================
// ServiceInfo 序列化（deserialize 已移至 message.h 模板）
// ============================================================

bool serializeServiceInfo(const ServiceInfo& info, Buffer& buf) {
    if (!buf.writeString(info.name)
        || !buf.writeString(info.host)
        || !buf.writeUint16(info.port)
        || !buf.writeString(info.host_id)
        || !buf.writeUint32(static_cast<uint32_t>(info.shm_config.req_ring_capacity))
        || !buf.writeUint32(static_cast<uint32_t>(info.shm_config.resp_ring_capacity))) {
        return false;
    }

    // 接口列表
    uint16_t iface_count = static_cast<uint16_t>(info.interfaces.size());
    if (!buf.writeUint16(iface_count)) {
        return false;
    }
    for (uint16_t i = 0; i < iface_count; ++i) {
        if (!serializeInterfaceInfo(info.interfaces[i], buf)) {
            return false;
        }
    }
    return true;
}

bool serializePublishedTopicsReply(bool found,
                                   const std::vector<std::string>& topics,
                                   Buffer& buf) {
    if (topics.size() > MAX_PUBLISHED_TOPICS || (!found && !topics.empty())) {
        return false;
    }

    size_t aggregate_bytes = 0;
    for (size_t i = 0; i < topics.size(); ++i) {
        if (topics[i].empty()
            || topics[i].size() > MAX_TOPIC_NAME_LENGTH
            || topics[i].size() > MAX_PUBLISHED_TOPICS_BYTES - aggregate_bytes) {
            return false;
        }
        aggregate_bytes += topics[i].size();
    }

    Buffer encoded;
    if (!encoded.writeBool(found)) {
        return false;
    }
    if (found) {
        if (!encoded.writeUint32(static_cast<uint32_t>(topics.size()))) {
            return false;
        }
        for (size_t i = 0; i < topics.size(); ++i) {
            if (!encoded.writeString(topics[i])) {
                return false;
            }
        }
    }
    return buf.writeRaw(encoded.data(), encoded.size());
}

bool serializeInterfaceInfo(const InterfaceInfo& info, Buffer& buf) {
    if (!buf.writeUint32(info.interface_id)
        || !buf.writeString(info.name)) {
        return false;
    }

    uint16_t method_count = static_cast<uint16_t>(info.methods.size());
    if (!buf.writeUint16(method_count)) {
        return false;
    }
    for (uint16_t i = 0; i < method_count; ++i) {
        if (!buf.writeUint32(info.methods[i].method_id)
            || !buf.writeString(info.methods[i].name)
            || !buf.writeString(info.methods[i].param_types)
            || !buf.writeString(info.methods[i].return_type)
            || !buf.writeUint32(info.methods[i].idl_hash)) {
            return false;
        }
    }
    return true;
}

} // namespace omnibinder
