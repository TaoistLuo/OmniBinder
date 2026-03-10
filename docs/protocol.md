# OmniBinder 通信协议规范

## 1. 概述

OmniBinder 使用自定义的二进制协议进行通信，所有通信（控制通道和数据通道）都使用统一的消息帧格式。

## 2. 消息帧格式

所有消息都遵循以下帧格式：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Magic (0x42494E44)                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            Version            |             Type              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                         Payload Data                          |
|                            (变长)                              |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 2.1 字段说明

| 字段 | 大小 | 说明 |
|------|------|------|
| Magic | 4 bytes | 固定值 `0x42494E44`（ASCII "BIND"），用于帧同步 |
| Version | 2 bytes | 协议版本号，当前为 `0x0001` |
| Type | 2 bytes | 消息类型，见下文 |
| Sequence | 4 bytes | 序列号，用于请求/响应匹配 |
| Length | 4 bytes | Payload 长度（字节数） |
| Payload | N bytes | 消息体，根据 Type 不同有不同结构 |

**字节序**：所有多字节字段使用**小端序（Little Endian）**

### 2.2 消息头 C++ 定义

```cpp
namespace omnibinder {

const uint32_t OMNI_MAGIC = 0x42494E44;  // "BIND"
const uint16_t OMNI_VERSION = 0x0001;

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;      // OMNI_MAGIC
    uint16_t version;    // OMNI_VERSION
    uint16_t type;       // MessageType
    uint32_t sequence;   // 序列号
    uint32_t length;     // payload 长度
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 16, "MessageHeader size mismatch");

} // namespace omnibinder
```

## 3. 消息类型定义

### 3.1 控制通道消息（ServiceManager <-> 服务）

| Type 值 | 名称 | 方向 | 说明 |
|---------|------|------|------|
| 0x0001 | MSG_REGISTER | Service -> SM | 服务注册请求 |
| 0x0002 | MSG_REGISTER_REPLY | SM -> Service | 服务注册响应 |
| 0x0003 | MSG_UNREGISTER | Service -> SM | 服务注销请求 |
| 0x0004 | MSG_UNREGISTER_REPLY | SM -> Service | 服务注销响应 |
| 0x0005 | MSG_HEARTBEAT | Service -> SM | 心跳请求 |
| 0x0006 | MSG_HEARTBEAT_ACK | SM -> Service | 心跳响应 |
| 0x0010 | MSG_LOOKUP | Service -> SM | 查询服务请求 |
| 0x0011 | MSG_LOOKUP_REPLY | SM -> Service | 查询服务响应 |
| 0x0012 | MSG_LIST_SERVICES | Service -> SM | 列出所有服务请求 |
| 0x0013 | MSG_LIST_SERVICES_REPLY | SM -> Service | 列出所有服务响应 |
| 0x0014 | MSG_QUERY_INTERFACES | Service -> SM | 查询服务接口请求 |
| 0x0015 | MSG_QUERY_INTERFACES_REPLY | SM -> Service | 查询服务接口响应 |
| 0x0020 | MSG_SUBSCRIBE_SERVICE | Service -> SM | 订阅服务死亡通知 |
| 0x0021 | MSG_SUBSCRIBE_SERVICE_REPLY | SM -> Service | 订阅响应 |
| 0x0022 | MSG_UNSUBSCRIBE_SERVICE | Service -> SM | 取消订阅 |
| 0x0023 | MSG_DEATH_NOTIFY | SM -> Service | 服务死亡通知 |
| 0x0030 | MSG_PUBLISH_TOPIC | Service -> SM | 声明发布话题 |
| 0x0031 | MSG_PUBLISH_TOPIC_REPLY | SM -> Service | 声明响应 |
| 0x0032 | MSG_SUBSCRIBE_TOPIC | Service -> SM | 订阅话题 |
| 0x0033 | MSG_SUBSCRIBE_TOPIC_REPLY | SM -> Service | 订阅响应 |
| 0x0034 | MSG_TOPIC_PUBLISHER_NOTIFY | SM -> Service | 通知订阅者发布者地址 |
| 0x0035 | MSG_UNPUBLISH_TOPIC | Service -> SM | 取消发布话题 |
| 0x0036 | MSG_UNSUBSCRIBE_TOPIC | Service -> SM | 取消订阅话题 |

### 3.2 数据通道消息（服务 <-> 服务）

| Type 值 | 名称 | 方向 | 说明 |
|---------|------|------|------|
| 0x0100 | MSG_INVOKE | Client -> Server | 接口调用请求 |
| 0x0101 | MSG_INVOKE_REPLY | Server -> Client | 接口调用响应 |
| 0x0102 | MSG_INVOKE_ONEWAY | Client -> Server | 单向调用（服务端不发送回复） |
| 0x0110 | MSG_BROADCAST | Publisher -> Subscriber | 广播消息 |
| 0x0111 | MSG_SUBSCRIBE_BROADCAST | Subscriber -> Publisher | 订阅者直连发布者后发送，携带 topic_id |
| 0x0120 | MSG_PING | Any | 连接保活 |
| 0x0121 | MSG_PONG | Any | 保活响应 |

### 3.3 C++ 枚举定义

```cpp
namespace omnibinder {

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
    MSG_SUBSCRIBE_BROADCAST   = 0x0111,  // 订阅者直连发布者后发送

    // 数据通道 - 保活
    MSG_PING                  = 0x0120,
    MSG_PONG                  = 0x0121,
};

} // namespace omnibinder
```

## 4. Payload 结构定义

### 4.1 服务注册（MSG_REGISTER）

服务初始化时同时创建 TCP 监听和共享内存，然后将所有信息注册到 SM。

```
+------------------+------------------+------------------+
| service_name_len | service_name     | host_len         |
| (4 bytes)        | (N bytes)        | (4 bytes)        |
+------------------+------------------+------------------+
| host             | port             | host_id_len      |
| (N bytes)        | (2 bytes)        | (4 bytes)        |
+------------------+------------------+------------------+
| host_id          | shm_name_len     | shm_name         |
| (N bytes)        | (4 bytes)        | (N bytes)        |
+------------------+------------------+------------------+
| interface_count  | interfaces...    |
| (4 bytes)        | (变长)            |
+------------------+------------------+

shm_name: 服务创建的共享内存名称（如 "/binder_ServiceA"）。
          同机客户端通过此名称打开 SHM 进行通信。
          空字符串表示该服务不支持 SHM。

interfaces 数组中每个元素:
+------------------+------------------+------------------+
| interface_id     | name_len         | name             |
| (4 bytes)        | (4 bytes)        | (N bytes)        |
+------------------+------------------+------------------+
| method_count     | methods...       |
| (4 bytes)        | (变长)            |
+------------------+------------------+

methods 数组中每个元素:
+------------------+------------------+------------------+
| method_id        | name_len         | name             |
| (4 bytes)        | (4 bytes)        | (N bytes)        |
+------------------+------------------+------------------+
```

### 4.2 服务注册响应（MSG_REGISTER_REPLY）

```
+------------------+
| service_handle   |
| (4 bytes)        |
+------------------+

service_handle:
  非零值 = 成功，返回服务句柄
  0 (INVALID_HANDLE) = 失败
```

### 4.3 心跳（MSG_HEARTBEAT）

```
+------------------+------------------+
| service_name_len | service_name     |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.4 心跳响应（MSG_HEARTBEAT_ACK）

```
+------------------+------------------+
| service_name_len | service_name     |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.5 服务查询（MSG_LOOKUP）

```
+------------------+------------------+
| service_name_len | service_name     |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.6 服务查询响应（MSG_LOOKUP_REPLY）

客户端通过此响应获取目标服务的完整信息，包括 SHM 名称，从而判断是否可以使用共享内存通信。

```
+------------------+----------------------------------------------+
| found            | [if found: serialized ServiceInfo]           |
| (1 byte bool)    | (使用与 MSG_REGISTER 相同的序列化格式)          |
+------------------+----------------------------------------------+

found:
  1 = 找到服务，后续为完整的 ServiceInfo 序列化数据
      （格式与 MSG_REGISTER payload 相同：service_name, host,
       port, host_id, shm_name, interfaces[]）
  0 = 服务不存在，无后续数据

shm_name: 服务的共享内存名称。客户端判断逻辑：
  if (本机 host_id == 响应中的 host_id) && (shm_name 非空):
      使用 SHM 连接（打开已有共享内存，获取 slot）
      通过 UDS (/tmp/binder_<shm_name>.sock) 交换 eventfd
  else:
      使用 TCP 连接（host:port）
```

### 4.7 列出所有服务（MSG_LIST_SERVICES）

```
（无 payload）
```

### 4.8 列出所有服务响应（MSG_LIST_SERVICES_REPLY）

```
+------------------+------------------+
| service_count    | services...      |
| (4 bytes uint32) | (变长)            |
+------------------+------------------+

services 数组中每个元素为完整的 serialized ServiceInfo，
格式与 MSG_REGISTER payload 相同（service_name, host, port,
host_id, shm_name, interfaces[]）。
```

### 4.9 订阅服务死亡通知（MSG_SUBSCRIBE_SERVICE）

```
+------------------+------------------+
| service_name_len | service_name     |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.10 死亡通知（MSG_DEATH_NOTIFY）

```
+------------------+------------------+
| service_name_len | service_name     |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.11 声明发布话题（MSG_PUBLISH_TOPIC）

```
+------------------+------------------+
| topic_name_len   | topic_name       |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.12 订阅话题（MSG_SUBSCRIBE_TOPIC）

```
+------------------+------------------+
| topic_name_len   | topic_name       |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+
```

### 4.13 话题发布者通知（MSG_TOPIC_PUBLISHER_NOTIFY）

```
+------------------+------------------+------------------+
| topic_name_len   | topic_name       | host_len         |
| (4 bytes)        | (N bytes)        | (4 bytes)        |
+------------------+------------------+------------------+
| host             | port             | host_id_len      |
| (N bytes)        | (2 bytes)        | (4 bytes)        |
+------------------+------------------+------------------+
| host_id          |
| (N bytes)        |
+------------------+
```

### 4.14 接口调用（MSG_INVOKE）

```
+------------------+------------------+------------------+
| interface_id     | method_id        | payload_len      |
| (4 bytes)        | (4 bytes)        | (4 bytes)        |
+------------------+------------------+------------------+
| payload          |
| (N bytes)        |
+------------------+

interface_id: 接口的唯一标识（由 IDL 编译器生成的哈希值）
method_id:    方法的唯一标识（由 IDL 编译器生成的哈希值）
payload:      方法参数的序列化数据
```

### 4.15 接口调用响应（MSG_INVOKE_REPLY）

```
+------------------+------------------+------------------+
| status           | payload_len      | payload          |
| (4 bytes)        | (4 bytes)        | (N bytes)        |
+------------------+------------------+------------------+

status:
  0 = 成功
  1 = 接口不存在
  2 = 方法不存在
  3 = 参数错误
  4 = 执行异常
```

### 4.16 广播消息（MSG_BROADCAST）

```
+------------------+------------------+------------------+
| topic_id         | payload_len      | payload          |
| (4 bytes)        | (4 bytes)        | (N bytes)        |
+------------------+------------------+------------------+

topic_id: 话题的唯一标识（由 IDL 编译器生成的哈希值）
payload:  广播数据的序列化内容
```

## 5. 序列化规则

### 5.1 基础类型序列化

所有基础类型使用小端序：

| 类型 | 序列化方式 |
|------|-----------|
| bool | 1 byte, 0=false, 1=true |
| int8/uint8 | 1 byte |
| int16/uint16 | 2 bytes, little-endian |
| int32/uint32 | 4 bytes, little-endian |
| int64/uint64 | 8 bytes, little-endian |
| float32 | 4 bytes, IEEE 754 |
| float64 | 8 bytes, IEEE 754 |

### 5.2 字符串序列化

```
+------------------+------------------+
| length           | data             |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+

- length: 字符串字节长度（不含结尾 \0）
- data: UTF-8 编码的字符串内容（不含结尾 \0）
```

### 5.3 字节数组序列化

```
+------------------+------------------+
| length           | data             |
| (4 bytes)        | (N bytes)        |
+------------------+------------------+

- length: 字节数组长度
- data: 原始字节数据
```

### 5.4 结构体序列化

结构体按字段定义顺序依次序列化，无对齐填充：

```
struct Example {
    int32  a;
    string b;
    float64 c;
}

序列化后:
+----------+----------+----------+----------+
| a        | b.len    | b.data   | c        |
| (4 bytes)| (4 bytes)| (N bytes)| (8 bytes)|
+----------+----------+----------+----------+
```

### 5.5 数组序列化

```
+------------------+------------------+
| count            | elements...      |
| (4 bytes)        | (变长)            |
+------------------+------------------+

- count: 元素个数
- elements: 每个元素按其类型序列化
```

## 6. 错误码定义

```cpp
namespace omnibinder {

enum class ErrorCode : int32_t {
    OK                      = 0,      // 成功
    
    // 通用错误 (-1 ~ -99)
    ERR_UNKNOWN             = -1,     // 未知错误
    ERR_INVALID_PARAM       = -2,     // 参数无效
    ERR_OUT_OF_MEMORY       = -3,     // 内存不足
    ERR_TIMEOUT             = -4,     // 超时
    ERR_NOT_INITIALIZED     = -5,     // 未初始化
    ERR_INTERNAL            = -8,     // 内部错误
    
    // 网络错误 (-100 ~ -199)
    ERR_CONNECT_FAILED      = -100,   // 连接失败
    ERR_CONNECTION_CLOSED   = -101,   // 连接已关闭
    ERR_SEND_FAILED         = -102,   // 发送失败
    ERR_RECV_FAILED         = -103,   // 接收失败
    ERR_PROTOCOL_ERROR      = -104,   // 协议错误
    ERR_BIND_FAILED         = -106,   // 绑定失败
    ERR_LISTEN_FAILED       = -107,   // 监听失败
    ERR_ACCEPT_FAILED       = -108,   // 接受连接失败
    
    // 服务错误 (-200 ~ -299)
    ERR_SERVICE_NOT_FOUND   = -200,   // 服务不存在
    ERR_SERVICE_EXISTS      = -201,   // 服务已存在
    ERR_SERVICE_OFFLINE     = -202,   // 服务离线
    ERR_INTERFACE_NOT_FOUND = -203,   // 接口不存在
    ERR_METHOD_NOT_FOUND    = -204,   // 方法不存在
    ERR_INVOKE_FAILED       = -205,   // 调用失败
    ERR_UNREGISTER_FAILED   = -207,   // 注销失败
    
    // 话题错误 (-300 ~ -399)
    ERR_TOPIC_NOT_FOUND     = -300,   // 话题不存在
    ERR_TOPIC_EXISTS        = -301,   // 话题已存在
    ERR_NOT_SUBSCRIBED      = -302,   // 未订阅
    
    // 传输层错误 (-400 ~ -499)
    ERR_TRANSPORT_INIT      = -400,   // 传输层初始化失败
    ERR_SHM_CREATE          = -401,   // 共享内存创建失败
    ERR_SHM_ATTACH          = -402,   // 共享内存附加失败
    ERR_SHM_TIMEOUT         = -404,   // 共享内存超时
};

} // namespace omnibinder
```

## 6.5 SHM eventfd 交换协议

当客户端选择 SHM 通信时，需要通过 UDS 与服务端交换 eventfd 文件描述符，
以实现事件驱动的跨进程通知。

### 交换流程

```
Client                                     Server (UDS listener)
   |                                             |
   |  [本地] 打开 SHM, 获取 slot_id              |
   |                                             |
   |-------- UDS Connect ----------------------->|
   |         /tmp/binder_<shm_name>.sock         |
   |                                             |
   |-------- 发送 slot_id (4 bytes) ------------>|
   |                                             |
   |<------- SCM_RIGHTS -------------------------|
   |         req_eventfd + resp_eventfd          |
   |         (2 个 fd 通过 ancillary data 传递)   |
   |                                             |
   |-------- UDS 断开 -------------------------->|
   |                                             |
   |  [本地] 注册 resp_eventfd 到 EventLoop       |
   |                                             |
```

### eventfd 使用约定

- 服务端创建 1 个 `req_eventfd`（所有客户端共享）和 32 个 `resp_eventfd`（每 slot 一个）
- 客户端写入 SHM RequestQueue 后，调用 `eventfd_write(req_eventfd, 1)` 唤醒服务端
- 服务端写入 SHM ResponseSlot 后，调用 `eventfd_write(resp_eventfds[slot_id], 1)` 唤醒对应客户端
- Topic 广播：服务端写入 SHM 后，对所有已连接客户端的 `resp_eventfd` 发送通知
- eventfd 使用 `EFD_NONBLOCK` 标志创建，注册到 epoll 的 `EPOLLIN` 事件

### UDS 路径命名

```
/tmp/binder_<shm_name>.sock

示例: SHM 名称为 "/binder_ServiceA"
      UDS 路径为 "/tmp/binder_binder_ServiceA.sock"
```

## 7. 通信时序

### 7.1 服务注册时序

```
Service                                    ServiceManager
   |                                             |
   |  [本地] 创建 TCP 监听 (端口自动分配)           |
   |  [本地] 创建 SHM "/binder_<name>"            |
   |  [本地] 创建 eventfd (req + resp x 32)       |
   |  [本地] 创建 UDS 监听 "/tmp/binder_<name>.sock"|
   |                                             |
   |-------- TCP Connect -----------------------|
   |                                             |
   |-------- MSG_REGISTER --------------------->|
   |         {name, host, port, host_id,        |
   |          shm_name, interfaces[]}            |
   |                                             |
   |<------- MSG_REGISTER_REPLY ----------------|
   |         {status=OK, handle=1}              |
   |                                             |
   |-------- MSG_HEARTBEAT -------------------->|  (每3秒)
   |<------- MSG_HEARTBEAT_ACK -----------------|
   |                                             |
```

### 7.2 服务调用时序

```
ServiceB                ServiceManager              ServiceA
   |                          |                          |
   |--- MSG_LOOKUP ---------->|                          |
   |    {name="ServiceA"}     |                          |
   |                          |                          |
   |<-- MSG_LOOKUP_REPLY -----|                          |
   |    {host, port, host_id, |                          |
   |     shm_name,            |                          |
   |     interfaces[]}        |                          |
   |                          |                          |
   |  [判断] host_id 相同？                               |
   |    是 -> 打开 SHM(shm_name), 获取 slot              |
   |         UDS 连接 /tmp/binder_<name>.sock            |
   |         发送 slot_id                                |
   |         接收 req_eventfd + resp_eventfd (SCM_RIGHTS)|
   |         UDS 断开                                    |
   |         注册 resp_eventfd 到 EventLoop              |
   |    否 -> TCP 连接 host:port                         |
   |                          |                          |
   |========= 直接建立连接 (TCP 或 SHM) ================>|
   |                          |                          |
   |--- MSG_INVOKE ------------------------------------ >|
   |    {interface_id, method_id, params}               |
   |                          |                          |
   |<-- MSG_INVOKE_REPLY ---------------------------------|
   |    {status, result}      |                          |
```

### 7.3 广播订阅时序

```
ServiceB              ServiceManager              ServiceA
   |                        |                          |
   |                        |<-- MSG_PUBLISH_TOPIC ----|
   |                        |    {topic="sensor"}      |
   |                        |                          |
   |-- MSG_SUBSCRIBE_TOPIC->|                          |
   |   {topic="sensor"}     |                          |
   |                        |                          |
   |<- MSG_TOPIC_PUBLISHER--|                          |
   |   _NOTIFY              |                          |
   |   {topic, A's address} |                          |
   |                        |                          |
   |========= 直接建立连接 ============================>|
   |                        |                          |
   |<-- MSG_BROADCAST ----------------------------------|
   |    {topic_id, data}    |                          |
```

## 8. 心跳与超时机制

### 8.1 心跳参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| HEARTBEAT_INTERVAL | 3000 ms | 心跳发送间隔 |
| HEARTBEAT_TIMEOUT | 10000 ms | 心跳超时时间 |
| MAX_MISSED_HEARTBEATS | 3 | 最大允许丢失心跳次数 |

### 8.2 超时判定

```
if (当前时间 - 最后心跳时间 > HEARTBEAT_TIMEOUT):
    missed_count++
    if (missed_count >= MAX_MISSED_HEARTBEATS):
        判定服务死亡
        触发死亡通知
```

## 9. 版本兼容性

### 9.1 版本号规则

- 主版本号变化：协议不兼容
- 次版本号变化：向后兼容（新增字段在末尾）

### 9.2 版本协商

当前协议版本（v1）不进行连接级版本协商，使用固定版本号。
