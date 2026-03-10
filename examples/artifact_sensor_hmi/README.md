# Sensor ↔ HMI 下游集成示例

本文档不是简单的“怎么运行 demo”，而是把 `examples/artifact_sensor_hmi/` 当作一个**下游业务工程模板**来说明：

- 如何在你自己的项目中依赖 OmniBinder 的构建产物
- 如何编写 `.bidl` 接口定义
- 如何生成 C / C++ Stub / Proxy 代码
- 如何实现服务端（sensor）
- 如何实现客户端（hmi）
- 如何做 RPC 调用
- 如何做发布 / 订阅
- 如何做死亡通知
- 线程模型和回调线程语义应该怎样理解

如果你照着这份 README 走，应该可以很快把这个示例改造成你自己的业务项目。若需要更底层的规范和 API 细节，本文也给出对应文档链接。

---

## 1. 这个示例解决什么问题

这组示例演示的是一种**下游消费式构建方式**。

它和仓库里原有的 `examples/example_cpp` / `examples/example_c` 不同：

- 原有示例是仓库内部构建（in-tree），直接依赖仓库里的 CMake target
- 这个示例是**下游工程模式**，直接依赖当前项目构建出的安装产物：
  - OmniBinder 导出的库包（`find_package(OmniBinder REQUIRED)`）
  - 已构建 / 已安装的 `omni-idlc` 编译器

因此，这个目录更适合作为你自己业务工程的参考模板。

---

## 2. 示例场景

这里模拟一个典型的工业/嵌入式场景：

- `sensor_*`：传感器服务进程，作为服务提供者
- `hmi_*`：HMI / 上位机界面进程，作为服务消费者

业务关系如下：

1. HMI 通过 RPC 调用 sensor 的控制接口
2. sensor 周期性广播实时状态
3. HMI 订阅该广播，实时刷新界面
4. 如果 sensor 进程退出，HMI 收到死亡通知并进入离线等待状态
5. sensor 恢复后，HMI 继续保活并依赖库内部恢复路径恢复调用链路

本示例同时提供两套版本：

- C++ 版本：`sensor_cpp` / `hmi_cpp`
- C 版本：`sensor_c` / `hmi_c`

---

## 3. 目录结构说明

```text
examples/artifact_sensor_hmi/
├── CMakeLists.txt
├── README.md
├── idl/
│   ├── sensor_hmi_common.bidl
│   └── sensor_hmi_service.bidl
├── sensor_cpp/
│   └── sensor_service.cpp
├── hmi_cpp/
│   └── hmi_client.cpp
├── sensor_c/
│   └── sensor_service.c
├── hmi_c/
│   └── hmi_client.c
└── scripts/
    ├── build_downstream_example.sh
    ├── run_cpp_stack.sh
    └── run_c_stack.sh
```

各部分职责：

- `idl/`：定义业务接口与数据结构
- `sensor_cpp/`、`sensor_c/`：服务端实现
- `hmi_cpp/`、`hmi_c/`：客户端实现
- `CMakeLists.txt`：下游工程如何引用 OmniBinder
- `scripts/`：一键构建 / 一键拉起示例栈

---

## 4. 在你自己的项目里，推荐怎么组织

如果你要参考这个示例做业务工程，推荐组织方式如下：

```text
my_app/
├── CMakeLists.txt
├── idl/
│   ├── common_types.bidl
│   └── my_service.bidl
├── service/
│   └── my_service_impl.cpp
├── client/
│   └── my_client.cpp
└── tools/
```

建议你把工程分成三层：

1. **IDL 层**：定义对外接口与数据模型
2. **服务端层**：实现 Stub 生成的服务接口
3. **客户端层**：通过 Proxy 发起 RPC / 订阅广播 / 监听死亡通知

这样做的好处是：

- 接口定义和实现解耦
- C / C++ 可以共享同一套 `.bidl`
- 后续新增客户端（如测试工具、诊断工具、第二个业务模块）时复用成本低

---

## 5. 构建前提

先在仓库根目录构建并安装 OmniBinder：

```bash
cmake -S . -B build
cmake --build build -j4
cmake --install build
```

确保安装后的 `omni-idlc` 可被找到：

```bash
export PATH="$(pwd)/build/install/bin:$PATH"
```

这一步很重要，因为下游工程中的 `omnic_generate()` 会通过 `PATH` 查找 `omni-idlc`。

---

## 6. 这个下游示例如何构建

### 6.1 一键构建

```bash
./examples/artifact_sensor_hmi/scripts/build_downstream_example.sh
```

该脚本会自动完成：

1. 重新构建 OmniBinder 主工程
2. 安装到 `build/install`
3. 配置下游示例工程 `build/example_sensor_hmi`
4. 编译生成：
   - `sensor_cpp`
   - `hmi_cpp`
   - `sensor_c`
   - `hmi_c`

### 6.2 手工构建（理解原理更推荐看这个）

```bash
cmake -S examples/artifact_sensor_hmi -B build/example_sensor_hmi \
  -DOmniBinder_DIR="$(pwd)/build/install/lib/cmake/OmniBinder"

cmake --build build/example_sensor_hmi -j4
```

这里的关键点：

- `OmniBinder_DIR` 指向当前项目安装后的 CMake package 目录
- 下游工程通过 `find_package(OmniBinder REQUIRED)` 获取导出的库 target
- 通过 `omnic_generate()` 调用 `omni-idlc` 生成代码

---

## 7. 下游工程 CMake 应该怎么写

本示例的核心 CMake 写法在：

- `examples/artifact_sensor_hmi/CMakeLists.txt`

关键思路如下：

```cmake
find_package(OmniBinder REQUIRED)

add_executable(sensor_cpp sensor_cpp/sensor_service.cpp)
target_link_libraries(sensor_cpp PRIVATE OmniBinder::omnibinder_static)

omnic_generate(
    TARGET sensor_cpp
    LANGUAGE cpp
    FILES ${BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp
)
```

如果你做的是自己的项目，通常只需要替换：

- 目标名（`sensor_cpp` / `hmi_cpp`）
- 你的源文件路径
- 你的 `.bidl` 文件列表

更详细的 CMake / IDL 生成说明见：

- [IDL 规范](../../docs/idl-specification.md)
- [使用示例文档](../../docs/examples.md)

---

## 8. 如何编写 IDL

本示例使用两份 IDL：

- `idl/sensor_hmi_common.bidl`
- `idl/sensor_hmi_service.bidl`

### 8.1 公共类型示例

`sensor_hmi_common.bidl`：

```bidl
package hmi_common;

struct OperationResult {
    int32 code;
    string message;
}
```

这类文件适合放：

- 通用返回值
- 通用错误模型
- 公共结构体

### 8.2 服务 IDL 示例

`sensor_hmi_service.bidl`：

```bidl
package sensor_hmi;

import "sensor_hmi_common.bidl";

struct SensorSnapshot {
    int32 sensor_id;
    bool enabled;
    int32 sampling_interval_ms;
    float64 current_value;
    string mode;
    int64 timestamp_ms;
}

struct SensorControlRequest {
    bool enabled;
    int32 sampling_interval_ms;
    string mode;
}

topic SensorStatusTopic {
    SensorSnapshot snapshot;
    int64 publish_time_ms;
}

service SensorControlService {
    SensorSnapshot GetSnapshot();
    hmi_common.OperationResult ApplyControl(SensorControlRequest request);
    void TriggerCalibration(int32 sensor_id);

    publishes SensorStatusTopic;
}
```

这个 IDL 覆盖了最常见的三种能力：

1. **有返回值的 RPC**：`GetSnapshot()`
2. **带参数的 RPC**：`ApplyControl(...)`
3. **one-way/无返回调用**：`TriggerCalibration(...)`
4. **发布订阅**：`publishes SensorStatusTopic`
5. **延迟测量 RPC**：`MeasureRpcLatency(...)`

此外，这个示例专门加入了时间戳字段，便于在运行时直接观察延迟（单位统一按 **us** 打印）：

- `SensorSnapshot.timestamp_us`：sensor 生成这份状态快照的时间
- `SensorStatusTopic.publish_time_us`：广播真正发出的时间
- `SensorControlRequest.request_time_us`：HMI 发起控制请求的时间
- `RpcProbeResult`：用于测量 RPC 从发起到响应返回的总耗时，以及服务端收到请求后的处理时刻

### 8.3 设计 IDL 的实用建议

建议你的业务 IDL 按下面思路设计：

- **查询类接口**：返回完整状态快照，例如 `GetStatus()`、`GetConfig()`
- **控制类接口**：参数收敛成 request struct，例如 `ApplyControl(ControlRequest req)`
- **广播类数据**：单独定义 topic，例如 `StatusUpdate`、`AlarmEvent`
- **公共返回值**：统一使用 `OperationResult` / `StatusResponse` 这类结构

这样做比大量零散参数更稳定，也更方便以后扩展字段。

IDL 语法、类型系统、`import`、`topic`、`publishes` 的详细规则见：

- [IDL 语法规范](../../docs/idl-specification.md)

---

## 9. 代码生成后会得到什么

通过 `omni-idlc` 生成后，会得到两套接口：

### 9.1 C++ 侧

会生成类似：

- `sensor_hmi_service.bidl.h`
- `sensor_hmi_service.bidl.cpp`

其中包含：

- `sensor_hmi::SensorControlServiceStub`
- `sensor_hmi::SensorControlServiceProxy`
- `sensor_hmi::SensorStatusTopic`
- `sensor_hmi::SensorSnapshot`
- `sensor_hmi::SensorControlRequest`

### 9.2 C 侧

会生成类似：

- `sensor_hmi_service.bidl_c.h`
- `sensor_hmi_service.bidl.c`

其中包含：

- `sensor_hmi_SensorControlService_callbacks`
- `sensor_hmi_SensorControlService_stub_create(...)`
- `sensor_hmi_SensorControlService_proxy_*`
- `sensor_hmi_SensorControlService_broadcast_sensor_status_topic(...)`

如果你不确定生成代码的 API 形状，可以直接看：

- `build/example_sensor_hmi/generated/cpp/`
- `build/example_sensor_hmi/generated/c/`

---

## 10. 服务端怎么写（sensor）

### 10.1 C++ 服务端思路

参考文件：

- `sensor_cpp/sensor_service.cpp`

核心步骤：

1. 创建 `OmniRuntime`
2. `init()` 连接 `ServiceManager`
3. 继承生成的 `SensorControlServiceStub`
4. 实现 IDL 中定义的方法
5. `registerService()` 注册服务
6. `publishTopic()` 声明自己会发布哪个 topic
7. 在主循环中 `pollOnce()` + 广播数据

典型骨架：

```cpp
class MyService : public sensor_hmi::SensorControlServiceStub {
public:
    sensor_hmi::SensorSnapshot GetSnapshot() override { ... }
    hmi_common::OperationResult ApplyControl(const sensor_hmi::SensorControlRequest& req) override { ... }
    void TriggerCalibration(int32_t sensor_id) override { ... }
};

omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

MyService service;
runtime.registerService(&service);
runtime.publishTopic("SensorStatusTopic");

while (running) {
    runtime.pollOnce(100);
    service.BroadcastSensorStatusTopic(topic_msg);
}
```

### 10.2 C 服务端思路

参考文件：

- `sensor_c/sensor_service.c`

核心区别在于：

- C 没有类继承
- 通过回调表 `sensor_hmi_SensorControlService_callbacks` 提供服务实现
- 通过 `sensor_hmi_SensorControlService_stub_create()` 创建服务对象

典型骨架：

```c
sensor_hmi_SensorControlService_callbacks cbs;
memset(&cbs, 0, sizeof(cbs));
cbs.GetSnapshot = on_get_snapshot;
cbs.ApplyControl = on_apply_control;
cbs.TriggerCalibration = on_trigger_calibration;

omni_service_t* service = sensor_hmi_SensorControlService_stub_create(&cbs);
omni_runtime_register_service(runtime, service);
omni_runtime_publish_topic(runtime, "SensorStatusTopic");
```

### 10.3 服务端设计建议

建议服务端把逻辑分成两层：

- **IDL 接口层**：只负责参数解包、结果封装
- **业务状态层**：维护真正的业务状态机 / 硬件状态 / 配置状态

这样可以避免把大量业务逻辑直接堆进回调函数里。

---

## 11. 客户端怎么写（hmi）

### 11.1 C++ 客户端思路

参考文件：

- `hmi_cpp/hmi_client.cpp`

核心步骤：

1. 创建 `OmniRuntime`
2. 连接 `ServiceManager`
3. 创建生成的 `Proxy`
4. `connect()` 连接服务
5. 发起 RPC
6. 订阅 topic
7. 注册死亡通知
8. 在事件循环中处理回调

典型骨架：

```cpp
omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

sensor_hmi::SensorControlServiceProxy proxy(runtime);
proxy.connect();

auto snapshot = proxy.GetSnapshot();

sensor_hmi::SensorControlRequest req;
req.enabled = true;
req.sampling_interval_ms = 250;
req.mode = "boost";
proxy.ApplyControl(req);

proxy.SubscribeSensorStatusTopic([](const sensor_hmi::SensorStatusTopic& msg) {
    // 收到广播
});

proxy.OnServiceDied([]() {
    // 服务死亡
});

while (running) {
    runtime.pollOnce(100);
}
```

### 11.2 C 客户端思路

参考文件：

- `hmi_c/hmi_client.c`

典型骨架：

```c
sensor_hmi_SensorControlService_proxy proxy;
sensor_hmi_SensorControlService_proxy_init(&proxy, runtime);
sensor_hmi_SensorControlService_proxy_connect(&proxy);

sensor_hmi_SensorSnapshot snapshot;
sensor_hmi_SensorControlService_proxy_get_snapshot(&proxy, &snapshot);

sensor_hmi_SensorControlService_proxy_subscribe_sensor_status_topic(
    &proxy, on_sensor_status_topic, NULL);

sensor_hmi_SensorControlService_proxy_on_service_died(
    &proxy, on_service_died, NULL);
```

---

## 12. 如何做 RPC 调用

### 12.1 查询类 RPC

示例：

- `GetSnapshot()`

适合读取一份当前状态快照。

建议：

- 如果 UI / HMI 启动后需要先获取完整状态，再开始订阅广播，这类 RPC 很有用
- 广播适合持续更新，RPC 适合初始化时拉一份完整状态

### 12.2 控制类 RPC

示例：

- `ApplyControl(SensorControlRequest request)`

建议把控制参数收敛为一个 request struct，而不是拆成大量独立参数。这样新增字段时不容易破坏接口兼容性。

### 12.3 one-way 调用

示例：

- `TriggerCalibration(int32 sensor_id)`

适合：

- 触发动作
- 不关心返回值
- “发出去就行”的指令

### 12.4 延迟测量 RPC

示例：

- `MeasureRpcLatency(int64 request_time_ms)`

这个接口专门用于示例里的观测逻辑：

- HMI 每 1000ms 发起一次请求
- 请求参数里带上发送时间戳
- 服务端返回 `RpcProbeResult`
- HMI 在收到响应时计算：
  - **rpc_roundtrip**：从调用发起到收到返回的总耗时
  - **server_queue**：服务端真正处理该请求时，相比请求发起时间过去了多久

这能帮助你在业务 demo 中快速感知链路延迟，而不需要自己额外抓包。

更详细的公共 API 与同步/阻塞语义见：

- [API 参考](../../docs/api-reference.md)

---

## 13. 如何做发布 / 订阅

本示例中：

- 服务端发布：`SensorStatusTopic`
- 客户端订阅：`SubscribeSensorStatusTopic(...)`

### 服务端侧

1. `runtime.publishTopic("SensorStatusTopic")`
2. 组装 topic 结构体
3. 调用生成的广播辅助函数发布

C++：

```cpp
sensor_hmi::SensorStatusTopic topic;
topic.snapshot = ...;
topic.publish_time_ms = ...;
service.BroadcastSensorStatusTopic(topic);
```

C：

```c
sensor_hmi_SensorStatusTopic topic;
sensor_hmi_SensorStatusTopic_init(&topic);
// 填充字段
sensor_hmi_SensorControlService_broadcast_sensor_status_topic(runtime, &topic);
sensor_hmi_SensorStatusTopic_destroy(&topic);
```

### 客户端侧

C++：

```cpp
proxy.SubscribeSensorStatusTopic([](const sensor_hmi::SensorStatusTopic& msg) {
    // 更新界面 / 更新状态缓存
});
```

C：

```c
sensor_hmi_SensorControlService_proxy_subscribe_sensor_status_topic(
    &proxy, on_sensor_status_topic, NULL);
```

### 使用建议

- **广播内容尽量是“可直接展示/消费”的状态结构体**
- 如果状态字段很多，优先广播一个完整快照，而不是分裂成很多小 topic
- 如果消息频率很高，回调里只做轻量工作，复杂逻辑丢到其他线程

本示例中，广播延迟的计算方式是：

- 发布方写入 `publish_time_ms`
- 接收方收到 topic 后，用 `当前时间 - publish_time_ms`
- 控制台打印 `pubsub_latency=xxx ms`

更详细的 topic 语法和生成规则见：

- [IDL 规范：topic / publishes](../../docs/idl-specification.md)

---

## 14. 如何做死亡通知

死亡通知用于感知“服务是否还活着”。

本示例中，HMI 会注册：

- C++：`proxy.OnServiceDied(...)`
- C：`sensor_hmi_SensorControlService_proxy_on_service_died(...)`

典型用途：

- UI 上把设备状态切成“离线”
- 停止继续发送控制命令
- 提示用户服务异常退出
- 触发重连 / 重试机制

### C++ 示例

```cpp
proxy.OnServiceDied([]() {
    std::printf("service died\n");
});
```

### C 示例

```c
static void on_service_died(void* user_data) {
    (void)user_data;
    printf("service died\n");
}
```

### 使用建议

- 不要把死亡通知理解成“系统自动帮你重连完成”
- 它更像一个**状态变化信号**：告诉你对端已经不可用
- 收到通知后，你的业务代码需要自己决定：
  - 进入离线态
  - 等待服务重启
  - 是否重试 connect / 重建订阅

本示例里的 HMI 采用的是**保活 + 继续探测**策略，而不是自己重写一套应用层重连状态机：

1. 收到 death notification 后，不退出
2. 切换到离线等待状态
3. 继续每 1000ms 做一次 RPC probe
4. 由库内部恢复路径处理后续控制面 / 数据面恢复
5. 一旦 probe 再次拿到有效结果，就打印“service recovered”

---

## 15. 线程模型和回调线程语义

这一点对业务工程很重要。

当前发布态的对外语义是：

- `OmniRuntime` 对外提供线程安全公共 API
- 内部运行时由单个 owner event-loop 串行驱动
- 回调（如 topic / death callback）默认在 owner event-loop 线程执行

你可以把它理解成：

- 多个线程可以安全调用同一个 `OmniRuntime`
- 但内部事件处理仍然是“一个 owner loop 串行推进”
- 回调不是任意线程乱飞，而是默认在 owner event-loop 所在线程执行

### 实际使用建议

#### 方式一：一个专用线程 `run()`

适合复杂业务系统：

```cpp
std::thread loop_thread([&runtime]() {
    runtime.run();
});
```

其他线程再并发发起 RPC。

#### 方式二：主循环定期 `pollOnce()`

适合简单进程或已有主循环的系统：

```cpp
while (running) {
    runtime.pollOnce(50);
    // 其他逻辑
}
```

### 非常重要的约束

- 不要让多个线程同时对同一个 runtime 并发 `run()` / `pollOnce()`
- 不建议在 `TopicCallback` / `DeathCallback` 里再对同一个 runtime 发同步阻塞 RPC

更详细的并发规则见：

- [线程模型文档](../../docs/threading-model.md)
- [API 参考中的线程模型摘要](../../docs/api-reference.md)

---

## 16. 如何把本示例改造成你自己的项目

最实用的改法是按下面顺序来：

### 第一步：复制目录骨架

把 `examples/artifact_sensor_hmi/` 复制成你自己的业务工程目录，比如：

```text
examples/my_gateway_project/
```

### 第二步：先改 IDL

优先修改：

- `idl/sensor_hmi_common.bidl`
- `idl/sensor_hmi_service.bidl`

把里面的：

- 结构体名称
- 服务名称
- topic 名称
- 方法名称

替换成你的业务定义。

### 第三步：再改服务端 / 客户端实现

当 IDL 一变，生成代码里的 Stub / Proxy 名称也会变。

然后去修改：

- `sensor_cpp/sensor_service.cpp`
- `hmi_cpp/hmi_client.cpp`
- `sensor_c/sensor_service.c`
- `hmi_c/hmi_client.c`

把调用的类名 / 函数名替换成新的生成 API。

### 第四步：最后改脚本和文档

只要你的 target 名称变了，再同步改：

- `CMakeLists.txt`
- `scripts/*.sh`
- `README.md`

### 推荐演进方式

不要一上来就设计很大的 IDL。建议按这个顺序增量扩展：

1. 先做一个 `GetStatus()`
2. 再做一个 `ApplyControl(request)`
3. 再加一个 `topic StatusUpdate`
4. 最后再补更多方法和复杂结构

这样最容易验证链路是否正确。

---

## 17. 运行方式

### 17.1 手工启动 C++ 版本

终端 1：

```bash
./build/target/bin/service_manager
```

终端 2：

```bash
./build/example_sensor_hmi/bin/sensor_cpp
```

终端 3：

```bash
./build/example_sensor_hmi/bin/hmi_cpp
```

### 17.2 手工启动 C 版本

终端 1：

```bash
./build/target/bin/service_manager
```

终端 2：

```bash
./build/example_sensor_hmi/bin/sensor_c
```

终端 3：

```bash
./build/example_sensor_hmi/bin/hmi_c
```

### 17.3 一键启动

```bash
./examples/artifact_sensor_hmi/scripts/run_cpp_stack.sh
./examples/artifact_sensor_hmi/scripts/run_c_stack.sh
```

脚本会自动：

- 构建主工程
- 安装 OmniBinder
- 构建下游示例
- 启动 `service_manager + sensor + hmi`

> 注意：一键启动脚本默认会尝试启动自己的 `service_manager`，因此要求默认端口 `9900` 当前**没有被其他进程占用**。
> 如果你已经手工启动过一个 `service_manager`，或者已有其他示例正在运行，请先停止它，否则会出现：
> `Failed to bind on 0.0.0.0:9900`

默认情况下，如果你是在交互式终端里执行脚本，它会**自动进入 tmux 会话**，所以你可以直接看到三个进程的运行输出。

如果你只想后台启动，不想立即 attach，可以使用：

```bash
./examples/artifact_sensor_hmi/scripts/run_cpp_stack.sh --detach
./examples/artifact_sensor_hmi/scripts/run_c_stack.sh --detach
```

后台启动后可手动进入：

```bash
tmux attach -t omnibinder-artifact-cpp
tmux attach -t omnibinder-artifact-c
```

---

## 18. 如何确认示例工作正常

正常情况下，你会看到以下现象：

### C++ 版本

- `hmi_cpp` 启动后能先拉到一份初始 snapshot
- `ApplyControl` 调用成功
- `TriggerCalibration` 调用成功
- 每 1000ms 输出一次 RPC 延迟：`rpc_roundtrip=... ms`
- 持续收到 broadcast，并打印 `pubsub_latency=... ms`
- 停掉 `sensor_cpp` 后，`hmi_cpp` 输出 death notification，并保持存活
- 重新启动 `sensor_cpp` 后，`hmi_cpp` 会在后续 probe 成功时打印恢复信息

### C 版本

- `hmi_c` 启动后能收到广播
- 每 1000ms 输出一次 RPC 延迟：`rpc_roundtrip=... ms`
- 广播输出里带 `pubsub_latency=... ms`
- 停掉 `sensor_c` 后，`hmi_c` 输出 death notification，并保持存活
- 重新启动 `sensor_c` 后，`hmi_c` 会在后续 probe 成功时打印恢复信息

---

## 19. 推荐继续阅读的文档

如果你要真正基于这个示例写自己的项目，推荐继续看：

- [IDL 语法规范](../../docs/idl-specification.md)
  - 看什么：`package`、`import`、`struct`、`topic`、`publishes`、类型映射

- [API 参考](../../docs/api-reference.md)
  - 看什么：`OmniRuntime`、`registerService()`、`invoke()`、`publishTopic()`、`subscribeTopic()`、`subscribeServiceDeath()`

- [线程模型文档](../../docs/threading-model.md)
  - 看什么：owner event-loop、回调线程语义、`run()` / `pollOnce()` / `stop()` 的并发协作规则

- [使用示例文档](../../docs/examples.md)
  - 看什么：C / C++ 生成代码使用方式、示例风格、下游工程写法

- [架构文档](../../docs/architecture.md)
  - 看什么：整体通信模型、控制面 / 数据面、SHM / TCP 的角色分工

---

## 20. 一句话总结

如果你只记住一件事，那就是：

> 先用 `.bidl` 把服务接口、控制请求、广播消息定义清楚，再让下游工程通过 `find_package(OmniBinder)` + `omnic_generate()` 生成 Stub / Proxy，然后分别实现服务端和客户端。

这就是把 OmniBinder 真正落到业务工程中的最推荐路径。
