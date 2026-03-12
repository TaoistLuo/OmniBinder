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

换句话说，OmniBinder 解决的是**“服务与服务之间怎么高效、统一、可管理地通信”**，而不仅仅是“底层报文怎么收发”。

---

## 核心特性

OmniBinder 同时覆盖了**服务管理**和**服务间数据通路**两部分能力：

- **服务注册与发现** — 中心化 ServiceManager 管理所有服务的生命周期
- **远程方法调用 (RPC)** — 同步调用远端服务接口，支持请求/响应模式和单向调用
- **发布/订阅广播** — 话题机制，发布者广播数据，订阅者实时接收
- **死亡通知** — 服务异常退出时自动通知所有关注者
- **传输层自动选择** — 同机通信自动使用共享内存 (SHM)，跨机通信走 TCP
- **服务级 SHM 配置** — 默认使用小容量 SHM ring（当前 `4KB / 4KB`），特殊服务可按需放大 request / response 容量
- **IDL 代码生成** — 通过 `.bidl` 接口定义文件自动生成 Stub/Proxy 代码
- **零外部依赖** — 仅依赖 POSIX API 和标准 C++11，无需第三方库
- **命令行工具** — `omni-cli` 支持运行时查询服务、调用接口，支持 JSON 格式输入输出

### 线程模型说明

- `OmniRuntime` 对外提供线程安全公共 API
- 内部运行时由单个 owner event-loop 串行驱动
- 回调（如 topic / death callback）默认在 owner event-loop 线程执行
- `run()` / `pollOnce()` / `stop()` 的并发协作规则见 [线程模型文档](docs/threading-model.md)

---

## 性能表现

以下数据用于展示 OmniBinder 在**同机 SHM 场景**下的典型性能表现。

**测试环境摘要：**

- 传输方式：TCP + SHM 自动选择（同机通信使用 SHM）
- RPC 预热轮数：50；RPC 测试轮数：1000 / 用例
- Topic 预热轮数：10；Topic 测试轮数：1000 / 用例

| 测试项 | 样本数 | 平均值 | 95% 情况 | 99% 情况 | 说明 |
|--------|--------|--------|-----------|-----------|------|
| RPC Echo (0 bytes) | 1000 | 67.0 us | 99 us | 123 us | 空 payload，主要反映协议与调度开销 |
| RPC Echo (256 bytes) | 1000 | 66.4 us | 96 us | 130 us | 常见小 payload RPC |
| RPC Echo (1024 bytes) | 1000 | 67.1 us | 94 us | 121 us | 1KB 级 payload |
| RPC Echo (4096 bytes) | 1000 | 81.1 us | 107 us | 126 us | 4KB payload，测试服务显式放大 SHM ring |
| RPC Echo (8192 bytes) | 1000 | 93.3 us | 127 us | 168 us | 8KB payload，测试服务显式放大 SHM ring |
| RPC Add (2 x int32) | 1000 | 68.8 us | 95 us | 127 us | 小计算型 RPC |
| Topic pub/sub (64 bytes) | 1000 | 58.0 us | 91 us | 113 us | 小广播数据，发布 → 订阅者回调 |
| Topic pub/sub (256 bytes) | 1000 | 53.4 us | 83 us | 107 us | 常见小广播数据 |
| Topic pub/sub (1024 bytes) | 1000 | 56.2 us | 87 us | 103 us | 1KB 广播数据 |
| Topic pub/sub (8192 bytes) | 1000 | 74.3 us | 109 us | 134 us | 8KB 广播数据 |

从报告中的完整数据看：

- **0~1024 bytes 常见 RPC** 在当前机器上的平均值约为 **65.9~67.1 us**
- **4096~8192 bytes payload RPC** 在显式放大 SHM ring 的测试配置下，平均值约为 **81.1~93.3 us**
- **Topic pub/sub** 在当前机器上的平均值约为 **53.4~74.3 us**

> **性能说明**：当前 SHM 路径已使用 `eventfd + EventLoop` 的事件驱动模型。
> 报告中的延迟数据主要反映序列化、共享内存拷贝、eventfd 唤醒、epoll 调度与业务处理开销。
> 其中 `4096 / 8192 bytes` 用例基于测试服务显式配置的 `16KB / 16KB` SHM ring，并不代表默认 `4KB / 4KB` 配置下的行为。

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

如果你的系统本质上是“多个服务协同完成一件事”，而你希望它们之间有一层统一的通信桥梁，OmniBinder 就是为此设计的。

---

## 快速开始

### 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

构建选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `OMNIBINDER_BUILD_TESTS` | ON | 编译单元测试 |
| `OMNIBINDER_BUILD_EXAMPLES` | ON | 编译示例程序 |
| `OMNIBINDER_BUILD_TOOLS` | ON | 编译 omni-idlc 和 omni-cli |

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
# 先安装当前项目产物
cmake --install build

# 再构建下游 Sensor/HMI 示例（依赖 build/install 下的 lib + omni-idlc）
./examples/artifact_sensor_hmi/scripts/build_downstream_example.sh

# 启动完整 C++ 示例栈
./examples/artifact_sensor_hmi/scripts/run_cpp_stack.sh
```

该示例同时提供 `sensor_cpp` / `hmi_cpp` 和 `sensor_c` / `hmi_c` 两套版本，
用于演示：HMI 调用 sensor 控制接口、订阅 sensor 广播、以及在 sensor 退出时接收死亡通知。

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

---

## 编程示例

### 定义接口 (IDL)

```bidl
package demo;

struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    string  location;
}

topic SensorUpdate {
    SensorData data;
}

service SensorService {
    SensorData GetLatestData();
    void       ResetSensor();
}
```

使用 `omni-idlc` 编译生成 C++ Stub/Proxy 代码：

```bash
./build/target/bin/omni-idlc sensor_service.bidl --lang cpp --outdir generated/
```

### 服务端

```cpp
#include <omnibinder/omnibinder.h>

class MySensorService : public omnibinder::Service {
public:
    MySensorService() : Service("SensorService") {
        setShmConfig(omnibinder::ShmConfig(8 * 1024, 16 * 1024));
    }
    const char* serviceName() const override { return "SensorService"; }
protected:
    void onInvoke(uint32_t method_id, const omnibinder::Buffer& req,
                  omnibinder::Buffer& resp) override {
        // 处理远程调用
    }
};

int main() {
    omnibinder::OmniRuntime runtime;
    runtime.init("127.0.0.1", 9900);          // 连接 ServiceManager

    MySensorService service;
    runtime.registerService(&service);          // 注册服务
    runtime.publishTopic("SensorUpdate");       // 发布话题

    while (running) {
        runtime.pollOnce(100);                  // 事件循环
        runtime.broadcast(topic_id, data);      // 广播数据
    }
}
```

默认情况下，OmniBinder 的 SHM request/response ring 容量为 `4KB / 4KB`。这适合多数嵌入式控制类 RPC。
如果某个服务存在更大的本机请求或响应数据，可以像上面那样在服务端显式调用 `setShmConfig()` 单独放大。

### 客户端

```cpp
#include <omnibinder/omnibinder.h>

int main() {
    omnibinder::OmniRuntime runtime;
    runtime.init("127.0.0.1", 9900);

    // 远程调用
    omnibinder::Buffer req, resp;
    runtime.invoke("SensorService", iface_id, method_id, req, resp, 5000);

    // 订阅广播
    runtime.subscribeTopic("SensorUpdate", [](uint32_t tid, const omnibinder::Buffer& data) {
        // 收到广播数据
    });

    // 死亡通知
    runtime.subscribeServiceDeath("SensorService", [](const std::string& name) {
        // 服务异常退出
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
| [下游 Sensor/HMI 示例](examples/artifact_sensor_hmi/README.md) | 使用已构建 `lib` 与 `omni-idlc` 独立构建的 C/C++ 业务示例 |
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
│   └── artifact_sensor_hmi/       # 下游独立消费式示例
├── tests/                         # 单元 / 集成 / 性能 / 长稳测试
├── docs/                          # 对外文档
├── RELEASE_FEATURES.md            # 当前版本发布特性文档
└── CMakeLists.txt
```

---

## 环境要求

- **操作系统**: Linux（需要 epoll、eventfd、POSIX SHM 支持）
- **编译器**: GCC 4.8+ 或 Clang 3.4+（支持 C++11）
- **CMake**: 3.10+
- **外部依赖**: 无

---

## 运行测试

```bash
cd build

# 运行全部测试
ctest --output-on-failure

# 单独运行性能测试（结果输出到 docs/performance-report.md）
./target/test/test_performance --report ../docs/performance-report.md
```

---

## License

MIT
