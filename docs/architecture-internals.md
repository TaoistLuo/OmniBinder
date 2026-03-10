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
|     |-- ServiceHostRuntime                                         |
|     `-- ConnectionManager                                          |
|                                                                   |
|   EventLoop                                                        |
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
- `src/core/sm_control_channel.h`
- `src/core/sm_control_channel.cpp`
- `src/core/rpc_runtime.h`
- `src/core/rpc_runtime.cpp`
- `src/core/topic_runtime.h`
- `src/core/topic_runtime.cpp`
- `src/core/service_host_runtime.h`
- `src/core/service_host_runtime.cpp`
- `src/core/connection_manager.h`
- `src/core/connection_manager.cpp`
- `src/core/event_loop.h`
- `src/core/event_loop.cpp`

### 3.2 传输层文件

- `src/transport/tcp_transport.h`
- `src/transport/tcp_transport.cpp`
- `src/transport/shm_transport.h`
- `src/transport/shm_transport.cpp`
- `src/transport/transport_factory.h`
- `src/transport/transport_factory.cpp`

### 3.3 控制面进程文件

- `service_manager/main.cpp`
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
- `ServiceHostRuntime service_host_runtime_`
- `ConnectionManager* conn_mgr_`
- `std::map<std::string, LocalServiceEntry*> local_services_`
- `std::map<int, std::string> listen_fd_to_service_`
- `std::map<int, std::string> client_fd_to_service_`
- `std::map<std::string, ServiceInfo> service_cache_`
- `std::map<std::string, DeathCallback> death_callbacks_`

### 4.3 LocalServiceEntry

每个本地注册服务对应一个 `LocalServiceEntry`，包含：

- `Service* service`
- `TcpTransportServer* server`
- `ShmTransport* shm_server`
- `std::string shm_name`
- `uint16_t port`
- `std::map<int, ITransport*> client_transports`
- `std::map<int, Buffer*> client_recv_buffers`

这个结构代表“一个本地服务完整的对外暴露状态”。

## 5. SmControlChannel

### 5.1 作用

`SmControlChannel` 只负责 `OmniRuntime` 与 `ServiceManager` 之间的控制通道。

### 5.2 关键状态

- `TcpTransport* transport`
- `Buffer recv_buffer`
- `std::map<uint32_t, Message*> pending_replies_`

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
- 它通过外部传入的 `pollOnce()` 驱动事件循环

## 7. TopicRuntime

### 7.1 作用

`TopicRuntime` 负责 topic 的本地运行时状态。

### 7.2 当前持有的数据

- `callbacks_`
- `topic_name_to_id_`
- `callbacks_by_id_`
- `tcp_subscribers_`
- `shm_subscriber_services_`
- `published_topics`
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

## 8. ServiceHostRuntime

### 8.1 作用

`ServiceHostRuntime` 负责本地服务托管侧的骨架逻辑。

### 8.2 当前已承接的职责

- service accept
- service client data receive
- service client message dispatch skeleton
- SHM request dispatch skeleton

### 8.3 已下沉的方法

- `onServiceAccept(...)`
- `onServiceClientData(...)`
- `processServiceClientMessages(...)`
- `onShmRequest(...)`

### 8.4 当前未下沉的细节

为了保持当前架构清晰和稳定，以下内容仍保留在 `OmniRuntime` 中：

- invoke / oneway 的具体业务执行
- SHM invoke reply 的具体构造
- service client 断开后的资源回收

原因不是功能做不到，而是：

- `Service::onInvoke()` 是 protected
- 继续强行下沉会引入额外 adapter / bridge 层
- 会使边界虽然更“纯”，但显著增加复杂度和侵入性

当前实现选择的是**更清晰、可读、稳定的切分边界**。

## 9. ConnectionManager

### 9.1 作用

`ConnectionManager` 负责所有服务直连和数据面 transport 选择。

### 9.2 当前职责

- 服务连接池
- 同机 / 跨机 transport 选择
- SHM -> TCP fallback
- 直连消息接收
- 直连断开通知

### 9.3 选择逻辑

1. 拿到目标服务 `ServiceInfo`
2. 比较 `host_id`
3. 同机且有 `shm_name`：优先 SHM
4. SHM 建立失败：TCP fallback
5. 跨机：TCP

## 10. ShmTransport

### 10.1 作用

`ShmTransport` 提供同机低延迟数据面传输。

### 10.2 当前内存布局

一个服务一块主 SHM：

```text
[ShmControlBlock]
[Global RequestQueue]
[ResponseArena]
```

### 10.3 ShmControlBlock 关键字段

- `magic`
- `version`
- `max_clients`
- `active_clients`
- `req_ring_capacity`
- `resp_ring_capacity`
- `response_bitmap`
- `req_lock`
- `slots[32]`

### 10.4 slot 关键字段

每个 `slot` 包含：

- `active`
- `response_offset`
- `response_capacity`

### 10.5 SHM 数据流模型

- request：
  - 多客户端共享一个 request ring
  - 服务端统一消费
- response：
  - 每个活跃 client 占一个 response block
  - 服务端按 slot 写回
- 通知：
  - `req_eventfd`
  - `resp_eventfd`

### 10.6 默认配置

- request ring：`4KB`
- response ring：`4KB`
- `max_clients = 32`

服务端可通过 `Service::setShmConfig()` 显式放大容量，并通过 `ServiceInfo.shm_config` 传播到客户端。

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

- 心跳超时检测
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
  -> OmniRuntime / ConnectionManager 使用返回的 host_id / shm_name / shm_config
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
  -> OmniRuntime::handleSMMessage()
  -> 执行本地 death callback
```

## 13. RPC 完整数据流

### 13.1 TCP 同步 RPC

```text
Caller
  -> OmniRuntime::invoke()
  -> ConnectionManager 获取或建立 TCP 直连
  -> 发送 MSG_INVOKE

Service Side
  -> ServiceHostRuntime::onServiceClientData()
  -> processServiceClientMessages()
  -> OmniRuntime::handleInvokeRequest()
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
  -> ConnectionManager 选择 SHM
  -> 获取 slot
  -> 请求写入 Global RequestQueue
  -> notify req_eventfd

Service Side
  -> EventLoop 被 req_eventfd 唤醒
  -> ShmTransport::serverRecv()
  -> OmniRuntime::handleShmRequest()
  -> ServiceHostRuntime::onShmRequest()
  -> OmniRuntime 执行 invoke 细节与 reply 构造
  -> ShmTransport::serverSend(slot)
  -> notify resp_eventfd

Caller
  -> EventLoop 被 resp_eventfd 唤醒
  -> ShmTransport::recv()
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
  -> TopicManager::registerSubscriber()
  -> 返回 MSG_SUBSCRIBE_TOPIC_REPLY
  -> 仅 reply 成功后，本地 TopicRuntime 记录 callback 和 topic id
```

### 14.3 publisher 信息下发数据流

```text
ServiceManager
  -> 发现某 topic 有订阅者且已有 publisher
  -> 向 subscriber 发送 MSG_TOPIC_PUBLISHER_NOTIFY

Subscriber
  -> OmniRuntime::handleSMMessage()
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
  -> 对 SHM subscriber service 逐个写入 SHM response block

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
