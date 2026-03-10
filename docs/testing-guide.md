# OmniBinder 测试说明

## 1. 文档目的

本文档说明 OmniBinder 测试用例的用途、运行方式以及推荐的执行入口。

手工执行某个测试与 `ctest` 行为不一致时，优先参考本文档中的**工作目录要求**。

## 2. 最重要的执行原则

### 2.1 优先使用 CTest

推荐优先使用：

```bash
ctest --test-dir build --output-on-failure
```

原因：

- `ctest` 会使用测试在 `tests/CMakeLists.txt` 中声明的 `WORKING_DIRECTORY`
- 某些集成测试和性能测试依赖 `build/target/bin/service_manager`、`build/target/test/*` 这类构建目录下的产物
- 直接在仓库根目录执行时，工作目录不同，可能导致行为不一致

### 2.2 如果要手工执行，请在正确工作目录中运行

当前测试二进制生成在：

- `build/target/test/`

对于依赖 `service_manager` 的测试，推荐两种运行方式：

```bash
# 方式一：使用 ctest（推荐）
ctest --test-dir build --output-on-failure -R test_full_integration

# 方式二：先进入 build，再运行 target/test 下的二进制
cd build
./target/test/test_full_integration
```

不推荐从仓库根目录直接执行：

```bash
./target/test/test_full_integration
```

除非你明确知道当前构建产物就放在根目录 `target/`，否则这种方式很容易跑到旧二进制、错误工作目录，或者找不到依赖的 `service_manager`。

## 3. 当前测试列表

当前通过 `ctest --test-dir build -N` 可见 21 个测试（若检测到 Python 3 解释器）：

- `test_buffer`
- `test_message`
- `test_event_loop`
- `test_transport`
- `test_service_registry`
- `test_heartbeat_death`
- `test_topic_manager`
- `test_shm_transport`
- `test_idl_compiler`
- `test_omni_cli_type_codec`
- `test_c_api`
- `test_soak_runner`
- `test_transient_disconnect_recovery`
- `test_integration`
- `test_full_integration`
- `test_control_plane_and_fallback`
- `test_performance`
- `test_threadsafe_client_and_reconnect`
- `test_runtime_stats`
- `test_error_logging`
- `test_longrun_tools`

## 4. 单元测试

### 4.1 `test_buffer`

用途：

- 验证 `Buffer` 的读写、扩容、字符串和基础类型序列化能力

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_buffer
```

或：

```bash
./build/target/test/test_buffer
```

### 4.2 `test_message`

用途：

- 验证 `Message` / `MessageHeader` 的序列化、反序列化和校验逻辑

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_message
```

### 4.3 `test_event_loop`

用途：

- 验证 `EventLoop` 的 fd 监听、定时器和基本事件分发

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_event_loop
```

### 4.4 `test_transport`

用途：

- 验证 TCP transport 基本行为
- 验证 transport policy 基础选择
- 验证 `TcpTransport::send()` 在背压下可返回 partial write

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_transport
```

### 4.5 `test_service_registry`

用途：

- 验证 `ServiceRegistry` 的增删查改与 ownership 语义

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_service_registry
```

### 4.6 `test_heartbeat_death`

用途：

- 验证 `HeartbeatMonitor` 与 `DeathNotifier` 的基础行为

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_heartbeat_death
```

### 4.7 `test_topic_manager`

用途：

- 验证 `TopicManager` 的 publisher / subscriber 管理
- 验证 publisher owner 语义
- 验证按 fd 清理 topic 边界

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_topic_manager
```

### 4.8 `test_shm_transport`

用途：

- 验证 SHM 基本收发
- 验证 slot 可复用
- 验证 arena 使用统计

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_shm_transport
```

### 4.9 `test_idl_compiler`

用途：

- 验证 `omni-idlc` 的词法 / 语法 / AST 基础能力

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_idl_compiler
```

### 4.10 `test_omni_cli_type_codec`

用途：

- 验证 `omni-cli` / `TypeCodec` 的 JSON <-> Buffer 编解码
- 验证同包嵌套自定义结构体 roundtrip
- 验证跨包自定义结构体 roundtrip

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_omni_cli_type_codec
```

### 4.11 `test_c_api`

用途：

- 验证 C Buffer API 基础读写与 hash
- 验证 C 服务注册 / C 客户端调用
- 验证 C API 运行时统计查询与重置

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_c_api
```

## 5. 集成测试

### 5.1 `test_integration`

用途：

- 验证 ServiceManager 启动
- 服务注册 / 注销
- 服务发现
- 接口查询
- 基础 RPC 调用

工作目录要求：

- 推荐使用 `ctest --test-dir build`
- 如果手工运行，推荐先进入 `build/`

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_integration
```

或：

```bash
cd build
./target/test/test_integration
```

### 5.2 `test_full_integration`

用途：

- 验证 SHM 多客户端
- topic 发布/订阅
- death notification
- interface mismatch 硬拒绝
- service 级 SHM 配置传播

这是当前覆盖最全的主链路测试之一。

工作目录要求：

- 强烈推荐 `ctest --test-dir build`
- 如果手工运行，请使用：

```bash
cd build
./target/test/test_full_integration
```

不推荐：

```bash
./target/test/test_full_integration
```

因为这很容易受到当前工作目录、旧二进制和 `service_manager` 残留进程影响。

### 5.3 `test_control_plane_and_fallback`

用途：

- 非法 `unregister` 测试
- 非法 `publish topic` 测试
- SHM 失败后 TCP fallback 测试
- 大 `LIST_SERVICES_REPLY` 场景测试

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_control_plane_and_fallback
```

或：

```bash
cd build
./target/test/test_control_plane_and_fallback
```

### 5.4 `test_threadsafe_client_and_reconnect`

用途：

- 验证同一个 `OmniRuntime` 的多线程并发调用路径
- 验证 ServiceManager 重启后的基础重连与控制面状态恢复

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_threadsafe_client_and_reconnect
```

### 5.5 `test_runtime_stats`

用途：

- 验证 `RuntimeStats` 初始值
- 验证成功/失败调用会更新统计计数器
- 验证 `resetStats()` 会清零计数器

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_runtime_stats
```

### 5.6 `test_error_logging`

用途：

- 验证关键错误日志关键词会出现在 stderr 输出中
- 覆盖 SM 连接失败、SM 断线重连、错误调用等主链路场景

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_error_logging
```

### 5.7 `test_soak_runner`

用途：

- 做重型测试组合的多轮稳定性回归
- 默认通过 CTest 入口执行 2 轮
- 覆盖以下重型测试：
  - `test_full_integration`
  - `test_control_plane_and_fallback`
  - `test_threadsafe_client_and_reconnect`
  - `test_performance`

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_soak_runner
```

如果要手工指定轮数：

```bash
cd build
./target/test/test_soak_runner 5
```

### 5.8 `test_transient_disconnect_recovery`

用途：

- 做断连恢复主链路的重复回归
- 默认通过 CTest 入口执行 3 轮
- 当前通过重复执行以下测试验证恢复链路稳定性：
  - `test_error_logging`
  - `test_threadsafe_client_and_reconnect`

推荐运行：

```bash
ctest --test-dir build --output-on-failure -R test_transient_disconnect_recovery
```

如果要手工指定轮数：

```bash
cd build
./target/test/test_transient_disconnect_recovery 5
```

## 6. 观测与排障建议

### 6.1 运行时统计

当前已提供 `RuntimeStats / getStats / resetStats`，可用于最小可行观测：

- RPC 调用总数 / 成功数 / 失败数 / 超时数
- 数据面连接错误数
- ServiceManager 重连尝试数 / 成功数
- 当前 active / TCP / SHM 连接数

### 6.2 错误日志关键词

当前可直接用于 grep / 日志平台检索的关键词包括：

- `sm_connect_failed`
- `sm_connect_timeout`
- `sm_connection_lost`
- `sm_reconnect_begin`
- `sm_reconnect_success`
- `rpc_send_failed`
- `rpc_timeout`
- `data_connect_failed`
- `data_connect_timeout`
- `data_connect_fallback`
- `data_send_failed`
- `data_connection_lost`

## 7. 性能测试与稳定性长测

### 7.1 `test_performance`

用途：

- RPC Echo 延迟
- RPC Add 延迟
- topic pub/sub 延迟
- 自动生成 `docs/performance-report.md`

### 7.2 `test_soak_runner`

用于长时间重复验证以下重型路径：

- 主集成链路
- 控制面/SHM fallback
- 多线程安全调用与重连
- 性能路径

通过参数可控制轮数：

```bash
cd build
./target/test/test_soak_runner 10
```

### 7.3 `test_transient_disconnect_recovery`

用于重复验证断连与重连恢复主链路：

```bash
cd build
./target/test/test_transient_disconnect_recovery 10
```

### 7.4 推荐运行方式

推荐：

```bash
ctest --test-dir build --output-on-failure -R test_performance
```

或手工运行：

```bash
cd build
./target/test/test_performance
```

### 7.5 常见执行错误来源

如果直接在仓库根目录执行：

```bash
./target/test/test_performance
```

那么有两类常见问题：

- 跑到的不是当前最新构建产物
- 工作目录不正确，导致测试内部依赖的相对路径、`service_manager` 查找和生成报告路径出现偏差

因此，性能测试的推荐方式始终是：

```bash
ctest --test-dir build --output-on-failure -R test_performance
```

或者：

```bash
cd build
./target/test/test_performance
```

### 7.6 性能测试产物

成功执行后，会更新：

- `docs/performance-report.md`

### 7.7 一周长测（7x24 小时）推荐方法

如果需要连续执行一周，建议不要只跑一次 `ctest`，而是持续循环执行以下两个稳定性入口：

- `test_soak_runner`
- `test_transient_disconnect_recovery`

推荐做法：

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
mkdir -p build/longrun_logs
cd build

while true; do
  date '+[%F %T] START long-run cycle' | tee -a longrun_logs/weekly.log
  ./target/test/test_soak_runner 5 >> longrun_logs/weekly.log 2>&1 || break
  ./target/test/test_transient_disconnect_recovery 5 >> longrun_logs/weekly.log 2>&1 || break
  ctest --output-on-failure -R "test_c_api|test_omni_cli_type_codec|test_runtime_stats|test_error_logging" >> longrun_logs/weekly.log 2>&1 || break
  date '+[%F %T] END long-run cycle' | tee -a longrun_logs/weekly.log
done
```

说明：

- `test_soak_runner 5`：每轮会执行 4 个重型测试，共 5 轮
- `test_transient_disconnect_recovery 5`：每轮会执行 2 个恢复相关测试，共 5 轮
- 额外再跑一组关键轻量回归，避免只盯重型链路
- 任一命令失败会 `break`，便于你第一时间发现问题

如果需要在后台持续运行并保留日志，推荐：

```bash
cd build
nohup bash -c '
while true; do
  date "+[%F %T] START long-run cycle"
  ./target/test/test_soak_runner 5
  ./target/test/test_transient_disconnect_recovery 5
  ctest --output-on-failure -R "test_c_api|test_omni_cli_type_codec|test_runtime_stats|test_error_logging"
  date "+[%F %T] END long-run cycle"
done
' > longrun_logs/weekly.log 2>&1 &
```

建议同时观察：

- `build/longrun_logs/weekly.log`
- `docs/performance-report.md` 是否持续被正常更新
- 以下关键词是否持续增长：
  - `sm_connection_lost`
  - `sm_reconnect_begin`
  - `sm_reconnect_success`
  - `rpc_timeout`
  - `data_connection_lost`

一周门禁的建议判定标准：

- 一周内无测试退出非 0
- 无持续增长的超时/断连错误模式
- 性能报告没有出现明显退化或异常波动
- 长测日志中不存在卡死、僵死进程、反复无法恢复的重连循环

### 7.8 长测结果汇总脚本

仓库提供了一个长测日志汇总脚本：

```bash
python3 tools/longrun_summary.py build/longrun_logs/weekly.log
```

默认会输出：

- `START/END long-run cycle` 统计
- 完整 cycle 数量
- soak / disconnect recovery 成功标记数量
- 关键错误关键词频次
- 最后若干行日志摘要

如果需要更长的尾部日志：

```bash
python3 tools/longrun_summary.py build/longrun_logs/weekly.log --tail 100
```

### 7.9 长测自动判定脚本

仓库还提供一个自动判定脚本，用于输出 `PASS / WARNING / FAIL`：

```bash
python3 tools/longrun_verdict.py build/longrun_logs/weekly.log --min-cycles 10
```

判定逻辑会综合考虑：

- 完整 cycle 数量是否达到最低要求
- `START/END` 标记是否匹配
- 是否存在 `FAIL:`、`FAILED`、`Timeout` 等失败信号
- 是否存在 `sm_reconnect_begin` 但缺少 `sm_reconnect_success`
- `rpc_timeout` / `data_connection_lost` 是否相对 cycle 数异常偏高

脚本返回码：

- `0`：`PASS` 或 `WARNING`
- `1`：`FAIL`
- `2`：日志文件不存在

## 7. examples 与工具链验证

### 7.1 examples

examples 不属于 `ctest` 注册测试，但建议在重要改动后做功能验证。

当前建议验证：

- `example_cpp_sensor_server`
- `example_cpp_sensor_client`
- `example_c_sensor_server`
- `example_c_sensor_client`

推荐方式：

```bash
./build/target/bin/service_manager --port 19930 --log-level 3
./build/target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 19930
./build/target/example/example_cpp_sensor_client --sm-host 127.0.0.1 --sm-port 19930
```

如果只是做 smoke test，可以对 client 使用：

```bash
timeout 10s ./build/target/example/example_cpp_sensor_client --sm-host 127.0.0.1 --sm-port 19930
```

### 7.2 omni-cli

建议验证：

```bash
./build/target/bin/omni-cli -h 127.0.0.1 -p 19930 list
./build/target/bin/omni-cli -h 127.0.0.1 -p 19930 info SensorService
./build/target/bin/omni-cli --idl examples/sensor_service.bidl -h 127.0.0.1 -p 19930 call SensorService GetLatestData
```

### 7.3 omni-idlc

建议验证：

```bash
cd examples
../build/target/bin/omni-idlc --lang=cpp --output=/tmp/omni-idlc_out sensor_service.bidl
```

然后检查输出目录中是否生成 `.h / .cpp` 文件。

## 8. 推荐测试顺序

如果需要做一轮较完整的本地验证，推荐顺序是：

1. 单元测试：
   - `test_buffer`
   - `test_message`
   - `test_event_loop`
   - `test_transport`
   - `test_service_registry`
   - `test_heartbeat_death`
   - `test_topic_manager`
   - `test_shm_transport`
   - `test_idl_compiler`
   - `test_omni_cli_type_codec`
   - `test_c_api`

2. 集成测试：
   - `test_integration`
   - `test_full_integration`
   - `test_control_plane_and_fallback`
   - `test_threadsafe_client_and_reconnect`
   - `test_runtime_stats`
   - `test_error_logging`

 3. 性能与稳定性：
    - `test_performance`
   - `test_soak_runner`
   - `test_transient_disconnect_recovery`

4. examples / 工具链功能验证：
   - C / C++ examples
   - `omni-cli`
   - `omni-idlc`

## 9. 推荐命令集

### 9.1 关键回归

```bash
ctest --test-dir build --output-on-failure -R "test_transport|test_topic_manager|test_shm_transport|test_integration|test_full_integration|test_control_plane_and_fallback|test_threadsafe_client_and_reconnect|test_runtime_stats|test_error_logging"
```

### 9.2 性能测试

```bash
ctest --test-dir build --output-on-failure -R test_performance
```

### 9.3 稳定性与断连恢复

```bash
ctest --test-dir build --output-on-failure -R "test_soak_runner|test_transient_disconnect_recovery"
```

### 9.4 全量测试

```bash
ctest --test-dir build --output-on-failure
```

## 10. 结论

当前最稳妥的测试运行方式是：

- **优先使用 `ctest --test-dir build`**
- 如果手工运行集成/性能测试，**先进入 `build/` 再执行 `./target/test/...`**

这样能最大程度避免：

- 跑到旧二进制
- 工作目录不一致
- 找不到 `service_manager`
- 报告输出路径异常
