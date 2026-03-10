# OmniBinder

**面向嵌入式 Linux 与多板系统的分布式 IPC/RPC 框架**

OmniBinder 是一个面向嵌入式 Linux 和分布式多板系统的应用层 IPC/RPC 框架。
它提供服务注册与发现、远程方法调用、发布/订阅广播、死亡通知等核心能力，
让多个服务进程之间的通信像调用本地函数一样简单。

---

## 项目背景

在嵌入式和工业场景中，一个系统往往由多个服务进程协作完成：传感器采集、算法处理、
UI 显示、设备控制等模块运行在不同进程甚至不同板卡上。这些进程之间需要频繁地互相
调用接口、推送数据。

现有方案的痛点：

- **Android Binder** 深度绑定 Android 内核驱动，在标准 Linux 上无法直接使用，
  且不支持跨网络通信，接口定义和使用方式对非 Android 开发者不够友好
- **D-Bus** 性能较差，序列化开销大，不适合高频数据传输场景
- **gRPC / Thrift** 依赖庞大，对嵌入式环境不友好，编译工具链复杂
- **裸 Socket + 自定义协议** 每个项目重复造轮子，缺乏统一的服务治理能力

OmniBinder 的设计目标是：**零外部依赖、C++11 编译、同时支持同机 SHM 高速通信和
跨板 TCP 网络通信，提供类似 Binder 的服务化编程模型，但不依赖任何内核驱动，并同时提供 C / C++ 接口。**

---

## 核心特性

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

## 适用场景

| 场景 | 说明 |
|------|------|
| **单板多进程** | 同一台 Linux 设备上的多个服务进程互相调用（自动走 SHM，低延迟） |
| **局域网分布式** | 多台设备通过以太网组成分布式系统，服务跨板调用（走 TCP） |
| **嵌入式网关** | 网关设备汇聚多个子板的服务，统一对外提供接口 |
| **机器人/自动驾驶** | 传感器、感知、规划、控制等模块分布在不同计算单元上 |
| **工业控制** | PLC、HMI、数据采集等模块之间的实时数据交换 |

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

## 性能

同机 SHM 通道下的测试结果：

| 测试项 | 中位数 | P99 | 说明 |
|--------|--------|-----|------|
| RPC 往返 (0 bytes) | ~75 us | ~147 us | 空 payload，纯协议开销 |
| RPC 往返 (256 bytes) | ~77 us | ~142 us | 小 payload |
| RPC 往返 (4096 bytes) | ~83 us | ~157 us | 大 payload 影响不大 |
| 话题发布/订阅 (256 bytes) | ~15 us | ~28 us | 发布 → 订阅者回调 |

> **性能说明**：OmniBinder 使用 eventfd 事件驱动机制（已实施），实现微秒级延迟。
> 测试数据基于 SHM 本地通信，纯事件驱动，无轮询开销。

详细数据和分析见 [性能测试报告](docs/performance-report.md)。

---

## 架构概览

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
