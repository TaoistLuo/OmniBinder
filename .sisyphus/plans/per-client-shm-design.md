# Per-Client SHM 设计方案

## 一、当前架构（服务端预分配）

```
registerService() 时一次性分配:
┌──────────────────────────────┐
│ ShmControlBlock (560B)       │  服务端 shm_open("/binder_Svc")
├──────────────────────────────┤
│ RequestQueue (4KB)           │  所有客户端共享，自旋锁保护
├──────────────────────────────┤
│ ResponseSlot[0] (4KB)        │
│ ResponseSlot[1] (4KB)        │
│ ...                          │  32 个 slot，全部预占位
│ ResponseSlot[31] (4KB)       │
└──────────────────────────────┘
默认 133KB，固定 32 客户端上限
```

**问题：**
- 0 个客户端也占 133KB
- 硬编码上限 32 客户端
- 共享 RequestQueue → 自旋锁争用
- 客户端崩溃 slot 永久泄漏（直到服务端重启）

---

## 二、目标架构（客户端创建 per-connection SHM）

```
每个客户端自己创建独立 SHM，通过 UDS 把名字告诉服务端：

客户端 A: shm_open("/omni_Svc_cli_1001")  →  独立 8KB SHM
客户端 B: shm_open("/omni_Svc_cli_1002")  →  独立 8KB SHM
客户端 C: shm_open("/omni_Svc_cli_1003")  →  独立 8KB SHM

每个 SHM 内部布局：
┌──────────────────────┐
│ RingHeader (16B)     │  请求 ring（客户端写，服务端读）
│ req_data (4KB)       │
├──────────────────────┤
│ RingHeader (16B)     │  响应 ring（服务端写，客户端读）
│ resp_data (4KB)      │
└──────────────────────┘
```

**收益：**
- 0 客户端 = 0 内存
- 无客户端数上限
- 每个客户端独立 ring → 完全无锁
- 客户端崩溃 → 服务端检测 fd 关闭 → 自动清理

---

## 三、连接流程

### 3.1 服务端启动

```
1. registerService("SensorService")
2. 创建 TcpTransportServer（和现在一样）
3. 创建 UDS listener（仅用于握手阶段接收客户端的 shm_name + eventfd）
4. 注册到 SM（ServiceInfo 无 shm_name）
5. epoll 等待：TCP accept + UDS accept（仅 accept 新连接，不维持）
```

### 3.2 客户端连接

```
1. connectService("SensorService") → SM lookup → 得到 host/port/host_id
2. 判断同机（host_id 相同）→ 走 SHM
3. shm_open("/omni_SensorService_cli_" + PID) → ftruncate → mmap
4. UDS connect(server_uds_path)              ← 一次性握手
5. 发送: [shm_name_len][shm_name][req_eventfd(scm)]
6. 接收: [resp_eventfd(scm)]
7. UDS close                                  ← 握手完成，关闭 UDS
8. 后续通信全部走 SHM + eventfd
```

### 3.3 服务端 accept（UDS 一次性握手）

```
1. UDS accept → onUdsClientConnect
2. 接收: [shm_name_len][shm_name][client_req_eventfd]
3. shm_open(shm_name) → mmap
4. 注册 client_req_eventfd 到 EventLoop       ← 此后靠 eventfd 感知客户端存活
5. 创建 resp_eventfd
6. 发送: [resp_eventfd(scm)]
7. UDS close                                   ← 握手完成，关闭 UDS
8. 记录到 map<client_id, ClientShmContext>
```

**连接存活检测：** SM 心跳（不变） + eventfd 关闭检测（epoll 自动感知）。UDS 不参与。

### 3.4 数据流

```
客户端 → 服务端（请求）:
  1. 写 req_ring → eventfd_notify(req_eventfd)
  2. 服务端 epoll 被唤醒 → 读对应客户端的 req_ring
  3. 处理请求

服务端 → 客户端（响应）:
  1. 写 resp_ring → eventfd_notify(resp_eventfd)
  2. 客户端 epoll 被唤醒 → 读 resp_ring
```

---

## 四、崩溃恢复

### 4.1 服务端崩溃

```
客户端 SHM 不受影响（客户端进程还活着）
  → UDS 检测到断开
  → 重连 UDS → 发送同一个 shm_name
  → 服务端 shm_open → 恢复通信
  → 无需重建 SHM，无需重新 mmap
```

### 4.2 客户端崩溃

```
服务端检测到 client_req_eventfd 关闭
  → munmap → 清理 ClientShmContext
  → 客户端进程死亡 → 内核自动关闭 shm fd
  → SHM 段引用计数归零 → 内核回收
```

---

## 五、各组件影响

| 组件 | 改动量 | 说明 |
|---|---|---|
| **types.h** | ~5 行 | ServiceInfo 删除 `shm_name` 和 `shm_config` 字段。ShmConfig 结构体保留（客户端创建 SHM 时用） |
| **message.h/cpp** | ~10 行 | `serializeServiceInfo` / `deserializeServiceInfo` 删除 shm_name/shm_config 读写 |
| **ServiceManager** | ~5 行 | 协议适配——注册/查询/通知中 ServiceInfo 序列化代码缩短（两个字段消失），日志中 shm_name 引用删除 |
| **omni-tool** | 0 行 | 完全不碰 SHM |
| **omni-idlc** | 0 行 | 完全不碰 SHM |
| **platform 层** | 0 行 | shm_open/mmap/UDS 原语不变 |
| **ConnectionManager** | ~20 行 | `getOrCreateConnection` 不再接收 shm_name/shm_config 参数，SHM 协商内聚到 ShmTransport |
| **OmniRuntime** | ~80 行 | 删除 `initializeServiceShm()`；服务端构造 ShmTransport 不再传固定 shm_name；`publishTopicInternal` 删除 shm_name 赋值 |
| **ShmTransport** | ~350 行 | initServer/initClient 重写；删除 ShmControlBlock.slots[32]、response_bitmap、req_lock、SHM_MAX_CLIENTS；UDS 协议升级 |

### 协议变更

```
ServiceInfo 序列化（当前）:
  [name][host][port][host_id][shm_name][shm_config.req][shm_config.resp][iface_count]...

ServiceInfo 序列化（改后）:
  [name][host][port][host_id][iface_count]...
  ↑ shm_name 和 shm_config 两个字段彻底消失
```

`ShmConfig` 不再通过 SM 传递——客户端自己决定 ring 大小（从 service 的 `setShmConfig()` 获取，或使用默认值）。

---

## 六、UDS 协议变更

```
当前:
  Client → Server: [client_id(uint32)]
  Server → Client: SCM_RIGHTS{req_eventfd, resp_eventfds[client_id]}

改后:
  Client → Server: [shm_name_len(uint32)][shm_name(bytes)][req_eventfd(scm)]
  Server → Client: SCM_RIGHTS{resp_eventfd}
```

---

## 七、内存对比

| 场景 | 当前 | per-client |
|---|---|---|
| 0 客户端 | 133KB | 0 |
| 1 客户端 | 133KB | ~8KB |
| 3 客户端 | 133KB | ~24KB |
| 32 客户端 | 133KB | ~256KB |
| 100 客户端 | ❌ 不支持 | ~800KB |

---

## 八、锁模型对比

| 操作 | 当前 | per-client |
|---|---|---|
| 客户端写请求 | spinlock_acquire → write → release | write（无锁） |
| 服务端读请求 | 读共享 ring | 读当前 client 的 ring |
| 服务端写响应 | 写 slot[N] | 写当前 client 的 ring |
| 客户端读响应 | 读 slot[N] | 读自己的 ring |

---

## 九、优劣势总结

| 维度 | 优势 | 代价 |
|---|---|---|
| **内存** | 0 客户端 = 0KB | 大量客户端时总内存更大（256KB vs 133KB） |
| **并发** | 完全无锁 | |
| **扩展性** | 无客户端上限 | |
| **崩溃恢复** | 服务端崩溃不影响客户端 SHM | |
| **协议** | ServiceInfo 瘦身（删除 shm_name/shm_config） | SM 协议变更 |
| **代码** | 删除 ~800 行（slot/Bitmap/自旋锁全部清掉） | 新增 ~400 行（per-client 管理） |

---

## 十、实施步骤

### Step 1: 协议清理 — ServiceInfo 瘦身
**文件:** `include/omnibinder/types.h`, `include/omnibinder/message.h`, `src/core/message.cpp`
**内容:** 从 ServiceInfo 删除 `shm_name` 和 `shm_config`，更新序列化/反序列化。
**验证:** `cmake --build build -j$(nproc)` — 编译通过即协议一致
**通过标准:** 零编译错误

### Step 2: SM + OmniRuntime 适配协议变更
**文件:** `service_manager/main.cpp`, `src/core/omni_runtime.cpp`
**内容:** SM 删除 shm_name/shm_config 相关日志引用；OmniRuntime 删除 `initializeServiceShm()`、`publishTopicInternal` 中 shm_name 赋值。
**验证:** `ctest --test-dir build -R "test_stats|test_c_api" --output-on-failure`
**通过标准:** SM 相关测试 PASS

### Step 3: ShmTransport 核心重写
**文件:** `src/transport/shm_transport.h`, `src/transport/shm_transport.cpp`
**内容:** 删除 `ShmSlotStatus`、`ShmControlBlock.slots[32]`、`response_bitmap`、`req_lock`、`SHM_MAX_CLIENTS` 常量。重写 `initServer()`（只创建 UDS listener），重写 `initClient()`（shm_open 创建自有 SHM + UDS 握手发送名字），删除 `allocateResponseSlot()`、slot 扫描、`response_bitmap` 操作。Server 端改为 per-client 独立 ShmContext map。
**验证:** `ctest --test-dir build -R test_shm_transport --output-on-failure`
**通过标准:** `test_shm_transport` 全部 PASS

### Step 4: UDS 协议升级
**文件:** `src/transport/shm_transport.cpp`（`onUdsClientConnect`）
**内容:** 协议从 `[client_id(4B)]` → `[name_len(4B)][name(N)][req_eventfd(scm)]`，回复从 `SCM_RIGHTS{req, resp}` → `SCM_RIGHTS{resp}`。
**验证:** `ctest --test-dir build -R test_shm_transport --output-on-failure`
**通过标准:** 多客户端并发测试 PASS，确认各自独立 SHM

### Step 5: ConnectionManager 简化
**文件:** `src/core/connection_manager.cpp`
**内容:** `getOrCreateConnection` 不再接收 shm_name/shm_config 参数，SHM 协商完全内聚到 ShmTransport::connect()。
**验证:** `ctest --test-dir build -R "test_integration|test_full_integration" --output-on-failure`
**通过标准:** 集成测试全部 PASS

### Step 6: per-client 请求分发 + 性能验证
**文件:** `src/core/omni_runtime.cpp`
**内容:** 服务端为每个客户端注册独立 eventfd，`onShmRequest` 按 client_id 分发到对应 ShmContext。
**验证:** `ctest --test-dir build -R test_performance --output-on-failure`
**通过标准:** RPC 延迟不劣于当前（~68μs），无自旋锁

### Step 7: 全量回归
**命令:** `ctest --test-dir build -j4 --output-on-failure`
**通过标准:** 28/28 PASS
