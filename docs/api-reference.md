# OmniBinder API 参考

## 1. 概述

本文档描述 libomnibinder 库提供的公共 API。所有 API 位于 `omnibinder` 命名空间下。

## 2. 核心类

### 2.1 Buffer（序列化缓冲区）

`include/omnibinder/buffer.h`

Buffer 是序列化/反序列化的核心工具类，提供自动扩容的字节缓冲区。

```cpp
namespace omnibinder {

class Buffer {
public:
    Buffer();
    explicit Buffer(size_t initial_capacity);
    Buffer(const uint8_t* data, size_t length);
    ~Buffer();

    // 移动语义
    Buffer(Buffer&& other);
    Buffer& operator=(Buffer&& other);

    // ---- 写入（序列化）----

    void writeBool(bool value);
    void writeInt8(int8_t value);
    void writeUint8(uint8_t value);
    void writeInt16(int16_t value);
    void writeUint16(uint16_t value);
    void writeInt32(int32_t value);
    void writeUint32(uint32_t value);
    void writeInt64(int64_t value);
    void writeUint64(uint64_t value);
    void writeFloat32(float value);
    void writeFloat64(double value);
    void writeString(const std::string& value);
    void writeBytes(const void* data, size_t length);
    void writeBytes(const std::vector<uint8_t>& data);
    void writeRaw(const void* data, size_t length);

    // ---- 读取（反序列化）----

    bool readBool();
    int8_t readInt8();
    uint8_t readUint8();
    int16_t readInt16();
    uint16_t readUint16();
    int32_t readInt32();
    uint32_t readUint32();
    int64_t readInt64();
    uint64_t readUint64();
    float readFloat32();
    double readFloat64();
    std::string readString();
    std::vector<uint8_t> readBytes();
    void readRaw(void* buf, size_t length);

    // ---- 缓冲区管理 ----

    // 获取已写入数据的指针
    const uint8_t* data() const;

    // 获取可写数据的指针
    uint8_t* mutableData();

    // 获取已写入数据的大小
    size_t size() const;

    // 获取缓冲区总容量
    size_t capacity() const;

    // 预留容量
    void reserve(size_t capacity);

    // 调整大小
    void resize(size_t new_size);

    // 重置读写位置
    void reset();

    // 清空缓冲区
    void clear();

    // 获取当前读取位置
    size_t readPosition() const;

    // 设置读取位置
    void setReadPosition(size_t pos);

    // 获取当前写入位置
    size_t writePosition() const;

    // 设置写入位置
    void setWritePosition(size_t pos);

    // 检查是否还有数据可读
    bool hasRemaining() const;

    // 剩余可读字节数
    size_t remaining() const;

    // 从原始数据构造（用于接收到的数据）
    void assign(const uint8_t* data, size_t length);

private:
    // 禁止拷贝
    Buffer(const Buffer&);
    Buffer& operator=(const Buffer&);
};

} // namespace omnibinder
```

### 2.2 OmniRuntime（客户端核心类）

`include/omnibinder/runtime.h`

OmniRuntime 是用户使用 OmniBinder 的主要入口，负责与 ServiceManager 通信、
注册服务、发现服务以及管理运行时连接。

#### 线程模型说明

- `OmniRuntime` 对外提供**线程安全公共 API**
- 内部核心状态由单个 owner event-loop 串行驱动
- 非 owner 线程发起的同步调用，会投递到 owner event-loop 执行并等待结果
- `TopicCallback` 与 `DeathCallback` 默认在 owner event-loop 线程执行
- `run()` / `pollOnce()` / `stop()` 的并发协作规则见 [线程模型文档](threading-model.md)

#### 并发语义摘要

| 方法类别 | 方法 | 行为说明 |
|---|---|---|
| 持续驱动 | `run()` | 线程安全，但同一时刻只允许一个线程持续驱动 event-loop |
| 单次驱动 | `pollOnce()` | 线程安全，但不应与另一个线程中的 `run()` 或 `pollOnce()` 并发驱动同一实例 |
| 停止 | `stop()` | 可从任意线程并发调用 |
| 同步阻塞 | `lookupService()`、`listServices()`、`queryInterfaces()`、`invoke()`、`subscribeServiceDeath()`、`publishTopic()`、`subscribeTopic()` | 可从任意线程安全调用，但内部发送/等待/状态写入由 owner event-loop 串行处理 |
| 异步/配置 | `invokeOneWay()`、`broadcast()`、`setHeartbeatInterval()`、`setDefaultTimeout()` | 可从任意线程安全调用，内部状态更新在 owner event-loop 串行处理 |
| 回调 | `TopicCallback`、`DeathCallback` | 默认在 owner event-loop 线程执行 |

#### 超时与阻塞语义

- `invoke()` 提供 `timeout_ms` 参数
- `timeout_ms == 0` 时使用默认超时，默认值可通过 `setDefaultTimeout()` 修改
- 同步等待由运行时 deadline 控制，超时返回错误码 `ERR_TIMEOUT`
- 正常调用路径不会无限期阻塞等待 reply
- 但不建议在 owner event-loop 回调中再发起同步阻塞 API，因为那会进入嵌套等待语义

```cpp
namespace omnibinder {

// 服务信息
struct ShmConfig {
    size_t req_ring_capacity;    // 请求 ring 容量，0 表示使用默认值
    size_t resp_ring_capacity;   // 响应 ring 容量，0 表示使用默认值
};

struct ServiceInfo {
    std::string name;           // 服务名
    std::string host;           // 主机地址
    uint16_t    port;           // TCP 端口
    std::string host_id;        // 主机标识
    std::string shm_name;       // 共享内存名称（同机通信用，空表示不支持 SHM）
    ShmConfig   shm_config;     // SHM 容量配置
    std::vector<InterfaceInfo> interfaces;  // 接口列表
};

struct RuntimeStats {
    uint64_t total_rpc_calls;
    uint64_t total_rpc_success;
    uint64_t total_rpc_failures;
    uint64_t total_rpc_timeouts;
    uint64_t connection_errors;
    uint64_t sm_reconnect_attempts;
    uint64_t sm_reconnect_successes;
    uint32_t active_connections;
    uint32_t tcp_connections;
    uint32_t shm_connections;
};

// 接口信息
struct InterfaceInfo {
    uint32_t    interface_id;   // 接口ID
    std::string name;           // 接口名
    std::vector<MethodInfo> methods;  // 方法列表
};

// 方法信息
struct MethodInfo {
    uint32_t    method_id;      // 方法ID
    std::string name;           // 方法名
};

// 死亡通知回调
typedef std::function<void(const std::string& service_name)> DeathCallback;

// 话题消息回调
typedef std::function<void(uint32_t topic_id, const Buffer& data)> TopicCallback;

class OmniRuntime {
public:
    OmniRuntime();
    ~OmniRuntime();

    // ---- 初始化和生命周期 ----

    // 初始化客户端，连接到 ServiceManager
    // sm_host: ServiceManager 地址
    // sm_port: ServiceManager 端口（默认 9900）
    // 返回: 0 成功，负值为错误码
    int init(const std::string& sm_host, uint16_t sm_port = 9900);

    // 启动事件循环（阻塞，直到 stop() 被调用）
    // 线程安全；同一时刻只允许一个线程持续驱动该 OmniRuntime event-loop
    void run();

    // 停止事件循环
    // 可从任意线程安全调用
    void stop();

    // 检查是否正在运行
    bool isRunning() const;

    // 处理一次事件（非阻塞，用于集成到用户自己的事件循环）
    // 线程安全，但不应与另一个线程中的 run()/pollOnce() 共同驱动同一实例
    // timeout_ms: 最大等待时间，0 表示不等待
    void pollOnce(int timeout_ms = 0);

    // ---- 服务注册 ----

    // 注册本地服务
    // 内部会同时创建 TCP 监听和共享内存，然后将信息注册到 SM
    // service: 服务实例（继承自 Service 基类）
    // 返回: 0 成功，负值为错误码
    int registerService(Service* service);

    // 注销本地服务
    int unregisterService(Service* service);

    // ---- 服务发现 ----

    // 查询服务信息
    // service_name: 要查询的服务名
    // info: 输出参数，服务信息
    // 可从任意线程安全调用；同步等待与缓存更新由 owner event-loop 串行处理
    // 返回: 0 成功，负值为错误码
    int lookupService(const std::string& service_name, ServiceInfo& info);

    // 列出所有在线服务
    // services: 输出参数，服务列表
    // 可从任意线程安全调用；内部控制面交互由 owner event-loop 串行处理
    // 返回: 0 成功，负值为错误码
    int listServices(std::vector<ServiceInfo>& services);

    // 查询服务的接口信息
    // service_name: 服务名
    // interfaces: 输出参数，接口列表
    // 可从任意线程安全调用；内部控制面交互由 owner event-loop 串行处理
    // 返回: 0 成功，负值为错误码
    int queryInterfaces(const std::string& service_name,
                        std::vector<InterfaceInfo>& interfaces);

    // ---- 服务调用 ----

    // 调用远程服务方法（同步阻塞）
    // service_name: 目标服务名
    // interface_id: 接口ID
    // method_id: 方法ID
    // request: 请求数据
    // response: 响应数据
    // timeout_ms: 超时时间（毫秒），0 表示使用默认超时
    // 可从任意线程安全调用；调用线程可阻塞，但内部发送/等待/状态更新由 owner event-loop 串行处理
    // 超时语义：timeout_ms == 0 使用默认超时；超时返回 ERR_TIMEOUT，不会无限阻塞
    // 返回: 0 成功，负值为错误码
    int invoke(const std::string& service_name,
               uint32_t interface_id,
               uint32_t method_id,
               const Buffer& request,
               Buffer& response,
               uint32_t timeout_ms = 0);

    // 单向调用（不等待响应）
    // 可从任意线程安全调用；内部发送与连接状态更新由 owner event-loop 串行处理
    int invokeOneWay(const std::string& service_name,
                     uint32_t interface_id,
                     uint32_t method_id,
                     const Buffer& request);

    // ---- 死亡通知 ----

    // 订阅服务死亡通知
    // service_name: 要监控的服务名
    // callback: 死亡时的回调函数（默认在 owner event-loop 线程执行）
    // 返回: 0 成功，负值为错误码
    int subscribeServiceDeath(const std::string& service_name,
                              const DeathCallback& callback);

    // 取消订阅
    int unsubscribeServiceDeath(const std::string& service_name);

    // ---- 话题/广播 ----

    // 声明发布话题
    // topic_name: 话题名
    // 返回: 0 成功，负值为错误码
    int publishTopic(const std::string& topic_name);

    // 广播话题数据（发送给所有订阅者）
    // topic_id: 话题ID
    // data: 广播数据
    // 返回: 0 成功，负值为错误码
    int broadcast(uint32_t topic_id, const Buffer& data);

    // 订阅话题
    // topic_name: 话题名
    // callback: 收到广播时的回调函数（默认在 owner event-loop 线程执行）
    // 返回: 0 成功，负值为错误码
    int subscribeTopic(const std::string& topic_name,
                       const TopicCallback& callback);

    // 取消订阅话题
    int unsubscribeTopic(const std::string& topic_name);

    // ---- 配置 ----

    // 设置心跳间隔（毫秒）
    void setHeartbeatInterval(uint32_t interval_ms);

    // 设置默认调用超时（毫秒）
    void setDefaultTimeout(uint32_t timeout_ms);

    // 获取本机 host_id
    const std::string& hostId() const;

    // 获取运行时统计信息
    int getStats(RuntimeStats& stats);

    // 重置运行时统计计数器
    void resetStats();

private:
    // 禁止拷贝
    OmniRuntime(const OmniRuntime&);
    OmniRuntime& operator=(const OmniRuntime&);

    class Impl;
    Impl* impl_;
};

} // namespace omnibinder
```

#### SHM 配置说明

- 当前默认 SHM 容量为：
  - `req_ring_capacity = 4KB`
  - `resp_ring_capacity = 4KB`
- 默认值面向嵌入式控制类 RPC 场景，小请求、小响应优先
- 对于某些需要更大请求或响应缓冲区的服务，可以在服务端显式配置，并通过服务注册信息传播到客户端

#### RuntimeStats 说明

`RuntimeStats` 提供最小可行运行时观测能力，包含：

- `total_rpc_calls`：RPC / one-way 总调用数
- `total_rpc_success`：成功调用数
- `total_rpc_failures`：失败调用数
- `total_rpc_timeouts`：超时调用数
- `connection_errors`：数据面连接/发送错误累计次数
- `sm_reconnect_attempts`：ServiceManager 重连尝试次数
- `sm_reconnect_successes`：ServiceManager 重连成功次数
- `active_connections`：当前活跃数据面连接数
- `tcp_connections`：当前 TCP 数据面连接数
- `shm_connections`：当前 SHM 数据面连接数

典型用法：

```cpp
omnibinder::RuntimeStats stats;
if (runtime.getStats(stats) == 0) {
    printf("rpc=%lu success=%lu fail=%lu active=%u\n",
           static_cast<unsigned long>(stats.total_rpc_calls),
           static_cast<unsigned long>(stats.total_rpc_success),
           static_cast<unsigned long>(stats.total_rpc_failures),
           stats.active_connections);
}
```

### 2.3 Service（服务基类）

`include/omnibinder/service.h`

Service 是所有 Stub / 业务服务的基类。当前除了服务名、端口、接口元数据之外，也支持声明服务级 SHM 配置。

```cpp
namespace omnibinder {

class Service {
public:
    explicit Service(const std::string& name);
    virtual ~Service();

    const std::string& name() const;
    uint16_t port() const;
    void setPort(uint16_t p);

    void setShmConfig(const ShmConfig& config);
    ShmConfig shmConfig() const;

    virtual const char* serviceName() const = 0;
    virtual const InterfaceInfo& interfaceInfo() const = 0;
};

} // namespace omnibinder
```

#### 服务级 SHM 配置示例

```cpp
class SensorService : public omnibinder::Service {
public:
    SensorService() : Service("SensorService") {
        setShmConfig(omnibinder::ShmConfig(8 * 1024, 16 * 1024));
    }
};
```

说明：

- 如果不调用 `setShmConfig()`，则服务使用默认 `4KB / 4KB`
- 如果调用 `setShmConfig()`，该配置会：
  - 用于服务端创建 SHM transport
  - 写入 `ServiceInfo.shm_config`
  - 在客户端 lookup 后用于创建客户端侧 SHM transport

### 2.3 Service（服务基类）

`include/omnibinder/service.h`

所有服务端骨架（Stub）的基类。IDL 生成的 Stub 类继承此基类。

```cpp
namespace omnibinder {

class Service {
public:
    explicit Service(const std::string& name);
    virtual ~Service();

    // 获取服务名
    const std::string& name() const;

    // 获取服务监听端口（注册后由框架分配）
    uint16_t port() const;

    // 获取接口信息（由 IDL 生成的子类实现）
    virtual const InterfaceInfo& interfaceInfo() const = 0;

    // 获取服务名（由 IDL 生成的子类实现）
    virtual const char* serviceName() const = 0;

protected:
    // 处理接口调用请求（由 IDL 生成的子类实现）
    // method_id: 方法ID
    // request: 请求数据
    // response: 响应数据（需要填充）
    virtual void onInvoke(uint32_t method_id,
                          const Buffer& request,
                          Buffer& response) = 0;

    // 服务启动时回调（可选覆盖）
    virtual void onStart();

    // 服务停止时回调（可选覆盖）
    virtual void onStop();

    // 新客户端连接时回调（可选覆盖）
    virtual void onClientConnected(const std::string& client_info);

    // 客户端断开时回调（可选覆盖）
    virtual void onClientDisconnected(const std::string& client_info);

    // 获取关联的 OmniRuntime（用于广播等操作）
    OmniRuntime* runtime() const;

    friend class OmniRuntime;

private:
    std::string name_;
    uint16_t port_;
    OmniRuntime* runtime_;
};

} // namespace omnibinder
```

### 2.4 ITransport（传输层接口）

`include/omnibinder/transport.h`

传输层抽象接口。SHM 通过 eventfd 事件驱动，TCP 通过 socket fd 事件驱动，均注册到 EventLoop。

```cpp
namespace omnibinder {

// 传输层类型
enum class TransportType {
    TCP,        // TCP 传输
    SHM,        // 共享内存传输
};

// 连接状态
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR,
};

// 传输层服务端接口（监听连接）
class ITransportServer {
public:
    virtual ~ITransportServer() {}

    // 开始监听
    // host: 监听地址（"0.0.0.0" 表示所有接口）
    // port: 监听端口（0 表示自动分配）
    // 返回: 实际监听的端口，负值为错误
    virtual int listen(const std::string& host, uint16_t port) = 0;

    // 停止监听
    virtual void close() = 0;

    // 获取监听端口
    virtual uint16_t port() const = 0;

    // 获取文件描述符（用于 EventLoop 注册）
    virtual int fd() const = 0;

    // 接受新连接
    // 返回: 新连接的传输实例，失败返回 NULL
    virtual ITransport* accept() = 0;
};

// 传输层客户端接口（单个连接）
class ITransport {
public:
    virtual ~ITransport() {}

    // 连接到远端
    virtual int connect(const std::string& host, uint16_t port) = 0;

    // 发送数据
    virtual int send(const uint8_t* data, size_t length) = 0;

    // 接收数据（非阻塞）
    // buf: 接收缓冲区
    // buf_size: 缓冲区大小
    // 返回: 实际接收的字节数，0 表示无数据，负值为错误
    virtual int recv(uint8_t* buf, size_t buf_size) = 0;

    // 关闭连接
    virtual void close() = 0;

    // 获取连接状态
    virtual ConnectionState state() const = 0;

    // 获取文件描述符（用于 EventLoop 注册）
    virtual int fd() const = 0;

    // 获取传输类型
    virtual TransportType type() const = 0;
};

// 传输层工厂（用于自动选择传输层）
// 注意：TransportFactory 为内部 API（位于 src/transport/transport_factory.h），
// 不在公共头文件中暴露。以下为其接口说明，供内部开发参考。
class TransportFactory {
public:
    // 获取单例
    static TransportFactory& instance();

    // 根据条件创建传输实例
    // local_host_id: 本机标识
    // remote_host_id: 远端标识
    // 如果同机（host_id 相同），创建 SHM 传输
    // 如果跨机（host_id 不同），创建 TCP 传输
    // 返回: 合适的传输实例
    ITransport* createTransport(const std::string& local_host_id,
                                const std::string& remote_host_id);

    // 创建服务端传输
    // TCP: 创建 TcpTransportServer
    // SHM: 服务端 SHM 应直接通过 ShmTransport(name, true) 创建
    ITransportServer* createServer(TransportType type);

    // 强制创建指定类型的传输
    ITransport* createTransport(TransportType type);

    // 判断是否同机
    static bool isSameMachine(const std::string& local_host_id,
                              const std::string& remote_host_id);

private:
    TransportFactory();
};

} // namespace omnibinder
```

## 3. 错误码

`include/omnibinder/error.h`

```cpp
namespace omnibinder {

enum class ErrorCode : int32_t {
    // 成功
    OK                      = 0,

    // 通用错误 (1-99)
    ERR_UNKNOWN             = -1,
    ERR_INVALID_PARAM       = -2,
    ERR_OUT_OF_MEMORY       = -3,
    ERR_TIMEOUT             = -4,
    ERR_NOT_INITIALIZED     = -5,
    ERR_ALREADY_INITIALIZED = -6,
    ERR_NOT_SUPPORTED       = -7,
    ERR_INTERNAL            = -8,

    // 网络错误 (100-199)
    ERR_CONNECT_FAILED      = -100,
    ERR_CONNECTION_CLOSED   = -101,
    ERR_SEND_FAILED         = -102,
    ERR_RECV_FAILED         = -103,
    ERR_PROTOCOL_ERROR      = -104,
    ERR_SM_UNREACHABLE      = -105,
    ERR_BIND_FAILED         = -106,
    ERR_LISTEN_FAILED       = -107,
    ERR_ACCEPT_FAILED       = -108,

    // 服务错误 (200-299)
    ERR_SERVICE_NOT_FOUND   = -200,
    ERR_SERVICE_EXISTS      = -201,
    ERR_SERVICE_OFFLINE     = -202,
    ERR_INTERFACE_NOT_FOUND = -203,
    ERR_METHOD_NOT_FOUND    = -204,
    ERR_INVOKE_FAILED       = -205,
    ERR_REGISTER_FAILED     = -206,
    ERR_UNREGISTER_FAILED   = -207,

    // 话题错误 (300-399)
    ERR_TOPIC_NOT_FOUND     = -300,
    ERR_TOPIC_EXISTS        = -301,
    ERR_NOT_SUBSCRIBED      = -302,
    ERR_NOT_PUBLISHER       = -303,

    // 传输层错误 (400-499)
    ERR_TRANSPORT_INIT      = -400,
    ERR_SHM_CREATE          = -401,
    ERR_SHM_ATTACH          = -402,
    ERR_SHM_FULL            = -403,
    ERR_SHM_TIMEOUT         = -404,

    // 序列化错误 (500-599)
    ERR_SERIALIZE           = -500,
    ERR_DESERIALIZE         = -501,
    ERR_BUFFER_OVERFLOW     = -502,
    ERR_BUFFER_UNDERFLOW    = -503,
};

// 错误码转字符串
const char* errorCodeToString(ErrorCode code);

// 判断是否成功
inline bool isSuccess(ErrorCode code);
inline bool isSuccess(int code);

} // namespace omnibinder
```

## 4. 类型定义

`include/omnibinder/types.h`

```cpp
namespace omnibinder {

// 服务句柄
typedef uint32_t ServiceHandle;

// 无效句柄
const ServiceHandle INVALID_HANDLE = 0;

// ServiceManager 默认端口
const uint16_t DEFAULT_SM_PORT = 9900;

// 默认心跳间隔（毫秒）
const uint32_t DEFAULT_HEARTBEAT_INTERVAL = 3000;

// 默认心跳超时（毫秒）
const uint32_t DEFAULT_HEARTBEAT_TIMEOUT = 10000;

// 默认最大丢失心跳次数
const uint32_t DEFAULT_MAX_MISSED_HEARTBEATS = 3;

// 默认调用超时（毫秒）
const uint32_t DEFAULT_INVOKE_TIMEOUT = 5000;

// 默认缓冲区大小
const size_t DEFAULT_BUFFER_SIZE = 4096;

// 最大服务名长度
const size_t MAX_SERVICE_NAME_LENGTH = 256;

// 最大话题名长度
const size_t MAX_TOPIC_NAME_LENGTH = 256;

// 最大消息大小（16MB）
const size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

// FNV-1a 32位哈希（用于接口/方法 ID 生成）
inline uint32_t fnv1a_32(const char* str);
inline uint32_t fnv1a_32(const std::string& str);

} // namespace omnibinder
```

## 5. 主头文件

`include/omnibinder/omnibinder.h`

```cpp
#ifndef OMNIBINDER_H
#define OMNIBINDER_H

#include "omnibinder/types.h"
#include "omnibinder/error.h"
#include "omnibinder/buffer.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include "omnibinder/service.h"
#include "omnibinder/runtime.h"
#include "omnibinder/log.h"

namespace omnibinder {

// 获取 OmniBinder 版本字符串
const char* version();

// 获取 OmniBinder 版本号
// major: 主版本号
// minor: 次版本号
// patch: 补丁版本号
void versionNumbers(int& major, int& minor, int& patch);

} // namespace omnibinder

#endif // OMNIBINDER_H
```

### 5.1 日志系统 (log.h)

`include/omnibinder/log.h`

```cpp
namespace omnibinder {

enum class LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
    LOG_NONE  = 4,
};

// 设置全局日志级别（默认 LOG_INFO）
void setLogLevel(LogLevel level);
LogLevel& globalLogLevel();

// 日志宏（tag 为模块标识字符串）
OMNI_LOG_DEBUG(tag, fmt, ...)
OMNI_LOG_INFO(tag, fmt, ...)
OMNI_LOG_WARN(tag, fmt, ...)
OMNI_LOG_ERROR(tag, fmt, ...)

} // namespace omnibinder
```

#### 关键错误日志关键词

当前控制面 / 数据面主链路中，以下日志关键词已标准化，适合作为 grep 或日志平台检索入口：

- `sm_connect_failed`
- `sm_connect_timeout`
- `sm_connection_lost`
- `sm_reconnect_begin`
- `sm_reconnect_success`
- `rpc_send_failed`
- `rpc_timeout`
- `data_connect_failed`
- `data_connect_timeout`
- `data_connect_fallback`
- `data_send_failed`
- `data_connection_lost`

## 6. C 语言 API

> **状态：已完全实现** ✅  
> **头文件**：`include/omnibinder/omnibinder_c.h`  
> **实现文件**：`src/core/omnibinder_c.cpp`  
> **示例代码**：`examples/example_c/`

对于 C 语言用户，提供一套完整的 C 风格 API 封装，功能与 C++ API 完全对等。

### 6.1 类型定义

```c
#ifndef OMNIBINDER_C_H
#define OMNIBINDER_C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄 */
typedef struct omni_runtime_t  omni_runtime_t;
typedef struct omni_buffer_t  omni_buffer_t;
typedef struct omni_service_t omni_service_t;

typedef struct omni_runtime_stats_t {
    uint64_t total_rpc_calls;
    uint64_t total_rpc_success;
    uint64_t total_rpc_failures;
    uint64_t total_rpc_timeouts;
    uint64_t connection_errors;
    uint64_t sm_reconnect_attempts;
    uint64_t sm_reconnect_successes;
    uint32_t active_connections;
    uint32_t tcp_connections;
    uint32_t shm_connections;
} omni_runtime_stats_t;

/* 回调函数类型 */
typedef void (*omni_invoke_callback_t)(uint32_t method_id,
    const omni_buffer_t* request, omni_buffer_t* response, void* user_data);

typedef void (*omni_topic_callback_t)(uint32_t topic_id,
    const omni_buffer_t* data, void* user_data);

typedef void (*omni_death_callback_t)(const char* service_name, void* user_data);
```

### 6.2 Buffer API

```c
/* 创建和销毁 */
omni_buffer_t* omni_buffer_create(void);
omni_buffer_t* omni_buffer_create_from(const uint8_t* data, size_t len);
void           omni_buffer_destroy(omni_buffer_t* buf);
void           omni_buffer_reset(omni_buffer_t* buf);
const uint8_t* omni_buffer_data(const omni_buffer_t* buf);
size_t         omni_buffer_size(const omni_buffer_t* buf);

/* 写入基础类型 */
void omni_buffer_write_bool(omni_buffer_t* buf, uint8_t val);
void omni_buffer_write_int32(omni_buffer_t* buf, int32_t val);
void omni_buffer_write_uint32(omni_buffer_t* buf, uint32_t val);
void omni_buffer_write_int64(omni_buffer_t* buf, int64_t val);
void omni_buffer_write_float32(omni_buffer_t* buf, float val);
void omni_buffer_write_float64(omni_buffer_t* buf, double val);
void omni_buffer_write_string(omni_buffer_t* buf, const char* val, uint32_t len);
void omni_buffer_write_bytes(omni_buffer_t* buf, const uint8_t* data, uint32_t len);

/* 读取基础类型 */
uint8_t  omni_buffer_read_bool(omni_buffer_t* buf);
int32_t  omni_buffer_read_int32(omni_buffer_t* buf);
uint32_t omni_buffer_read_uint32(omni_buffer_t* buf);
int64_t  omni_buffer_read_int64(omni_buffer_t* buf);
float    omni_buffer_read_float32(omni_buffer_t* buf);
double   omni_buffer_read_float64(omni_buffer_t* buf);
char*    omni_buffer_read_string(omni_buffer_t* buf, uint32_t* len);
uint8_t* omni_buffer_read_bytes(omni_buffer_t* buf, uint32_t* len);
```

### 6.3 Runtime API

```c
/* 客户端生命周期 */
omni_runtime_t* omni_runtime_create(void);
void           omni_runtime_destroy(omni_runtime_t* runtime);
int            omni_runtime_init(omni_runtime_t* runtime, const char* sm_host, uint16_t sm_port);
void           omni_runtime_poll_once(omni_runtime_t* runtime, int timeout_ms);
void           omni_runtime_stop(omni_runtime_t* runtime);

/* 服务注册/注销 */
int  omni_runtime_register_service(omni_runtime_t* runtime, omni_service_t* svc);
int  omni_runtime_unregister_service(omni_runtime_t* runtime, omni_service_t* svc);

/* RPC 调用 */
int  omni_runtime_invoke(omni_runtime_t* runtime, const char* service_name,
         uint32_t interface_id, uint32_t method_id,
         const omni_buffer_t* request, omni_buffer_t* response,
         uint32_t timeout_ms);

int  omni_runtime_invoke_oneway(omni_runtime_t* runtime, const char* service_name,
         uint32_t interface_id, uint32_t method_id,
         const omni_buffer_t* request);

/* 话题 */
int  omni_runtime_publish_topic(omni_runtime_t* runtime, const char* topic_name);
int  omni_runtime_broadcast(omni_runtime_t* runtime, uint32_t topic_id,
         const omni_buffer_t* data);
int  omni_runtime_subscribe_topic(omni_runtime_t* runtime, const char* topic_name,
         omni_topic_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_topic(omni_runtime_t* runtime, const char* topic_name);

/* 死亡通知 */
int  omni_runtime_subscribe_death(omni_runtime_t* runtime, const char* service_name,
         omni_death_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_death(omni_runtime_t* runtime, const char* service_name);

/* 运行时统计 */
int  omni_runtime_get_stats(omni_runtime_t* runtime, omni_runtime_stats_t* stats);
void omni_runtime_reset_stats(omni_runtime_t* runtime);
```

### 6.4 Service API

```c
/* 服务端生命周期 */
omni_service_t* omni_service_create(const char* service_name);
void            omni_service_destroy(omni_service_t* svc);
int             omni_service_init(omni_service_t* svc,
                                  omni_runtime_t* runtime,
                                  omni_invoke_callback_t callback,
                                  void* user_data);

/* 添加接口方法 */
void omni_service_add_interface(omni_service_t* svc,
                                 uint32_t interface_id,
                                 const char* interface_name);
void omni_service_add_method(omni_service_t* svc,
                              uint32_t method_id,
                              const char* method_name);

/* 话题发布 */
int omni_service_publish_topic(omni_service_t* svc, const char* topic_name);
int omni_service_broadcast(omni_service_t* svc,
                            const char* topic_name,
                            const omni_buffer_t* data);
```

### 6.5 使用示例

完整的 C 语言示例代码位于 `examples/example_c/` 目录：

**服务端示例** (`sensor_server.c`)：
```c
#include <omnibinder/omnibinder_c.h>
#include "sensor_service.bidl_c.h"

// 方法回调
void on_invoke(uint32_t method_id, const omni_buffer_t* req,
               omni_buffer_t* resp, void* user_data) {
    if (method_id == demo_SensorService_METHOD_GET_LATEST_DATA) {
        demo_SensorData data;
        demo_SensorData_init(&data);
        data.sensor_id = 1;
        data.temperature = 25.5;
        // ... 填充数据
        demo_SensorData_serialize(&data, resp);
    }
}

int main() {
    omni_runtime_t* runtime = omni_runtime_create();
    omni_runtime_init(client, "127.0.0.1", 9900);
    
    omni_service_t* svc = omni_service_create("SensorService");
    omni_service_init(svc, client, on_invoke, NULL);
    
    omni_runtime_run(client);
    
    omni_service_destroy(svc);
    omni_runtime_destroy(client);
    return 0;
}
```

**客户端示例** (`sensor_client.c`)：
```c
#include <omnibinder/omnibinder_c.h>
#include "sensor_service.bidl_c.h"

int main() {
    omni_runtime_t* runtime = omni_runtime_create();
    omni_runtime_init(client, "127.0.0.1", 9900);
    omni_runtime_run_async(client);
    
    // 调用服务
    omni_buffer_t* req = omni_buffer_create();
    omni_buffer_t* resp = omni_buffer_create();
    
    int ret = omni_runtime_invoke(client, "SensorService",
                                  DEMO_SENSORSERVICE_INTERFACE_ID,
                                  demo_SensorService_METHOD_GET_LATEST_DATA,
                                  req, resp, 5000);
    
    if (ret == 0) {
        demo_SensorData data;
        demo_SensorData_deserialize(&data, resp);
        printf("Temperature: %.2f\n", data.temperature);
    }
    
    omni_buffer_destroy(req);
    omni_buffer_destroy(resp);
    omni_runtime_stop(client);
    omni_runtime_destroy(client);
    return 0;
}
```

### 6.6 编译和链接

```cmake
# CMakeLists.txt
add_executable(my_c_server sensor_server.c)
target_link_libraries(my_c_server omnibinder_static)

add_executable(my_c_client sensor_client.c)
target_link_libraries(my_c_client omnibinder_static)
```

### 6.7 特性说明

- ✅ **完整功能**：与 C++ API 功能对等
- ✅ **类型安全**：使用不透明句柄避免内存泄漏
- ✅ **零拷贝**：Buffer 操作高效
- ✅ **IDL 支持**：omni-idlc 自动生成 C 代码
- ✅ **示例完整**：提供服务端和客户端示例

详细示例见 [examples.md](examples.md) 的 C 语言部分。

## 7. omni-cli 命令行接口

omni-cli 是 OmniBinder 的调试工具，支持查询服务信息和调用服务方法。支持两种输入模式：
- **Hex 模式**：直接传入二进制数据的十六进制表示（不需要 IDL 文件）
- **JSON 模式**：使用人类可读的 JSON 格式（需要通过 --idl 指定 IDL 文件）

```
用法: omni-cli [选项] <命令> [参数]

选项:
  -h, --host <addr>     ServiceManager 地址 (默认: 127.0.0.1)
  -p, --port <port>     ServiceManager 端口 (默认: 9900)
  --idl <file.bidl>     IDL 文件路径（启用 JSON 支持和字段展开）
  --help                显示帮助信息

命令:
  list                  列出所有在线服务
  info <service_name>   查询服务详细信息（包括接口列表）
  call <service_name> <method_name> [params]
                        调用服务方法
                        params: hex 字符串（不带 --idl）或 JSON（带 --idl）

示例:
  # 基础模式
  omni-cli list
  omni-cli info SensorService
  omni-cli call SensorService GetLatestData
  omni-cli call SensorService ResetSensor 01000000
  
  # 详细模式（JSON）
  omni-cli --idl sensor_service.bidl info SensorService
  omni-cli --idl sensor_service.bidl call SensorService GetLatestData
  omni-cli --idl sensor_service.bidl call SensorService SetThreshold \
    '{"command_type":1,"target":"sensor1","value":100}'
  
  # 远程连接
  omni-cli -h 192.168.1.10 -p 9900 list
```

详细使用说明见 [omni-cli 使用指南](omni-tool-usage.md)。

### 7.1 list 命令输出格式

```
$ omni-cli list
NAME                HOST            PORT    STATUS
----                ----            ----    ------
SensorService       192.168.1.10    8001    ONLINE
ControlService      192.168.1.10    8002    ONLINE
LogService          192.168.1.11    8003    ONLINE

Total: 3 services online
```

### 7.2 info 命令输出格式

**基础模式（不带 --idl）：**
```
$ omni-cli info SensorService
Service: SensorService
  Host:    192.168.1.10
  Port:    8001
  HostID:  a1b2c3d4e5f6
  Status:  ONLINE

  Interface: SensorService (id=0x1a2b3c4d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x9c0d1e2f)
      - SetThreshold(ControlCommand) -> StatusResponse  (id=0x5e6f7a8b)
      - ResetSensor(int32) -> void  (id=0x3a4b5c6d)

    Published Topics:
      - SensorUpdate  (id=0x7e8f9a0b)
      - SensorAlert   (id=0x1c2d3e4f)
```

**详细模式（带 --idl）：**
```
$ omni-cli --idl sensor_service.bidl info SensorService
Service: SensorService
  Host:    192.168.1.10
  Port:    8001
  HostID:  a1b2c3d4e5f6
  Status:  ONLINE

  Interface: SensorService (id=0x1a2b3c4d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x9c0d1e2f)
          return:           {
            sensor_id: int32
            temperature: float64
            humidity: float64
            timestamp: int64
            location: string
          }
      - SetThreshold(ControlCommand) -> StatusResponse  (id=0x5e6f7a8b)
          param:           {
            command_type: int32
            target: string
            value: int32
          }
          return:           {
            code: int32
            message: string
          }
      - ResetSensor(int32) -> void  (id=0x3a4b5c6d)
```

### 7.3 call 命令输出格式

**Hex 模式（不带 --idl）：**
```
$ omni-cli call SensorService GetLatestData
Calling SensorService.GetLatestData() ...
  interface_id = 0x1a2b3c4d
  method_id    = 0x9c0d1e2f
Response (status=OK, 42 bytes, 1.23 ms):
  Hex: 01 00 00 00 00 00 00 00 80 39 40 ...
```

**JSON 模式（带 --idl）：**
```
$ omni-cli --idl sensor_service.bidl call SensorService GetLatestData
Calling SensorService.GetLatestData() ...
  interface_id = 0x1a2b3c4d
  method_id    = 0x9c0d1e2f
Response (status=OK, 42 bytes, 0.85 ms):
  {
    "sensor_id": 1,
    "temperature": 25.50,
    "humidity": 60.20,
    "timestamp": 1707321600,
    "location": "Room-A"
  }
```

**说明：**
- 所有调用都显示耗时统计（毫秒精度）
- JSON 模式自动格式化输出
- 向后兼容 hex 模式
