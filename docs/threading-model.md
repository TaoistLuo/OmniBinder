# OmniBinder 线程模型

本文档描述 `OmniRuntime` 的线程模型与推荐使用方式。

---

## 1. 基本原则

`OmniRuntime` 提供线程安全公共 API。  
调用方可以从多个线程安全调用同一个 `OmniRuntime` 实例，但同一实例的事件循环驱动职责必须保持明确。

---

## 2. 核心语义

## 2.1 公共 API 线程安全

以下 API 支持并发安全调用：

- 生命周期/驱动：`init()`、`start()`（含 `poll_timeout_ms`/`on_tick` 重载）、`stop()`、`isRunning()`、`isReady()`
- 服务注册：`registerService()`、`unregisterService()`
- 服务发现：`lookupService()`、`listServices()`、`listServiceInstances()`、`queryInterfaces()`
- 连接管理：`connectService()`、`connectServiceInstance()`、`disconnectService()`、`disconnectServiceInstance()`、`isServiceConnected()`、`isServiceInstanceConnected()`
- RPC：`invoke()`、`invokeOneWay()`
- 服务状态 watch：`watchServices()`、`unwatchServices()`
- 话题：`publishTopic()`、`broadcast()`、`subscribeTopic()`、`unsubscribeTopic()`（含 `TopicSubscriptionSelector` 重载）
- 配置：`setHeartbeatPolicy()`、`setDefaultTimeout()`、`setHostId()`、`hostId()`、`setRuntimeId()`、`runtimeId()`

这里的“线程安全”含义是：

- 不出现未定义的数据竞争
- 不要求调用方自己在 `OmniRuntime` 外再包互斥锁
- 不承诺所有调用都会并行执行；内部可以串行化

### 2.1.1 按 API 分类的当前语义

| API 类别 | 方法 | 当前并发语义 |
|---|---|---|
| 生命周期 | `init()` / `start()` | `init()` 只创建本地资源；`start()` 阻塞当前线程并在 owner event-loop 中激活服务/topic/proxy |
| 持续驱动 | `start()` | 线程安全；同一时刻只允许一个线程调用 `start()` 承担 owner event-loop 驱动职责 |
| 停止 | `stop()` | 可从任意线程安全调用；必须可靠唤醒 owner event-loop 并使 `start()` 返回 |
| 状态查询 | `isRunning()`、`hostId()` | 可从任意线程并发读取，但返回值只保证内部状态的可见性，不代表更强的事务语义 |
| 配置 | `setHeartbeatPolicy()`、`setDefaultTimeout()`、`setHostId()`、`setRuntimeId()` | 可从任意线程安全调用；`setHostId()` / `setHeartbeatPolicy()` / `setRuntimeId()` 必须在 `init()` 之前调用，init 后调用被忽略 |
| 服务发现 | `lookupService()`、`listServices()`、`listServiceInstances()`、`queryInterfaces()` | 可从任意线程安全调用；同步等待与缓存更新由 owner event-loop 串行处理 |
| 连接管理 | `connectService()`、`connectServiceInstance()`、`disconnectService()`、`disconnectServiceInstance()`、`isServiceConnected()`、`isServiceInstanceConnected()` | 可从任意线程安全调用；连接建立/断开与 SM 交互由 owner event-loop 串行处理 |
| RPC | `invoke()`、`invokeOneWay()` | 可从任意线程安全调用；同步调用允许阻塞调用线程，但内部发送/等待/状态更新由 owner event-loop 串行处理 |
| 服务注册 | `registerService()`、`unregisterService()` | 可从任意线程安全调用；监听 socket、SHM、SM 注册状态仍由 owner event-loop 管理 |
| topic / service watch | `publishTopic()`、`broadcast()`、`subscribeTopic()`、`unsubscribeTopic()`（含选择器重载）、`watchServices()`、`unwatchServices()` | 可从任意线程安全调用；回调注册和派发在 owner event-loop 串行处理 |

### 2.1.2 同步阻塞 API 的额外约束

以下 API 属于同步阻塞调用：

- `lookupService()`
- `listServices()`
- `listServiceInstances()`
- `queryInterfaces()`
- `invoke()`
- `connectService()`
- `connectServiceInstance()`
- `watchServices()`
- `publishTopic()`
- `subscribeTopic()`（含选择器重载）

它们的当前语义是：

- 调用线程可以阻塞等待结果
- 超时前会持续处理 fd / timer 事件并等待 reply
- 不应把这类同步等待理解成“内部可由多个线程并行处理同一实例状态”

补充：reply wait 期间不会执行通过 `post()` 投递的 pending API functor，这样可以避免共享同一个 runtime 时出现 re-entrant `waitForReply`。

### 2.1.3 超时与阻塞语义

- `invoke(..., timeout_ms)` 支持显式超时参数
- 当 `timeout_ms == 0` 时，使用 `setDefaultTimeout()` 配置的默认超时
- 当前同步等待最终由运行时 deadline 控制，超时会返回错误码，而不是无限阻塞
- 当前语义下，正常调用路径不会“永远卡住等 reply”

## 2.2 回调线程语义

以下回调统一在 owner event-loop 线程执行：

- `TopicCallback` / `TopicSourceCallback`（后者携带发布者 host_id + instance_id）
- `ServiceWatchCallback`（携带逻辑实例 name + instance_id 与 online 状态；host_id 仅作部署元数据）
- 服务端请求处理相关回调
- 连接断开、topic 发布者通知等内部派发回调

**重要限制：**

- 不建议在 `TopicCallback`、`ServiceWatchCallback`、服务端 `onInvoke()`、`onStart()`、`onStop()` 等 owner event-loop 线程回调中，再对同一个 `OmniRuntime` 发起**同步阻塞 API**（如 `invoke()`、`lookupService()`、`subscribeTopic()`）
- 回调中应尽量只做轻量处理，必要时把后续同步工作转交给其他线程或异步任务

当前实现采用纯错误码模型，不依赖 C++ 异常。所有业务回调（`onInvoke`、`onClientConnected`、`onClientDisconnected` 等）均不应抛出异常。如果业务代码抛出未捕获异常，程序将直接终止。

---

## 3. `init()` / `start()` / `stop()` 语义

### 3.1 `init()`

`init()` 只创建本地资源：`EventLoop`、`ConnectionManager`、`RpcRuntime` 等。
它不连接 ServiceManager，不注册服务，不发布 topic，也不连接 proxy。

### 3.2 `start()`

`start()` 是唯一启动入口，阻塞当前调用线程：

```text
当前线程认领 owner event-loop
  -> 连接 SM / runtime hello
  -> 激活已声明服务（创建端点、调用 onStart）
  -> 发布服务声明的 topic（SM 不可用时保持 pending）
  -> 自动连接已声明的 proxy
  -> 进入 event-loop，直到 stop()
```

`start()` 不会创建线程。业务需要自己创建线程：

```cpp
std::thread runtime_thread([&runtime]() {
    runtime.start();
});
```

需要控制 event-loop 轮询间隔或周期性喂狗时，可使用重载：

```cpp
std::thread runtime_thread([&runtime]() {
    runtime.start(100, []() { watchdog.feed(); });
});
```

- `poll_timeout_ms == -1`：阻塞等待，默认低 CPU 模式；此时 `on_tick` 必须为空
- `poll_timeout_ms == 0`：非阻塞忙轮询，延迟最低但 CPU 占用 100%
- `poll_timeout_ms > 0`：最多等待 N ms，空闲时 tick 至少每 N ms 执行一次

`on_tick` 在 owner 线程每轮 poll 后执行；必须短小、非阻塞、不得抛异常。
owner 线程处于同步 RPC reply-wait 期间不会执行 tick，长阻塞会延迟喂狗。

如果 SM 不可达，`start()` 仍进入 event-loop 并持续重连；`isReady()` 在 SM 连接和服务/
proxy 激活完成前保持 false。

### 3.3 `stop()`

- `stop()` 可从任意线程调用
- 停止后 `start()` 返回
- 停止后业务 API 快速失败，不应继续调用
- Service 对象生命周期由调用方管理；需要 `onStop` 时应在服务仍存活时 `unregisterService()`

### 3.5 同步等待期间

同步 RPC / SM 请求等待期间只处理 fd/timer，不执行新的 pending API functor；
该行为保持不变。

---

## 4. 推荐使用方式

### 方式一：业务线程驱动 runtime（推荐）

```cpp
OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

MyService service;
runtime.registerService(&service);

std::thread runtime_thread([&runtime]() {
    runtime.start();
});

// 业务线程继续工作
while (running) {
    // ... 业务逻辑 / Broadcast
}

runtime.stop();
runtime_thread.join();
```

## 5. 回调线程语义（不变）

所有 topic / service watch / onInvoke / onStart 回调在 owner event-loop 线程执行。
`onStart()` 在 `start()` 的激活流程中执行，不在 `registerService()` 中提前执行。

---

## 7. 连接管理线程模型

### 7.1 连接操作

- `connectService()` / `connectServiceInstance()` / `disconnectService()` / `disconnectServiceInstance()` 通过 `callSerialized` 在 owner event-loop 串行执行
- 连接建立过程中同步等待 SM 回复（`sendSMRequestAndWaitReply`）；`connectServiceInstance` 先经 `listServiceInstances` 解析目标实例
- `isServiceConnected()` / `isServiceInstanceConnected()` 可从任意线程调用（内部经 `callSerialized` 串行到 owner event-loop；`ConnectionManager::getConnection` 本身无锁，只在 owner 线程访问）
- 连接以逻辑连接键索引：默认实例键即服务名，显式实例键由 `makeServiceConnectionKey(name, instance_id)` 生成；host_id 不参与

### 7.2 自动重连

- 服务离线时通过 `scheduleReconnect` 在 event-loop 上注册定时器
- 重连定时器回调在 event-loop 线程执行
- 重连过程中会查询 SM：显式实例绑定按 `(name, instance_id)` 经 `listServiceInstances` 重新解析 endpoint；host_id 不参与重连身份，实例迁移后自动连到新位置

### 7.3 心跳与会话

控制面：

- 每个 runtime 一个 `MSG_CONTROL_HEARTBEAT`，定时器在 event-loop 线程执行；
- SM 的 `MSG_CONTROL_HEARTBEAT_ACK` 与 runtime 的 ACK deadline 均在 event-loop 线程处理；
- session deadline 到期 → 标记 SM 不可达 → 关闭控制连接 → 重连 + 恢复。

数据面：

- 连接建立后由 `ensureDataSession()` 发送 `MSG_SESSION_HELLO` 并等待 ACK；
- 心跳消息在 owner event-loop 线程通过 `conn_mgr_->sendMessage` 发送；
- `MSG_HEARTBEAT_ACK` 在 event-loop 线程处理，取消 client 侧 deadline；
- 服务端 per-client deadline 与 `onServiceClientDisconnected()` 同样在 owner event-loop 线程执行。

所有心跳/会话定时器都是 owner event-loop 的定时器，不涉及跨线程加锁。

### 7.4 Proxy 基类

- Proxy 构造时登记到 runtime；`start()` 后由 runtime 自动连接
- `setInstance()` 只设置目标实例与连接键；`connectionKey()` 供生成的方法寻址数据面连接
- 生成方法首次调用时自动连接；`setInstance()` 决定连接键，无需手动 connect
- `enableAutoReconnect()` / `setReconnectInterval()` / `startHeartbeat()` / `stopHeartbeat()` 通过 `callSerialized` 串行执行，并以 `connectionKey()`（默认实例即服务名，绑定实例即实例连接键）为键
- 服务状态通告（`onServiceDeath`）在 event-loop 线程执行；Proxy 内部使用精确实例 watch，不依赖业务订阅
