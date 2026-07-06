# OmniBinder

中文 | [English](README.en.md)

**面向嵌入式 Linux 与分布式服务场景的服务通信中间件**

OmniBinder 是一个面向嵌入式 Linux、分布式多板系统和多服务协作场景的服务通信中间件。
它的目标不是只解决“进程怎么传数据”，而是解决**多个服务之间如何以统一方式完成注册、发现、调用、广播和状态感知**。
通过提供统一的服务通信与管理入口，以及自动选择的 SHM/TCP 数据通道，OmniBinder 让跨进程、跨板、跨设备的服务协作都能以一致模型完成集成。

一句话概括：**OmniBinder 是在分布式服务之间充当通信桥梁的中间件，让服务与服务之间的连接、调用和数据分发标准化。**

---

## 项目背景

在嵌入式、工业和边缘计算场景中，一个完整系统通常不是单体程序，而是由多个服务共同协作：
传感器采集、算法处理、设备控制、网关转发、HMI 展示、日志与监控等模块，往往运行在不同进程、
不同板卡，甚至不同设备上。

真正困难的地方，往往不是“某一次 socket 发送”，而是**如何把这些分散的服务稳定地连接起来，形成可发现、可调用、可广播、可感知状态的通信桥梁**：

- 服务启动后如何被其他模块发现
- 服务之间如何统一调用，而不是每个模块各写一套通信协议
- 状态和事件如何广播给多个消费者
- 某个服务异常退出后，其它模块如何感知并恢复
- 同机和跨机部署变化时，业务层如何不改接口

## 与常见方案的定位区别

OmniBinder 并不是要替代所有已有通信框架，而是面向一类更具体的工程问题：
**当系统由多个服务组成，并且这些服务可能分布在同机、跨板、跨设备环境中时，如何用一套统一模型完成服务管理与服务间通信。**

从这个角度看，常见方案可以这样理解：

| 方案类别 | 代表方案 | 更适合的场景 | 与 OmniBinder 的区别 |
|---|---|---|---|
| 本地 IPC / 服务总线 | Android Binder, D-Bus | 同机服务化通信、系统服务编排、本地进程协作 | 更偏本地系统内通信；若要继续覆盖跨板、跨设备场景，通常还需要额外的远程通信机制。与此同时，业务侧往往还要自行统一接口约定、封装服务端/客户端接入方式 |
| RPC 框架 | gRPC, Thrift, Cap'n Proto RPC | 标准化接口定义、跨语言服务调用、典型分布式系统 | 这类方案通常也具备成熟的接口描述与代码生成能力；OmniBinder 的区别不在于“有没有 codegen”，而在于把 **IDL、Stub/Proxy 生成、同机 IPC 与跨设备通信模型** 收敛到一套更统一的运行时中 |
| 消息中间件 / Broker | MQTT, RabbitMQ, NATS, Kafka | 事件分发、异步解耦、消息路由、状态上报 | 更偏异步消息模型；若系统还需要服务发现、同步 RPC 和服务生命周期感知，通常要与其它机制配合。业务接口、请求响应语义和类型封装也往往需要额外约定 |
| 消息通信库 | ZeroMQ 等 | 灵活的点对点、发布订阅、pipeline 通信拓扑 | 更偏底层通信抽象；若要形成完整服务管理模型，通常还需补充注册发现、接口约定、Stub/Proxy 封装和运行时管理 |
| 领域型分布式总线 | ROS 2, DDS, SOME-IP | 机器人、车载、实时控制、强数据分发导向系统 | 往往伴随更明确的领域假设、QoS 模型或生态约束 |
| 服务网格 / 云原生基础设施 | Istio, Linkerd, Envoy | 云原生微服务中的流量治理、观测、安全控制 | 与 OmniBinder 有交集，但并不直接替代进程内外统一通信运行时 |
| 自定义协议 | Socket + 自定义协议 | 完全定制的通信链路、极强可控性 | 灵活度最高，但当系统演化为多服务协作时，服务发现、接口管理、广播机制和状态感知通常仍需额外建设 |

OmniBinder 更适合的场景是：

- 你希望把**同机进程通信**和**跨设备服务调用**统一到一套接口模型中
- 你需要的不只是数据传输，还包括**服务注册发现、广播分发、死亡通知、运行时调试**等能力
- 你希望通过 **统一 IDL、自动生成 Stub/Proxy 代码和一致的服务接入方式** 来减少重复封装与手写协议适配
- 你的部署环境偏 Linux / 嵌入式，希望运行时足够轻量，并能同时支持 **C / C++** 接入
- 你希望业务代码关注“调用哪个服务”，而不是持续处理底层 transport、连接关系和接口编解码细节

OmniBinder 的设计目标是：**作为多个服务之间的统一通信中间件，为分布式服务系统提供服务注册发现、服务调用、事件广播和状态感知能力；同时保持零外部依赖、C++11 编译、同机 SHM 高速通信与跨板 TCP 网络通信兼容，并提供 C / C++ 双接口。**

换句话说，OmniBinder 解决的是**服务与服务之间怎么高效、统一、可管理地通信**，而不仅仅是“底层报文怎么收发”。

---

## 核心特性

OmniBinder 同时覆盖了**服务管理**和**服务间数据通路**两部分能力：

- **服务注册与发现** — 中心化 ServiceManager 管理所有服务的生命周期
- **远程方法调用 (RPC)** — 同步调用远端服务接口，支持请求/响应模式和单向调用
- **发布/订阅广播** — 话题机制，发布者广播数据，订阅者实时接收
- **命令** — 服务异常退出时自动通知所有关注者
- **主动连接管理** — Proxy 提供 `connect()`/`disconnect()` 主动建立/断开服务连接
- **自动重连** — 服务恢复后自动重连（可配指数退避策略）
- **心跳检测** — 定期心跳及时发现服务异常，超时自动触发重连
- **传输层自动选择** — 同机通信自动使用共享内存 (SHM)，跨机通信走 TCP
- **服务级 SHM 配置** — 默认使用小容量 SHM ring（当前 `4KB / 4KB`），特殊服务可按需放大 request / response 容量
- **IDL 代码生成** — 通过 `.bidl` 接口定义文件自动生成 Stub/Proxy 代码
- **零外部依赖** — 仅依赖 POSIX API 和标准 C++11，无需第三方库
- **命令行工具** — `omni-cli` 支持运行时查询服务、调用接口，支持 JSON 格式输入输出

### 线程模型说明

- `OmniRuntime` 对外提供线程安全公共 API
- 内部运行时由单个 owner event-loop 串行驱动
- 回调（如 topic / death callback）默认在 owner event-loop 线程执行
- 同步 reply wait 期间只处理 fd/timer，不执行新的 pending API functor
- `run()` / `pollOnce()` / `stop()` 的并发协作规则见 [线程模型文档](docs/threading-model.md)

### 错误处理模型说明

- Buffer 写接口返回 `bool`
- Buffer 读接口采用 `tryReadXxx(out)` 返回 `bool`
- 服务端 `onInvoke()` 返回 `int` 错误码
- 参数反序列化失败通常返回 `ERR_DESERIALIZE`
- 响应序列化失败通常返回 `ERR_SERIALIZE`
- 运行时主路径不再依赖 C++ 异常传播普通协议错误

---

## 性能表现

以下数据用于展示 OmniBinder 在**同机 SHM 场景**下的典型性能表现。

**测试环境摘要（Windows WSL2 Ubuntu20.04）：**

- 传输方式：TCP + SHM 自动选择（同机通信使用 SHM）
- RPC 预热轮数：50；RPC 测试轮数：1000 / 用例
- 话题模式：服务端每 500us 轮转发布 topic，客户端 `pollOnce(0)` busy-spin 收包
- 测试服务 SHM ring：64KB / 64KB
- Topic 预热轮数：100；Topic 测试轮数：1000 / 用例

| 测试项 | 样本数 | 平均值 | 95% 情况 | 99% 情况 | 说明 |
|--------|--------|--------|-----------|-----------|------|
| RPC EchoBytes (0 bytes) | 1000 | 8.5 us | 10 us | 34 us | 空 payload，主要反映协议与调度开销 |
| RPC EchoBytes (64 bytes) | 1000 | 13.1 us | 39 us | 54 us | 常见小 payload RPC |
| RPC EchoBytes (256 bytes) | 1000 | 9.1 us | 13 us | 44 us | 常见小 payload RPC |
| RPC EchoBytes (1024 bytes) | 1000 | 11.4 us | 33 us | 56 us | 1KB 级 payload |
| RPC EchoBytes (4096 bytes) | 1000 | 36.5 us | 52 us | 65 us | 4KB payload，测试服务显式放大 SHM ring |
| RPC EchoBytes (8192 bytes) | 1000 | 50.7 us | 68 us | 85 us | 8KB payload，测试服务显式放大 SHM ring |
| RPC EchoInt32 | 1000 | 8.1 us | 9 us | 36 us | 小基础类型 RPC |
| RPC EchoStruct | 1000 | 18.0 us | 36 us | 51 us | 结构体 RPC |
| RPC Add (2 x int32) | 1000 | 9.7 us | 24 us | 44 us | 小计算型 RPC |
| Topic pub/sub (0 bytes) | 1000 | 4.1 us | 9 us | 15 us | 空广播 payload |
| Topic pub/sub (64 bytes) | 1000 | 5.0 us | 11 us | 20 us | 小广播数据，发布 → 订阅者回调 |
| Topic pub/sub (256 bytes) | 1000 | 4.5 us | 10 us | 19 us | 常见小广播数据 |
| Topic pub/sub (1024 bytes) | 1000 | 5.6 us | 11 us | 18 us | 1KB 广播数据 |
| Topic pub/sub (4096 bytes) | 1000 | 10.2 us | 17 us | 23 us | 4KB 广播数据 |
| Topic pub/sub (8192 bytes) | 1000 | 17.0 us | 28 us | 43 us | 8KB 广播数据 |

从报告中的完整数据看：

- **0~1024 bytes 常见 RPC** 在当前机器上的平均值约为 **8.5~13.1 us**
- **4096~8192 bytes payload RPC** 在显式放大 SHM ring 的测试配置下，平均值约为 **36.5~50.7 us**
- **Topic pub/sub** 在当前机器上的平均值约为 **4.1~17.0 us**

> **性能说明**：当前 SHM 路径已使用 `eventfd + EventLoop` 的事件驱动模型。
> 报告中的延迟数据主要反映序列化、共享内存拷贝、eventfd 唤醒、epoll 调度与业务处理开销。
> 其中当前性能报告基于测试服务显式配置的 `64KB / 64KB` SHM ring，并不代表默认小容量 SHM ring 配置下的行为。

详细数据和分析见 [性能测试报告](docs/performance-report.md)。

---

## 适用场景

| 场景 | 说明 |
|------|------|
| **单板多进程服务系统** | 同一台 Linux 设备上的多个服务进程互相调用，形成统一的本地服务总线（自动走 SHM，低延迟） |
| **局域网分布式服务系统** | 多台设备通过以太网组成分布式系统，服务跨板调用但仍保持统一接口与管理方式（走 TCP） |
| **嵌入式网关 / 边缘节点** | 网关设备汇聚多个子板或外设服务，对内做服务桥接，对外统一暴露能力 |
| **机器人 / 自动驾驶** | 传感器、感知、规划、控制等模块分布在不同计算单元上，需要稳定的服务调用与事件分发 |
| **工业控制** | PLC、HMI、数据采集、控制策略等多个业务服务之间的实时通信与状态同步 |

如果你的系统本质上是**多个服务协同完成一件事**，而你希望它们之间有一层统一的通信桥梁，OmniBinder 就是为此设计的。

---

## 快速开始

### 编译

#### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### Windows (MinGW / MSVC)

```bash
# MinGW (推荐)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

# MSVC
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j8
```

要求：
- Windows: MinGW 7.3+ (GCC) 或 MSVC 2017+
- CMake 3.10+
- 零外部依赖

> **注意**：Windows 上 SHM 使用 Named Pipe + TCP loopback 实现，功能等价于 Linux 的 eventfd + AF_UNIX。
> 跨进程 SHM 完全支持，同进程多 EventLoop 场景 SHM 自动回退 TCP。

如果你只是普通主机构建，不需要交叉编译，直接继续安装即可：

```bash
cmake --install build
```

安装后目录为：

```text
install/
├── bin_host/
├── include/
└── lib/
```

如果你要做交叉编译，推荐使用双阶段构建脚本，一步完成主机工具 + 目标板运行时的安装：

```bash
CC=/path/to/target-gcc CXX=/path/to/target-g++ ./scripts/build_dual_stage_install.sh
```

如果工具链需要显式 toolchain 文件或 sysroot，再补：

```bash
CC=/path/to/target-gcc CXX=/path/to/target-g++ \
TOOLCHAIN_FILE=/path/to/toolchain.cmake \
./scripts/build_dual_stage_install.sh
```

> **注意**：host stage 必须在干净的主机环境中执行，不能提前把 `CC/CXX` 指到交叉编译器。
>
> 详细的工具链配置、环境变量说明、安装目录布局和手工构建流程见 [ARM 交叉编译指南](docs/cross-compiling-arm.md)。

构建选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `OMNIBINDER_BUILD_TESTS` | ON | 编译单元测试 |
| `OMNIBINDER_BUILD_EXAMPLES` | ON | 编译示例程序 |
| `OMNIBINDER_BUILD_TOOLS` | ON | 编译工具目标 |
| `OMNIBINDER_BUILD_HOST_IDLC` | Host 默认 ON / 交叉默认 OFF | 是否编译主机版 `omni-idlc` |

### 运行示例

```bash
# 1. 启动 ServiceManager（中心注册服务）
./build/target/bin/service_manager

# 2. 启动服务提供者（SensorService，注册服务 + 广播数据）
./build/target/example/example_cpp_sensor_server

# 3. 启动服务消费者（调用接口 + 订阅广播）
./build/target/example/example_cpp_sensor_client
```

如果你要验证**业务工程如何依赖 OmniBinder 的构建产物**，请使用下游示例：

```bash
cmake --install build

# C++ examples
cmake -S examples/artifact_examples/cpp -B build/example_artifacts_cpp \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_cpp -j$(nproc)

# C examples
cmake -S examples/artifact_examples/c -B build/example_artifacts_c \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_c -j$(nproc)
```

该示例同时提供 C 和 CPP 两套版本，用于演示：服务之间调用控制接口、订阅 sensor 广播、以及在 sensor 退出时接收死亡通知。
更详细的下游构建说明请参考 `examples/artifact_examples/README.md`。

当前内置示例 `examples/sensor_service.bidl` / `examples/example_cpp/*` / `examples/example_c/*` 已经扩展为一组更完整的 demo，覆盖：

- 基础类型 RPC（`EchoBool` ~ `EchoFloat64`、`EchoString`、`EchoBytes`）
- 自定义结构体 / 嵌套结构体 RPC（`EchoStatus`、`EchoConfig`、`EchoEnvelope`）
- 数组 RPC（`EchoIdArray`、`EchoLabelArray`、`EchoSensorArray`、`EchoBundle`）
- 普通服务接口（`GetLatestData`、`SetThreshold`、`ResetSensor`、`GetSensorCount`）
- 话题广播（`SensorUpdate`）
- 异步请求 + 话题回推结果（`RequestLatestDataAsync` + `AsyncResultReady`）
- 服务死亡通知（client 侧）

如果你想看完整示例，请直接参考 [examples/](examples/) 目录和 [示例文档](docs/examples.md)。

### 使用 omni-cli 查询和调试

**基础模式（Hex 格式）：**
```bash
# 列出所有已注册的服务
./build/target/bin/omni-cli list

# 查看某个服务的详细信息（接口、方法列表）
./build/target/bin/omni-cli info SensorService

# 调用远程接口（hex 输入/输出）
./build/target/bin/omni-cli call SensorService GetLatestData
```

**详细模式（JSON 格式）：**
```bash
# 查看服务接口（展开字段定义）
./build/target/bin/omni-cli --idl examples/sensor_service.bidl info SensorService

# 调用接口（JSON 输入/输出）
./build/target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData

# 带参数调用（JSON 格式）
./build/target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"sensor1","value":100}'
```

**输出示例：**
```
Response (status=OK, 38 bytes, 0.81 ms):
  {
    "humidity": 61.95,
    "location": "Room-A",
    "sensor_id": 1,
    "temperature": 23.99,
    "timestamp": 1772645628
  }
```

详细使用说明请参考：[omni-cli 使用指南](docs/omni-tool-usage.md)

> `runtime.init(sm_host, sm_port)` 中的 `sm_host` 表示当前进程连接哪个 `ServiceManager`。
> `runtime.setRegisterHost(...)` 或 `service.setRegisterHost(...)` 表示服务注册到 `ServiceManager`
> 时写入 `ServiceInfo.host` 的地址，也就是其它机器后续直连该服务时使用的地址。
> 在跨机部署场景中，这两个地址通常不同，不应混用。
>
> 当前同步 `invoke()` 在 TCP 路径上会先在 timeout 预算内把完整请求写入 socket，
> 只有请求真正发完后才进入 reply 等待阶段。
> 因此跨设备慢链路下如果请求发送本身耗时较长，会先体现为发送阶段 timeout，
> 而不是“本地排队成功但 reply 提前超时”。

---

## 编程示例

### 定义接口 (IDL)

```bidl
package demo;

struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    int64   timestamp;
    string  location;
}

struct ControlCommand {
    int32   command_type;
    string  target;
    int32   value;
}

struct AsyncRequest {
    int32   request_id;
    string  client_tag;
}

topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic AsyncResultReady {
    AsyncResult result;
    int64       publish_time;
}

service SensorService {
    bool                  EchoBool(bool value);
    int32                 EchoInt32(int32 value);
    string                EchoString(string value);
    SensorData            GetLatestData();
    common.StatusResponse SetThreshold(ControlCommand cmd);
    int32                 GetSensorCount();
    common.StatusResponse RequestLatestDataAsync(AsyncRequest request);

    publishes SensorUpdate;
    publishes AsyncResultReady;
}
```

使用 `omni-idlc` 编译生成 C++ Stub/Proxy 代码：

```bash
./build/target/bin/omni-idlc sensor_service.bidl --lang cpp --outdir generated/
```

### 服务端

```cpp
#include "sensor_service.bidl.h"

class MySensorService : public demo::SensorServiceStub {
public:
    bool EchoBool(bool value) override { return !value; }
    int32_t EchoInt32(int32_t value) override { return value + 3; }
    std::string EchoString(const std::string& value) override { return value + "_echo"; }

    demo::SensorData GetLatestData() override { ... }
    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override { ... }
    int32_t GetSensorCount() override { return 3; }

    common::StatusResponse RequestLatestDataAsync(const demo::AsyncRequest& request) override {
        demo::AsyncResultReady ready;
        ready.result.request_id = request.request_id;
        ready.result.client_tag = request.client_tag;
        ...
        BroadcastAsyncResultReady(ready);

        common::StatusResponse ack;
        ack.code = 0;
        ack.message = "accepted";
        return ack;
    }
};

int main() {
    omnibinder::OmniRuntime runtime;
    runtime.init("127.0.0.1", 9900);
    runtime.setRegisterHost("192.168.1.10");

    MySensorService service;
    runtime.registerService(&service);
    runtime.publishTopic("SensorUpdate");
    runtime.publishTopic("AsyncResultReady");

    while (running) {
        runtime.pollOnce(100);
        demo::SensorUpdate msg;
        ...
        service.BroadcastSensorUpdate(msg);
    }
}
```

默认情况下，OmniBinder 的 SHM request/response ring 容量为 `4KB / 4KB`。这适合多数嵌入式控制类 RPC。
如果某个服务存在更大的本机请求或响应数据，可以像上面那样在服务端显式调用 `setShmConfig()` 单独放大。

### 客户端

```cpp
#include "sensor_service.bidl.h"

int main() {
    omnibinder::OmniRuntime runtime;
    runtime.init("127.0.0.1", 9900);

    demo::SensorServiceProxy proxy(runtime);
    proxy.connect();

    bool echo_bool = false;
    proxy.EchoBool(false, &echo_bool);

    int32_t echo_i32 = 0;
    proxy.EchoInt32(32, &echo_i32);

    std::string echo_string;
    proxy.EchoString("hello", &echo_string);

    demo::SensorData latest;
    proxy.GetLatestData(&latest);

    demo::ControlCommand cmd;
    cmd.command_type = 1;
    cmd.target = "temperature";
    cmd.value = 30;
    common::StatusResponse resp;
    proxy.SetThreshold(cmd, &resp);

    int32_t sensor_count = 0;
    proxy.GetSensorCount(&sensor_count);

    proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
        // 收到普通话题广播
    });

    proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& msg) {
        // 收到异步请求的结果回推
    });

    demo::AsyncRequest req;
    req.request_id = 42;
    req.client_tag = "cpp-client";
    common::StatusResponse ack;
    proxy.RequestLatestDataAsync(req, &ack);

    proxy.OnServiceDied([]() {
        // 服务异常退出通知
    });

    while (running) {
        runtime.pollOnce(100);
    }
}
```

更完整的示例请参考 [examples/](examples/) 目录和 [示例文档](docs/examples.md)。

---

## 架构概览

从职责上看，OmniBinder 可以理解为一层位于多个业务服务之间的**通信桥梁中间件**：

- **向上**为业务模块提供统一的服务化接口
- **向下**屏蔽本机 IPC 与跨机 RPC 的差异
- **横向**把不同服务连接成一个可发现、可调用、可广播的分布式协作网络

```
┌─────────────────────────────────────────────────────┐
│                   ServiceManager                     │
│          (服务注册/发现/心跳/死亡通知/话题管理)         │
└──────────┬──────────────────────────┬────────────────┘
           │ TCP (控制通道)            │ TCP (控制通道)
     ┌─────┴──────┐            ┌──────┴─────┐
     │  Service A  │            │  Service B  │
     │ (提供者)     │◄──────────►│ (消费者)     │
     └────────────┘  SHM/TCP    └────────────┘
                    (数据通道)
```

- **控制通道**：所有服务通过 TCP 连接 ServiceManager，完成注册、发现、心跳等
- **数据通道**：服务之间直连通信（同机自动选择 SHM，跨机走 TCP）
- **传输自动选择**：通过 `host_id` 判断是否同机，同机优先使用共享内存

详细架构设计见 [架构文档](docs/architecture.md)。

---

## 运行时与可观测性

当前版本提供以下能力：

- `OmniRuntime` 公共 API 线程安全访问
- ServiceManager 基础重连与控制面状态恢复
- 最小运行时统计：`RuntimeStats / getStats / resetStats`
- 标准化错误日志关键词（便于 grep / 日志平台检索）

建议重点关注的日志关键词：

- `sm_connect_failed`
- `sm_connect_timeout`
- `sm_connection_lost`
- `sm_reconnect_begin`
- `sm_reconnect_success`
- `rpc_timeout`
- `data_connect_failed`
- `data_connect_fallback`

相关测试包括：

- `test_threadsafe_client_and_reconnect`
- `test_runtime_stats`
- `test_error_logging`

---

## 文档

| 文档 | 说明 |
|------|------|
| [架构设计](docs/architecture.md) | 系统架构、组件说明、通信流程、线程模型 |
| [内部实现架构](docs/architecture-internals.md) | 面向二次开发者的内部 runtime、数据流、内存模型说明 |
| [通信协议](docs/protocol.md) | 二进制协议格式、消息类型、序列化规则、通信时序 |
| [API 参考](docs/api-reference.md) | OmniRuntime / Service / Buffer 等核心类的完整 API |
| [IDL 规范](docs/idl-specification.md) | `.bidl` 接口定义语法、类型系统、代码生成规则 |
| [ARM 交叉编译指南](docs/cross-compiling-arm.md) | Host/Target 分离构建、ARM 工具链、部署建议 |
| [omni-cli 使用指南](docs/omni-tool-usage.md) | 命令行工具使用说明、JSON 格式支持 |
| [测试说明](docs/testing-guide.md) | 测试用例用途、启动方式、推荐执行方法 |
| [使用示例](docs/examples.md) | 完整的服务端/客户端示例、跨板通信示例 |
| [下游 Sensor/HMI 示例](examples/artifact_examples/README.md) | 使用已构建 `lib` 与 `omni-idlc` 独立构建的 C/C++ 业务示例 |
| [发布特性](RELEASE_FEATURES.md) | 当前版本核心能力、交付内容与适用场景 |
| [性能报告](docs/performance-report.md) | RPC 和话题的延迟测试数据（微秒级） 

---

## 项目结构

```
omnibinder/
├── include/omnibinder/            # 公共头文件
├── src/                           # 核心库实现
│   ├── core/                      # OmniRuntime、EventLoop、ConnectionManager 等
│   ├── transport/                 # TCP / SHM 传输实现
│   └── platform/                  # 平台抽象层
├── service_manager/               # ServiceManager 进程
├── tools/
│   ├── omni_idlc/                 # IDL 编译器源码
│   └── omni_cli/                  # 命令行工具源码
├── examples/
│   ├── example_cpp/               # 仓库内 C++ 示例
│   ├── example_c/                 # 仓库内 C 示例
│   └── artifact_examples/       # 下游独立消费式示例
├── tests/                         # 单元 / 集成 / 性能 / 长稳测试
├── docs/                          # 对外文档
├── RELEASE_FEATURES.md            # 当前版本发布特性文档
└── CMakeLists.txt
```

---

## 环境要求

- **操作系统**: Linux（epoll / eventfd / POSIX SHM）或 Windows（MinGW / MSVC）
- **编译器**: GCC 4.8+, Clang 3.4+, MinGW 7.3+, MSVC 2017+（支持 C++11）
- **CMake**: 3.10+
- **外部依赖**: 无

### 平台差异

| 特性 | Linux | Windows |
|------|-------|---------|
| I/O 多路复用 | epoll | IOCP + WSAPoll |
| 跨进程通知 | eventfd | Named Pipe |
| Unix Domain Socket | AF_UNIX | TCP loopback (127.0.0.1) |
| 共享内存 | POSIX SHM | Named File Mapping |
| 跨进程 SHM | ✅ 完整支持 | ✅ 完整支持 |

---

## 运行测试

```bash
cd build

# 运行全部测试（Linux / Windows（部分不支持） 均支持）
ctest --output-on-failure

# 单独运行性能测试（结果输出到 docs/performance-report.md）
./target/test/test_performance --report ../docs/performance-report.md
```

> **注意**：`test_generated_runtime_integration` 因依赖 g++ 编译流水线，仅 Linux 可用。

---

## License

MIT
