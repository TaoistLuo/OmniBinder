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

- 生命周期/驱动：`init()`、`run()`、`stop()`、`isRunning()`、`pollOnce()`
- 服务注册：`registerService()`、`unregisterService()`
- 服务发现：`lookupService()`、`listServices()`、`queryInterfaces()`
- RPC：`invoke()`、`invokeOneWay()`
- 死亡通知：`subscribeServiceDeath()`、`unsubscribeServiceDeath()`
- 话题：`publishTopic()`、`broadcast()`、`subscribeTopic()`、`unsubscribeTopic()`
- 配置：`setHeartbeatInterval()`、`setDefaultTimeout()`、`hostId()`

这里的“线程安全”含义是：

- 不出现未定义的数据竞争
- 不要求调用方自己在 `OmniRuntime` 外再包互斥锁
- 不承诺所有调用都会并行执行；内部可以串行化

### 2.1.1 按 API 分类的当前语义

| API 类别 | 方法 | 当前并发语义 |
|---|---|---|
| 生命周期 | `init()` | 允许从任意线程调用，但同一实例只应成功完成一次初始化；与运行中的 `run()` / `pollOnce()` 不能无序交叠 |
| 持续驱动 | `run()` | 线程安全，但同一时刻只允许一个线程承担 owner event-loop 的持续驱动职责 |
| 单次驱动 | `pollOnce()` | 线程安全，但不能与另一个线程中的 `run()` 形成“双重驱动”；若 `run()` 已接管 event-loop，应禁止并发 `pollOnce()` |
| 停止 | `stop()` | 可从任意线程安全调用；必须可靠唤醒 owner event-loop 并使 `run()` 退出 |
| 状态查询 | `isRunning()`、`hostId()` | 可从任意线程并发读取，但返回值只保证内部状态的可见性，不代表更强的事务语义 |
| 配置 | `setHeartbeatInterval()`、`setDefaultTimeout()` | 可从任意线程安全调用；配置变更应在 owner event-loop 上串行生效 |
| 服务发现 | `lookupService()`、`listServices()`、`queryInterfaces()` | 可从任意线程安全调用；同步等待与缓存更新由 owner event-loop 串行处理 |
| RPC | `invoke()`、`invokeOneWay()` | 可从任意线程安全调用；同步调用允许阻塞调用线程，但内部发送/等待/状态更新由 owner event-loop 串行处理 |
| 服务注册 | `registerService()`、`unregisterService()` | 可从任意线程安全调用；监听 socket、SHM、SM 注册状态仍由 owner event-loop 管理 |
| topic / death | `publishTopic()`、`broadcast()`、`subscribeTopic()`、`unsubscribeTopic()`、`subscribeServiceDeath()`、`unsubscribeServiceDeath()` | 可从任意线程安全调用；回调注册和派发在 owner event-loop 串行处理 |

### 2.1.2 同步阻塞 API 的额外约束

以下 API 属于同步阻塞调用：

- `lookupService()`
- `listServices()`
- `queryInterfaces()`
- `invoke()`
- `subscribeServiceDeath()`
- `publishTopic()`
- `subscribeTopic()`

它们的当前语义是：

- 调用线程可以阻塞等待结果
- 超时前会持续处理事件并等待 reply
- 不应把这类同步等待理解成“内部可由多个线程并行处理同一实例状态”

### 2.1.3 超时与阻塞语义

- `invoke(..., timeout_ms)` 支持显式超时参数
- 当 `timeout_ms == 0` 时，使用 `setDefaultTimeout()` 配置的默认超时
- 当前同步等待最终由运行时 deadline 控制，超时会返回错误码，而不是无限阻塞
- 当前语义下，正常调用路径不会“永远卡住等 reply”

## 2.2 回调线程语义

以下回调统一在 owner event-loop 线程执行：

- `TopicCallback`
- `DeathCallback`
- 服务端请求处理相关回调
- 连接断开、topic 发布者通知等内部派发回调

**重要限制：**

- 不建议在 `TopicCallback`、`DeathCallback`、服务端 `onInvoke()`、`onStart()`、`onStop()` 等 owner event-loop 线程回调中，再对同一个 `OmniRuntime` 发起**同步阻塞 API**（如 `invoke()`、`lookupService()`、`subscribeTopic()`）
- 回调中应尽量只做轻量处理，必要时把后续同步工作转交给其他线程或异步任务

---

## 3. `run()` / `pollOnce()` / `stop()` 语义

线程安全设计里，最容易出问题的不是 `invoke()`，而是事件循环驱动方法。

### 3.1 `run()`

- `run()` 表示当前线程接管 owner event-loop，持续驱动事件处理
- 一次只能有一个线程处于“持续驱动事件循环”的角色
- 不应允许多个线程同时调用 `run()` 试图共同驱动同一个 `OmniRuntime`

### 3.2 `pollOnce()`

- `pollOnce()` 用于外部手动泵一轮事件循环
- 如果已经有线程在执行长期 `run()`，则不应由其他线程并发调用 `pollOnce()` 再次驱动同一个 event-loop

### 3.3 `stop()`

- `stop()` 应可从任意线程安全调用
- 调用后应可靠唤醒 owner event-loop 并结束 `run()`

### 3.4 明确规则

按以下规则使用：

1. 同一 `OmniRuntime` 实例只允许一个 owner event-loop 驱动者
2. `run()` 与其他线程中的 `run()` 并发调用：**禁止**
3. `run()` 与其他线程中的 `pollOnce()` 并发调用：**禁止**
4. `pollOnce()` 与其他线程中的 `pollOnce()` 并发调用：**禁止**
5. `stop()` 与上述任意方法并发调用：**允许**

这个规则集较保守，但最不容易误导调用方。

---

## 4. 当前支持的使用方式

`OmniRuntime` 支持以下使用方式：

1. 多个线程并发调用公共 API
2. 一个线程持续执行 `run()`，其他线程发起调用
3. 外部主循环定期调用 `pollOnce()` 驱动事件处理

使用限制：

- 同一实例一次只能有一个线程持续驱动 event-loop
- 不建议在 owner event-loop 回调中再对同一个 `OmniRuntime` 发起同步阻塞 API
- 线程安全并不意味着内部状态会被多个线程并行修改

---

## 5. 调用方使用建议

## 5.1 推荐方式

### 方式一：专用事件线程 + 其他线程并发调用 API

```cpp
omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

std::thread loop_thread([&client]() {
    client.run();
});

// 其他线程安全发起调用
std::thread worker([&client]() {
    omnibinder::Buffer req, resp;
    runtime.invoke("SensorService", 0x1234, 0x1, req, resp, 3000);
});
```

### 方式二：外部主循环定期调用 `pollOnce()`

```cpp
while (running) {
    runtime.pollOnce(50);
    // 其他业务逻辑
}
```

## 5.2 调用方不应假设的事情

- 不应假设多个线程会并行执行内部状态修改
- 不应假设回调会在“发起调用的那个线程”执行
- 不应把“可并发调用”理解成“所有行为都无顺序约束”

---

## 6. 一句话建议

- 想最省心：一个线程 `run()`，其他线程安全调用 API
- 想自己驱动：在外部循环里调用 `pollOnce()`
- 想避免风险：不要在同一个 runtime 的回调里再发同步阻塞 API
