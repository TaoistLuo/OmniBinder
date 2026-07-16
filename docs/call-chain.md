# 调用链路详解

本文按当前代码实现描述 OmniBinder 的主要调用链路。代码是本文档的依据；为了降低漂移风险，本文只写稳定的类名、函数名和消息类型，不再绑定源码行号。

---

## 1. 全局职责划分

OmniBinder 的运行时分成控制面和数据面：

- **控制面**：`OmniRuntime::Impl` 通过 `SmControlChannel` 连接 `ServiceManager`，发送注册、查找、订阅、运行时诊断等控制消息。
- **数据面**：客户端和服务端通过 `ConnectionManager` 建立直连，数据路径自动选择 SHM 或 TCP。
- **服务端数据分派**：factory 创建的 TCP/SHM host 持有具体 transport 与 EventLoop 注册，向 `OmniRuntime::Impl` 回调完整消息。
- **Topic 状态**：`TopicRuntime` 保存本地订阅回调、已发布 topic、TCP 订阅者 fd 和 SHM 订阅者 `(service_name, client_id)`。
- **同步等待**：`RpcRuntime` 管理 sequence、默认超时和 `waitForReply()` 的等待状态；实际 reply 存储在 `SmControlChannel::pending_replies_`。

ServiceManager 只负责服务注册发现、Topic 发布/订阅关系、死亡通知和诊断控制面；RPC request/response 与 topic broadcast 一旦建立数据面直连后不再经过 ServiceManager。

---

## 2. 服务注册链路

```text
用户代码 registerService(service)
  -> OmniRuntime::Impl::registerService()
  -> registerServiceInternal()
     - 通过同一个 TransportFactory 创建 TCP 和 SHM host
     - host 注册 listener、client、SHM request/handshake 通知到 EventLoop
  -> registerServiceWithManager()
     - 构造 ServiceInfo{name, host, port, host_id, shm_config, interfaces}
     - 发送 MSG_REGISTER 到 ServiceManager
     - 等待 MSG_REGISTER_REPLY
  -> ServiceManager::handleRegister()
     - ServiceRegistry::addService()
     - HeartbeatMonitor::startTracking()
```

当前 `ServiceInfo` 不包含固定 `shm_name`。SHM 是 per-client 模型：客户端后续直连服务端时创建自己的 SHM，再通过服务端 UDS listener 完成握手。

---

## 3. 客户端主动连接链路

生成的 Proxy 通常先调用 `ServiceProxyBase::connect()`，该函数会执行：

```text
ServiceProxyBase::connect()
  -> OmniRuntime::Impl::connectService(service_name)
  -> connectServiceInternal()
     - lookupServiceInfo(): 查 service_cache_；未命中则向 SM 发 MSG_LOOKUP
     - ConnectionManager::getOrCreateConnection(service_name, host, port, host_id, shm_config)
  -> subscribeServiceDeathInternal()
     - 向 SM 发 MSG_SUBSCRIBE_SERVICE，注册死亡通知
```

`invokeInternal()` 本身要求连接已经存在：它只调用 `conn_mgr_->getConnection(service_name)`，连接不存在时返回 `ERR_CONNECT_FAILED`，不会在 invoke 内隐式 lookup/connect。

---

## 4. 传输层选择链路

```text
ConnectionManager::getOrCreateConnection()
  -> 构造 TransportClientContext
     - service_name / host / port
     - local_host_id / remote_host_id
     - ShmConfig / timeout
  -> TransportFactory::candidates(context)
     - snapshot 持有 provider shared_ptr
     - priority 降序，priority 相同保持注册顺序
  -> 依次调用 ITransportProvider::createClient(context)
     - SUCCESS：必须返回已 CONNECTED transport，所有权移入 ServiceConnection
     - NOT_APPLICABLE / RETRYABLE_FAILURE：尝试下一个 provider
     - FATAL_FAILURE：立即停止
  -> 默认同机：SHM provider -> TCP provider
  -> 默认跨机：TCP provider；SHM provider 返回 NOT_APPLICABLE
```

SHM ring 容量来自服务注册时上报的 `ServiceInfo.shm_config`；未配置时使用 `SHM_DEFAULT_REQ_RING_CAPACITY` / `SHM_DEFAULT_RESP_RING_CAPACITY`。
TCP provider 内部完成非阻塞 connect 的 writable 等待与 `checkConnectComplete()`，因此 `ConnectionManager` 不再依赖具体 TCP 类型。
该 factory 是内部源码扩展边界，不是公共动态插件 ABI；service hosting 已使用 host provider，ServiceManager 控制面仍固定使用 TCP。

---

## 5. 同步 RPC 调用链路

### 5.1 客户端发送

```text
生成 Proxy 方法
  -> 序列化参数到 Buffer
  -> OmniRuntime::Impl::invoke(...)
  -> callSerialized(...)
  -> invokeInternal(...)
     - conn_mgr_->getConnection(service_name)
     - allocSequence()
     - populateInvokeMessage(): interface_id + idl_hash + method_id + request_length + request_data
     - emitDiagEvent(DIAG_EVENT_REQUEST, MSG_INVOKE)
     - ConnectionManager::sendMessageWithinTimeout()
     - waitForReply(seq, remaining_timeout, is_alive)
```

`waitForReply()` 委托给 `RpcRuntime::waitForReply()`：

- `SmControlChannel::beginWait(seq)` 创建等待槽。
- 循环调用 `pollOnceWithoutFunctors(wait_ms)`，只处理 fd/timer，不执行新的 pending API functor。
- `onDirectMessage()` 收到匹配 `MSG_INVOKE_REPLY` 后调用 `storePendingReply()`。
- `SmControlChannel::takeReply(seq)` 取走 reply。

### 5.2 服务端处理 TCP 请求

```text
TCP host listener/client fd 可读
  -> TcpTransportHost accept / recv / 拆 Message frame
  -> OmniRuntime::Impl::onHostedMessage()
     - 拆 Message frame
     - MSG_INVOKE         -> OmniRuntime::Impl::onInvokeRequest()
     - MSG_INVOKE_ONEWAY  -> OmniRuntime::Impl::onInvokeOneWayRequest()
     - MSG_HEARTBEAT      -> 自动发送 MSG_HEARTBEAT_ACK
     - MSG_SUBSCRIBE_BROADCAST -> OmniRuntime::Impl::onSubscribeBroadcast()
     - MSG_BROADCAST      -> TopicRuntime::dispatch()
```

`onInvokeRequest()` 调用 `dispatchLocalInvoke()`：

1. 解码 invoke payload。
2. 校验 `interface_id`。
3. 如果请求带 `idl_hash`，校验客户端/服务端方法 hash。
4. 调用用户服务的 `Service::onInvoke(method_id, request, response)`。
5. 成功时构造 `MSG_INVOKE_REPLY(status=0)`；失败时构造错误 reply。
6. TCP 路径通过 `ITransportHost::send(peer_id, reply)` 发送 reply。

### 5.3 服务端处理 SHM 请求

```text
SHM host request notify fd 可读
  -> eventFdConsume()
  -> ShmTransport::serverRecv(buf, ..., client_id)
  -> ShmTransportHost 校验并构造完整 Message
  -> OmniRuntime::Impl::onHostedMessage(service_name, SHM, client_id, msg)
```

`OmniRuntime::Impl::onHostedMessage()` 对 SHM 单帧消息分派：

- `MSG_INVOKE`：回到 `dispatchLocalInvoke()`，再把 reply 序列化后 `ShmTransport::serverSend(client_id, ...)`。
- `MSG_INVOKE_ONEWAY`：只执行本地调用，不发 reply。
- `MSG_SUBSCRIBE_BROADCAST`：记录 SHM topic 订阅者。
- `MSG_BROADCAST`：本地 dispatch 给订阅回调。

### 5.4 客户端接收 reply

```text
ConnectionManager fd 回调
  -> ConnectionManager::onConnectionData()
  -> message_cb_(service_name, msg)
  -> OmniRuntime::Impl::onDirectMessage()
     - MSG_INVOKE_REPLY：storePendingReply(seq, msg)
     - MSG_BROADCAST：decodeBroadcastPayload() -> TopicRuntime::dispatch()
     - MSG_HEARTBEAT_ACK：刷新 heartbeat state
```

对于 SHM 客户端，`ShmTransport::recv()` 从 response ring 读取消息并触发同一条 `onDirectMessage()` 路径。

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
     - 向 SM 发送 MSG_SUBSCRIBE_TOPIC(topic_name)
     - SM handleSubscribeTopic(): TopicManager::addSubscriber()
     - 如果 publisher 已存在，SM 发送 MSG_TOPIC_PUBLISHER_NOTIFY 给订阅者
     - runtime 收到 MSG_SUBSCRIBE_TOPIC_REPLY 后 TopicRuntime::rememberSubscription()
```

`MSG_SUBSCRIBE_TOPIC_REPLY` 中可能附带 publisher idl hash；当前 runtime 读取后不使用该值。

### 7.3 订阅者连接发布者

```text
SM 推送 MSG_TOPIC_PUBLISHER_NOTIFY(topic_name, publisher ServiceInfo)
  -> OmniRuntime::Impl::onSMMessage()
  -> ensureTopicPublisherConnection(topic_name, pub_info)
     - conn_mgr_->getOrCreateConnection("topic_pub_" + topic_name, pub_info.host, pub_info.port, pub_info.host_id, pub_info.shm_config)
     - 构造 MSG_SUBSCRIBE_BROADCAST(topic_id, topic_name)
     - conn_mgr_->sendMessage("topic_pub_" + topic_name, sub_msg)
```

发布者收到 `MSG_SUBSCRIBE_BROADCAST` 后：

- TCP 路径：`onSubscribeBroadcast(client_fd, msg, "TCP")` -> `TopicRuntime::addTcpSubscriber(topic_id, client_fd)`。
- SHM 路径：`onShmRequest()` 中的 subscribe 回调 -> `TopicRuntime::addShmSubscriberService(topic_id, service_name, client_id)`。

### 7.4 发布者广播数据

```text
生成 Stub 的 BroadcastXxx(data)
  -> runtime.broadcast(topic_id, payload)
  -> broadcastInternal(topic_id, payload)
     - 构造 MSG_BROADCAST(topic_id, data_length, data)
     - TCP 订阅者：按 fd 找到 ITransport，sendOnFd()
     - SHM 订阅者：按 service_name/client_id 找到 LocalServiceEntry，ShmTransport::serverSend(client_id, ...)
```

订阅者收到 `MSG_BROADCAST` 后，TCP 和 SHM 最终都调用 `TopicRuntime::dispatch(topic_id, data)`，再进入用户注册的 `TopicCallback`。

---

## 8. 死亡通知、心跳和自动重连

### 8.1 ServiceManager 侧死亡通知

客户端通过 `MSG_SUBSCRIBE_SERVICE` 注册死亡通知。服务 unregister、连接断开或 ServiceManager 判定服务死亡时，SM 调用 `notifyServiceDeath()` 向订阅者发送 `MSG_DEATH_NOTIFY`。

runtime 在 `onSMMessage()` 中处理 `MSG_DEATH_NOTIFY`：

- 删除 `service_cache_`。
- 调用 `conn_mgr_->removeConnection(service_name)`。
- 停止 heartbeat。
- 触发用户 `DeathCallback`。
- 如果启用了自动重连，调度 `tryReconnectService()`。

### 8.2 数据面 heartbeat

`startHeartbeat(service_name, interval, timeout)` 启动客户端到服务端的数据面 heartbeat：

```text
timer
  -> sendHeartbeatToService()
     - conn_mgr_->sendMessage(MSG_HEARTBEAT)
  -> checkHeartbeatTimeout()
```

TCP 服务端路径由 `ServiceHostRuntime::processServiceClientMessages()` 自动返回 `MSG_HEARTBEAT_ACK`；SHM 服务端路径由 `OmniRuntime::Impl::onShmRequest()` 特判 heartbeat 后用 `ShmTransport::serverSend()` 返回 ACK。客户端在 `onDirectMessage(MSG_HEARTBEAT_ACK)` 中清除 pending 并刷新时间戳。

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
     - 记录 watcher_to_pid_ / pid_watchers_
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
| `src/core/omni_runtime.cpp` | Runtime 编排：SM 控制面、服务注册、连接管理入口、RPC、topic、诊断、心跳、重连 |
| `src/core/omni_runtime.h` | `OmniRuntime::Impl` 状态与内部 API；`callSerialized()` 串行化入口 |
| `src/core/rpc_runtime.cpp` | sequence、默认超时、`waitForReply()` 等 RPC 等待状态 |
| `src/core/sm_control_channel.cpp` | SM 控制连接收发缓冲与 pending reply slots |
| `src/core/connection_manager.cpp` | 数据面连接创建/复用/删除，SHM/TCP 选择和发送 |
| `src/core/service_host_runtime.cpp` | 服务端 TCP/SHM 消息拆包和分派 |
| `src/core/topic_runtime.cpp` | 本地 topic 回调、publisher、TCP/SHM subscriber 状态 |
| `src/transport/shm_transport.cpp` | per-client SHM、UDS 握手、request/response ring、eventfd |
| `src/transport/tcp_transport.cpp` | TCP connect/listen/accept/send/recv |
| `service_manager/main.cpp` | 服务注册发现、Topic 关系、死亡通知、runtime PID/log/watch 控制面 |
| `include/omnibinder/message.h` | 消息类型、消息名、ServiceInfo/RuntimeInfo 序列化接口 |
| `include/omnibinder/types.h` | `ServiceInfo`、`RuntimeInfo`、`ShmConfig` 等公共类型 |
