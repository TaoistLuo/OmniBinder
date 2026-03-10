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
 * @date        2026-02-11
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

    // 数据通道 - 接口调用
    MSG_INVOKE                = 0x0100,
    MSG_INVOKE_REPLY          = 0x0101,
    MSG_INVOKE_ONEWAY         = 0x0102,  // 单向调用，服务端不发送回复

    // 数据通道 - 广播
    MSG_BROADCAST             = 0x0110,
    MSG_SUBSCRIBE_BROADCAST   = 0x0111,  // 订阅者直连发布者后发送，携带 topic_id

    // 数据通道 - 保活
    MSG_PING                  = 0x0120,
    MSG_PONG                  = 0x0121,

    // 数据通道 - SHM 升级（同机 TCP 连接升级为 SHM）
    MSG_SHM_UPGRADE           = 0x0130,  // 客户端 -> 服务端，携带 shm_name
    MSG_SHM_UPGRADE_ACK       = 0x0131,  // 服务端 -> 客户端，确认升级成功
};

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
bool deserializeServiceInfo(Buffer& buf, ServiceInfo& info);

void serializeInterfaceInfo(const InterfaceInfo& info, Buffer& buf);
bool deserializeInterfaceInfo(Buffer& buf, InterfaceInfo& info);

} // namespace omnibinder

#endif // OMNIBINDER_MESSAGE_H
