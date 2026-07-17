/**************************************************************************************************
 * @file        message.h
 * @brief       协议栈消息定义
 * @details     定义 OmniBinder 通信协议的消息格式，包括 16 字节固定消息头
 *              （magic + version + type + sequence + length）、消息类型枚举
 *              （控制通道：注册/发现/死亡通知/话题；数据通道：RPC 调用/广播/SHM 升级），
 *              以及 ServiceInfo/InterfaceInfo 的序列化辅助函数。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-02-11
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
#ifndef OMNIBINDER_MESSAGE_H
#define OMNIBINDER_MESSAGE_H

#include "omnibinder/types.h"
#include "omnibinder/buffer.h"
#include "omnibinder/buffer_view.h"
#include <stdint.h>

namespace omnibinder {

// ============================================================
// 协议常量
// ============================================================
const uint32_t OMNI_MAGIC   = 0x42494E44u;  // "BIND"
const uint16_t OMNI_VERSION = 0x0001;

// ============================================================
// 消息类型
// ============================================================
enum class MessageType : uint16_t {
    // 控制通道 - 注册/注销
    MSG_REGISTER              = 0x0001,
    MSG_REGISTER_REPLY        = 0x0002,
    MSG_UNREGISTER            = 0x0003,
    MSG_UNREGISTER_REPLY      = 0x0004,
    MSG_HEARTBEAT             = 0x0005,
    MSG_HEARTBEAT_ACK         = 0x0006,

    // 控制通道 - 服务发现
    MSG_LOOKUP                = 0x0010,
    MSG_LOOKUP_REPLY          = 0x0011,
    MSG_LIST_SERVICES         = 0x0012,
    MSG_LIST_SERVICES_REPLY   = 0x0013,
    MSG_QUERY_INTERFACES      = 0x0014,
    MSG_QUERY_INTERFACES_REPLY= 0x0015,

    // 控制通道 - 死亡通知
    MSG_SUBSCRIBE_SERVICE     = 0x0020,
    MSG_SUBSCRIBE_SERVICE_REPLY = 0x0021,
    MSG_UNSUBSCRIBE_SERVICE   = 0x0022,
    MSG_DEATH_NOTIFY          = 0x0023,

    // 控制通道 - 话题
    MSG_PUBLISH_TOPIC         = 0x0030,
    MSG_PUBLISH_TOPIC_REPLY   = 0x0031,
    MSG_SUBSCRIBE_TOPIC       = 0x0032,
    MSG_SUBSCRIBE_TOPIC_REPLY = 0x0033,
    MSG_TOPIC_PUBLISHER_NOTIFY= 0x0034,
    MSG_UNPUBLISH_TOPIC       = 0x0035,
    MSG_UNSUBSCRIBE_TOPIC     = 0x0036,
    MSG_QUERY_PUBLISHED_TOPICS= 0x0037,
    MSG_QUERY_PUBLISHED_TOPICS_REPLY = 0x0038,

    // 控制通道 - 运行时诊断
    MSG_RUNTIME_HELLO         = 0x0040,
    MSG_RUNTIME_HELLO_REPLY   = 0x0041,
    MSG_DIAG_SET_LOG_LEVEL    = 0x0042,
    MSG_DIAG_SET_LOG_LEVEL_REPLY = 0x0043,
    MSG_DIAG_WATCH_START      = 0x0044,
    MSG_DIAG_WATCH_START_REPLY = 0x0045,
    MSG_DIAG_WATCH_STOP       = 0x0046,
    MSG_DIAG_WATCH_STOP_REPLY = 0x0047,
    MSG_RUNTIME_LIST          = 0x0049,
    MSG_RUNTIME_LIST_REPLY    = 0x004A,

    // 数据通道 - 接口调用
    MSG_INVOKE                = 0x0100,
    MSG_INVOKE_REPLY          = 0x0101,
    MSG_INVOKE_ONEWAY         = 0x0102,  // 单向调用，服务端不发送回复

    // 数据通道 - 广播
    MSG_BROADCAST             = 0x0110,
    MSG_SUBSCRIBE_BROADCAST   = 0x0111,  // 订阅者直连发布者后发送，携带 topic_id
};

enum DiagEventDirection : uint8_t {
    DIAG_EVENT_REQUEST = 0,
    DIAG_EVENT_RESPONSE = 1,
    DIAG_EVENT_ONE_WAY = 2,
    DIAG_EVENT_SUBSCRIBE = 3,
    DIAG_EVENT_BROADCAST = 4
};

const size_t DIAG_EVENT_DIRECTION_SIZE = 1;
const size_t DIAG_EVENT_TIMESTAMP_SIZE = 8;
const size_t DIAG_EVENT_TYPE_SIZE = 2;
const size_t DIAG_EVENT_SEQUENCE_SIZE = 4;
const size_t DIAG_EVENT_PAYLOAD_LENGTH_SIZE = 4;
const size_t DIAG_EVENT_WIRE_HEADER_SIZE = DIAG_EVENT_DIRECTION_SIZE
    + DIAG_EVENT_TIMESTAMP_SIZE
    + DIAG_EVENT_TYPE_SIZE
    + DIAG_EVENT_SEQUENCE_SIZE
    + DIAG_EVENT_PAYLOAD_LENGTH_SIZE;

// ============================================================
// 消息头（16字节，固定大小）
// ============================================================
#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;      // OMNI_MAGIC
    uint16_t version;    // OMNI_VERSION
    uint16_t type;       // MessageType
    uint32_t sequence;   // 序列号
    uint32_t length;     // payload 长度
};
#pragma pack(pop)

const size_t MESSAGE_HEADER_SIZE = 16;

// ============================================================
// 完整消息（头 + 载荷）
// ============================================================
struct Message {
    MessageHeader header;
    Buffer        payload;

    Message();
    Message(MessageType type, uint32_t seq);

    // 设置消息类型
    void setType(MessageType type);
    MessageType getType() const;

    // 序列号
    void setSequence(uint32_t seq);
    uint32_t getSequence() const;

    // 将整个消息序列化为字节流（头 + 载荷）
    bool serialize(Buffer& output) const;

    // 从字节流解析消息头（不消费载荷数据）
    // 返回: true 表示头部解析成功
    static bool parseHeader(const uint8_t* data, size_t length, MessageHeader& header);

    // 验证消息头
    static bool validateHeader(const MessageHeader& header);
};

// ============================================================
// 消息类型转字符串（调试用）
// ============================================================
const char* messageTypeToString(MessageType type);

// ============================================================
// 序列号生成器
// ============================================================
uint32_t nextSequenceNumber();

// ============================================================
// 辅助函数：序列化/反序列化 ServiceInfo 到 Buffer
// ============================================================
void serializeServiceInfo(const ServiceInfo& info, Buffer& buf);

bool serializePublishedTopicsReply(bool found,
                                   const std::vector<std::string>& topics,
                                   Buffer& buf);

template <typename BufT>
bool deserializePublishedTopicsReply(BufT& buf, bool& found,
                                     std::vector<std::string>& topics) {
    bool decoded_found = false;
    if (!buf.tryReadBool(decoded_found)) {
        return false;
    }

    std::vector<std::string> decoded_topics;
    if (decoded_found) {
        uint32_t count = 0;
        if (!buf.tryReadUint32(count)
            || count > MAX_PUBLISHED_TOPICS
            || count > buf.remaining() / sizeof(uint32_t)) {
            return false;
        }
        decoded_topics.reserve(count);
        size_t aggregate_bytes = 0;
        for (uint32_t i = 0; i < count; ++i) {
            std::string topic;
            if (!buf.tryReadString(topic)
                || topic.empty()
                || topic.size() > MAX_TOPIC_NAME_LENGTH
                || topic.size() > MAX_PUBLISHED_TOPICS_BYTES - aggregate_bytes) {
                return false;
            }
            aggregate_bytes += topic.size();
            decoded_topics.push_back(topic);
        }
    }
    if (buf.remaining() != 0) {
        return false;
    }

    found = decoded_found;
    topics.swap(decoded_topics);
    return true;
}

void serializeRuntimeInfo(const RuntimeInfo& info, Buffer& buf);

template <typename BufT>
bool deserializeRuntimeInfo(BufT& buf, RuntimeInfo& info) {
    uint16_t service_count = 0;
    if (!(buf.tryReadUint32(info.pid)
        && buf.tryReadString(info.process_name)
        && buf.tryReadString(info.role)
        && buf.tryReadUint32(info.log_level)
        && buf.tryReadUint32(info.diag_capabilities)
        && buf.tryReadUint16(service_count))) {
        return false;
    }
    info.services.clear();
    for (uint16_t i = 0; i < service_count; ++i) {
        std::string service;
        if (!buf.tryReadString(service)) {
            return false;
        }
        info.services.push_back(service);
    }
    return true;
}

template <typename BufT>
bool deserializeServiceInfo(BufT& buf, ServiceInfo& info) {
    uint16_t iface_count = 0;
    uint32_t req_ring_capacity = 0;
    uint32_t resp_ring_capacity = 0;
    if (!buf.tryReadString(info.name)
        || !buf.tryReadString(info.host)
        || !buf.tryReadUint16(info.port)
        || !buf.tryReadString(info.host_id)
        || !buf.tryReadUint32(req_ring_capacity)
        || !buf.tryReadUint32(resp_ring_capacity)
        || !buf.tryReadUint16(iface_count)) {
        return false;
    }
    info.shm_config.req_ring_capacity = req_ring_capacity;
    info.shm_config.resp_ring_capacity = resp_ring_capacity;
    info.interfaces.resize(iface_count);
    for (uint16_t i = 0; i < iface_count; ++i) {
        if (!deserializeInterfaceInfo(buf, info.interfaces[i])) {
            return false;
        }
    }
    return true;
}

void serializeInterfaceInfo(const InterfaceInfo& info, Buffer& buf);

template <typename BufT>
bool deserializeInterfaceInfo(BufT& buf, InterfaceInfo& info) {
    uint16_t method_count = 0;
    if (!buf.tryReadUint32(info.interface_id)
        || !buf.tryReadString(info.name)
        || !buf.tryReadUint16(method_count)) {
        return false;
    }
    info.methods.resize(method_count);
    for (uint16_t i = 0; i < method_count; ++i) {
        if (!buf.tryReadUint32(info.methods[i].method_id)
            || !buf.tryReadString(info.methods[i].name)
            || !buf.tryReadString(info.methods[i].param_types)
            || !buf.tryReadString(info.methods[i].return_type)) {
            return false;
        }
        if (!buf.tryReadUint32(info.methods[i].idl_hash)) {
            return false;
        }
    }
    return true;
}

} // namespace omnibinder

#endif // OMNIBINDER_MESSAGE_H
