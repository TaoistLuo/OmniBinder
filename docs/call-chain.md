# 调用链路详解

本文按当前代码实现描述 OmniBinder 的主要调用链路。代码是本文档的依据；为了降低漂移风险，本文只写稳定的类名、函数名和消息类型，不再绑定源码行号。

---

## 1. 全局职责划分

OmniBinder 的运行时分成控制面和数据面：

- **控制面**：`OmniRuntime::Impl` 通过 `SmControlChannel` 连接 `ServiceManager`，发送注册、查找、订阅、运行时诊断等控制消息。
- **数据面**：客户端和服务端通过 `ConnectionManager` 建立直连，数据路径自动选择 SHM 或 TCP。
- **服务端数据分派**：`LocalServiceEntry` 通过 `std::vector<IServerEndpoint*>` 持有 TCP / SHM 端点（`TcpServerEndpoint` / `ShmServerEndpoint`），通过 `OmniRuntime::Impl` 的私有方法分派完整消息。
- **Topic 状态**：`TopicRuntime` 保存本地订阅回调、已发布 topic、TCP 订阅者 fd 和 SHM 订阅者 `(service_name, client_id)`。
- **同步等待**：`RpcRuntime` 管理 sequence、默认超时和 `waitForReply()` 的等待状态；控制面 reply 存储在 `SmControlChannel::pending_replies_`，数据面 reply 存储在 `RpcRuntime` 独立的 `dataReplies()` 槽表，SM 重连只清理控制面槽。

ServiceManager 只负责服务注册发现、Topic 发布/订阅关系、服务状态通知和诊断控制面；RPC request/response 与 topic broadcast 一旦建立数据面直连后不再经过 ServiceManager。

---

## 2. 服务注册链路

```text
用户代码 registerService(service)
  -> OmniRuntime::Impl::registerService()
  -> registerServiceInternal()
     - initializeServiceListener()：createServerEndpoint(TCP) 启动 listen 并 wireServiceEndpoint()
     - initializeServiceShm()：createServerEndpoint(SHM) 启动 UDS 握手监听（失败仅告警，自动降级 TCP）
     - syncEndpointFds() 把端点级 fd（listen / master / handshake 等）注册到 EventLoop
  -> registerServiceWithManager()
     - sendRegisterToManager() 构造 ServiceInfo{name, host, port, host_id, instance_id, shm_config, interfaces}
     - 发送 MSG_REGISTER 到 ServiceManager
     - 等待 MSG_REGISTER_REPLY
  -> ServiceManagerApp::handleRegister()
     - ServiceRegistry::addService()：以逻辑身份 (name, instance_id) 存储；
       同逻辑身份重复注册为后来者顶替/迁移（保留 handle），不同 instance_id 新增并存；
       host_id 仅作为 endpoint 元数据保存
     - 控制面会话由 MSG_CONTROL_HEARTBEAT 续期；服务不再单独跟踪心跳
```

`instance_id` 由服务在注册前经 `Service::setInstanceId()` 声明，默认空串表示该服务的默认实例；业务需保证同一服务名下 `instance_id` 集群内唯一。

当前 `ServiceInfo` 不包含固定 `shm_name`。SHM 是 per-client 模型：客户端后续直连服务端时创建自己的 SHM，再通过服务端 UDS listener 完成握手；服务端握手路径由 `(service_name, host_id, instance_id)` 共同派生（`generateShmName`），因此同机同名不同实例（或不同 `host_id`）互不抢占。

---

## 3. 客户端主动连接链路

生成的 Proxy 通常先调用 `ServiceProxyBase::connect()`。未绑定实例时按服务名连接，绑定实例时按实例连接：

```text
ServiceProxyBase::connect()
  -> 未绑定实例（connection_key_ == service_name_）：
       OmniRuntime::Impl::connectService(service_name)
       -> connectServiceInternal()
          - lookupServiceInfo(): 查 services_ 中该服务的 ServiceState.info；未命中则向 SM 发 MSG_LOOKUP
            （同名多实例时 SM 返回实例键字典序第一实例）
          - ConnectionManager::getOrCreateConnectionByKey(service_name, service_name, host, port, host_id, shm_config)
  -> 已绑定逻辑实例（setInstance(instance_id) 或 connect(instance_id) 之后）：
       OmniRuntime::Impl::connectServiceInstance(name, instance_id)
       -> listServiceInstancesInternal(name): 向 SM 发 MSG_LIST_INSTANCES，按 instance_id 解析目标实例
       -> connectResolvedConnection(makeServiceConnectionKey(name, instance_id), instance_info, true, &identity)
          - ConnectionManager::getOrCreateConnectionByKey(connection_key, name, host, port, host_id, shm_config, instance_id)
          - host_id 只参与 endpoint 选择；逻辑连接键不包含 host_id
  -> enableAutoReconnect(true)
  -> connectService 内 ensureDataSession()：MSG_SESSION_HELLO/ACK；数据面心跳自动启动
     （可选：startHeartbeat(interval, timeout) 自定义策略/重新协商）
  -> watchServices()：按目标实例（未绑定时使用 lookupService 解析到的实例身份）注册状态 watch
     - 向 SM 发 MSG_SUBSCRIBE_SERVICE_WATCH
```

连接以 `makeServiceConnectionKey(name, instance_id)` 为索引键：生成的 Proxy 方法据此调用 `runtime_.invoke(connectionKey(), ...)` / `invokeOneWay(connectionKey(), ...)`；`ServiceProxyBase` 的 `disconnect()`、`enableAutoReconnect()`、`setReconnectInterval()`、`startHeartbeat()`、`stopHeartbeat()` 也都以 `connectionKey()` 为键。默认实例（未显式指定）的连接键即服务名本身；host_id 不参与连接键。

`invokeInternal()` 本身要求连接已经存在：它只调用 `conn_mgr_->getConnection(service_name)`，连接不存在时返回 `ERR_CONNECT_FAILED`，不会在 invoke 内隐式 lookup/connect。

---

## 4. 传输层选择链路

```text
ConnectionManager::getOrCreateConnectionByKey(connection_key, service_name, host, port, host_id, shm_config, instance_id)
  -> 调用 createClientConnection(service_name, host, port, local_host_id, remote_host_id, shm_config, instance_id)
     - 同机：尝试 SHM（创建 ShmClientConnection(service_name, ..., instance_id, remote_host_id) 并 connect）
     - 同机 SHM 失败/跨机：创建 TcpConnection 并 connect
     - 成功返回 CONNECTED 状态的 IMessageConnection*，以 connection_key 存入 ServiceConnection
     - 失败返回 NULL，上层返回 ERR_CONNECT_FAILED
```

`getOrCreateConnection(service_name, ...)` 是 `getOrCreateConnectionByKey` 的默认实例封装：connection_key 与服务名相同；显式实例寻址使用 `makeServiceConnectionKey` 生成独立键。

SHM ring 容量来自服务注册时上报的 `ServiceInfo.shm_config`；未配置时使用 `SHM_DEFAULT_REQ_RING_CAPACITY` / `SHM_DEFAULT_RESP_RING_CAPACITY`。
同机 SHM 的服务端握手路径由 `generateShmName(service_name, instance_id, host_id)` 派生：`host_id` 与 `instance_id` 参与 FNV-1a 哈希输入但不进入名称前缀，因此同机同名不同实例或不同 `host_id` 各自得到独立的 SHM 名与 UDS 路径。
TCP provider 内部完成非阻塞 connect 的 writable 等待与 `checkConnectComplete()`，因此 `ConnectionManager` 不再依赖具体 TCP 类型。
`createClientConnection()` 位于 `src/transport/transport_selector.{h,cpp}`，是新增传输的落点，但属源码级、侵入式扩展（新增传输还需同步修改 core：`TransportType` 分支、端点半创建、topic 订阅模型），非单点接入。service hosting 在 `OmniRuntime::Impl` 中通过 `createServerEndpoint()` 创建服务端端点（`TransportType::TCP` / `TransportType::SHM`），不经过 `createClientConnection()`。ServiceManager 控制面仍固定使用 TCP。

---

## 5. 同步 RPC 调用链路

### 5.1 客户端发送

```text
生成 Proxy 方法
  -> 序列化参数到 Buffer
  -> OmniRuntime::Impl::invoke(connectionKey(), ...)   // 默认实例时 connectionKey() == 服务名
  -> callSerialized(...)
  -> invokeInternal(...)
     - conn_mgr_->getConnection(connection_key)
     - allocSequence()
     - populateInvokeMessage(): interface_id + idl_hash + method_id + request_length + request_data
     - emitDiagEvent(DIAG_EVENT_REQUEST, MSG_INVOKE)
     - ConnectionManager::sendMessageWithinTimeout()
     - waitForDataReply(seq, reply_timeout_ms, is_alive)
```

`waitForDataReply()` 委托给 `RpcRuntime::waitForReply()`（使用数据面独立槽表 `dataReplies()`）：

- `RpcRuntime::beginWait()` 建立等待状态，`PendingReplyTable::beginWait(seq)` 创建等待槽。
- 循环调用 `pollOnceWithoutFunctors(wait_ms)`，只处理 fd/timer，不执行新的 pending API functor。
- `onDirectMessage()` 收到匹配 `MSG_INVOKE_REPLY` 后调用 `storeAndConsumeDataReply()`（内部 `PendingReplyTable::storeReply()`）。
- `PendingReplyTable::takeReply(seq)` 取走 reply。

### 5.2 服务端处理 TCP 请求

```text
TCP listener/client fd 可读
  -> TcpServerEndpoint::onPollEvent() / 客户端 IMessageConnection::recv() / 拆 Message frame
  -> OmniRuntime::Impl::onServiceClientReadable()
  -> OmniRuntime::Impl::handleServiceClientMessage()
     - MSG_INVOKE         -> OmniRuntime::Impl::onInvokeRequest()
     - MSG_INVOKE_ONEWAY  -> OmniRuntime::Impl::onInvokeOneWayRequest()
     - MSG_HEARTBEAT      -> 自动发送 MSG_HEARTBEAT_ACK
     - MSG_SUBSCRIBE_BROADCAST -> 记录 TCP/SHM topic 订阅者
     - MSG_BROADCAST      -> TopicRuntime::dispatch()
```

`onInvokeRequest()` 调用 `dispatchLocalInvoke()`：

1. 解码 invoke payload。
2. 校验 `interface_id`。
3. 如果请求带 `idl_hash`，校验客户端/服务端方法 hash。
4. 调用用户服务的 `Service::onInvoke(method_id, request, response)`。
5. 成功时构造 `MSG_INVOKE_REPLY(status=0)`；失败时构造错误 reply。
6. TCP 路径通过 `OmniRuntime::Impl::sendOnFd()` 发送 reply。

### 5.3 服务端处理 SHM 请求

```text
SHM request notify fd 可读
  -> eventFdConsume()
  -> ShmServerEndpoint::onPollEvent() 扫描 client request ring / 客户端 IMessageConnection::recv() 拆帧
  -> OmniRuntime::Impl::handleServiceClientMessage(service_name, client_id, msg)
```

`OmniRuntime::Impl::handleServiceClientMessage()` 对 SHM 单帧消息分派：

- `MSG_INVOKE`：回到 `dispatchLocalInvoke()`，再把 reply 序列化后通过服务端客户端连接（`IMessageConnection::send()`）写回响应 ring。
- `MSG_INVOKE_ONEWAY`：只执行本地调用，不发 reply。
- `MSG_SUBSCRIBE_BROADCAST`：记录 SHM topic 订阅者。
- `MSG_BROADCAST`：本地 dispatch 给订阅回调。

### 5.4 客户端接收 reply

```text
ConnectionManager fd 回调
  -> ConnectionManager::onConnectionData()
  -> message_cb_(service_name, msg)
  -> OmniRuntime::Impl::onDirectMessage()
     - MSG_INVOKE_REPLY：storeAndConsumeDataReply(seq, msg)
     - MSG_BROADCAST：dispatchBroadcastMessage() -> TopicRuntime::dispatch()
     - MSG_HEARTBEAT_ACK：刷新 heartbeat state
```

对于 SHM 客户端，`ShmClientConnection::recv()` 从 response ring 读取消息并触发同一条 `onDirectMessage()` 路径。

---

## 6. One-Way RPC 调用链路

```text
Proxy oneway 方法
  -> OmniRuntime::Impl::invokeOneWay()
  -> invokeOneWayInternal()
     - conn_mgr_->getConnection(service_name)
     - populateInvokeMessage(MSG_INVOKE_ONEWAY)
     - emitDiagEvent(DIAG_EVENT_ONE_WAY, msg)
     - ConnectionManager::sendMessage()
     - 不创建 wait slot，不等待 reply
```

服务端收到 `MSG_INVOKE_ONEWAY` 后仍走 `dispatchLocalInvoke()` 执行业务逻辑，但不构造 `MSG_INVOKE_REPLY`。如果发生 IDL hash mismatch，会记录错误并丢弃该 oneway 消息。

---

## 7. Topic 发布/订阅链路

### 7.1 发布者注册 Topic

```text
runtime.publishTopic(topic_name)
  -> publishTopicInternal(topic_name)
     - 要求当前 runtime 至少已注册一个本地服务
     - 使用第一个本地服务的 listen host/port/shm_config 构造 publisher ServiceInfo
     - 向 SM 发送 MSG_PUBLISH_TOPIC(topic_name, ServiceInfo)
     - SM handlePublishTopic(): TopicManager::registerPublisher()
     - SM 回复 MSG_PUBLISH_TOPIC_REPLY
     - 本地 TopicRuntime::rememberPublishedTopic(topic_name, topic_id, owner_service)
```

当前实现不再为普通 topic 单独创建隐藏 publisher listener；它复用本地已注册服务的数据面入口。

### 7.2 订阅者订阅 Topic

```text
runtime.subscribeTopic(topic_name, callback)
  -> subscribeTopicInternal(topic_name, callback)
     - 向 SM 发送 MSG_SUBSCRIBE_TOPIC(topic_name)（无选择器，mode 0：仅按主题名）
     - SM handleSubscribeTopic(): TopicManager::addSubscriber()
     - 如果 publisher 已存在，SM 发送 MSG_TOPIC_PUBLISHER_NOTIFY 给订阅者
     - runtime 收到 MSG_SUBSCRIBE_TOPIC_REPLY 后 TopicRuntime::rememberSubscription()
```

按选择器订阅（服务级 / 实例级）走同一消息类型，但在 `topic_name` 后追加订阅选择器：

```text
runtime.subscribeTopic(selector, topic_name, expected_idl_hash, on_msg, on_err)
  -> subscribeSourceTopicInternal(selector, ...)
     - 向 SM 发送 MSG_SUBSCRIBE_TOPIC(topic_name, selector)
       选择器线格式 mode：1=服务级（publisher_service）/ 2=实例级（+ instance_id；host_id 字段兼容保留但不参与匹配）
     - SM 按 (发布者服务名, 主题名) 与选择器匹配，为每个匹配的在线发布者实例各下发
       一条 MSG_TOPIC_PUBLISHER_NOTIFY
     - runtime 收到 reply 后 TopicRuntime::rememberSourceSubscription() + setSourceErrorCallback()
```

`MSG_SUBSCRIBE_TOPIC_REPLY` 中可能附带 publisher idl hash；runtime 会与订阅者期望哈希严格校验，不一致时撤销订阅并返回 `ERR_IDL_MISMATCH`。`subscribeTopic(topic_name, ...)`（mode 0）是仅按主题名、匹配任意服务发布的该主题的订阅变体。

### 7.3 订阅者连接发布者

```text
SM 推送 MSG_TOPIC_PUBLISHER_NOTIFY(topic_name, publisher ServiceInfo{name, host_id, instance_id}, idl_hash)
  -> OmniRuntime::Impl::onSMMessage()
  -> topic_runtime_.matchSubscriptions(topic, name, host_id, instance_id)  // 按选择器过滤
  -> ensureTopicPublisherConnection(topic_name, pub_info)
     - pub_name = pub_info.name 非空时复用真实服务名，否则用 "topic_pub_" + topic_name
     - conn_key = makeServiceConnectionKey(pub_name, pub_info.instance_id)
     - conn_mgr_->getOrCreateConnectionByKey(conn_key, pub_name, pub_info.host, pub_info.port, pub_info.host_id, pub_info.shm_config, pub_info.instance_id)
     - 构造 MSG_SUBSCRIBE_BROADCAST(topic_id, topic_name)
     - conn_mgr_->sendMessage(conn_key, sub_msg)，并把话题订阅与发布者实例信息绑定到该连接的 ReconnectConfig
```

服务级订阅下同名的多个发布者实例各占一条独立连接（各自一个实例键），订阅方按连接区分来源实例。未匹配选择器的 `MSG_TOPIC_PUBLISHER_NOTIFY` 直接忽略，不建立连接。

发布者收到 `MSG_SUBSCRIBE_BROADCAST` 后，由 `handleServiceClientMessage()` 的 subscribe 分支按 `IMessageConnection::isFramed()` 区分：

- TCP 路径：`TopicRuntime::addTcpSubscriber(topic_id, client_id)`。
- SHM 路径：`TopicRuntime::addShmSubscriberService(topic_id, service_name, client_id)`。

### 7.4 发布者广播数据

```text
生成 Stub 的 BroadcastXxx(data)
  -> runtime.broadcast(topic_id, payload)
  -> broadcastInternal(topic_id, payload)
     - 构造 MSG_BROADCAST(topic_id, data_length, data)
     - TCP 订阅者：按 client_id 找到 IMessageConnection，sendAll(timeout=0)；发送不完整视为连接损坏，走断开路径
     - SHM 订阅者：按 service_name/client_id 找到 LocalServiceEntry 的客户端连接，走 IMessageConnection::send()（ring 满丢帧不断开）
```

订阅者收到 `MSG_BROADCAST` 后，TCP 和 SHM 最终都进入 `dispatchBroadcastMessage()`：连接上登记了发布者实例信息（`ServiceState.info`）时以该身份调用 `TopicRuntime::dispatchFrom(publisher_service, host_id, instance_id, topic_id, data)`，按选择器过滤后进入用户注册的 `TopicCallback`（mode 0）或 `TopicSourceCallback`（携带来源 `host_id` / `instance_id`）。

---

## 8. 服务状态通知、心跳和自动重连

### 8.1 ServiceManager 侧服务状态通知

客户端通过 `watchServices()` 注册持久状态 watch，逻辑 filter 为 service_name + 可选 instance_id；host_id 字段兼容保留但不参与匹配：

- 服务实例注册 → `MSG_SERVICE_WATCH_NOTIFY`，online=true
- 实例注销、连接断开、心跳超时 → `MSG_SERVICE_WATCH_NOTIFY`，online=false
- 同逻辑身份迁移/takeover 且 fd/端点变化 → 先 offline（旧 endpoint），再 online（新 endpoint）
- name-only watch 会收到每个匹配逻辑实例的每次上下线事件

runtime 在 `onSMMessage()` 的 `handleServiceWatchNotify()` 中解析 watch_id 与实例身份，
先清理离线实例的数据面连接/重连状态，再调用 `ServiceWatchCallback`。

`watchServices()` 在 SM 尚未启动/未就绪时先本地登记并返回成功；SM 连接后由
`restoreControlPlaneState()` 重放 watch，SM 补发初始状态（无匹配实例时为 OFFLINE）。

### 8.2 数据面会话与 heartbeat

`connectService()` / `Proxy.connect()` 首先完成数据面会话协商：

```text
ensureDataSession()
  -> conn_mgr_->sendMessage(MSG_SESSION_HELLO)
  -> 等待 MSG_SESSION_HELLO_ACK（5s 超时）
  -> 保存 accepted_interval / effective_timeout / peer_incarnation
  -> 自动启动 heartbeat 定时器
```

heartbeat 流程：

```text
timer
  -> sendHeartbeatToService()
     - msg.payload.writeUint32(incarnation_id_)
     - conn_mgr_->sendMessage(MSG_HEARTBEAT)
     - 挂 ACK deadline（effective_timeout）
  -> MSG_HEARTBEAT_ACK
     - 清除 pending / 取消 deadline / 刷新 peer_incarnation
  -> deadline 到期且仍 pending
     - handleServiceLost()：移除连接、暂停 heartbeat、调度重连
```

服务端 `handleServiceClientMessage()`：

- 连接第一帧必须是 `MSG_SESSION_HELLO`；协商 per-client session 并回 ACK；
- 之后 `MSG_HEARTBEAT` 续期该 client 的 deadline，回 `MSG_HEARTBEAT_ACK`；
- deadline 到期调用 `onServiceClientDisconnected()` 清理订阅/缓冲/连接。

重连成功后 `tryReconnectService()` 会再次 `ensureDataSession()`，然后重放
`MSG_SUBSCRIBE_BROADCAST`，最后恢复 heartbeat。

---

## 9. 诊断 watch 链路

`omni-cli watch --pid <pid> --idl <file.bidl>` 观察的是 IDL 业务接口 I/O，不是 core 控制消息抓包。

控制面：

```text
watcher runtime
  -> OmniRuntime::watchPid(pid, callback)
  -> 向 SM 发送 MSG_DIAG_WATCH_START(pid)
  -> SM handleDiagWatchStart()
     - 按 pid_to_fds_ 找到目标 runtime 控制连接
     - 转发 MSG_DIAG_WATCH_START 给目标 runtime
     - 记录 watcher_to_pids_ / pid_watchers_
```

目标 runtime 收到 watch start 后启用 lazy 诊断数据面：

- 诊断 topic 名为 `__diag_pid_<pid>`。
- 如果目标 runtime 没有本地业务 service，临时注册隐藏 `RuntimeDiagService` 作为数据面入口。
- 如果已有业务 service，则复用现有 service 数据面入口，只发布诊断 topic。
- watch stop 或最后一个 watcher 断开时，SM 转发 `MSG_DIAG_WATCH_STOP`，目标 runtime 清理诊断 topic/隐藏 service。

数据面：目标 runtime 在 RPC request/response、oneway、topic broadcast 等业务 I/O 点调用 `emitDiagEvent()`，再通过 `broadcastInternal(__diag_pid_<pid>, payload)` 发送诊断 payload。该数据面复用现有 topic 直连路径：同机自动 SHM，跨机自动 TCP，不经过 ServiceManager。

---

## 10. 关键代码文件索引

| 文件 | 当前职责 |
|------|----------|
| `src/core/omni_runtime.cpp` | Runtime 编排入口（已拆分为多个 runtime_*.cpp） |
| `src/core/runtime_sm.cpp` | ServiceManager 控制面通信（注册/发现/心跳/服务状态通知/重连恢复） |
| `src/core/runtime_service.cpp` | 本地服务注册 / 注销 |
| `src/core/runtime_connection.cpp` | 数据面连接管理与服务心跳 |
| `src/core/runtime_rpc.cpp` | RPC 调用与回复 |
| `src/core/runtime_topic.cpp` | topic 发布/订阅/广播 |
| `src/core/runtime_dispatch.cpp` | 服务端 TCP/SHM 请求分派 |
| `src/core/runtime_diag.cpp` | 诊断子系统（watch 数据面 + 诊断服务生命周期） |
| `src/core/runtime_config.cpp` | 配置 / 统计 / 线程模型基础设施 |
| `src/core/runtime_helpers.cpp` | 协议 payload 解码与诊断事件序列化辅助 |
| `src/core/message_reader.cpp` | 统一消息读取器（成帧 SHM / 流式 TCP 组帧） |
| `src/core/pending_reply_table.cpp` | 控制面/数据面独立的 pending reply 槽表 |
| `src/core/omni_runtime.h` | `OmniRuntime::Impl` 状态与内部 API |
| `src/core/rpc_runtime.cpp` | sequence、默认超时、`waitForReply()` 与数据面槽表 `dataReplies()` |
| `src/core/sm_control_channel.cpp` | SM 控制连接收发缓冲（持有控制面 PendingReplyTable） |
| `src/core/connection_manager.cpp` | 数据面连接创建/复用/删除，SHM/TCP 选择和发送 |
| `src/core/topic_runtime.cpp` | 本地 topic 回调、publisher、TCP/SHM subscriber 状态 |
| `src/transport/shm_client_connection.cpp` | 客户端 per-client SHM 出站连接：UDS 握手、request/response ring、eventfd |
| `src/transport/shm_server_connection.cpp` | 服务端 per-client SHM 入站连接：请求/响应 ring 适配 |
| `src/transport/shm_server_endpoint.cpp` | 服务端 SHM 端点：握手监听、per-client ring 映射、eventfd 通知 |
| `src/transport/tcp_connection.cpp` | TCP 连接：connect/send/recv（出站+入站共用） |
| `src/transport/tcp_server_endpoint.cpp` | TCP 端点：listen/accept |
| `src/transport/transport_selector.cpp` | `createClientConnection()` 自由函数：同机 SHM→TCP 自动选择 |
| `service_manager/main.cpp` | 入口函数（已拆分为 5 个文件） |
| `service_manager/service_manager_app.cpp` | 主循环与消息分派 |
| `service_manager/sm_registry.cpp` | 服务注册/发现控制面 |
| `service_manager/sm_topic.cpp` | topic 发布/订阅控制面 |
| `service_manager/sm_diag.cpp` | 诊断 watch 控制面 |
| `include/omnibinder/message.h` | 消息类型、消息名、ServiceInfo/RuntimeInfo 序列化接口 |
| `include/omnibinder/types.h` | `ServiceInfo`、`RuntimeInfo`、`ShmConfig` 等公共类型 |
