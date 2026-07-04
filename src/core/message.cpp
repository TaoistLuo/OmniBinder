#include "omnibinder/message.h"
#include <cstring>
#include <atomic>

namespace omnibinder {

// ============================================================
// 序列号生成器（原子递增）
// ============================================================
static std::atomic<uint32_t> s_sequence_counter(0);

uint32_t nextSequenceNumber() {
    return s_sequence_counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

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

void Message::setType(MessageType type) {
    header.type = static_cast<uint16_t>(type);
}

MessageType Message::getType() const {
    return static_cast<MessageType>(header.type);
}

void Message::setSequence(uint32_t seq) {
    header.sequence = seq;
}

uint32_t Message::getSequence() const {
    return header.sequence;
}

bool Message::serialize(Buffer& output) const {
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

bool Message::parseHeader(const uint8_t* data, size_t length, MessageHeader& hdr) {
    if (!data || length < MESSAGE_HEADER_SIZE) {
        return false;
    }

    // 小端序读取
    Buffer buf(data, MESSAGE_HEADER_SIZE);
    if (!buf.tryReadUint32(hdr.magic)
        || !buf.tryReadUint16(hdr.version)
        || !buf.tryReadUint16(hdr.type)
        || !buf.tryReadUint32(hdr.sequence)
        || !buf.tryReadUint32(hdr.length)) {
        return false;
    }

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
    case MessageType::MSG_INVOKE:                  return "INVOKE";
    case MessageType::MSG_INVOKE_REPLY:            return "INVOKE_REPLY";
    case MessageType::MSG_BROADCAST:               return "BROADCAST";
    case MessageType::MSG_PING:                    return "PING";
    case MessageType::MSG_PONG:                    return "PONG";
    default:                                       return "UNKNOWN";
    }
}

// ============================================================
// ServiceInfo 序列化（deserialize 已移至 message.h 模板）
// ============================================================

void serializeServiceInfo(const ServiceInfo& info, Buffer& buf) {
    if (!buf.writeString(info.name)
        || !buf.writeString(info.host)
        || !buf.writeUint16(info.port)
        || !buf.writeString(info.host_id)
        || !buf.writeUint32(static_cast<uint32_t>(info.shm_config.req_ring_capacity))
        || !buf.writeUint32(static_cast<uint32_t>(info.shm_config.resp_ring_capacity))) {
        return;
    }

    // 接口列表
    uint16_t iface_count = static_cast<uint16_t>(info.interfaces.size());
    if (!buf.writeUint16(iface_count)) {
        return;
    }
    for (uint16_t i = 0; i < iface_count; ++i) {
        serializeInterfaceInfo(info.interfaces[i], buf);
        if (!buf.writeOk()) {
            return;
        }
    }
}

void serializeInterfaceInfo(const InterfaceInfo& info, Buffer& buf) {
    if (!buf.writeUint32(info.interface_id)
        || !buf.writeString(info.name)) {
        return;
    }

    uint16_t method_count = static_cast<uint16_t>(info.methods.size());
    if (!buf.writeUint16(method_count)) {
        return;
    }
    for (uint16_t i = 0; i < method_count; ++i) {
        if (!buf.writeUint32(info.methods[i].method_id)
            || !buf.writeString(info.methods[i].name)
            || !buf.writeString(info.methods[i].param_types)
            || !buf.writeString(info.methods[i].return_type)
            || !buf.writeUint32(info.methods[i].idl_hash)) {
            return;
        }
    }
}

} // namespace omnibinder
