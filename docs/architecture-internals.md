# OmniBinder 内部实现架构说明

## 1. 文档目的

本文档面向准备对 OmniBinder 做二次开发、性能调优、模块替换或行为扩展的开发者。

文档目标是：

- 准确描述当前实现中的核心模块、职责边界和关键数据结构
- 说明控制面、数据面、SHM、RPC、发布订阅、死亡通知的完整数据流
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
|   Transport Layer: TcpTransport / ShmTransport                     |
+-------------------------------------------------------------------+
|                     ServiceManager（独立进程）                      |
|   ServiceRegistry / HeartbeatMonitor / DeathNotifier / TopicManager |
+-------------------------------------------------------------------+
```

### 2.2 两类平面

OmniBinder 的内部实现分成两个平面：

- **控制面**：注册、发现、心跳、死亡通知、topic 关系管理
- **数据面**：RPC、one-way、broadcast 数据传输

控制面由 `ServiceManager` 统一处理，数据面由服务间直连处理。

## 3. 代码入口与主要文件

### 3.1 核心运行时文件

- `src/core/omni_runtime.h`
- `src/core/omni_runtime.cpp`
- `src/core/omni_sm.cpp` — ServiceManager 控制面通信
- `src/core/omni_service.cpp` — 本地服务注册 / 注销
- `src/core/omni_connection.cpp` — 连接管理与心跳
- `src/core/omni_rpc.cpp` — RPC 调用与回复
- `src/core/omni_topic.cpp` — topic 发布/订阅/广播
- `src/core/omni_dispatch.cpp` — 服务端请求分派
- `src/core/omni_diag.cpp` — 诊断 watch 数据面
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

- `src/transport/tcp_transport.h`
- `src/transport/tcp_transport.cpp`
- `src/transport/shm_transport.h`
- `src/transport/shm_transport.cpp`
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
- `service_manager/heartbeat_monitor.h`
- `service_manager/heartbeat_monitor.cpp`
- `service_manager/death_notifier.h`
- `service_manager/death_notifier.cpp`
- `service_manager/topic_manager.h`
- `service_manager/topic_manager.cpp`

## 4. OmniRuntime::Impl 的角色

### 4.1 主要职责

`OmniRuntime::Impl` 是运行时装配与协调中心，负责：

- 初始化和销毁 runtime
- 管理 `ServiceManager` 连接生命周期
- 注册和注销本地服务
- 协调控制面、RPC、topic、本地服务托管、连接管理之间的交互
- 持有高层缓存和全局状态（如服务缓存、死亡回调、本地服务表）

### 4.2 关键成员

核心成员包括：

- `EventLoop* loop_`
- `SmControlChannel sm_channel_`
- `RpcRuntime rpc_runtime_`
- `TopicRuntime topic_runtime_`
- `ConnectionManager* conn_mgr_`
- `std::map<std::string, LocalServiceEntry*> local_services_`
- `std::map<int, std::string> client_fd_to_service_`
- `std::map<std::string, ServiceInfo> service_cache_`
- `std::map<std::string, DeathCallback> death_callbacks_`

### 4.3 LocalServiceEntry

每个本地注册服务对应一个 `LocalServiceEntry`，包含：

- `Service* service`
- `TcpTransportServer* server` — TCP listener
- `ShmTransport* shm_transport` — SHM 服务端（UDS listener + per-client context）
- `std::map<int, ITransport*> client_transports` — accept 的 TCP 客户端
- `uint16_t port`
- `std::string advertise_host`
- `std::map<int, Buffer*> client_recv_buffers`

这个结构代表“一个本地服务完整的对外暴露状态”。

## 5. SmControlChannel

### 5.1 作用

`SmControlChannel` 只负责 `OmniRuntime` 与 `ServiceManager` 之间的控制通道。

### 5.2 关键状态

- `TcpTransport* transport`
- `Buffer recv_buffer`
- `std::map<uint32_t, PendingReplySlot> pending_replies_`

### 5.3 核心职责

- 发送控制面消息
- 接收控制面字节流
- 从接收缓冲中切出完整 `Message`
- 管理等待中的 reply

### 5.4 关键方法

- `isConnected()`
- `sendMessage(const Message&)`
- `recvSome(uint8_t*, size_t)`
- `appendReceived(const uint8_t*, size_t)`
- `tryPopMessage(Message&)`
- `beginWait(seq)`
- `isWaiting(seq)`
- `pendingReply(seq)`
- `storeReply(seq, msg)`
- `eraseWait(seq)`

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
- `endWait()`
- `waitForReply(...)`

### 6.5 与其他模块的关系

- 它不直接持有 transport
- 它通过 `SmControlChannel` 读取 pending reply
- 它通过外部传入的 reply-wait pump 驱动事件循环

### 6.6 当前等待语义

当前 `waitForReply()` 不再使用会执行 `post()` functor 的完整 `pollOnce()`。

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

- `callbacks_`
- `topic_name_to_id_`
- `callbacks_by_id_`
- `tcp_subscribers_`
- `shm_subscribers_`
- `published_topics_`（private，通过 `isTopicPublished()` / `getTopicId()` 访问）
- `published_topic_owners_`

### 7.3 当前职责

- 记录 topic callback
- 维护 topic name / id 映射
- 记录已发布 topic 及其 owner service
- 记录 publisher 侧 TCP / SHM subscriber 集合
- 在本地 dispatch 广播回调

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

`OmniRuntime::Impl` 直接管理本地服务的数据面入口，不再经过独立的 Host 抽象层。
每个本地注册的服务对应一个 `LocalServiceEntry`，直接持有 TCP listener 和 SHM 服务端实例。

### 8.2 LocalServiceEntry 结构

```cpp
struct LocalServiceEntry {
    Service*            service;              // 业务服务指针
    TcpTransportServer* server;               // TCP listener
    ShmTransport*       shm_transport;        // SHM 服务端（UDS listener + per-client context）
    uint16_t            port;                 // 监听端口
    std::map<int, ITransport*>  client_transports;   // accept 的 TCP 客户端
    std::map<int, Buffer*>      client_recv_buffers; // 客户端拆包缓冲
    bool                diag_enabled;
    uint32_t            diag_topic_id;
};
```

### 8.3 已内联的方法

以下原属于 `ServiceHostRuntime` 的功能，现已作为 `OmniRuntime::Impl` 的私有方法实现：

- `onServiceAccept()` — TCP accept 回调
- `onServiceClientData()` — TCP 客户端数据回调（拆包 + 分派）
- `onShmRequest()` — SHM 请求回调（拆帧 + 分派）
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
- 调用 `selectTransport()` 自由函数执行同机 / 跨机 transport 选择
- SHM -> TCP fallback
- 直连消息接收
- 直连断开通知

### 9.3 选择逻辑

1. 拿到目标服务 `ServiceInfo`
2. 调用 `selectTransport(service_name, host, port, local_host_id, remote_host_id, shm_config)`
3. 函数内部判断：同机先尝试 SHM，失败或跨机回退到 TCP
4. 成功返回 `ITransport*`（`CONNECTED` 状态），移入 `ServiceConnection` 由连接生命周期管理
5. 失败返回 `NULL`，上层返回 `ERR_CONNECT_FAILED`

`selectTransport()` 位于 `src/transport/transport_selector.cpp`，是源码级扩展点。
如需添加新传输类型，在此函数中扩展。这不是动态插件 ABI。
`ConnectionManager` 出站连接使用 `selectTransport()`；本地服务 hosting 在 `OmniRuntime::Impl` 中
直接创建 `TcpTransportServer` 和 `ShmTransport`（不经过 `selectTransport()`）。
ServiceManager 控制面仍固定使用 TCP。

## 10. ShmTransport

### 10.1 作用

`ShmTransport` 提供同机低延迟数据面传输。

### 10.2 架构变更：从全局 SHM 到 per-client SHM

旧架构使用一块全局 SHM，所有客户端共用一个 request ring，服务端按 slot 写回 response。
新架构改为 **per-client SHM**：每个客户端创建自己独立的共享内存块，各自持有独立的 request ring 和 response ring。

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

不再包含 `max_clients`、`response_bitmap`、`req_lock`、`slots[32]` 等字段。
每个 SHM 仅属于一个客户端，不存在跨客户端竞争，因此无需 spinlock。

### 10.5 服务端数据结构

服务端持有 `client_contexts_` 映射表，按 `client_id` 索引：

```text
client_contexts_ : map<client_id, ClientShmContext>

ClientShmContext:
  - shm_fd
  - mapped_address
  - shm_size
  - client_eventfd   — 客户端用于通知 server 的 eventfd
  - resp_eventfd     — server 写入 response 后通知客户端
```

客户端无固定上限，仅受系统 SHM 资源限制。

### 10.6 SHM 建立流程（UDS 握手）

1. 客户端创建自己的 SHM，初始化 `ShmControlBlock` 和 request/response ring
2. 客户端通过 UDS 连接服务端，发送自己的 `client_shm_name`
3. 服务端打开并映射客户端 SHM
4. 服务端通过 UDS 回复 `[resp_eventfd, master_eventfd]`
5. 客户端设置 `ready_flag`，握手完成

### 10.7 SHM 数据流模型

- request：
  - 客户端写入自己的 request ring
  - 通知服务端 `master_eventfd`
  - 服务端 `serverRecv()` 遍历所有 `client_contexts_`，依次 drain
- response：
  - 服务端 `serverSend(client_id)` 写入对应客户端的 response ring
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
- `HeartbeatMonitor`
- `DeathNotifier`
- `TopicManager`

### 11.1 ServiceRegistry

负责：

- 注册服务表
- `name -> ServiceEntry`
- fd ownership 关联

### 11.2 HeartbeatMonitor

负责：

- 心跳超时检测（`missed_count` 已改为局部变量，不再持久化）
- 定期检查服务是否死亡

### 11.3 DeathNotifier

负责：

- 通知所有订阅了目标服务死亡事件的客户端

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
  -> ServiceManager::handleRegister()
  -> ServiceRegistry::addService()
  -> 返回 MSG_REGISTER_REPLY
  -> OmniRuntime 更新本地 service 状态
```

### 12.2 服务发现数据流

```text
Client
  -> OmniRuntime::lookupService()
  -> SmControlChannel::sendMessage(MSG_LOOKUP)
  -> ServiceManager::handleLookup()
  -> ServiceRegistry::findService()
  -> 返回 ServiceInfo
  -> OmniRuntime / ConnectionManager 使用返回的 host_id / shm_config
```

### 12.3 服务死亡订阅数据流

```text
Client
  -> OmniRuntime::subscribeServiceDeath()
  -> SmControlChannel::sendMessage(MSG_SUBSCRIBE_SERVICE)
  -> ServiceManager::handleSubscribeService()
  -> DeathNotifier 记录订阅关系
  -> 返回 MSG_SUBSCRIBE_SERVICE_REPLY
  -> OmniRuntime 仅在 reply 成功后更新本地 death callback
```

### 12.4 服务死亡通知数据流

```text
ServiceManager
  -> HeartbeatMonitor 检测超时 / 连接断开
  -> ServiceRegistry 删除服务
  -> DeathNotifier 生成死亡通知
  -> 通过控制面 TCP 发送给订阅者
  -> OmniRuntime::onSMMessage()
  -> 执行本地 death callback
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
  -> TcpTransportServer 拆 Message frame
  -> OmniRuntime::onHostedMessage()
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
  -> ShmTransport::serverRecv() drain request ring
  -> OmniRuntime::onHostedMessage()
  -> OmniRuntime 执行 invoke 细节与 reply 构造
  -> ShmTransport::serverSend(client_id) 写入对应 client 的 response ring
  -> notify resp_eventfd

Caller
  -> EventLoop 被 resp_eventfd 唤醒
  -> ShmTransport::recv() 从自己的 response ring 读取
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
  -> ServiceManager::handlePublishTopic()
  -> TopicManager::registerPublisher()
  -> 返回 MSG_PUBLISH_TOPIC_REPLY
  -> 仅 reply 成功后，本地 TopicRuntime 记录 published topic
```

### 14.2 topic 订阅注册数据流

```text
Subscriber
  -> OmniRuntime::subscribeTopic()
  -> SmControlChannel::sendMessage(MSG_SUBSCRIBE_TOPIC)
  -> ServiceManager::handleSubscribeTopic()
  -> TopicManager::addSubscriber()
  -> 返回 MSG_SUBSCRIBE_TOPIC_REPLY
  -> 仅 reply 成功后，本地 TopicRuntime 记录 callback 和 topic id
```

### 14.3 publisher 信息下发数据流

```text
ServiceManager
  -> 发现某 topic 有订阅者且已有 publisher
  -> 向 subscriber 发送 MSG_TOPIC_PUBLISHER_NOTIFY

Subscriber
  -> OmniRuntime::onSMMessage()
  -> ensureTopicPublisherConnection()
  -> ConnectionManager 建立到 publisher 的直连
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
  -> TopicRuntime::dispatch(topic_id, data)
  -> 执行本地 callback
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

- 控制面、RPC、Topic、ServiceHost 已形成清晰的 runtime 边界
- `OmniRuntime` 已主要承担协调层职责
- SHM / TCP / control plane / topic 的关键路径均已有明确边界
- examples、工具链、性能测试和关键回归均已对应当前实现验证

这个状态对二次开发者来说，已经具备：

- 可理解的模块边界
- 可定位的核心文件
- 可追踪的完整数据流
- 可扩展的实现基础

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
    int disconnectService(const std::string& service_name);
    bool isServiceConnected(const std::string& service_name) const;
    
    // 自动重连
    void enableAutoReconnect(const std::string& service_name, bool enable = true);
    void setReconnectInterval(const std::string& service_name, uint32_t interval_ms);
    
    // 心跳检测
    void startHeartbeat(const std::string& service_name, uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000);
    void stopHeartbeat(const std::string& service_name);
};
```

### 17.3 ServiceProxyBase 基类

生成的 Proxy 类继承自 `ServiceProxyBase`，提供统一的连接管理接口：

```cpp
class ServiceProxyBase {
public:
    int connect();           // 查询服务 + 建立连接 + 注册死亡通知
    void disconnect();       // 断开连接 + 停止心跳
    bool isConnected() const;
    void enableAutoReconnect(bool enable = true);
    void setReconnectInterval(uint32_t interval_ms);
    void startHeartbeat(uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000);
    void stopHeartbeat();
};
```

### 17.4 自动重连机制

#### 触发条件

1. **服务死亡通知**：SM 发送 `MSG_DEATH_NOTIFY`
2. **心跳超时**：心跳 ACK 超时未收到

#### 重连流程

```
触发重连
    │
    ├── 清除缓存（service_cache_）
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
uint32_t delay_ms = interval_ms * (1u << (current_retry < 5 ? current_retry : 5));
```

- 第 1 次重试：interval_ms * 1
- 第 2 次重试：interval_ms * 2
- 第 3 次重试：interval_ms * 4
- 第 4 次重试：interval_ms * 8
- 第 5 次及以后：interval_ms * 32（最大延迟）

### 17.5 心跳机制

#### 工作流程

```
客户端                                     服务端
  │                                          │
  ├── startHeartbeat(interval, timeout)       │
  │       │                                   │
  │       └── 启动定时器                       │
  │               │                           │
  │               ├── sendHeartbeatToService() │
  │               │       │                   │
  │               │       └── MSG_HEARTBEAT ──>│
  │               │                           ├── 自动响应
  │               │       MSG_HEARTBEAT_ACK <──┤
  │               │       │                   │
  │               │       └── 更新 last_ack    │
  │               │                           │
  │               └── checkHeartbeatTimeout()  │
  │                       │                   │
  │                       ├── 未超时: 继续     │
  │                       │                   │
  │                       └── 超时:            │
  │                               ├── 清除缓存 │
  │                               ├── 移除连接 │
  │                               └── 触发重连 │
```

#### 服务端自动响应

`OmniRuntime::Impl` 的 TCP/SHM 请求分派方法自动响应 `MSG_HEARTBEAT`，无需业务代码干预：

```cpp
if (msg.getType() == MessageType::MSG_HEARTBEAT) {
    Message ack(MessageType::MSG_HEARTBEAT_ACK, msg.getSequence());
    // 发送 ACK 响应
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
        int ret = runtime_.invoke("SensorService", 0x..., 0x..., 0x..., req, resp, 0);
        if (ret != 0) return ret;
        if (!out) return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
        // ... 反序列化 resp 到 out
        return 0;
    }
};
```

### 17.7 使用示例

```cpp
// 创建 Proxy
demo::SensorServiceProxy proxy(runtime);

// 连接服务（建立连接 + 注册死亡通知）
proxy.connect();

// 启用自动重连
proxy.enableAutoReconnect(true);

// 可选：启动心跳（检测服务存活）
proxy.startHeartbeat(5000, 10000);  // 5秒间隔，10秒超时

// 调用方法（直接使用已建立的连接）
SensorData data;
proxy.GetData(&data);

// 断开连接（自动停止心跳）
proxy.disconnect();
```
