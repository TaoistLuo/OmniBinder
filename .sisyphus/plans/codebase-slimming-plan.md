# OmniBinder 代码库精简计划

## 背景

基于全代码库架构审计（4 个并行 explore agent + 手动验证），识别出约 1,000 行死代码和 ~600 行可精简的冗余抽象。所有建议均经过 GCC 4.8.4 兼容性验证。

## 约束条件

- GCC 4.8.4 兼容（无 `std::promise`、`std::make_unique`、`constexpr if` 等）
- 零外部依赖
- 嵌入式 Linux 优先，Windows 兼容
- 不改变对外 API 和行为

---

## P0: `omni_runtime.cpp` 按域拆分为 8 个文件

### 现状
`src/core/omni_runtime.cpp` 共 2,761 行，是次大文件（629 行）的 4.4 倍。8 个逻辑域混在一个文件中。

### 方案
不新增类，不改 `OmniRuntime::Impl` 声明。纯将方法实现体按域搬到独立 `.cpp` 文件，每个 `#include "omni_runtime.h"`：

| 新文件 | 内容 | 约行数 |
|--------|------|--------|
| `omni_runtime.cpp` | 构造/析构/init/公共API转发/匿名namespace工具函数 | ~490 |
| `omni_sm.cpp` | SM通信：sendToSM/onSMData/onSMMessage/SM重连 | ~267 |
| `omni_service.cpp` | 服务注册/注销/发现/lookup/listServices | ~430 |
| `omni_connection.cpp` | 连接管理：connect/disconnect/心跳/重连 | ~236 |
| `omni_rpc.cpp` | RPC调用：invoke/invokeOneWay/waitForReply | ~160 |
| `omni_topic.cpp` | 话题pub/sub/broadcast/死亡通知 | ~377 |
| `omni_dispatch.cpp` | 入站请求处理：accept/clientData/SHM请求分发 | ~350 |
| `omni_diag.cpp` | 诊断/watch/统计/clearCache/状态恢复 | ~438 |

### 理由
- 2,761 行单文件在嵌入式项目中不合理
- 无架构变更，纯文件组织
- GCC 4.8.4 无影响
- 每个文件 ≤500 行，易于导航和维护

---

## P1: 删除平台层 18 个真正死函数

### 现状
`platform.h` 声明约 80 个函数，其中 18 个在**任意平台（Linux + Windows）上均无内部或外部调用**，仅在自身定义中存在。

### 删除清单

**命名信号量（6 个）**— 项目并发模型基于 `std::mutex` + `eventfd`，从未使用命名信号量：
```
src/platform/platform.h:        semCreate / semOpen / semWait / semPost / semClose / semUnlink
src/platform/platform_linux.cpp: 对应 POSIX sem_open/sem_wait/sem_post 实现 (~55 行)
src/platform/platform_win.cpp:   对应 CreateSemaphoreA/OpenSemaphoreA/ReleaseSemaphore (~30 行)
```

**自旋锁（3 个）**— SHM 已迁移到 `std::atomic`，自旋锁不再使用：
```
src/platform/platform.h:        spinLockTestAndSet / spinLockRelease / spinWaitHint
src/platform/platform_linux.cpp: 对应实现 (~12 行)
src/platform/platform_win.cpp:   对应实现 (~12 行)
```

**底层原子操作（4 个）**— 同上，所有原子操作已由 `<atomic>` 接管：
```
src/platform/platform.h:        atomicCompareSwap / atomicFetchAdd / atomicFetchSub / atomicFetchAnd
src/platform/platform_linux.cpp: 对应 __sync_* 内置函数实现 (~16 行)
src/platform/platform_win.cpp:   对应 Interlocked* API 实现 (~16 行)
```

**UDS 基础收发（3 个）**— SHM 使用 `udsSendFds/udsRecvFds`（带 SCM_RIGHTS），不需要基础 send/recv：
```
src/platform/platform.h:        udsCreate / udsSend / udsRecv
src/platform/platform_linux.cpp: 对应实现 (~18 行)
src/platform/platform_win.cpp:   对应实现 (~18 行)
```

**UDS 工具（1 个）**：
```
src/platform/platform.h:        udsPollReadable
src/platform/platform_linux.cpp: 对应实现 (~6 行)
src/platform/platform_win.cpp:   对应实现 (~6 行)
```

**杂项（2 个）**：
```
src/platform/platform.h:        popcount32 / isInProgress
src/platform/platform_linux.cpp: 对应实现 (~12 行)
src/platform/platform_win.cpp:   对应实现 (~12 行)
```

### 不删除的跨平台抽象（之前评估已纠正）

| 函数 | 保留理由 |
|------|---------|
| `createNamedEventFd` | Windows: `createEventFd()` 内部调用，创建 Named Pipe。Linux: stub 委托给 `createEventFd()` |
| `openNamedEventFdByPipeName` | Windows: `udsRecvFds()` 内部调用，打开对端管道。Linux: 不需要（SCM_RIGHTS） |
| `openNamedEventFd` | 公共 API 包装，保证跨平台 API 对称性 |
| `memoryBarrier` | `shm_transport.cpp:234` 生产使用，确保 `ready_flag` 跨核可见 |

### 理由
- 全代码库穷举搜索确认零外部引用
- 信号量：项目未使用任何命名信号量（并发模型基于 `std::mutex` + `eventfd`）
- 自旋锁/原子：SHM 迁移到 `std::atomic` 后的遗留
- UDS 基础函数：SHM 使用 `*Fds` 变体（带 SCM_RIGHTS），不需要基础版本

### 影响
~200 行死代码消失，platform.h 接口更清晰。

---

## P1: 删除 `ServiceHostRuntime`（~322 行）

### 现状
```cpp
// src/core/service_host_runtime.h (69 行) + .cpp (253 行)
class ServiceHostRuntime {
    // 零成员变量，所有方法 const
    void onServiceClientData(...);  // 11 个参数，含 5 个 std::function 回调
};
```

### 理由
- **零成员变量**：无状态封装，是"命名空间伪装成类"
- **每个 `onServiceClientData` 调用分配 6 个 `std::function`**（动态内存，嵌入式应避免）
- **方法体仅 ~25 行有效逻辑**，其余是回调转发样板
- **调用方已持有所有状态**：`OmniRuntime::Impl` 拥有 `LocalServiceEntry*`、`EventLoop*`、`TopicRuntime&`，不需要中间层

### 替代方案
将 4 个方法作为 `OmniRuntime::Impl` 的私有方法，或匿名 namespace 自由函数。

### 影响
- 删除 2 个文件（69 + 253 = 322 行）
- 消除每个入站事件的 6 次 `std::function` 动态分配
- 减少一层无意义的间接调用

---

## P1: 删除死协议类型和错误码

### 4 个死消息类型

`include/omnibinder/message.h` 中：

| 类型 | 值 | 理由 |
|------|-----|------|
| `MSG_PING` | 0x0120 | 仅在 `messageTypeToString()` 存在，从未在协议中发送或接收 |
| `MSG_PONG` | 0x0121 | 同上（心跳使用 `MSG_HEARTBEAT`/`MSG_HEARTBEAT_ACK`） |
| `MSG_SHM_UPGRADE` | 0x0130 | 从未发送或接收。传输选择在连接建立时完成，无需协议升级 |
| `MSG_SHM_UPGRADE_ACK` | 0x0131 | 同上 |

同步删除 `src/core/message.cpp` 中 `messageTypeToString()` 的对应 case。

### 8 个死错误码

`include/omnibinder/error.h` 中：

| 错误码 | 值 | 理由 |
|--------|-----|------|
| `ERR_OUT_OF_MEMORY` | -3 | 仅存于 `errorCodeToString()`，零代码路径返回 |
| `ERR_RECV_FAILED` | -103 | 同上 |
| `ERR_ACCEPT_FAILED` | -108 | 同上 |
| `ERR_INVOKE_FAILED` | -205 | 仅 `omnibinder_c.cpp:66` C API wrapper 返回，可消除 |
| `ERR_UNREGISTER_FAILED` | -207 | 仅存于 `errorCodeToString()` |
| `ERR_TOPIC_NOT_FOUND` | -300 | 同上 |
| `ERR_NOT_SUBSCRIBED` | -302 | 同上 |
| `ERR_BUFFER_OVERFLOW` | -502 | 同上 |

同步删除 `src/core/error.cpp` 中 `errorCodeToString()` 的对应 case。

### 理由
- 死消息类型占协议表面 10%，死错误码占 24%
- 从未被任何代码路径使用（全代码库 grep 确认）
- 误导协议阅读者

---

## P2: 删除 `shm_transport` 6 个残留方法（~55 行）

### 现状
旧共享 Arena 设计（32 槽、位图、自旋锁）的遗留方法：

| 方法 | 问题 |
|------|------|
| `responseSlotsInUse()` | `return clientCount()` — 按客户端模型无 slot 概念 |
| `totalResponseArenaSize()` | **零调用者**（全代码库 grep 确认） |
| `activeResponseArenaSize()` | `return totalResponseArenaSize()` — 从未调用 |
| `maxClients()` | `return clientCount()` — 按客户端模型无上限 |
| `serverBroadcast()` | **零调用者**（17 行），真正的死代码 |
| `activeClientIds()` | 仅测试使用，可标注为 test helper |

### 理由
- 按客户端 SHM 模型不需要 slot/arena/broadcast 概念
- 旧设计的词汇残留，误导新开发者

### 处理
- `responseSlotsInUse` / `totalResponseArenaSize` / `activeResponseArenaSize` / `maxClients` / `serverBroadcast`：删除
- `activeClientIds`：标注 `// Test helper`，移到测试可见区域

---

## P2: 裁剪 `OwnerThreadExecutor` 死代码路径（~50 行）

### 保留背景
GCC 4.8.4 不支持 `std::promise`。手写 `SyncCallState<T>`（`std::mutex` + `std::condition_variable`）是正确方案。核心模式保留。

### 删除项

| 删除 | 位置 | 理由 |
|------|------|------|
| `invoke()` 重载（~20 行） | `owner_thread_executor.h:240-260` | 零调用者，仅 `invokeOnOwner` 被使用 |
| `ExecutionResult` 错误传播路径（~30 行） | `owner_thread_executor.h:66-72, 222-237` | `!result.ok()` 分支总是返回默认值，错误信息被丢弃 |

### 理由
核心同步模式正确，但死路径增加阅读负担。删除后 `OwnerThreadExecutor` 从 310 行减至 ~260 行。

---

## P2: 修复 `TopicRuntime` 封装

### 现状
```cpp
// src/core/topic_runtime.h
class TopicRuntime {
public:
    std::map<std::string, uint32_t> published_topics;  // 公共成员！
    // ...
    std::vector<int>* tcpSubscribers(uint32_t);          // 返回原始指针
    std::vector<ShmSubscriber>* shmSubscribers(uint32_t); // 返回原始指针
};
```

### 修改
1. `published_topics` → `private`，添加：
   ```cpp
   bool isTopicPublished(const std::string& name) const;
   uint32_t topicId(const std::string& name) const;
   ```
2. `tcpSubscribers` / `shmSubscribers` → 返回 `const std::vector&`

### 理由
- 公共成员被外部 3+ 处直接访问（`omni_runtime.cpp` 诊断路径）
- 返回原始指针存在悬空风险（调用者持有指针期间 map 可能被修改）

---

## P3: 简化 `heartbeat_monitor`（~60 行）

### 设计意图分析

原始设计采用"双旋钮"心跳检测模型：
- `timeout_ms_`：预期心跳间隔（检测粒度）
- `max_missed_`：容忍倍数（多少倍间隔的静默才判定死亡）
- 死亡阈值 = `timeout_ms_ × max_missed_` 毫秒

`missed_count` 不是实际丢失计数器，而是 `elapsed / timeout_ms_` 的反推值，作用：
1. 日志可读性：`"missed 2/3 heartbeats"` 比 `"elapsed 8500ms"` 直观
2. 渐进式告警：超时发生前就在日志中显示增长

### 修改

| 操作 | 内容 | 理由 |
|------|------|------|
| **删除** | `missed_count` 持久字段 → 改为 `checkTimeouts()` 局部变量 | 仅在该方法内使用一次，无需持久化。`HeartbeatEntry` 从 16 字节减至 8 字节 |
| **删除** | `isTimedOut()` | 零调用者（生产+测试），`checkTimeouts()` 覆盖其语义 |
| **删除** | `setTimeout()` / `setMaxMissed()` | 零调用者 |
| **删除** | `getTimeout()` / `getMaxMissed()` | 零调用者 |
| **标注** | `trackedCount()` → `// Test helper` | 仅测试使用（18 处引用），保留但标注归类 |

### 理由
- `missed_count` 的日志语义保留（改为局部变量），但消除冗余持久状态
- 4 个 getter/setter + 1 个查询方法无调用者

---

## P3: `platform.h` 按区域归类

### 现状
80 个函数声明无组织，核心 API 与测试辅助混在一起。

### 修改
按使用场景分组，加注释分隔：

```cpp
// ============================================================
// 网络/传输 — 生产代码使用
// ============================================================
bool netInit();
void netCleanup();
int  createSocket(...);
// ... (约 15 个函数)

// ============================================================
// 共享内存 — 生产代码使用
// ============================================================
void* shmCreate(...);
void  shmDetach(...);
// ... (约 5 个函数)

// ============================================================
// EventFd — 生产代码使用
// ============================================================
int  createEventFd();
int  createNamedEventFd(...);
// ...

// ============================================================
// UDS — 生产代码使用
// ============================================================
int  udsBindListen(...);
int  udsAccept(...);
bool udsSendFds(...);
// ...

// ============================================================
// 时间/系统/原子 — 生产代码使用
// ============================================================
int64_t currentTimeMs();
uint32_t getPid();
void memoryBarrier();
// ...

// ============================================================
// Test helpers — 仅测试代码引用
// ============================================================
bool waitFdReadable(...);
```

### 理由
- 零代码变更，纯组织优化
- 新开发者可快速定位所需 API 区域
- 测试辅助与核心 API 不再混淆

---

## 不删除的项目及理由

| 项目 | 理由 |
|------|------|
| **`TransportFactory`** | 传输层扩展点。provider registry + freeze 机制为未来传输层扩展预留 |
| **Pimpl (`OmniRuntime::Impl`)** | GCC 4.8.4 嵌入式工具链下编译隔离有价值。隐藏 400+ 行 Impl 声明避免增加用户编译时间 |
| **`RpcRuntime`** | 132 行小型封装，提供序列号+超时+等待的真实封装，非空壳 |
| **`OwnerThreadExecutor` 核心** | GCC 4.8.4 不支持 `std::promise`，手写同步是正确方案。仅裁剪死路径 |
| **命名 EventFd 系列** | 跨平台抽象。Windows 内部使用（Named Pipe），Linux stub 保证 API 对称 |
| **`memoryBarrier`** | `shm_transport.cpp:234` 生产使用，确保 `ready_flag` 跨核可见 |
| **`std::unique_ptr` 迁移** | raw pointer 是项目惯例而非疏忽。现有析构路径已正确处理 |
| **诊断 6 个 API** | `enableDiagnostic/disableDiagnostic/watchPid/unwatchPid/setLogLevelByPid/listRuntimes` — omni-cli 和测试实际使用，非死代码 |

---

## 影响汇总

| 优先级 | 操作 | 影响 | 风险 |
|--------|------|------|------|
| P0 | `omni_runtime.cpp` 拆分 | 2761→8×~400 行 | 低（纯文件组织） |
| P1 | 删除平台层 18 死函数 | ~200 行 | 低（确认零引用） |
| P1 | 删除 `ServiceHostRuntime` | ~322 行 | 中（需验证回调内联） |
| P1 | 删除死消息类型+错误码 | ~20 行定义 | 低 |
| P2 | 删除 shm_transport 残留 | ~55 行 | 低 |
| P2 | 裁剪 `OwnerThreadExecutor` | ~50 行 | 低 |
| P2 | `TopicRuntime` 封装修复 | 0 行 | 低 |
| P3 | `heartbeat_monitor` 精简 | ~60 行 | 低 |
| P3 | `platform.h` 区域归类 | 0 行 | 无 |

**净效果：~700 行死代码消失，omni_runtime.cpp 从单体拆为 8 文件，零架构变更，GCC 4.8.4 完全兼容。**
