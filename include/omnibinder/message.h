/**************************************************************************************************
 * @file        message.h
 * @brief       协议栈消息定义
 * @details     定义 OmniBinder 通信协议的消息格式，包括 16 字节固定消息头
 *              （magic + version + type + sequence + length）、消息类型枚举
 *              （控制通道：注册/发现/死亡通知/话题；数据通道：RPC 调用/广播），
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
#include <new>
#include <vector>

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
    // 按服务名列出全部实例（跨 host_id / instance_id）
    MSG_LIST_INSTANCES        = 0x0016,
    MSG_LIST_INSTANCES_REPLY  = 0x0017,

    // 控制通道 - 死亡通知
    // 名字级：MSG_SUBSCRIBE_SERVICE / MSG_UNSUBSCRIBE_SERVICE / MSG_DEATH_NOTIFY
    // 实例级：MSG_SUBSCRIBE_SERVICE_INSTANCE / MSG_UNSUBSCRIBE_SERVICE_INSTANCE /
    //         MSG_DEATH_NOTIFY_INSTANCE（仅指定实例死亡时下发）
    MSG_SUBSCRIBE_SERVICE     = 0x0020,
    MSG_SUBSCRIBE_SERVICE_REPLY = 0x0021,
    MSG_UNSUBSCRIBE_SERVICE   = 0x0022,
    MSG_DEATH_NOTIFY          = 0x0023,
    MSG_DEATH_NOTIFY_INSTANCE = 0x0024,
    MSG_SUBSCRIBE_SERVICE_INSTANCE = 0x0025,
    MSG_UNSUBSCRIBE_SERVICE_INSTANCE = 0x0026,

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

static_assert(MESSAGE_HEADER_SIZE == sizeof(MessageHeader),
              "MESSAGE_HEADER_SIZE must equal sizeof(MessageHeader)");

// ============================================================
// 完整消息（头 + 载荷）
// ============================================================
struct Message {
    MessageHeader header;
    Buffer        payload;

    Message();
    Message(MessageType type, uint32_t seq);

    MessageType getType() const;
    uint32_t getSequence() const;

    /* @brief 将整个消息序列化为字节流（头 + 载荷） */
    bool serialize(Buffer& output) const;

    /* @brief 原地序列化：把 header 写到 payload 前部（payload 右移），
     *        使 payload 直接成为完整线上帧，省去再分配一个序列化 Buffer。
     * @return true 成功；false 载荷过大或分配失败
     * @note   成功后用 payload.data()/payload.size() 作为待发送字节；
     *         本对象随后不能再当作消息体使用 */
    bool serializeInPlace();

    /* @brief 从字节流解析消息头（不消费载荷数据）
     * @return true 表示头部解析成功 */
    static bool parseHeader(const uint8_t* data, size_t length, MessageHeader& header);

    /* @brief 验证消息头 */
    static bool validateHeader(const MessageHeader& header);
};

// ============================================================
// 消息帧提取
// ============================================================
enum class FrameStatus { NeedMore, Complete, Corrupt };

/* @brief 从字节流中提取一个完整消息帧（不推进任何游标）
 * @param[in]  data       缓冲区起始地址
 * @param[in]  avail      当前可读字节数
 * @param[out] header     解析出的消息头
 * @param[out] frame_size 完整帧字节数（仅 Complete 时有效）
 * @return NeedMore 数据不足 / Complete 帧完整 / Corrupt 头部非法 */
FrameStatus tryExtractFrame(const uint8_t* data, size_t avail,
                            MessageHeader& header, size_t& frame_size);

// ============================================================
// 消息类型转字符串（调试用）
// ============================================================
/* @brief 消息类型转字符串（调试用） */
const char* messageTypeToString(MessageType type);

// ============================================================
// 辅助函数：序列化/反序列化 ServiceInfo 到 Buffer
// ============================================================
/* @brief 序列化服务信息（含实例身份、SHM 配置与接口列表）到 Buffer
 * @param[in]  info 服务信息
 * @param[out] buf  输出缓冲
 * @return true 成功；false 写入失败（缓冲写失败） */
bool serializeServiceInfo(const ServiceInfo& info, Buffer& buf);

// ============================================================
// 服务实例身份线格式（MSG_DEATH_NOTIFY / MSG_DEATH_NOTIFY_INSTANCE /
// 实例级死亡订阅请求共用）：service_name + host_id + instance_id
// ============================================================
/* @brief 序列化服务实例身份 payload
 * @param[out] buf          输出缓冲
 * @param[in]  name         服务名称
 * @param[in]  host_id      板级主机身份
 * @param[in]  instance_id  实例标识
 * @return true 成功 */
bool writeServiceIdentityPayload(Buffer& buf, const std::string& name,
                                 const std::string& host_id,
                                 const std::string& instance_id);

/* @brief 反序列化服务实例身份 payload
 * @param[in]  buf          输入缓冲
 * @param[out] name         服务名称
 * @param[out] host_id      板级主机身份
 * @param[out] instance_id  实例标识
 * @return true 成功；畸形/截断/超长/尾随字节返回 false 且不改动输出参数
 * @note   tryReadString 自带边界检查，恶意长度前缀不会越界读；本函数在此之上
 *         追加非空与长度上限校验，作为 MSG_DEATH_NOTIFY 的防御性入口。 */
template <typename BufT>
bool readServiceIdentityPayload(BufT& buf, std::string& name,
                                std::string& host_id, std::string& instance_id) {
    std::string decoded_name;
    std::string decoded_host_id;
    std::string decoded_instance_id;
    if (!buf.tryReadString(decoded_name)
        || !buf.tryReadString(decoded_host_id)
        || !buf.tryReadString(decoded_instance_id)) {
        return false;
    }
    if (decoded_name.empty()
        || decoded_name.size() > MAX_SERVICE_NAME_LENGTH
        || decoded_host_id.size() > MAX_SERVICE_NAME_LENGTH
        || decoded_instance_id.size() > MAX_SERVICE_NAME_LENGTH
        || buf.remaining() != 0) {
        return false;
    }
    name.swap(decoded_name);
    host_id.swap(decoded_host_id);
    instance_id.swap(decoded_instance_id);
    return true;
}

bool serializePublishedTopicsReply(bool found,
                                   const std::vector<std::string>& topics,
                                   Buffer& buf);

// ============================================================
// 话题订阅选择器线格式
//   uint8  mode: 0=仅主题名（任意服务）/ 1=服务级 / 2=实例级
//   [mode>=1] string publisher_service
//   [mode==2] string host_id + string instance_id
// ============================================================
const uint8_t TOPIC_SUB_MODE_TOPIC_ONLY = 0u;
const uint8_t TOPIC_SUB_MODE_SERVICE    = 1u;
const uint8_t TOPIC_SUB_MODE_INSTANCE   = 2u;

/* @brief 序列化话题订阅选择器
 * @param[in]   mode              订阅粒度（TOPIC_SUB_MODE_*）
 * @param[in]   publisher_service 发布者服务名（mode>=1）
 * @param[in]   host_id           目标实例 host_id（mode==2）
 * @param[in]   instance_id       目标实例标识（mode==2）
 * @param[out]  buf               输出缓冲
 * @return true 成功 */
bool writeTopicSubscriptionSelector(Buffer& buf, uint8_t mode,
                                    const std::string& publisher_service,
                                    const std::string& host_id,
                                    const std::string& instance_id);

/* @brief 反序列化话题订阅选择器（读取失败或 mode 非法返回 false） */
template <typename BufT>
bool readTopicSubscriptionSelector(BufT& buf, uint8_t& mode,
                                   std::string& publisher_service,
                                   std::string& host_id,
                                   std::string& instance_id) {
    if (!buf.tryReadUint8(mode) || mode > TOPIC_SUB_MODE_INSTANCE) {
        return false;
    }
    publisher_service.clear();
    host_id.clear();
    instance_id.clear();
    if (mode >= TOPIC_SUB_MODE_SERVICE && !buf.tryReadString(publisher_service)) {
        return false;
    }
    if (mode == TOPIC_SUB_MODE_INSTANCE
        && (!buf.tryReadString(host_id) || !buf.tryReadString(instance_id))) {
        return false;
    }
    return true;
}

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
        try {
            decoded_topics.reserve(count);
        } catch (const std::bad_alloc&) {
            // 内存耗尽：返回失败而非崩溃（无异常传播原则）
            return false;
        }
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

/* @brief 序列化运行时信息到 Buffer
 * @param[in]  info 运行时信息
 * @param[out] buf  输出缓冲
 * @return true 成功；false 写入失败 */
bool serializeRuntimeInfo(const RuntimeInfo& info, Buffer& buf);

/* @brief 反序列化运行时信息
 * @param[in]  buf  输入缓冲
 * @param[out] info 运行时信息
 * @return true 成功；false 畸形/截断/内存不足，且不改动 info */
template <typename BufT>
bool deserializeRuntimeInfo(BufT& buf, RuntimeInfo& info) {
    try {
        RuntimeInfo decoded;
        uint16_t service_count = 0;
        if (!(buf.tryReadUint32(decoded.pid)
            && buf.tryReadString(decoded.process_name)
            && buf.tryReadString(decoded.role)
            && buf.tryReadUint32(decoded.log_level)
            && buf.tryReadUint32(decoded.diag_capabilities)
            && buf.tryReadUint16(service_count))) {
            return false;
        }
        for (uint16_t i = 0; i < service_count; ++i) {
            std::string service;
            if (!buf.tryReadString(service)) {
                return false;
            }
            decoded.services.push_back(service);
        }
        info = decoded;
        return true;
    } catch (const std::bad_alloc&) {
        // 内存耗尽：返回失败而非崩溃（无异常传播原则）
        return false;
    }
}

/* @brief 反序列化接口信息（前置声明，供 deserializeServiceInfo 使用）
 * @param[in]  buf  输入缓冲
 * @param[out] info 接口信息
 * @return true 成功；false 畸形/截断/内存不足，且不改动 info */
template <typename BufT>
bool deserializeInterfaceInfo(BufT& buf, InterfaceInfo& info);

/* @brief 反序列化服务信息（含实例身份、SHM 配置与接口列表）
 * @param[in]  buf  输入缓冲
 * @param[out] info 服务信息
 * @return true 成功；false 畸形/截断/内存不足，且不改动 info */
template <typename BufT>
bool deserializeServiceInfo(BufT& buf, ServiceInfo& info) {
    try {
        ServiceInfo decoded;
        uint16_t iface_count = 0;
        uint32_t req_ring_capacity = 0;
        uint32_t resp_ring_capacity = 0;
        if (!buf.tryReadString(decoded.name)
            || !buf.tryReadString(decoded.host)
            || !buf.tryReadUint16(decoded.port)
            || !buf.tryReadString(decoded.host_id)
            || !buf.tryReadString(decoded.instance_id)
            || !buf.tryReadUint32(req_ring_capacity)
            || !buf.tryReadUint32(resp_ring_capacity)
            || !buf.tryReadUint16(iface_count)) {
            return false;
        }
        decoded.shm_config.req_ring_capacity = req_ring_capacity;
        decoded.shm_config.resp_ring_capacity = resp_ring_capacity;
        decoded.interfaces.resize(iface_count);
        for (uint16_t i = 0; i < iface_count; ++i) {
            if (!deserializeInterfaceInfo(buf, decoded.interfaces[i])) {
                return false;
            }
        }
        info = decoded;
        return true;
    } catch (const std::bad_alloc&) {
        // 内存耗尽：返回失败而非崩溃（无异常传播原则）
        return false;
    }
}

/* @brief 序列化接口信息到 Buffer
 * @param[in]  info 接口信息
 * @param[out] buf  输出缓冲
 * @return true 成功；false 写入失败 */
bool serializeInterfaceInfo(const InterfaceInfo& info, Buffer& buf);

/* @brief 反序列化接口信息
 * @param[in]  buf  输入缓冲
 * @param[out] info 接口信息
 * @return true 成功；false 畸形/截断/内存不足，且不改动 info */
template <typename BufT>
bool deserializeInterfaceInfo(BufT& buf, InterfaceInfo& info) {
    try {
        InterfaceInfo decoded;
        uint16_t method_count = 0;
        if (!buf.tryReadUint32(decoded.interface_id)
            || !buf.tryReadString(decoded.name)
            || !buf.tryReadUint16(method_count)) {
            return false;
        }
        decoded.methods.resize(method_count);
        for (uint16_t i = 0; i < method_count; ++i) {
            if (!buf.tryReadUint32(decoded.methods[i].method_id)
                || !buf.tryReadString(decoded.methods[i].name)
                || !buf.tryReadString(decoded.methods[i].param_types)
                || !buf.tryReadString(decoded.methods[i].return_type)) {
                return false;
            }
            if (!buf.tryReadUint32(decoded.methods[i].idl_hash)) {
                return false;
            }
        }
        info = decoded;
        return true;
    } catch (const std::bad_alloc&) {
        // 内存耗尽：返回失败而非崩溃（无异常传播原则）
        return false;
    }
}

} // namespace omnibinder

#endif // OMNIBINDER_MESSAGE_H
