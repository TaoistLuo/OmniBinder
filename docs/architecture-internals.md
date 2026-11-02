# OmniBinder 内部实现架构说明

## 1. 文档目的

本文档面向准备对 OmniBinder 做二次开发、性能调优、模块替换或行为扩展的开发者。

文档目标是：

- 准确描述当前实现中的核心模块、职责边界和关键数据结构
- 说明控制面、数据面、SHM、RPC、发布订阅、服务状态通知的完整数据流
- 说明运行时内存模型、连接模型和事件模型
- 让开发者读完后能快速定位代码、理解调用路径，并在不破坏整体设计的前提下继续扩展

本文档只描述**当前最终实现状态**。

## 2. 总体实现视图

### 2.1 模块层次

```
+-------------------------------------------------------------------+
|                          应用 / 工具层                              |
|   用户服务 / HMI / omni-cli / 生成的 Proxy / 生成的 Stub           |
+-------------------------------------------------------------------+
|                         libomnibinder                               |
|                                                                   |
|   OmniRuntime (Facade / 协调层)                                     |
|     |-- SmControlChannel                                           |
|     |-- RpcRuntime                                                 |
|     |-- TopicRuntime                                               |
|     `-- ConnectionManager                                          |
|                                                                   |
|   EventLoop / OwnerThreadExecutor                                  |
|   Transport Layer: TcpConnection / ShmClientConnection         |
+-------------------------------------------------------------------+
|                     ServiceManager（独立进程）                      |
|   ServiceRegistry / Session deadline / ServiceWatchNotifier / TopicManager |
+-------------------------------------------------------------------+
```

### 2.2 两类平面

OmniBinder 的内部实现分成两个平面：

- **控制面**：注册、发现、心跳、服务状态通知、topic 关系管理
- **数据面**：RPC、one-way、broadcast 数据传输

控制面由 `ServiceManager` 统一处理，数据面由服务间直连处理。

## 3. 代码入口与主要文件

### 3.1 核心运行时文件

- `src/core/omni_runtime.h`
- `src/core/omni_runtime.cpp`
- `src/core/runtime_sm.cpp` — ServiceManager 控制面通信
- `src/core/runtime_service.cpp` — 本地服务注册 / 注销
- `src/core/runtime_connection.cpp` — 连接管理与心跳
- `src/core/runtime_rpc.cpp` — RPC 调用与回复
- `src/core/runtime_topic.cpp` — topic 发布/订阅/广播
- `src/core/runtime_dispatch.cpp` — 服务端请求分派
- `src/core/runtime_diag.cpp` — 诊断 watch 数据面
- `src/core/runtime_config.cpp` — 配置 / 统计 / 线程模型基础设施
- `src/core/runtime_helpers.h` / `src/core/runtime_helpers.cpp` — 协议 payload 解码与诊断事件序列化辅助
- `src/core/message_reader.h` / `src/core/message_reader.cpp` — 统一消息读取器（成帧/流式组帧）
- `src/core/pending_reply_table.h` / `src/core/pending_reply_table.cpp` — 控制面/数据面独立的 pending reply 槽表
- `src/core/sm_control_channel.h`
- `src/core/sm_control_channel.cpp`
- `src/core/rpc_runtime.h`
- `src/core/rpc_runtime.cpp`
- `src/core/topic_runtime.h`
- `src/core/topic_runtime.cpp`
- `src/core/connection_manager.h`
- `src/core/connection_manager.cpp`
- `src/core/event_loop.h`
- `src/core/event_loop.cpp`
- `src/core/owner_thread_executor.h`
- `include/omnibinder/buffer.h`

### 3.2 传输层文件

- `src/transport/tcp_connection.h` — `TcpConnection`：TCP 双向连接（出站/入站共用）
- `src/transport/tcp_connection.cpp`
- `src/transport/tcp_server_endpoint.h` — `TcpServerEndpoint`：TCP 接入端点
- `src/transport/tcp_server_endpoint.cpp`
- `src/transport/shm_ring.h`
- `src/transport/shm_ring.cpp`
- `src/transport/shm_client_connection.h` — `ShmClientConnection`：SHM 出站连接（创建 SHM）
- `src/transport/shm_client_connection.cpp`
- `src/transport/shm_server_connection.h` — `ShmServerConnection`：SHM 入站连接（attach 客户端 SHM）
- `src/transport/shm_server_connection.cpp`
- `src/transport/shm_server_endpoint.h` — `ShmServerEndpoint`：SHM 握手监听端点
- `src/transport/shm_server_endpoint.cpp`
- `src/transport/transport_selector.h`
- `src/transport/transport_selector.cpp`

### 3.3 控制面进程文件

- `service_manager/main.cpp` — 入口函数
- `service_manager/service_manager_app.h/.cpp` — 主循环与消息分派
- `service_manager/sm_registry.cpp` — 服务注册/发现控制面
- `service_manager/sm_topic.cpp` — topic 发布/订阅控制面
- `service_manager/sm_diag.cpp` — 诊断 watch 控制面
- `service_manager/service_registry.h`
- `service_manager/service_registry.cpp`
- `service_manager/service_watch_notifier.h`
- `service_manager/service_watch_notifier.cpp`
- `service_manager/topic_manager.h`
- `service_manager/topic_manager.cpp`

## 4. OmniRuntime::Impl 的角色

### 4.1 主要职责

`OmniRuntime::Impl` 是运行时装配与协调中心，负责：

- 初始化和销毁 runtime
- 管理 `ServiceManager` 连接生命周期
- 注册和注销本地服务
- 协调控制面、RPC、topic、本地服务托管、连接管理之间的交互
- 持有高层缓存和全局状态（如服务缓存、服务状态回调、本地服务表）

### 4.2 关键成员

核心成员包括：

- `EventLoop* loop_`
- `SmControlChannel sm_channel_`
- `RpcRuntime rpc_runtime_`
- `TopicRuntime topic_runtime_`
- `ConnectionManager* conn_mgr_`
- `std::map<std::string, LocalServiceEntry*> local_services_`
- `std::map<int, std::string> client_id_to_service_`
- `std::map<std::string, ServiceState> services_`（按**连接键**索引：默认实例键即服务名，显式实例键由 `makeServiceConnectionKey` 生成；统一归并服务缓存 / 服务状态回调 / 重连配置 / 心跳状态）
- `std::map<uint64_t, ServiceWatchSubscription> service_watches_`（服务状态 watch 组）

### 4.3 LocalServiceEntry

每个本地注册服务对应一个 `LocalServiceEntry`，包含：

- `Service* service`
- `uint16_t port`
- `std::vector<IServerEndpoint*> endpoints` — 各传输端点（TCP listener / SHM UDS listener）
- `std::map<int, IMessageConnection*> clients` — 端点接入的客户端（非拥有）
- `std::map<int, Buffer*> client_recv_buffers` — 客户端拆包缓冲
- `std::set<int> endpoint_fds` — 已注册到 EventLoop 的端点级 fd
- `bool diag_enabled` / `uint32_t diag_topic_id`

这个结构代表“一个本地服务完整的对外暴露状态”。

## 5. SmControlChannel

### 5.1 作用

`SmControlChannel` 只负责 `OmniRuntime` 与 `ServiceManager` 之间的控制通道。

### 5.2 关键状态

- `IMessageConnection* transport_`
- `Buffer recv_buffer_`
- `PendingReplyTable pending_replies_`（控制面独立等待槽表；数据面由 `RpcRuntime` 单独持有）

### 5.3 核心职责

- 发送控制面消息
- 接收控制面字节流
- 从接收缓冲中切出完整 `Message`
- 管理等待中的 reply

### 5.4 关键方法

- `isConnected()`
- `sendMessage(const Message&)`
- `sendMessageWithinTimeout(const Message&, uint32_t, uint32_t*)`
- `recvMessage(Message&)`
- `pendingReplies()` / `clearReplies()`
- `transport()` / `resetTransport()` / `closeTransport(EventLoop&)`
- `clearReceiveBuffer()`

等待槽的 `beginWait` / `isWaiting` / `pendingReply` / `isFailed` / `takeReply` / `storeReply` / `eraseWait` 由 `PendingReplyTable` 提供。

### 5.5 设计约束

- 只处理控制面
- 不参与业务服务调用执行
- 不参与 topic callback 调用
- 不选择 transport

## 6. RpcRuntime

### 6.1 作用

`RpcRuntime` 负责同步 RPC 等待模型的核心状态与时间语义。

### 6.2 关键状态

- `default_timeout_ms_`
- `sequence_counter_`
- `in_wait_for_reply_`
- `wait_deadline_ms_`

### 6.3 核心职责

- 分配 request sequence
- 计算有效超时
- 维护当前 wait 状态
- 驱动 `waitForReply()` 主循环

### 6.4 关键方法

- `nextSequence()`
- `effectiveTimeout()`
- `setDefaultTimeout()`
- `beginWait()`
- `remainingWaitMs()`
- `isTimedOut()`
- `waitForReply(...)`
- `dataReplies()`

### 6.5 与其他模块的关系

- 它不直接持有 transport
- 它不拥有控制面槽表：`waitForReply()` 接受调用方传入的 `PendingReplyTable&`；控制面表由 `SmControlChannel` 持有，数据面表由自身 `dataReplies()` 持有
- 它通过外部传入的 reply-wait pump 驱动事件循环

### 6.6 等待语义

`waitForReply()` 并不运行完整的 `pollOnce()`（后者会执行 `post()` functor）。

reply wait 期间只处理：

- fd 事件
- timer 事件

这样可以避免共享同一个 `OmniRuntime` 做并发同步调用时出现 re-entrant `waitForReply`。

### 6.7 错误处理边界

当前实现采用**纯错误码模型**，完全不依赖 C++ 异常：

- Buffer / Message / invoke 主路径：使用 `bool` 或错误码
- `Service::onInvoke()`：显式返回 `int`
- owner-thread / event-loop / fd callback 边界：通过返回值传播错误，不抛出异常

所有业务回调（`onInvoke`、`onClientConnected`、`onClientDisconnected` 等）均不应抛出异常。
如果业务代码抛出未捕获异常，程序将直接终止（`std::terminate`）。

## 7. TopicRuntime

### 7.1 作用

`TopicRuntime` 负责 topic 的本地运行时状态。

### 7.2 当前持有的数据

- `topics_`（`std::map<uint32_t, TopicState>`：每个 topic 的订阅记录（选择器 + 回调 + 期望哈希）/ `tcp_subscribers` / `shm_subscribers` / `published` / `owner_service` / `published_hash`）
- `name_to_id_`（topic name → id 映射，topic id 经 `getTopicId()` 查询）

### 7.3 当前职责

- 记录 topic callback（无来源 `TopicCallback` 与带来源 `TopicSourceCallback` 并存）
- 维护 topic name / id 映射
- 记录已发布 topic 及其 owner service
- 记录 publisher 侧 TCP / SHM subscriber 集合
- 按订阅选择器（服务级 / 实例级）过滤发布者通知，并为每个发布者实例建立独立数据面连接
- 在本地 dispatch 广播回调：`dispatch()` 触发无来源回调，`dispatchFrom(service, host_id, instance_id, ...)` 按来源实例匹配选择器后分发

### 7.4 topic owner 语义

每个已发布 topic 都显式记录 owner service。

这保证：

- 注销某个 service 时，只清理它 own 的 topic
- 不会误伤同进程其他 service 发布的 topic

### 7.5 当前边界

- 只负责本地 topic state
- 不负责控制面订阅关系维护
- 不直接发控制面消息
- 不直接选择传输通道

## 8. 本地服务托管与请求分派 (Hosting)

### 8.1 作用

`OmniRuntime::Impl` 直接管理本地服务的数据面入口。
每个本地注册的服务对应一个 `LocalServiceEntry`，直接持有 TCP listener 和 SHM 服务端实例。

### 8.2 LocalServiceEntry 结构

```cpp
struct LocalServiceEntry {
    Service*                        service;              // 业务服务指针
    uint16_t                        port;                 // 监听端口
    std::vector<IServerEndpoint*>  endpoints;            // 各传输端点（TCP listener / SHM UDS listener）
    std::map<int, IMessageConnection*> clients;             // 端点接入的客户端（非拥有）
    std::map<int, Buffer*>          client_recv_buffers;  // 客户端拆包缓冲
    std::set<int>                   endpoint_fds;         // 已注册到 EventLoop 的端点级 fd
    bool                            diag_enabled;
    uint32_t                        diag_topic_id;
};
```

### 8.3 已内联的方法

以下请求分派功能由 `OmniRuntime::Impl` 的私有方法实现：

- `onServiceEndpointEvent()` — 端点级 fd 事件（accept / 握手 / 可读映射）
- `onServiceClientReadable()` — 客户端可读回调（拆帧 + 分派）
- `onServiceClientAccepted()` — 端点接入新客户端回调
- `onInvokeRequest()` — 处理 `MSG_INVOKE`
- `onInvokeOneWayRequest()` — 处理 `MSG_INVOKE_ONEWAY`
- `dispatchLocalInvoke()` — interface/IDL hash 校验、调用 `Service::onInvoke()`、构造 reply

### 8.4 本地服务调用状态模型

本地服务调用路径使用显式返回值模型：

- `Service::onInvoke()` 返回 `int`，`0` 为成功，非 `0` 为错误
- runtime 根据返回值构造 `MSG_INVOKE_REPLY`

## 9. ConnectionManager

### 9.1 作用

`ConnectionManager` 负责所有服务直连和数据面 transport 选择。

### 9.2 当前职责

- 服务连接池
- 调用 `createClientConnection()` 执行同机 / 跨机 transport 选择
- SHM -> TCP fallback
- 直连消息接收
- 直连断开通知

### 9.3 选择逻辑

1. 拿到目标服务 `ServiceInfo`
2. 调用 `createClientConnection(service_name, host, port, local_host_id, remote_host_id, shm_config, instance_id)`
3. 函数内部判断：同机先尝试 SHM，失败或跨机回退到 TCP
4. 成功返回 `IMessageConnection*`（`CONNECTED` 状态），以连接键存入 `ServiceConnection` 由连接生命周期管理
5. 失败返回 `NULL`，上层返回 `ERR_CONNECT_FAILED`

`ConnectionManager` 以连接键索引连接：`getOrCreateConnection(service_name, ...)` 是默认实例封装（键即服务名），`getOrCreateConnectionByKey(connection_key, service_name, ...)` 供显式逻辑实例寻址使用（键由 `makeServiceConnectionKey(name, instance_id)` 生成）。`service_name` 与 `connection_key` 分离，SHM 握手路径仍以真实服务名 + `instance_id` 派生；endpoint 变化时连接管理器按逻辑键重建连接。

`createClientConnection()` 位于 `src/transport/transport_selector.{h,cpp}`，是新增传输的落点，但属**源码级、侵入式**扩展：
新增传输还需同步改动 core 侧（`TransportType` 分支、服务端端点半创建、topic 订阅模型等），并非在此单点即可接入；当前不提供动态插件 / provider 注入 ABI。
仅适用于与"点对点连接"模型一致的传输；总线型传输（I2C/RS-485 等）拓扑与寻址模型不同，不在当前支持范围。
`ConnectionManager` 出站连接使用 `createClientConnection()`；本地服务 hosting 在 `OmniRuntime::Impl` 中
通过 `createServerEndpoint()` 创建服务端端点（`TransportType::TCP` 与 `TransportType::SHM`），
不经过 `createClientConnection()`。
ServiceManager 控制面仍固定使用 TCP。

## 10. ShmClientConnection / ShmServerEndpoint

### 10.1 作用

`ShmClientConnection`（客户端）与 `ShmServerEndpoint`（服务端端点）提供同机低延迟数据面传输，底层共用 `shm_ring.h` 的 per-client ring 布局与读写函数。

### 10.2 Per-client SHM 架构

每个客户端创建自己独立的共享内存块，各自持有独立的 request ring 和 response ring。

### 10.3 Per-client SHM 内存布局

每个客户端连接对应一块独立 SHM：

```text
[ShmControlBlock: magic, version, req_ring_capacity, resp_ring_capacity, ready_flag]
[Request Ring header + data]   — client 写入，server 读取
[Response Ring header + data]  — server 写入，client 读取
```

### 10.4 ShmControlBlock 关键字段

- `magic`
- `version`
- `req_ring_capacity`
- `resp_ring_capacity`
- `ready_flag`

每个 SHM 仅属于一个客户端，不存在跨客户端竞争，因此无需 spinlock。

### 10.5 服务端数据结构

服务端持有 `clients_` 映射表，按 `client_id` 索引，值为实现 `IMessageConnection` 的 `ShmServerConnection`：

```text
clients_ : map<client_id, ShmServerConnection*>

ShmServerConnection:
  - shm_name / shm_addr / shm_size
  - ctrl_ / req_ring_capacity_ / resp_ring_capacity_
  - resp_eventfd_      — 服务端写入 response 后通知客户端
  - request_notify_fd_ — 客户端请求通知（Windows 平台）
  - liveness_channel_  — 检测客户端退出的握手通道
```

客户端无固定上限，仅受系统 SHM 资源限制。

### 10.6 SHM 建立流程（UDS 握手）

1. 客户端创建自己的 SHM，初始化 `ShmControlBlock` 和 request/response ring
2. 客户端通过 UDS 连接服务端，发送自己的 `client_shm_name`
3. 服务端打开并映射客户端 SHM
4. 服务端通过 UDS 回复 `[resp_eventfd, master_eventfd]`
5. 客户端设置 `ready_flag`，握手完成

服务端 UDS 监听路径由 `generateShmName(service_name, instance_id, host_id)` → `shmHandshakePath()` 派生：`host_id` 与 `instance_id` 参与哈希输入但不进入可见前缀，保证同机同名多实例、以及多板仿真（同服务名同实例、不同 `host_id`）各自派生不同路径，互不 unlink 抢占。

### 10.7 SHM 数据流模型

- request：
  - 客户端写入自己的 request ring
  - 通知服务端 `master_eventfd`
  - 服务端 `ShmServerEndpoint::onPollEvent()` 扫描所有客户端连接，依次 drain request ring
- response：
  - 服务端经客户端连接 `IMessageConnection::send()` 写入对应客户端的 response ring
  - 通知 `resp_eventfd`
  - 客户端从自己的 response ring 读取
- 通知：
  - `master_eventfd`（服务端统一接收）
  - `resp_eventfd`（每个客户端独立）

### 10.8 默认配置

- request ring：`4KB`
- response ring：`4KB`
- 无 `max_clients` 限制

服务端可通过 `Service::setShmConfig()` 显式放大每个客户端 ring 容量。

## 11. ServiceManager 内部角色

`ServiceManager` 当前由这些核心组件组成：

- `ServiceRegistry`
- `Session deadline`（控制面 runtime / 数据面 client）
- `ServiceWatchNotifier`
- `TopicManager`

### 11.1 ServiceRegistry

负责：

- 注册服务表
- `name -> (instance_key -> ServiceEntry)` 两级映射，`instance_key` 由 `makeServiceInstanceKey(instance_id)` 生成
- handle -> 实例条目、控制 fd -> 实例条目等索引（同一 fd 同名至多一个实例）
- fd ownership 关联

同逻辑身份 `(name, instance_id)` 重复注册为后来者顶替/迁移（保留原 handle、刷新 ServiceInfo、endpoint 与控制 fd，并输出被顶替的旧 fd）；不同 instance_id 才是不同实例，新增条目并存。host_id 不参与注册表键。按名查找（`findService`）返回逻辑实例键字典序第一实例，完整枚举由 `listServiceInstances` 提供。

### 11.2 Session deadline

负责：

- 控制面：每个 runtime 控制会话一个 deadline 定时器，`MSG_CONTROL_HEARTBEAT` 到达后续期；超时 `closeClient(fd)` 统一注销全部服务/订阅/诊断状态
- 数据面：每个 client session 一个 deadline 定时器，`MSG_HEARTBEAT` 到达后续期；超时 `onServiceClientDisconnected` 清理该 client 的订阅与缓冲
- 低功耗长超时：无固定上限，deadline 可到分钟/小时级

### 11.3 ServiceWatchNotifier

负责：

- 管理按 service_name + instance_id 过滤的持久 watch 组；host_id 仅为兼容字段
- 实例注册、注销、心跳超时与 takeover 时向匹配客户端下发 online/offline 通知

### 11.4 TopicManager

负责：

- topic publisher / subscriber 控制关系
- publisher owner 校验
- topic 发布者信息下发

## 12. 控制面完整数据流

### 12.1 服务注册数据流

```text
Service Process
  -> OmniRuntime::registerService()
  -> 创建 TCP 监听 / SHM / ServiceInfo
  -> SmControlChannel::sendMessage(MSG_REGISTER)
  -> ServiceManagerApp::handleRegister()
  -> ServiceRegistry::addService()
  -> 返回 MSG_REGISTER_REPLY
  -> OmniRuntime 更新本地 service 状态
```

### 12.2 服务发现数据流

```text
Client
  -> OmniRuntime::lookupService()
  -> SmControlChannel::sendMessage(MSG_LOOKUP)
  -> ServiceManagerApp::handleLookup()
  -> ServiceRegistry::findService()   // 同名多实例时返回实例键字典序第一实例
  -> 返回 ServiceInfo
  -> OmniRuntime / ConnectionManager 使用返回的 host_id / instance_id / shm_config

Client（需要枚举同名全部实例时）
  -> OmniRuntime::listServiceInstances(name)
  -> SmControlChannel::sendMessage(MSG_LIST_INSTANCES)
  -> ServiceManagerApp::handleListInstances()
  -> ServiceRegistry::listServiceInstances(name)   // 实例键字典序
  -> 返回 ServiceInfo 列表
```

### 12.3 服务状态 watch 数据流

```text
Client
  -> OmniRuntime::watchServices()
  -> SmControlChannel::sendMessage(MSG_SUBSCRIBE_SERVICE_WATCH)
  -> ServiceManagerApp::handleSubscribeServiceWatch()
  -> ServiceWatchNotifier 记录订阅关系
  -> 返回 MSG_SUBSCRIBE_SERVICE_WATCH_REPLY
  -> OmniRuntime 先登记本地 watch；reply 成功后标记 active 并派发补发事件
  -> SM 未就绪/重连时保留本地登记，连接恢复后重放 MSG_SUBSCRIBE_SERVICE_WATCH
```

### 12.4 服务状态通知数据流

```text
ServiceManager
  -> Session deadline 检测超时 / 连接断开
  -> ServiceRegistry 删除服务
  -> ServiceWatchNotifier 生成服务状态通知
  -> 通过控制面 TCP 发送给订阅者
  -> OmniRuntime::onSMMessage()
  -> 执行本地 service state callback
```

## 13. RPC 完整数据流

### 13.1 TCP 同步 RPC

```text
Caller
  -> 之前已通过 connectService()/Proxy::connect() 建立 TCP 直连
  -> OmniRuntime::invoke()
  -> 使用已建立的 ConnectionManager TCP 直连
  -> 发送 MSG_INVOKE

Service Side
  -> TcpServerEndpoint 拆 Message frame
  -> OmniRuntime::handleServiceClientMessage()
  -> OmniRuntime::onInvokeRequest()
  -> Service::onInvoke()
  -> 构造 MSG_INVOKE_REPLY
  -> 返回 caller

Caller
  -> ConnectionManager 收到 reply
  -> RpcRuntime::waitForReply() 完成
  -> invoke() 返回 response
```

### 13.2 SHM 同步 RPC

```text
Caller
  -> 之前已通过 connectService()/Proxy::connect() 建立或复用 SHM 连接
  -> OmniRuntime::invoke()
  -> 使用已建立的 ConnectionManager SHM 直连
  -> 首次连接时已创建 per-client SHM
  -> UDS 握手：发送 client_shm_name，接收 [resp_eventfd, master_eventfd]
  -> 写入自己的 request ring
  -> notify master_eventfd

Service Side
  -> EventLoop 被 master_eventfd 唤醒
  -> ShmServerEndpoint::onPollEvent() 扫描各 client 的 request ring
  -> OmniRuntime::handleServiceClientMessage()
  -> OmniRuntime 执行 invoke 细节与 reply 构造
  -> 服务端客户端连接（IMessageConnection::send）写入对应 client 的 response ring
  -> notify resp_eventfd

Caller
  -> EventLoop 被 resp_eventfd 唤醒
  -> ShmClientConnection::recv() 从自己的 response ring 读取
  -> RpcRuntime::waitForReply() 完成
  -> invoke() 返回 response
```

### 13.3 one-way 调用数据流

```text
Caller
  -> OmniRuntime::invokeOneWay()
  -> 发送 MSG_INVOKE_ONEWAY

Service Side
  -> 收到消息
  -> interface 校验
  -> Service::onInvoke()
  -> 无 reply
```

### 13.4 interface mismatch 数据流

```text
Caller
  -> 发起 invoke / invokeOneWay

Service Side
  -> 解析 interface_id
  -> 与 service->interfaceInfo().interface_id 比较
  -> 不一致 => 直接硬拒绝

结果
  -> 同步调用返回 ERR_INTERFACE_NOT_FOUND
  -> one-way 直接丢弃，不进入业务实现
```

## 14. 发布订阅完整数据流

### 14.1 topic 发布注册数据流

```text
Publisher
  -> OmniRuntime::publishTopic()
  -> 组装 publisher ServiceInfo
  -> SmControlChannel::sendMessage(MSG_PUBLISH_TOPIC)
  -> ServiceManagerApp::handlePublishTopic()
  -> TopicManager::registerPublisher()
  -> 返回 MSG_PUBLISH_TOPIC_REPLY
  -> 仅 reply 成功后，本地 TopicRuntime 记录 published topic
```

### 14.2 topic 订阅注册数据流

```text
Subscriber
  -> OmniRuntime::subscribeTopic()（无选择器 mode 0，或 TopicSubscriptionSelector 服务级/实例级）
  -> SmControlChannel::sendMessage(MSG_SUBSCRIBE_TOPIC)
  -> ServiceManagerApp::handleSubscribeTopic()
  -> TopicManager::addSubscriber()
  -> 返回 MSG_SUBSCRIBE_TOPIC_REPLY
  -> 仅 reply 成功后，本地 TopicRuntime 记录 callback/selector 和 topic id
```

### 14.3 publisher 信息下发数据流

```text
ServiceManager
  -> 发现某 topic 有订阅者且已有 publisher
  -> 按 (发布者服务名, 主题名) 与订阅选择器匹配
  -> 向匹配的 subscriber 发送 MSG_TOPIC_PUBLISHER_NOTIFY（每个发布者实例一条）

Subscriber
  -> OmniRuntime::onSMMessage()
  -> topic_runtime_.matchSubscriptions() 过滤
  -> ensureTopicPublisherConnection()
  -> ConnectionManager::getOrCreateConnectionByKey(makeServiceConnectionKey(pub_name, instance_id), ...)
  -> 发送 MSG_SUBSCRIBE_BROADCAST 给 publisher
```

### 14.4 topic 广播数据流

```text
Publisher
  -> OmniRuntime::broadcast(topic_id, data)
  -> TopicRuntime 查询 publisher 侧 subscriber 列表
  -> 对 TCP subscriber 逐个发送 MSG_BROADCAST
  -> 对 SHM subscriber service 逐个写入其响应 ring

Subscriber
  -> 收到 MSG_BROADCAST
  -> 按连接登记的发布者实例身份回填来源
  -> TopicRuntime::dispatchFrom(publisher_service, host_id, instance_id, topic_id, data)
  -> 执行本地 callback（带来源回调可获知 host_id / instance_id）
```

## 15. examples / tools / tests 与实现的对应关系

### 15.1 examples

当前 C / C++ examples 已经验证：

- 服务注册
- 同步 RPC
- topic 发布 / 订阅
- `omni-cli` 查询和调用

### 15.2 omni-idlc

当前 `omni-idlc` 已实测可对 `.bidl` 文件成功生成代码。

### 15.3 关键测试

当前关键测试覆盖：

- transport
- SHM
- integration
- full integration
- control plane and fallback
- performance

## 16. 当前实现结论

当前架构已经形成一个清晰的稳定点：

- 控制面、RPC、Topic 已形成清晰的 runtime 边界
- `OmniRuntime` 已主要承担协调层职责
- SHM / TCP / control plane / topic 的关键路径均已有明确边界
- examples、工具链、性能测试和关键回归均已对应当前实现验证

这个状态对二次开发者来说，已经具备：

- 可理解的模块边界
- 可定位的核心文件
- 可追踪的完整数据流
- 可扩展的实现基础（注意：传输层为源码级、侵入式扩展，总线型传输不在支持范围）

## 17. 连接管理架构

### 17.1 概述

连接管理模块负责：
- 主动建立和断开与远程服务的连接
- 自动重连机制（指数退避策略）
- 心跳检测（及时发现服务异常）

### 17.2 核心 API

```cpp
class OmniRuntime {
    // 连接管理
    int connectService(const std::string& service_name);
    int connectServiceInstance(const std::string& service_name,
                               const std::string& instance_id);
    int disconnectService(const std::string& service_name);
    int disconnectServiceInstance(const std::string& service_name,
                                  const std::string& instance_id);
    bool isServiceConnected(const std::string& service_name) const;
    bool isServiceInstanceConnected(const std::string& service_name,
                                    const std::string& instance_id) const;

    // 实例发现
    int listServiceInstances(const std::string& service_name,
                             std::vector<ServiceInfo>& instances);

    // 自动重连
    void enableAutoReconnect(const std::string& service_name, bool enable = true);
    void setReconnectInterval(const std::string& service_name, uint32_t interval_ms);

    // 心跳检测
    void startHeartbeat(const std::string& service_name,
                        uint32_t interval_ms = DEFAULT_HEARTBEAT_INTERVAL,
                        uint32_t timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT);
    void stopHeartbeat(const std::string& service_name);

    // 服务实例状态 watch
    int watchServices(const std::vector<ServiceWatchRequest>& requests,
                      const ServiceWatchCallback& callback, uint64_t& watch_id);
    int unwatchServices(uint64_t watch_id);
};
```

> `enableAutoReconnect` / `setReconnectInterval` / `startHeartbeat` / `stopHeartbeat` 接收的参数就是**连接键**：传服务名即作用于默认实例，传 `makeServiceConnectionKey(name, instance_id)` 即作用于该逻辑实例。生成的 Proxy（C++ / C）在绑定实例后会自动传入实例连接键，因此绑定实例的 invoke / 心跳 / 重连 / 断开都寻址该实例；host_id 不参与连接键。

### 17.3 ServiceProxyBase 基类

生成的 Proxy 类继承自 `ServiceProxyBase`，提供统一的连接管理接口：

```cpp
class ServiceProxyBase {
public:
    int connect();           // 查询服务 + 建立连接 + 注册服务状态通知
    int connect(const std::string& instance_id);  // 绑定逻辑实例后连接
    int connect(const std::string& host_id, const std::string& instance_id);  // 兼容旧调用
    void setInstance(const std::string& instance_id);  // 设置下次 connect 目标
    void setInstance(const std::string& host_id, const std::string& instance_id);  // 兼容旧调用
    void disconnect();       // 断开连接 + 停止心跳
    bool isConnected() const;
    void enableAutoReconnect(bool enable = true);
    void setReconnectInterval(uint32_t interval_ms);
    void startHeartbeat(uint32_t interval_ms = DEFAULT_HEARTBEAT_INTERVAL,
                        uint32_t timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT);
    void stopHeartbeat();
    void OnServiceDied(const std::function<void()>& callback);

protected:
    // 当前实例绑定的数据面连接键；生成的 Proxy 方法据此寻址
    const std::string& connectionKey() const;
};
```

### 17.4 自动重连机制

#### 触发条件

1. **服务状态通知**：SM 发送 `MSG_SERVICE_WATCH_NOTIFY`
2. **心跳超时**：心跳 ACK 超时未收到

#### 重连流程

```
触发重连
    │
    ├── 清除服务缓存（ServiceState.info）
    ├── 移除连接（conn_mgr_->removeConnection）
    │
    └── scheduleReconnect(interval_ms)
            │
            └── 定时器触发 tryReconnectService
                    │
                    ├── connectServiceInternal
                    │       │
                    │       ├── lookupServiceInfo（查询 SM 获取最新信息）
                    │       │
                    │       └── getOrCreateConnection（建立连接）
                    │
                    ├── 成功 → 重置重试计数
                    │
                    └── 失败 → 递增延迟重试（指数退避）
```

#### 指数退避策略

```cpp
uint32_t shift = current_retry < 5 ? current_retry : 5;
int64_t  delay64 = interval_ms * (static_cast<int64_t>(1) << shift);
uint32_t delay_ms = delay64 > 30000 ? 30000 : static_cast<uint32_t>(delay64);  // 上限 30s
```

- 第 1 次重试：interval_ms * 1
- 第 2 次重试：interval_ms * 2
- 第 3 次重试：interval_ms * 4
- 第 4 次重试：interval_ms * 8
- 第 5 次及以后：interval_ms * 32，并 clamp 到 30s（如 interval_ms=1000 时为 30s）
- `scheduleReconnect()` 定时前再叠加 ±25% jitter（`addReconnectJitter`），避免多客户端同时重连

### 17.5 心跳与会话

控制面（Runtime ↔ SM）：

```text
init()
  -> MSG_RUNTIME_HELLO { runtime_id, incarnation_id, interval, requested_timeout }
  -> MSG_RUNTIME_HELLO_REPLY { accepted_interval, effective_timeout, sm_incarnation }
  每个 runtime 一个会话 + 一个 deadline timer
  -> MSG_CONTROL_HEARTBEAT { incarnation } 到达后重挂 deadline
  -> MSG_CONTROL_HEARTBEAT_ACK { sm_incarnation } 清除 runtime 侧 ACK deadline
  -> 超时: closeClient(fd) 注销该 runtime 全部服务/订阅/诊断并逐服务发服务状态通知
```

数据面（Client ↔ Service）：

```text
connectService / Proxy.connect()
  -> ensureDataSession()
     -> MSG_SESSION_HELLO { runtime_id, incarnation, interval, requested_timeout }
     -> MSG_SESSION_HELLO_ACK { accepted_interval, effective_timeout, service_incarnation }
  -> 自动启动数据面心跳
     -> MSG_HEARTBEAT { incarnation_id }
     -> MSG_HEARTBEAT_ACK { service_incarnation }
     -> 每次 ACK 重挂 client 侧 deadline
  -> 超时: handleServiceLost -> 移除连接 -> 重连 -> 重新 ensureDataSession
           -> 重放 MSG_SUBSCRIBE_BROADCAST
```

服务端为每个 client 维护独立 session：

- 连接第一帧必须是 `MSG_SESSION_HELLO`，否则断开；
- session 独立 deadline timer，`MSG_HEARTBEAT` 到达后续期；
- 超时 `onServiceClientDisconnected()` 清理订阅、接收缓冲并关闭连接；
- 同 `(host_id, runtime_id)` 不同 `incarnation_id` 的新代际立即顶替旧会话；
  同代际的多条连接允许并存。

服务端自动响应心跳：

```cpp
if (msg.getType() == MessageType::MSG_HEARTBEAT) {
    Message ack(MessageType::MSG_HEARTBEAT_ACK, msg.getSequence());
    ack.payload.writeUint32(incarnation_id_);  // 本服务端代际
    sendOnFdBestEffort(transport, ack);
}
```

### 17.6 生成代码结构

生成的 Proxy 类继承自 `ServiceProxyBase`，基类提供连接管理、自动重连、心跳等通用功能：

```cpp
class SensorServiceProxy : public omnibinder::ServiceProxyBase {
public:
    explicit SensorServiceProxy(OmniRuntime& runtime)
        : ServiceProxyBase(runtime, "SensorService") {}

    // 业务方法（仅生成这些）
    int GetData(SensorData* out) {
        Buffer req, resp;
        // connectionKey() 默认等于服务名；绑定实例后为实例连接键
        int ret = runtime_.invoke(connectionKey(), 0x..., 0x..., 0x..., req, resp, 0);
        if (ret != 0) return ret;
        if (!out) return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
        // ... 反序列化 resp 到 out
        return 0;
    }

    // 话题订阅：服务级 / 服务级带来源 / 实例级
    void SubscribeSensorUpdate(const std::function<void(const SensorUpdate&)>& cb);
    void SubscribeSensorUpdateWithSource(
        const std::function<void(const std::string& host_id,
                                 const std::string& instance_id,
                                 const SensorUpdate&)>& cb);
    void SubscribeSensorUpdateInstance(
        const std::string& instance_id,
        const std::function<void(const std::string&, const std::string&,
                                 const SensorUpdate&)>& cb);
};
```

### 17.7 使用示例

```cpp
// 创建 Proxy
demo::SensorServiceProxy proxy(runtime);

// 连接服务（建立连接 + 注册服务状态通知）
// proxy 构造后由 runtime start()/重连自动连接；实例绑定使用 setInstance()

// 或：绑定到指定实例（同名多实例场景）
// proxy.setInstance(instance_id);

// 启用自动重连
proxy.enableAutoReconnect(true);

// 可选：自定义数据面心跳（不调用则使用默认 3000/10000 自动启动）
proxy.startHeartbeat(3000, 10000);  // 默认策略；低功耗可传分钟/小时级

// 调用方法（直接使用已建立的连接）
SensorData data;
proxy.GetData(&data);

// 断开连接（自动停止心跳）
proxy.disconnect();
```
