# OmniBinder API 参考

## 1. 概述

本文档描述 libomnibinder 库提供的公共 API。所有 API 位于 `omnibinder` 命名空间下。

## 2. 核心类

### 2.1 Buffer（序列化缓冲区）

`include/omnibinder/buffer.h`

Buffer 是序列化/反序列化的核心工具类，提供自动扩容的字节缓冲区。
当前版本采用显式失败模型：写入接口返回 `bool`，读取接口采用 `tryReadXxx(out)` 返回 `bool`，普通协议错误不再依赖 C++ 异常传播。

```cpp
namespace omnibinder {

class Buffer {
public:
    Buffer() noexcept;
    explicit Buffer(size_t initial_capacity) noexcept;
    Buffer(const uint8_t* data, size_t length) noexcept;
    ~Buffer() noexcept;

    // 移动语义
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    // ---- 写入（序列化）----

    bool writeBool(bool value) noexcept;
    bool writeInt8(int8_t value) noexcept;
    bool writeUint8(uint8_t value) noexcept;
    bool writeInt16(int16_t value) noexcept;
    bool writeUint16(uint16_t value) noexcept;
    bool writeInt32(int32_t value) noexcept;
    bool writeUint32(uint32_t value) noexcept;
    bool writeInt64(int64_t value) noexcept;
    bool writeUint64(uint64_t value) noexcept;
    bool writeFloat32(float value) noexcept;
    bool writeFloat64(double value) noexcept;
    bool writeString(const std::string& value) noexcept;
    bool writeBytes(const void* data, size_t length) noexcept;
    bool writeBytes(const std::vector<uint8_t>& data) noexcept;
    bool writeRaw(const void* data, size_t length) noexcept;

    // ---- 读取（反序列化）----

    bool tryReadBool(bool& value) noexcept;
    bool tryReadInt8(int8_t& value) noexcept;
    bool tryReadUint8(uint8_t& value) noexcept;
    bool tryReadInt16(int16_t& value) noexcept;
    bool tryReadUint16(uint16_t& value) noexcept;
    bool tryReadInt32(int32_t& value) noexcept;
    bool tryReadUint32(uint32_t& value) noexcept;
    bool tryReadInt64(int64_t& value) noexcept;
    bool tryReadUint64(uint64_t& value) noexcept;
    bool tryReadFloat32(float& value) noexcept;
    bool tryReadFloat64(double& value) noexcept;
    bool tryReadString(std::string& value) noexcept;
    bool tryReadBytes(std::vector<uint8_t>& value) noexcept;

    // ---- 缓冲区管理 ----

    // 获取已写入数据的指针
    const uint8_t* data() const noexcept;

    // 获取可写数据的指针
    uint8_t* mutableData() noexcept;

    // 获取已写入数据的大小
    size_t size() const noexcept;

    // 获取缓冲区总容量
    size_t capacity() const noexcept;

    // 预留容量
    void reserve(size_t capacity) noexcept;

    // 调整大小
    void resize(size_t new_size) noexcept;

    // 压缩已读数据（把未读数据移到缓冲区头部）
    void compact() noexcept;

    // 重置读写位置
    void reset() noexcept;

    // 清空缓冲区
    void clear() noexcept;

    // 获取当前读取位置
    size_t readPosition() const noexcept;

    // 设置读取位置
    bool trySetReadPosition(size_t pos) noexcept;

    // 检查写入状态
    bool writeOk() const noexcept;

    // 获取当前写入位置
    size_t writePosition() const noexcept;

    // 设置写入位置
    void setWritePosition(size_t pos) noexcept;

    // 检查是否还有数据可读
    bool hasRemaining() const noexcept;

    // 剩余可读字节数
    size_t remaining() const noexcept;

    // 从原始数据构造（用于接收到的数据）
    void assign(const uint8_t* data, size_t length) noexcept;

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
- reply wait 期间只处理 fd / timer，不执行通过 `post()` 投递的 pending API functor
- 不建议在 owner event-loop 回调中再发起同步阻塞 API

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
    ShmConfig   shm_config;     // SHM 容量配置
    std::vector<InterfaceInfo> interfaces;  // 接口列表
};

struct RuntimeInfo {
    uint32_t pid;                       // runtime 进程 ID
    std::string process_name;           // 进程名
    std::string role;                   // client / service
    uint32_t log_level;                 // 当前日志级别
    uint32_t diag_capabilities;         // 诊断能力位
    std::vector<std::string> services;  // 该 runtime 注册的业务服务
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
    uint32_t    idl_hash;       // 方法签名与参数/返回类型递归哈希
    std::string name;           // 方法名
    std::string param_types;    // 参数类型，空表示无参数
    std::string return_type;    // 返回类型，void 表示无返回值
};

// 死亡通知回调
typedef std::function<void(const std::string& service_name)> DeathCallback;

// 话题消息回调
typedef std::function<void(uint32_t topic_id, const Buffer& data)> TopicCallback;

// 话题错误回调
typedef std::function<void(uint32_t topic_id, ErrorCode error, const Buffer& raw_data)> TopicErrorCallback;

// PID watch 诊断事件回调，data 为诊断 topic payload
typedef std::function<void(const Buffer& data)> DiagEventCallback;

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

    // 查询服务的已发布 topic 列表
    // service_name: 服务名
    // topics: 输出参数，topic 名称列表
    // 可从任意线程安全调用；内部控制面交互由 owner event-loop 串行处理
    // 返回: 0 成功，负值为错误码
    int queryPublishedTopics(const std::string& service_name,
                             std::vector<std::string>& topics);

    // 查询服务的已发布 topic 列表（带超时）
    // timeout_ms: 超时时间（毫秒，0 表示使用默认超时）
    // 返回: 0 成功，负值为错误码（含超时）
    int queryPublishedTopics(const std::string& service_name,
                             std::vector<std::string>& topics,
                             uint32_t timeout_ms);

    // ---- 连接管理 ----

    // 连接到远程服务（建立连接并缓存服务信息）
    // service_name: 目标服务名
    // 返回: 0 成功，负值为错误码
    int connectService(const std::string& service_name);

    // 断开与远程服务的连接
    // service_name: 目标服务名
    // 返回: 0 成功，负值为错误码
    int disconnectService(const std::string& service_name);

    // 检查是否已连接到远程服务
    // service_name: 目标服务名
    // 返回: true 已连接，false 未连接
    bool isServiceConnected(const std::string& service_name) const;

    // 启用自动重连
    // service_name: 目标服务名
    // enable: true 启用，false 禁用
    void enableAutoReconnect(const std::string& service_name, bool enable = true);

    // 设置重连间隔
    // service_name: 目标服务名
    // interval_ms: 重连间隔（毫秒）
    void setReconnectInterval(const std::string& service_name, uint32_t interval_ms);

    // 启动心跳检测
    // service_name: 目标服务名
    // interval_ms: 心跳间隔（毫秒，默认 5000）
    // timeout_ms: 心跳超时（毫秒，默认 10000）
    void startHeartbeat(const std::string& service_name, uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000);

    // 停止心跳检测
    // service_name: 目标服务名
    void stopHeartbeat(const std::string& service_name);

    // ---- 服务调用 ----

    // 调用远程服务方法（同步阻塞）
    // service_name: 目标服务名
    // interface_id: 接口ID
    // method_id: 方法ID
    // idl_hash: 方法签名与类型哈希，用于检测客户端/服务端 IDL 不匹配
    // request: 请求数据
    // response: 响应数据
    // timeout_ms: 超时时间（毫秒），0 表示使用默认超时
    // 可从任意线程安全调用；调用线程可阻塞，但内部发送/等待/状态更新由 owner event-loop 串行处理
    // 超时语义：timeout_ms == 0 使用默认超时；超时返回 ERR_TIMEOUT，不会无限阻塞
    // 返回: 0 成功，负值为错误码
    int invoke(const std::string& service_name,
               uint32_t interface_id,
               uint32_t method_id,
               uint32_t idl_hash,
               const Buffer& request,
               Buffer& response,
               uint32_t timeout_ms = 0);

    // 单向调用（不等待响应）
    // 可从任意线程安全调用；内部发送与连接状态更新由 owner event-loop 串行处理
    int invokeOneWay(const std::string& service_name,
                     uint32_t interface_id,
                     uint32_t method_id,
                     uint32_t idl_hash,
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

    // 声明发布话题（携带 IDL 哈希）
    // topic_name: 话题名
    // idl_hash:   发布者 topic IDL 哈希（由 omni-idlc 生成；0 表示不声明）
    // 返回: 0 成功，负值为错误码
    // 注意: 订阅者声明了期望哈希时，发布者哈希会经 SM 转发给订阅者，
    //       不一致将导致订阅失败（ERR_IDL_MISMATCH）且不投递数据
    int publishTopic(const std::string& topic_name, uint32_t idl_hash);

    // 广播话题数据（发送给所有订阅者）
    // topic_id: 话题ID
    // data: 广播数据
    // 返回: 0 成功，负值为错误码
    int broadcast(uint32_t topic_id, const Buffer& data);

    // 订阅话题
    // topic_name: 话题名
    // on_message: 收到广播时的回调函数（默认在 owner event-loop 线程执行）
    // on_error: 订阅错误回调
    // 返回: 0 成功，负值为错误码
    int subscribeTopic(const std::string& topic_name,
                       const TopicCallback& on_message,
                       const TopicErrorCallback& on_error);

    // 订阅话题（携带期望 IDL 哈希）
    // topic_name:         话题名
    // expected_idl_hash:  订阅者期望的 topic IDL 哈希（由 omni-idlc 生成；0 表示不校验）
    // on_message:         收到广播时的回调函数（默认在 owner event-loop 线程执行）
    // on_error:           订阅错误回调；发布者哈希不匹配时收到 ERR_IDL_MISMATCH
    // 返回: 0 成功；发布者哈希与期望不一致时返回 ERR_IDL_MISMATCH 且不投递数据
    // 注意: 发布者尚未上线时订阅成功，待发布者通知到达后校验
    int subscribeTopic(const std::string& topic_name,
                       uint32_t expected_idl_hash,
                       const TopicCallback& on_message,
                       const TopicErrorCallback& on_error);

    // 取消订阅话题
    int unsubscribeTopic(const std::string& topic_name);

    // ---- 配置 ----

    // 设置/获取服务注册到 ServiceManager 的默认可达地址
    void setRegisterHost(const std::string& host);
    std::string getRegisterHost() const;

    // 设置心跳间隔（毫秒）
    void setHeartbeatInterval(uint32_t interval_ms);

    // 设置默认调用超时（毫秒）
    void setDefaultTimeout(uint32_t timeout_ms);

    // 设置服务端 RPC 回复发送的专用超时预算（毫秒），0 表示仅尝试一次（不等待）
    // 该预算独立于 setDefaultTimeout，默认值 DEFAULT_REPLY_SEND_TIMEOUT（1000ms）
    void setReplySendTimeout(uint32_t ms);

    // 获取本机 host_id
    std::string hostId() const;

    // 获取运行时统计信息
    int getStats(RuntimeStats& stats);

    // 重置运行时统计计数器
    int resetStats();

    // 清理本地服务发现缓存、关闭所有数据面连接
    void clearServiceCache();
    void closeAllConnections();

    // 兼容旧服务级诊断 API。新 CLI watch 使用 PID 级 watchPid()/unwatchPid()。
    int enableDiagnostic(const std::string& service_name);
    int disableDiagnostic(const std::string& service_name);

    // PID 级运行时诊断控制
    int setLogLevelByPid(uint32_t pid, uint32_t level);
    int listRuntimes(std::vector<RuntimeInfo>& runtimes);
    int watchPid(uint32_t pid, const DiagEventCallback& callback);
    int unwatchPid(uint32_t pid);

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

Service 是所有 Stub / 业务服务的基类。IDL 生成的 Stub 类继承此基类。当前支持服务名、端口、接口元数据，以及服务级 SHM 配置。

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
    void setPort(uint16_t p);

    // 设置/获取服务注册到 ServiceManager 的可达地址
    void setRegisterHost(const std::string& host);
    const std::string& getRegisterHost() const;

    // 服务级 SHM 配置
    void setShmConfig(const ShmConfig& config);
    ShmConfig shmConfig() const;

    // 获取接口信息（由 IDL 生成的子类实现）
    virtual const InterfaceInfo& interfaceInfo() const = 0;

    // 获取服务名（默认返回构造时传入的名称，子类可覆盖）
    virtual const char* serviceName() const;

protected:
    // 处理接口调用请求（由 IDL 生成的子类实现）
    // 返回 0 表示成功，非 0 表示错误码
    virtual int onInvoke(uint32_t method_id,
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
    // 禁止拷贝
    Service(const Service&);
    Service& operator=(const Service&);

    std::string name_;
    uint16_t port_;
    std::string register_host_;
    ShmConfig shm_config_;
    OmniRuntime* runtime_;
};

} // namespace omnibinder
```

#### 调用契约

`onInvoke()` 当前采用显式返回状态码模型：

- `0` 表示成功
- `ERR_DESERIALIZE` 表示参数反序列化失败
- `ERR_SERIALIZE` 表示响应序列化失败
- 其它非 `0` 值表示业务或运行时错误

旧的 `reportInvokeError()` / `consumeInvokeError()` 已移除。

### 2.4 IClientTransport / IServerTransport（传输层接口）

`include/omnibinder/transport.h`

传输层抽象接口。客户端侧 `IClientTransport` 表示一条双向消息连接，服务端侧 `IServerTransport` 表示托管端点。
SHM 通过 eventfd 事件驱动，TCP 通过 socket fd 事件驱动，均注册到 EventLoop。

出站数据面由 `src/transport/transport_selector.h` 中的 `createClientTransport()` 创建；
服务端端点由 `createServerTransport()` 创建；ServiceManager 控制通道由 `createControlTransport()` 创建（始终 TCP）。
`transport_selector.h` 不随安装导出，仅用于仓库内构建/源码集成：`createClientTransport()` 供 `ConnectionManager`
建立客户端数据面连接（同机优先 SHM，失败或跨机回退 TCP）；`createServerTransport()` 供 runtime 服务 hosting
与 ServiceManager 创建服务端端点；`createControlTransport()` 供 runtime 连接 ServiceManager。
新增传输类型时在这三个工厂函数中扩展；当前不提供运行时 provider 注入或动态插件 ABI。

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

// 端点容量配置（与具体传输无关；非 SHM 传输可忽略）
struct TransportConfig {
    size_t req_capacity;
    size_t resp_capacity;

    TransportConfig() : req_capacity(0), resp_capacity(0) {}
    TransportConfig(size_t req, size_t resp) : req_capacity(req), resp_capacity(resp) {}
};

// 客户端传输接口（单条双向消息连接）
class IClientTransport {
public:
    virtual ~IClientTransport() {}

    // 发起非阻塞连接
    // 返回: 0 立即成功，1 连接进行中，-1 失败
    virtual int connect(const std::string& host, uint16_t port) = 0;

    // 发送数据（尽力而为，非阻塞）
    virtual int send(const uint8_t* data, size_t length) = 0;

    // 限时全量发送
    // timeout_ms: 发送超时预算（0 表示只尝试一次，不等待）
    // 返回: 0 全部发送成功，<0 超时或错误
    virtual int sendAll(const uint8_t* data, size_t length,
                        uint32_t timeout_ms, uint32_t* elapsed_ms) = 0;

    // 接收数据（非阻塞）
    // buf: 接收缓冲区
    // buf_size: 缓冲区大小
    // 返回: 实际接收的字节数，0 表示无数据，负值为错误
    virtual int recv(uint8_t* buf, size_t buf_size) = 0;

    // 消费底层就绪通知（SHM 消费 eventfd，TCP 为空操作）
    virtual void consumeReadiness() = 0;

    // 是否为成帧传输（SHM 返回 true，TCP 返回 false）
    virtual bool isFramed() const = 0;

    // 探测下一完整帧的长度（仅成帧传输有意义）
    // 返回: 1 有成帧待读，0 暂无可读帧，-1 元数据损坏
    virtual int peekFrameSize(size_t& out_length) = 0;

    // 关闭连接
    virtual void close() = 0;

    // 获取连接状态
    virtual ConnectionState state() const = 0;

    // 获取文件描述符（用于 EventLoop 注册）
    virtual int fd() const = 0;

    // 获取传输类型
    virtual TransportType type() const = 0;
};

// 服务端托管端点接口（TCP 监听/accept 与 SHM 握手/ring 共用）
class IServerTransport {
public:
    typedef std::function<void(int client_id, IClientTransport* client)> AcceptCallback;
    typedef std::function<void(int client_id)> ReadableCallback;
    typedef std::function<void(int client_id)> DisconnectCallback;

    virtual ~IServerTransport() {}

    virtual TransportType type() const = 0;

    // 启动端点
    // host: 监听/绑定地址，port: 监听端口（0 表示自动分配）
    // config: 端点容量配置（非 SHM 传输可忽略）
    // 返回: 实际监听端口，失败返回 -1
    virtual int start(const std::string& host, uint16_t port,
                      const TransportConfig& config) = 0;

    // 关闭端点并释放资源
    virtual void close() = 0;

    // 获取需要 core 注册到 EventLoop 的端点级 fd（监听/事件聚合）
    virtual void pollFds(std::vector<int>& fds) const = 0;

    // 处理一次端点级 fd 事件（accept / 握手 / 可读映射）
    virtual void onPollEvent(int fd, uint32_t events) = 0;

    virtual void setAcceptCallback(const AcceptCallback& cb) = 0;
    virtual void setReadableCallback(const ReadableCallback& cb) = 0;
    virtual void setDisconnectCallback(const DisconnectCallback& cb) = 0;

    // 移除一个客户端并回收其资源
    virtual void removeClient(int client_id) = 0;
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

    // 通用错误 (-1..-9)
    ERR_UNKNOWN             = -1,
    ERR_INVALID_PARAM       = -2,
    ERR_OUT_OF_MEMORY       = -3,
    ERR_TIMEOUT             = -4,
    ERR_NOT_INITIALIZED     = -5,
    ERR_ALREADY_INITIALIZED = -6,
    ERR_NOT_SUPPORTED       = -7,
    ERR_INTERNAL            = -8,
    ERR_NOT_RUNNING         = -9,  // event-loop 已停止，不能投递任务

    // 网络错误 (-100..-108)
    ERR_CONNECT_FAILED      = -100,
    ERR_CONNECTION_CLOSED   = -101,
    ERR_SEND_FAILED         = -102,
    ERR_RECV_FAILED         = -103,
    ERR_PROTOCOL_ERROR      = -104,
    ERR_SM_UNREACHABLE      = -105,
    ERR_BIND_FAILED         = -106,
    ERR_LISTEN_FAILED       = -107,
    ERR_ACCEPT_FAILED       = -108,

    // 服务错误 (-200..-208)
    ERR_SERVICE_NOT_FOUND   = -200,
    ERR_SERVICE_EXISTS      = -201,
    ERR_SERVICE_OFFLINE     = -202,
    ERR_INTERFACE_NOT_FOUND = -203,
    ERR_METHOD_NOT_FOUND    = -204,
    ERR_INVOKE_FAILED       = -205,
    ERR_REGISTER_FAILED     = -206,
    ERR_UNREGISTER_FAILED   = -207,
    ERR_IDL_MISMATCH        = -208,

    // 话题错误 (-300..-303)
    ERR_TOPIC_NOT_FOUND     = -300,
    ERR_TOPIC_EXISTS        = -301,
    ERR_NOT_SUBSCRIBED      = -302,
    ERR_NOT_PUBLISHER       = -303,

    // 传输层错误 (-400..-404)
    ERR_TRANSPORT_INIT      = -400,
    ERR_SHM_CREATE          = -401,
    ERR_SHM_ATTACH          = -402,
    ERR_SHM_FULL            = -403,
    ERR_SHM_TIMEOUT         = -404,

    // 序列化错误 (-500..-503)
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

// 诊断服务名前缀（__diag_pid_<pid>）
const char DIAG_SERVICE_NAME_PREFIX[] = "__diag_pid_";

// 默认心跳间隔（毫秒）
const uint32_t DEFAULT_HEARTBEAT_INTERVAL = 3000;

// 默认心跳超时（毫秒）
const uint32_t DEFAULT_HEARTBEAT_TIMEOUT = 10000;

// 默认最大丢失心跳次数
const uint32_t DEFAULT_MAX_MISSED_HEARTBEATS = 3;

// 默认调用超时（毫秒）
const uint32_t DEFAULT_INVOKE_TIMEOUT = 5000;

// 服务端 RPC 回复发送的专用超时预算（毫秒），独立于 DEFAULT_INVOKE_TIMEOUT
const uint32_t DEFAULT_REPLY_SEND_TIMEOUT = 1000;

// 默认缓冲区大小
const size_t DEFAULT_BUFFER_SIZE = 4096;

// 最大服务名长度
const size_t MAX_SERVICE_NAME_LENGTH = 256;

// 最大话题名长度
const size_t MAX_TOPIC_NAME_LENGTH = 256;

// 最大消息大小（16MB）
const size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

// 反序列化防护上限
const size_t MAX_ARRAY_ELEMENTS = 1024 * 1024;
const size_t MAX_ZERO_WIRE_ARRAY_ELEMENTS = 4096;
const size_t MAX_PUBLISHED_TOPICS = 4096;
const size_t MAX_PUBLISHED_TOPICS_BYTES = 512 * 1024;

// runtime 诊断能力位：支持 watch
const uint32_t RUNTIME_DIAG_CAP_WATCH = 1u;

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
#include "omnibinder/log.h"
#include "omnibinder/buffer.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include "omnibinder/service.h"
#include "omnibinder/runtime.h"

namespace omnibinder {

// 获取 OmniBinder 版本字符串（版本号由 CMake project(VERSION ...) 注入）
inline const char* version() { return OMNIBINDER_VERSION; }

// 获取 OmniBinder 版本号
// major: 主版本号
// minor: 次版本号
// patch: 补丁版本号
inline void versionNumbers(int& major, int& minor, int& patch) {
    major = OMNIBINDER_VERSION_MAJOR;
    minor = OMNIBINDER_VERSION_MINOR;
    patch = OMNIBINDER_VERSION_PATCH;
}

} // namespace omnibinder

#endif // OMNIBINDER_H
```

### 5.1 日志系统 (log.h)

`include/omnibinder/log.h`

```cpp
// C 枚举（C/C++ 通用，级别值 0~6）
typedef enum {
    OMNI_LOG_FATAL   = 0,  // 致命错误
    OMNI_LOG_ERROR   = 1,  // 功能错误
    OMNI_LOG_WARN    = 2,  // 告警
    OMNI_LOG_INFO    = 3,  // 重要信息
    OMNI_LOG_DEBUG   = 4,  // 调试信息
    OMNI_LOG_VERBOSE = 5,  // 详细日志
    OMNI_LOG_OFF     = 6,  // 关闭日志（仅 set，不用于打印）
} omni_log_level_t;

namespace omnibinder {

// 类型别名，与旧代码兼容
using LogLevel = omni_log_level_t;

// 常量别名
constexpr LogLevel LOG_FATAL   = OMNI_LOG_FATAL;
constexpr LogLevel LOG_ERROR   = OMNI_LOG_ERROR;
constexpr LogLevel LOG_WARN    = OMNI_LOG_WARN;
constexpr LogLevel LOG_INFO    = OMNI_LOG_INFO;
constexpr LogLevel LOG_DEBUG   = OMNI_LOG_DEBUG;
constexpr LogLevel LOG_VERBOSE = OMNI_LOG_VERBOSE;
constexpr LogLevel LOG_OFF     = OMNI_LOG_OFF;

// 设置全局日志级别（默认 LOG_INFO）
inline void setLogLevel(LogLevel level);
LogLevel& globalLogLevel();
inline const char* logLevelStr(LogLevel level);

// 控制是否打印时间戳（默认开启）
inline void enableTimestamp(bool enable);

// 格式化打印（C++ 包装）
void logPrint(LogLevel level, const char* tag, const char* fmt, ...);

} // namespace omnibinder

// 日志宏（tag 为模块标识字符串；C/C++ 通用）
OMNI_LOG_FATAL(tag, ...)
OMNI_LOG_ERROR(tag, ...)
OMNI_LOG_WARN(tag, ...)
OMNI_LOG_INFO(tag, ...)
OMNI_LOG_DEBUG(tag, ...)
OMNI_LOG_VERBOSE(tag, ...)

// 底层 C API：omni_log_set_level / omni_log_enable_timestamp / omni_log_level_str /
//             omni_log_print / omni_log_vprint
```

#### 关键错误日志关键词

当前控制面 / 数据面主链路中，以下日志关键词已标准化，适合作为 grep 或日志平台检索入口：

- `sm_connect_failed`
- `sm_connect_timeout`
- `sm_connection_lost`
- `sm_reconnect_begin`
- `sm_reconnect_success`
- `rpc_send_failed`
- `rpc_send_timeout`
- `rpc_timeout`
- `data_connect_failed`
- `data_connect_timeout`
- `data_connect_fallback`
- `data_send_incomplete`
- `data_send_deadline_expired`
- `data_connection_lost`

## 6. C 语言 API

> **状态：已完全实现** ✅  
> **头文件**：`include/omnibinder/omnibinder_c.h`  
> **实现文件**：`src/core/omnibinder_c.cpp`  
> **示例代码**：`examples/example_c/`

对于 C 语言用户，提供一套完整的 C 风格 API 封装，功能与 C++ API 完全对等。

句柄 API 对关键 NULL 参数做防御性拒绝：`omni_runtime_init`（runtime/sm_host）、`omni_runtime_invoke`（runtime/service_name/request/response）、`omni_runtime_invoke_oneway`、`omni_runtime_subscribe_death`、`omni_runtime_unsubscribe_death`、`omni_runtime_unsubscribe_topic`，以及 `omni_runtime_register_service`/`omni_runtime_broadcast`/`omni_runtime_subscribe_topic` 均返回 `-1`；`omni_service_create(NULL, ...)` 返回 `NULL`，`omni_fnv1a_32(NULL)` 返回 `0`。

### 6.1 类型定义

```c
#ifndef OMNIBINDER_C_H
#define OMNIBINDER_C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 反序列化防护上限（与 C++ 侧一致，供生成代码引用） */
#define OMNI_MAX_MESSAGE_SIZE (16u * 1024u * 1024u)
#define OMNI_MAX_ARRAY_ELEMENTS (1024u * 1024u)
#define OMNI_MAX_ZERO_WIRE_ARRAY_ELEMENTS (4096u)

/* C 错误码常量（与 omnibinder::ErrorCode 同值） */
#define OMNI_OK                     0
#define OMNI_ERR_UNKNOWN           (-1)
#define OMNI_ERR_INVALID_PARAM     (-2)
#define OMNI_ERR_OUT_OF_MEMORY     (-3)
#define OMNI_ERR_TIMEOUT           (-4)
#define OMNI_ERR_NOT_INITIALIZED   (-5)
#define OMNI_ERR_NOT_RUNNING       (-9)
#define OMNI_ERR_CONNECT_FAILED    (-100)
#define OMNI_ERR_CONNECTION_CLOSED (-101)
#define OMNI_ERR_SEND_FAILED       (-102)
#define OMNI_ERR_RECV_FAILED       (-103)
#define OMNI_ERR_SERVICE_NOT_FOUND (-200)
#define OMNI_ERR_INVOKE_FAILED     (-205)
#define OMNI_ERR_REGISTER_FAILED   (-206)
#define OMNI_ERR_IDL_MISMATCH      (-208)
#define OMNI_ERR_SERIALIZE         (-500)
#define OMNI_ERR_DESERIALIZE       (-501)

/* 不透明句柄 */
typedef struct omni_runtime_t  omni_runtime_t;
typedef struct omni_buffer_t  omni_buffer_t;
typedef struct omni_service_t omni_service_t;

/* 自定义内存分配器钩子类型 */
typedef void* (*OmniMallocFn)(size_t size);
typedef void  (*OmniFreeFn)(void* ptr);

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
typedef int (*omni_invoke_callback_t)(uint32_t method_id,
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
size_t         omni_buffer_remaining(const omni_buffer_t* buf);
int            omni_buffer_read_ok(const omni_buffer_t* buf);
void           omni_buffer_clear_error(omni_buffer_t* buf);
void           omni_buffer_mark_error(omni_buffer_t* buf, int32_t error_code);
int32_t        omni_buffer_error(const omni_buffer_t* buf);

/* 写入基础类型 */
void omni_buffer_write_bool(omni_buffer_t* buf, uint8_t val);
void omni_buffer_write_int8(omni_buffer_t* buf, int8_t val);
void omni_buffer_write_uint8(omni_buffer_t* buf, uint8_t val);
void omni_buffer_write_int16(omni_buffer_t* buf, int16_t val);
void omni_buffer_write_uint16(omni_buffer_t* buf, uint16_t val);
void omni_buffer_write_int32(omni_buffer_t* buf, int32_t val);
void omni_buffer_write_uint32(omni_buffer_t* buf, uint32_t val);
void omni_buffer_write_int64(omni_buffer_t* buf, int64_t val);
void omni_buffer_write_uint64(omni_buffer_t* buf, uint64_t val);
void omni_buffer_write_float32(omni_buffer_t* buf, float val);
void omni_buffer_write_float64(omni_buffer_t* buf, double val);
void omni_buffer_write_string(omni_buffer_t* buf, const char* val, uint32_t len);
void omni_buffer_write_bytes(omni_buffer_t* buf, const uint8_t* data, uint32_t len);

/* 读取基础类型 */
uint8_t  omni_buffer_read_bool(omni_buffer_t* buf);
int8_t   omni_buffer_read_int8(omni_buffer_t* buf);
uint8_t  omni_buffer_read_uint8(omni_buffer_t* buf);
int16_t  omni_buffer_read_int16(omni_buffer_t* buf);
uint16_t omni_buffer_read_uint16(omni_buffer_t* buf);
int32_t  omni_buffer_read_int32(omni_buffer_t* buf);
uint32_t omni_buffer_read_uint32(omni_buffer_t* buf);
int64_t  omni_buffer_read_int64(omni_buffer_t* buf);
uint64_t omni_buffer_read_uint64(omni_buffer_t* buf);
float    omni_buffer_read_float32(omni_buffer_t* buf);
double   omni_buffer_read_float64(omni_buffer_t* buf);
/* 返回堆分配内存，调用者需用 omni_free() 释放；out_len 可为 NULL */
char*    omni_buffer_read_string(omni_buffer_t* buf, uint32_t* out_len);
/* 返回堆分配内存，调用者需用 omni_free() 释放；out_len 不可为 NULL */
uint8_t* omni_buffer_read_bytes(omni_buffer_t* buf, uint32_t* out_len);
```

### 6.3 Runtime API

```c
/* 客户端生命周期 */
omni_runtime_t* omni_runtime_create(void);
void           omni_runtime_destroy(omni_runtime_t* runtime);
int            omni_runtime_init(omni_runtime_t* runtime, const char* sm_host, uint16_t sm_port);
void           omni_runtime_run(omni_runtime_t* runtime);
void           omni_runtime_poll_once(omni_runtime_t* runtime, int timeout_ms);
void           omni_runtime_stop(omni_runtime_t* runtime);
int            omni_runtime_is_running(const omni_runtime_t* runtime);

/* Runtime 配置 */
void        omni_runtime_set_register_host(omni_runtime_t* runtime, const char* host);
const char* omni_runtime_get_register_host(const omni_runtime_t* runtime);
void        omni_runtime_set_heartbeat_interval(omni_runtime_t* runtime, uint32_t interval_ms);
void        omni_runtime_set_default_timeout(omni_runtime_t* runtime, uint32_t timeout_ms);
const char* omni_runtime_host_id(const omni_runtime_t* runtime);

/* 服务注册/注销 */
int  omni_runtime_register_service(omni_runtime_t* runtime, omni_service_t* svc);
int  omni_runtime_unregister_service(omni_runtime_t* runtime, omni_service_t* svc);

/* RPC 调用 */
int  omni_runtime_invoke(omni_runtime_t* runtime, const char* service_name,
         uint32_t interface_id, uint32_t method_id, uint32_t idl_hash,
         const omni_buffer_t* request, omni_buffer_t* response,
         uint32_t timeout_ms);

int  omni_runtime_invoke_oneway(omni_runtime_t* runtime, const char* service_name,
         uint32_t interface_id, uint32_t method_id, uint32_t idl_hash,
         const omni_buffer_t* request);

/* 连接管理 */
int  omni_runtime_connect_service(omni_runtime_t* runtime, const char* service_name);
int  omni_runtime_disconnect_service(omni_runtime_t* runtime, const char* service_name);
int  omni_runtime_is_service_connected(const omni_runtime_t* runtime, const char* service_name);
void omni_runtime_enable_auto_reconnect(omni_runtime_t* runtime, const char* service_name, int enable);
void omni_runtime_set_reconnect_interval(omni_runtime_t* runtime, const char* service_name, uint32_t interval_ms);
void omni_runtime_start_heartbeat(omni_runtime_t* runtime, const char* service_name, uint32_t interval_ms, uint32_t timeout_ms);
void omni_runtime_stop_heartbeat(omni_runtime_t* runtime, const char* service_name);

/* 话题 */
int  omni_runtime_publish_topic(omni_runtime_t* runtime, const char* topic_name,
         uint32_t idl_hash);
int  omni_runtime_broadcast(omni_runtime_t* runtime, uint32_t topic_id,
         const omni_buffer_t* data);
int  omni_runtime_subscribe_topic(omni_runtime_t* runtime, const char* topic_name,
         uint32_t expected_idl_hash, omni_topic_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_topic(omni_runtime_t* runtime, const char* topic_name);

/* 死亡通知 */
int  omni_runtime_subscribe_death(omni_runtime_t* runtime, const char* service_name,
         omni_death_callback_t callback, void* user_data);
int  omni_runtime_unsubscribe_death(omni_runtime_t* runtime, const char* service_name);

/* 运行时统计 */
int  omni_runtime_get_stats(omni_runtime_t* runtime, omni_runtime_stats_t* stats);
int  omni_runtime_reset_stats(omni_runtime_t* runtime);
```

### 6.4 Service API

```c
/* 服务端生命周期 */
omni_service_t* omni_service_create(const char* name,
                                    uint32_t interface_id,
                                    omni_invoke_callback_t callback,
                                    void* user_data);
void            omni_service_destroy(omni_service_t* svc);
void*           omni_service_get_user_data(omni_service_t* svc);

/* 添加接口方法 */
void omni_service_add_method(omni_service_t* svc,
                               uint32_t method_id,
                               const char* method_name);
void omni_service_add_method_ex(omni_service_t* svc,
                                uint32_t method_id,
                                const char* method_name,
                                const char* param_types,
                                const char* return_type,
                                uint32_t idl_hash);

/* 服务配置 */
uint16_t    omni_service_port(const omni_service_t* svc);
void        omni_service_set_register_host(omni_service_t* svc, const char* host);
const char* omni_service_get_register_host(const omni_service_t* svc);
```

### 6.5 工具函数

```c
uint32_t omni_fnv1a_32(const char* str);

void  omniSetAllocator(OmniMallocFn malloc_fn, OmniFreeFn free_fn);
void* omni_malloc(size_t size);
void  omni_free(void* ptr);
void* omni_realloc_sized(void* ptr, size_t old_size, size_t new_size);
void* omni_realloc(void* ptr, size_t new_size);
```

### 6.6 使用示例

完整的 C 语言示例代码位于 `examples/example_c/` 目录：

**服务端示例** (`sensor_server.c`)：
```c
#include <omnibinder/omnibinder_c.h>
#include "sensor_service.bidl_c.h"

// 方法回调（返回 0 表示成功，非 0 为错误码）
int on_invoke(uint32_t method_id, const omni_buffer_t* req,
              omni_buffer_t* resp, void* user_data) {
    if (method_id == demo_SensorService_METHOD_GET_LATEST_DATA) {
        demo_SensorData data;
        demo_SensorData_init(&data);
        data.sensor_id = 1;
        data.temperature = 25.5;
        // ... 填充数据
        demo_SensorData_serialize(&data, resp);
    }
    return 0;
}

 int main() {
    omni_runtime_t* runtime = omni_runtime_create();
    omni_runtime_init(runtime, "127.0.0.1", 9900);
    
    omni_service_t* svc = omni_service_create("SensorService",
        demo_SensorService_INTERFACE_ID, on_invoke, NULL);
    omni_service_add_method_ex(svc,
        demo_SensorService_METHOD_GET_LATEST_DATA,
        "GetLatestData", "", "SensorData",
        demo_SensorService_METHOD_GET_LATEST_DATA_IDL_HASH);
    omni_runtime_register_service(runtime, svc);
    
    while (running) {
        omni_runtime_poll_once(runtime, 100);
    }
    
    omni_service_destroy(svc);
    omni_runtime_destroy(runtime);
    return 0;
}
```

**客户端示例** (`sensor_client.c`)：
```c
#include <omnibinder/omnibinder_c.h>
#include "sensor_service.bidl_c.h"

int main() {
    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, "127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }
    
    // 调用服务
    omni_buffer_t* req = omni_buffer_create();
    omni_buffer_t* resp = omni_buffer_create();
    
    int ret = omni_runtime_invoke(runtime, "SensorService",
                                  demo_SensorService_INTERFACE_ID,
                                  demo_SensorService_METHOD_GET_LATEST_DATA,
                                  demo_SensorService_METHOD_GET_LATEST_DATA_IDL_HASH,
                                  req, resp, 5000);
    
    if (ret == 0) {
        demo_SensorData data;
        demo_SensorData_deserialize(&data, resp);
        printf("Temperature: %.2f\n", data.temperature);
    }
    
    omni_buffer_destroy(req);
    omni_buffer_destroy(resp);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
```

### 6.7 编译和链接

```cmake
# CMakeLists.txt
add_executable(my_c_server sensor_server.c)
target_link_libraries(my_c_server omnibinder_static)

add_executable(my_c_client sensor_client.c)
target_link_libraries(my_c_client omnibinder_static)
```

### 6.8 特性说明

- ✅ **完整功能**：与 C++ API 功能对等
- ✅ **类型安全**：使用不透明句柄避免内存泄漏
- ✅ **零拷贝**：Buffer 操作高效
- ✅ **IDL 支持**：omni-idlc 自动生成 C 代码
- ✅ **示例完整**：提供服务端和客户端示例

详细示例见 [examples.md](examples.md) 的 C 语言部分。

## 7. omni-cli 命令行接口

omni-cli 是 OmniBinder 的调试工具，支持查询服务信息、查询 runtime/PID、调用服务方法、按 PID 调整日志级别和 watch 业务接口 I/O。调用和 watch 解码支持两种输入/显示模式：
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
  ps                    列出在线 runtime PID、角色、日志级别和服务
  info <service_name>   查询服务详细信息（包括接口列表）
  call <service_name> <method_name> [params]
                        调用服务方法
                        params: hex 字符串（不带 --idl）或 JSON（带 --idl）
  log set --pid <pid> --level <F|E|W|I|D|V|O>
                        设置指定 runtime 的日志级别
  watch --pid <pid> --idl <file.bidl> [--filter <method|topic>]
                        观察指定 PID 的业务接口输入/输出

示例:
  # 基础模式
  omni-cli list
  omni-cli ps
  omni-cli info SensorService
  omni-cli call SensorService GetLatestData
  omni-cli call SensorService ResetSensor 01000000
  
  # 详细模式（JSON）
  omni-cli --idl sensor_service.bidl info SensorService
  omni-cli --idl sensor_service.bidl call SensorService GetLatestData
  omni-cli --idl sensor_service.bidl call SensorService SetThreshold \
    '{"command_type":1,"target":"sensor1","value":100}'
  omni-cli watch --pid 12345 --idl sensor_service.bidl --filter GetLatestData
  omni-cli log set --pid 12345 --level D
  
  # 远程连接
  omni-cli -h 192.168.1.10 -p 9900 list
```

详细使用说明见 [omni-cli 使用指南](omni-tool-usage.md)。

### 7.1 list 命令输出格式

```
$ omni-cli list
NAME                     HOST             PORT     STATUS
----                     ----             ----     ------
SensorService            192.168.1.10     8001     ONLINE
ControlService           192.168.1.10     8002     ONLINE
LogService               192.168.1.11     8003     ONLINE

Total: 3 services online
```

### 7.2 ps 命令输出格式

```
$ omni-cli ps
PID      ROLE     LOG   PROCESS              SERVICES
-------- -------- ----- -------------------- ----------------
12345    service  I     example_cpp_sensor_server SensorService
12346    client   D     example_cpp_sensor_client -
```

`PROCESS` 为 runtime 启动时上报的可执行文件名，列宽固定 20 字符（`%-20s`），超出宽度时不截断、原样输出。旧 runtime 已注册的进程需要重启后才会刷新。

### 7.3 info 命令输出格式

**基础模式（不带 --idl）：**
```
$ omni-cli info SensorService
Service: SensorService
  Host:    192.168.1.10
  Port:    8001
  HostID:  a1b2c3d4e5f6
  Status:  ONLINE

  Published Topics:
    - SensorUpdate
    - SensorAlert

  Interface: SensorService (id=0x1a2b3c4d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x9c0d1e2f)
      - SetThreshold(ControlCommand) -> StatusResponse  (id=0x5e6f7a8b)
      - ResetSensor(int32) -> void  (id=0x3a4b5c6d)
```

**详细模式（带 --idl）：**
```
$ omni-cli --idl sensor_service.bidl info SensorService
Service: SensorService
  Host:    192.168.1.10
  Port:    8001
  HostID:  a1b2c3d4e5f6
  Status:  ONLINE

  Published Topics:
    - SensorUpdate
    - SensorAlert

  Interface: SensorService (id=0x1a2b3c4d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x9c0d1e2f)
          return: {
            sensor_id: int32
            temperature: float64
            humidity: float64
            timestamp: int64
            location: string
          }
      - SetThreshold(ControlCommand) -> StatusResponse  (id=0x5e6f7a8b)
          param: {
            command_type: int32
            target: string
            value: int32
          }
          return: {
            code: int32
            message: string
          }
      - ResetSensor(int32) -> void  (id=0x3a4b5c6d)
          param: int32
```

### 7.4 call 命令输出格式

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

### 7.5 log/watch 诊断命令

`log set` 通过 PID 调整目标 runtime 日志级别：

```bash
omni-cli log set --pid 12345 --level D
```

`watch` 通过 PID 观察 IDL 业务接口 I/O：

```bash
omni-cli watch --pid 12345 --idl sensor_service.bidl --filter GetLatestData
```

watch 数据面复用 OmniBinder 既有 topic 数据通道：同机自动使用 SHM，跨机自动使用 TCP；ServiceManager 只负责启动/停止控制面。
