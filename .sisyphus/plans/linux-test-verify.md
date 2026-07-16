# Linux 端测试验证与优化计划

## 背景

近期改动涉及：
- `service_manager/main.cpp` — 事件处理顺序调整（READ 优先于 ERROR）
- `tests/test_performance.cpp` — SM 进程管理改进（pkill 清理、二进制 fallback、优雅关闭）

目标：确保 Linux 端所有测试用例可运行、无问题。

---

## 一、验证清单

### 1.1 编译验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**检查点：**
- [ ] 核心库 `omnibinder_static` 编译通过
- [ ] `service_manager` 编译通过
- [ ] 所有测试目标编译通过
- [ ] 无新增 warning（`-Wall -Wextra -Wpedantic`）

### 1.2 service_manager 事件处理回归

改动：`onClientEvent()` 中 READ 先于 ERROR 处理，且 READ 处理后检查 client 存活再 fall-through。

**场景 A — 正常断开不打印假阳性：**
```bash
# 1. 启动 SM（INFO 级别，能看到所有日志）
./build/target/bin/service_manager --port 19999 --log-level 3 &
SM_PID=$!
sleep 1

# 2. 用脚本模拟 client 连接后正常关闭（TCP FIN）
python3 -c "
import socket
s = socket.socket()
s.connect(('127.0.0.1', 19999))
s.close()
"
sleep 1

# 3. 检查日志：不应出现 "Client error" 字样
kill $SM_PID 2>/dev/null; wait $SM_PID 2>/dev/null
```
**预期：** stderr 中无 `[WARN]` 或 `Client error` 关于 fd 的行。可能看到 `[INFO] Client disconnected` 或 `[INFO] Client closed`。

**场景 B — 真正 socket 错误仍能告警：**
```bash
# 1. 启动 SM
./build/target/bin/service_manager --port 19998 --log-level 3 &
SM_PID=$!
sleep 1

# 2. 用脚本创建连接后发送非法数据触发协议错误（导致 SM 端 close）
python3 -c "
import socket, struct
s = socket.socket()
s.connect(('127.0.0.1', 19998))
# 发送非法消息头（length=0xFFFFFFFF, type=0xFFFF）触发解析失败
s.send(struct.pack('<HHHHI', 0xFFFF, 0xFFFF, 0, 0xFFFFFFFF, 0))
s.close()
"
sleep 1

# 3. 检查日志：应出现 WARN 级别错误
kill $SM_PID 2>/dev/null; wait $SM_PID 2>/dev/null
```
**预期：** stderr 中出现 `[WARN]` 行，表示检测到异常并关闭连接。

**场景 C — READ+WRITE 同轮就绪（代码审查验证）：**
由于需要同时触发可读和可写事件，难以用脚本精确构造。此场景通过代码审查确认：
```cpp
// service_manager/main.cpp onClientEvent():
// READ 处理后不提前 return，而是检查 client 存活再 fall-through 到 WRITE：
if (events & EventLoop::EVENT_READ) {
    onClientRead(conn);
    if (clients_.find(fd) == clients_.end()) return;  // client 已关闭
}
// ... ERROR 检查 ...
if (events & EventLoop::EVENT_WRITE) {
    if (!flushPendingSends(conn)) closeClient(fd);
}
```
**预期：** `clients_.find(fd)` 存活检查存在，READ 不阻塞 WRITE。

### 1.3 性能测试

```bash
cd build
./target/test/test_performance --report ../docs/performance-report.md
```

**检查点：**
- [ ] SM 进程成功启动（`startSM` 找到二进制）
- [ ] `pkill -U $(id -u)` 能清理残留 SM（不报错）
- [ ] 二进制 fallback 搜索正确（`target/bin/` → `./build/target/bin/` → bare）
- [ ] RPC Echo 各组测试通过（0/256/1024/4096/8192 bytes）
- [ ] RPC Add 测试通过
- [ ] Topic pub/sub 各组测试通过（64/256/1024/8192 bytes）
- [ ] Markdown 报告成功生成到 `docs/performance-report.md`
- [ ] `stopSM` 优雅关闭：SIGTERM → 2s 内退出 → 不需要 SIGKILL

### 1.4 全量 ctest

```bash
cd build
ctest --output-on-failure -j4
```

**检查点：**
- [ ] 所有测试 PASS（无 FAIL、无 TIMEOUT）
- [ ] 无端口冲突导致的测试失败
- [ ] `test_generated_runtime_integration` 正确编译并运行
- [ ] 资源锁 `RESOURCE_LOCK sm_port_lock` 生效，并行测试不冲突

### 1.5 集成测试重点

以下测试因启动 SM 子进程，是回归重点：

| 测试 | SM 端口 | 关注点 |
|------|---------|--------|
| `test_stats` | 9900 | 默认端口，无 --port 参数 |
| `test_c_api` | 19910 | C API 功能 |
| `test_integration` | 19912 | 注册/发现/调用 |
| `test_full_integration` | 19902 | SHM + TCP 混合 |
| `test_threadsafe_client_and_reconnect` | 19931 | 多线程重连 |
| `test_runtime_stats` | 19932 | 运行时统计 |
| `test_error_logging` | 19933 | 错误日志关键词 |
| `test_thread_safety` | 19934 | 线程安全 |
| `test_control_plane_and_fallback` | 19935 | 控制面 fallback |
| `test_heartbeat_reconnect` | 19936 | 心跳重连 |
| `test_sm_reconnect` | 19937 | SM 重连 |
| `test_performance` | 19940 | 性能基准 |
| `test_transient_disconnect_recovery` | 多种 | 瞬断恢复 runner |

---

## 二、潜在问题及修复

### 2.1 `pkill` 不可用

**风险：** 极简 Linux 环境可能未安装 `pkill`（`procps` 包）。

**现象：** `startSM` 中 `system("pkill ... || true")` 静默失败，不影响后续流程。

**结论：** 无需修复（`|| true` 已兜底），但可在日志中标注。

### 2.2 残留 `/dev/shm/` 文件

**风险：** `test_performance.cpp:819` 用 `unlink("/dev/shm/binder_PerfService")` 清理，如果上次测试异常退出可能遗留。

**现象：** SHM 创建时可能因残留文件失败。

**现有处理：** 每次 `setUp` 时先 unlink 清理。但如果 SHM 文件属于其他用户则 unlink 失败。

**建议：** 无需改动（同用户测试场景不会有权限问题）。

### 2.3 `waitSM` 超时时间

**风险：** `waitSM(SM_PORT, 30)` 在新代码中轮询间隔从 500ms 改为 100ms，总超时从 15s 变为 3s。

**现象：** 慢机器上 SM 启动超过 3s 则误报 FATAL。

**评估：** 本地 SM 启动通常在 100ms 内，3s 绰绰有余。CI 环境如有需要可调大 retries 参数。

### 2.4 二进制路径 fallback

**现有逻辑：**
```cpp
const char* fallbacks[] = {
    "target/bin/service_manager",        // ctest 从 build/ 运行时的路径
    "./build/target/bin/service_manager", // 从项目根目录运行时的路径
    "service_manager",                    // 依赖 PATH
    NULL
};
```

**问题：** 
- `target/bin/service_manager`：ctest 的 `WORKING_DIRECTORY` 是 `${CMAKE_BINARY_DIR}`，正确 ✅
- `./build/target/bin/service_manager`：从项目根 `./build/target/bin/` 是 ctest 构建目录，正确 ✅
- `service_manager`：依赖 PATH，不太可靠但作为最后兜底

**结论：** 路径覆盖合理，无需改动。

### 2.5 stopSM 优雅关闭

**逻辑：** SIGTERM → 每 100ms 检查一次 → 最多等 2s → 超时则 SIGKILL

**检查点：**
- [ ] SM 收到 SIGTERM 后能在 2s 内正常退出（`waitpid` WNOHANG 返回 >0）
- [ ] SIGKILL 不会误触发

---

## 三、执行步骤

### Step 1: 环境准备

```bash
# 清理旧的构建产物
rm -rf build

# 全新构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Step 2: 快速冒烟

```bash
# 先跑不依赖 SM 的单元测试
cd build
ctest -R "test_buffer|test_message|test_event_loop|test_service_registry|test_heartbeat_death|test_topic_manager|test_owner_thread_executor|test_transport" --output-on-failure
```

**通过标准：** 输出中每个测试后显示 `Passed`，最后一行 `100% tests passed, 0 tests failed out of 8`。

### Step 3: 性能测试单独验证

```bash
# 这是用户最关心的测试
cd build
./target/test/test_performance --report ../docs/performance-report.md
```

**预期输出：**
```
=== OmniBinder Performance Benchmark ===
Starting ServiceManager on port 19940...
ServiceManager ready (pid=XXXXX)
PerfService registered on port XXXXX
--- RPC Round-Trip Latency ---
  Testing Echo (payload=0 bytes)...    [OK]
  ...
--- Topic Pub/Sub Latency ---
  ...
--- Generating Report ---
Cleaning up...
=== Performance benchmark completed ===
```

### Step 4: 全量 ctest

```bash
cd build
ctest --output-on-failure -j4 2>&1 | tee ctest.log
```

### Step 5: 检查失败项

```bash
# 查看失败的测试
grep -E "(FAILED|TIMEOUT|tests passed)" ctest.log
```

**通过标准：** 输出中 `100% tests passed`。如果有失败项，必须确认每一项均为预存问题（非本次改动引入），记录在 plan 中。

---

## 四、成功标准

- [ ] `cmake --build` 零 error、零新增 warning
- [ ] `test_performance` 完整运行，报告成功生成
- [ ] `ctest` 全量通过（允许预存失败项，需确认非本次改动引入）
- [ ] SM 正常断开不打印 "Client error" 假阳性
- [ ] 无端口冲突、无残留进程

---

## 五、注意事项

1. **不要并行跑性能测试：** `test_performance` 会独占端口 19940，且性能数据在并行环境下不可靠
2. **清理残留 SM：** 如果测试异常中断，手动执行 `pkill -U $(id -u) -f service_manager`
3. **SHM 清理：** 异常退出后可能需要手动 `rm /dev/shm/binder_*`
