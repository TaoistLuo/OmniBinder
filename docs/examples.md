# OmniBinder 使用示例

中文 | [English](examples.en.md)

## 1. 概述

本文档展示 OmniBinder 的完整使用示例，包括：
- C++ 服务端（SensorService）：提供传感器数据接口，定时广播传感器数据
- C++ 客户端（SensorClient）：调用服务端接口，订阅广播，监听死亡通知
- C 服务端 / C 客户端：纯 C 语言实现，功能与 C++ 示例等价
- 下游消费式 Sensor/HMI 示例：使用已构建出的 `lib` 与 `omni-idlc` 独立构建
- omni-cli：查询在线服务和调用接口

> 如果你需要验证 **“业务工程如何依赖 OmniBinder 的构建产物”**，请直接看
> `examples/artifact_sensor_hmi/`。该目录是独立 CMake 工程，通过
> `find_package(OmniBinder REQUIRED)` 和已安装的 `omni-idlc` 构建，
> 不依赖仓库内部 target 名称。

## 2. IDL 接口定义

本示例使用两个 IDL 文件，展示了 `import` 跨文件引用的用法。

### 2.1 common_types.bidl（公共类型定义）

```protobuf
// examples/common_types.bidl
// 公共类型定义

package common;

struct Timestamp {
    int64   seconds;
    int32   nanos;
}

struct StatusResponse {
    int32   code;
    string  message;
}
```

### 2.2 sensor_service.bidl（服务接口定义）

当前 demo 使用的 `examples/sensor_service.bidl` 已扩展为完整能力矩阵，包含：

- 基础类型 RPC：`EchoBool` / `EchoInt8` / `EchoUInt8` / ... / `EchoFloat64` / `EchoString` / `EchoBytes`
- 自定义结构体 RPC：`EchoStatus` / `EchoConfig`
- 嵌套结构体 RPC：`EchoEnvelope`
- 数组 RPC：`EchoIdArray` / `EchoLabelArray` / `EchoSensorArray` / `EchoBundle`
- 普通服务接口：`GetLatestData` / `SetThreshold` / `ResetSensor` / `GetSensorCount`
- callback-like 异步接口：`RequestLatestDataAsync`
- 发布订阅：`SensorUpdate` / `AsyncResultReady`

关键结构如下：

```protobuf
// examples/sensor_service.bidl
package demo;

import "common_types.bidl";

struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    int64   timestamp;
    string  location;
}

struct SensorConfig {
    bool    enabled;
    int32   sample_rate_hz;
    string  label;
}

struct SensorEnvelope {
    SensorData        data;
    SensorConfig      config;
    common.Timestamp  captured_at;
}

struct SensorArrayBundle {
    array<int32>      ids;
    array<string>     labels;
    array<bytes>      blobs;
    array<SensorData> samples;
}

struct AsyncRequest {
    int32   request_id;
    string  client_tag;
}

struct AsyncResult {
    int32                 request_id;
    string                client_tag;
    common.StatusResponse status;
    SensorData            data;
}

topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic AsyncResultReady {
    AsyncResult result;
    int64       publish_time;
}
```

完整方法列表请直接以仓库中的 `examples/sensor_service.bidl` 为准。

**关键点**：
- `import "common_types.bidl"` 导入公共类型定义
- 使用 `common.StatusResponse` 引用导入包中的类型（包名.类型名）
- 编译时需要确保 `common_types.bidl` 在同一目录或指定搜索路径

### 2.3 生成代码

```bash
# 生成 C++ 代码（会自动处理 import 依赖）
omni-idlc --lang=cpp --output=generated/ sensor_service.bidl

# 生成文件:
#   generated/common_types.bidl.h       # 公共类型头文件
#   generated/common_types.bidl.cpp     # 公共类型实现
#   generated/sensor_service.bidl.h     # 服务接口头文件（会 #include "common_types.bidl.h"）
#   generated/sensor_service.bidl.cpp   # 服务接口实现

# 生成 C 代码
omni-idlc --lang=c --output=generated/ sensor_service.bidl

# 生成文件:
#   generated/common_types.bidl_c.h
#   generated/common_types.bidl.c
#   generated/sensor_service.bidl_c.h   # 会 #include "common_types.bidl_c.h"
#   generated/sensor_service.bidl.c
```

**注意**：编译器会自动解析 `import` 依赖，生成所有需要的文件。

## 3. C++ 服务端：SensorService

### 3.1 CMakeLists.txt

```cmake
# examples/example_cpp/CMakeLists.txt

# 使用 IDL 生成代码
omnic_generate(
    TARGET example_cpp_sensor_server
    LANGUAGE cpp
    FILES ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_executable(example_cpp_sensor_server sensor_server.cpp)
target_link_libraries(example_cpp_sensor_server omnibinder_static)
```

### 3.2 `sensor_server.cpp`

当前 `examples/example_cpp/sensor_server.cpp` 已扩展为完整 demo 服务端。它实现了：

- 所有基础类型 RPC（`EchoBool` ~ `EchoFloat64`、`EchoString`、`EchoBytes`）
- 自定义结构体 RPC（`EchoStatus`、`EchoConfig`）
- 嵌套结构体 RPC（`EchoEnvelope`）
- 数组 RPC（`EchoIdArray`、`EchoLabelArray`、`EchoSensorArray`、`EchoBundle`）
- 普通服务接口（`GetLatestData`、`SetThreshold`、`ResetSensor`、`GetSensorCount`）
- callback-like 异步能力（`RequestLatestDataAsync` 通过 `AsyncResultReady` 话题回推结果）
- 普通发布订阅（`SensorUpdate`）

服务端核心形态如下：

```cpp
class MySensorService : public demo::SensorServiceStub {
public:
    bool EchoBool(bool value) override { return !value; }
    int32_t EchoInt32(int32_t value) override { return value + 3; }
    std::string EchoString(const std::string& value) override { return value + "_echo"; }
    common::StatusResponse EchoStatus(const common::StatusResponse& value) override { ... }
    demo::SensorEnvelope EchoEnvelope(const demo::SensorEnvelope& value) override { ... }
    std::vector<int32_t> EchoIdArray(const std::vector<int32_t>& value) override { ... }
    demo::SensorArrayBundle EchoBundle(const demo::SensorArrayBundle& value) override { ... }

    demo::SensorData GetLatestData() override { ... }
    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override { ... }
    void ResetSensor(int32_t sensor_id) override { ... }
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
```

完整代码请直接以 `examples/example_cpp/sensor_server.cpp` 为准。

## 4. C++ 客户端：SensorClient

### 4.1 CMakeLists.txt

```cmake
# examples/example_cpp/CMakeLists.txt

# 客户端使用同一份 IDL 生成的 Proxy
omnic_generate(
    TARGET example_cpp_sensor_client
    LANGUAGE cpp
    FILES ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_executable(example_cpp_sensor_client sensor_client.cpp)
target_link_libraries(example_cpp_sensor_client omnibinder_static)
```

### 4.2 sensor_client.cpp

当前 `examples/example_cpp/sensor_client.cpp` 已扩展为完整 demo 客户端，覆盖：

- 所有基础类型 RPC 调用
- 自定义结构体 / 嵌套结构体 RPC 调用
- 数组 RPC 调用
- 普通服务接口（`GetLatestData`、`SetThreshold`、`ResetSensor`、`GetSensorCount`）
- 话题订阅（`SensorUpdate`）
- callback-like 异步结果接收（`AsyncResultReady`）
- 死亡通知

典型调用形态如下：

```cpp
omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

demo::SensorServiceProxy proxy(runtime);
proxy.connect();

uint8_t bool_out = 0;
proxy.EchoBool(false, &bool_out);

int32_t int_out = 0;
proxy.EchoInt32(32, &int_out);

StatusResponse status_out;
proxy.EchoStatus(status, &status_out);

SensorEnvelope envelope_out;
proxy.EchoEnvelope(envelope, &envelope_out);

std::vector<int32_t> ids_out;
proxy.EchoIdArray(ids, &ids_out);

SensorArrayBundle bundle_out;
proxy.EchoBundle(bundle, &bundle_out);

proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) { ... });
proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& msg) { ... });

demo::AsyncRequest req;
req.request_id = 42;
req.client_tag = "cpp-client";
common::StatusResponse ack;
proxy.RequestLatestDataAsync(req, &ack);
```

完整代码请直接以 `examples/example_cpp/sensor_client.cpp` 为准。

## 5. C 服务端

纯 C 语言实现的服务端，功能与 C++ 服务端等价。通过 `omni-idlc --lang=c` 生成 C 代码，
使用 `omnibinder_c.h` 提供的 C 封装 API 访问 OmniBinder 功能。

### 5.1 CMakeLists.txt

```cmake
# examples/example_c/CMakeLists.txt

add_executable(example_c_sensor_server sensor_server.c)
target_link_libraries(example_c_sensor_server PRIVATE omnibinder_static)

if(TARGET omni-idlc)
    omnic_generate(
        TARGET example_c_sensor_server
        LANGUAGE c
        FILES ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    add_dependencies(example_c_sensor_server omni-idlc)
endif()
```

### 5.2 `sensor_server.c`

当前 `examples/example_c/sensor_server.c` 已扩展为完整 demo 服务端。它实现了：

- 所有基础类型 RPC
- 自定义结构体 / 嵌套结构体 RPC
- 数组 RPC
- 普通服务接口
- callback-like 异步接口（通过 `AsyncResultReady` 回推）
- 普通发布订阅（`SensorUpdate`）

它的关键形态是：

```c
void demo_SensorService_impl_echo_int32(int32_t value, int32_t* result, void* user_data) {
    (void)user_data;
    *result = value + 3;
}

void demo_SensorService_impl_request_latest_data_async(
    const demo_AsyncRequest* request,
    common_StatusResponse* result,
    void* user_data) {
    omni_runtime_t* runtime = (omni_runtime_t*)user_data;
    ...
    demo_SensorService_broadcast_async_result_ready(runtime, &ready);
    ...
}

omni_service_t* svc = demo_SensorService_stub_create(runtime);
```

完整代码请直接以 `examples/example_c/sensor_server.c` 为准。

与 C++ 版本的关键区别：
- 继承 Stub 基类 → 直接实现生成的 `demo_SensorService_impl_xxx(...)` 接口
- `demo_SensorService_stub_create(user_data)` 会自动绑定这些实现，不需要手动维护回调表
- `service.BroadcastSensorUpdate(msg)` → `demo_SensorService_broadcast_sensor_update(client, &msg)`
- `std::string` → `char*` + `_len` 字段，需要手动 `malloc/free`
- 结构体需要 `_init()` / `_destroy()` 管理生命周期

## 6. C 客户端

纯 C 语言实现的客户端，功能与 C++ 客户端等价。

### 6.1 `sensor_client.c`

当前 `examples/example_c/sensor_client.c` 也已经扩展为完整 demo 客户端，覆盖：

- 所有基础类型 RPC 调用
- 自定义结构体 / 嵌套结构体 RPC 调用
- 数组 RPC 调用
- 普通 pub/sub（`SensorUpdate`）
- callback-like 异步结果接收（`AsyncResultReady`）
- 死亡通知回调

典型调用形态如下：

```c
demo_SensorService_proxy proxy;
demo_SensorService_proxy_init(&proxy, runtime);
demo_SensorService_proxy_connect(&proxy);

uint8_t b = 0;
demo_SensorService_proxy_echo_bool(&proxy, 0, &b);

demo_SensorEnvelope env_in;
demo_SensorEnvelope env_out;
demo_SensorService_proxy_echo_envelope(&proxy, &env_in, &env_out);

demo_int32_t_array ids_in;
demo_int32_t_array ids_out;
demo_SensorService_proxy_echo_id_array(&proxy, &ids_in, &ids_out);

demo_SensorService_proxy_subscribe_sensor_update(&proxy, on_sensor_update, NULL);
demo_SensorService_proxy_subscribe_async_result_ready(&proxy, on_async_result, NULL);

demo_AsyncRequest async_req;
demo_SensorService_proxy_request_latest_data_async(&proxy, &async_req, &ack);
```

完整代码请直接以 `examples/example_c/sensor_client.c` 为准。

与 C++ 版本的关键区别：
- `SensorServiceProxy proxy(client)` → `demo_SensorService_proxy proxy; demo_SensorService_proxy_init(&proxy, runtime)`
- `proxy.GetLatestData(&data)` 显式输出参数 → `demo_SensorService_proxy_get_latest_data(&proxy, &data)`
- `proxy.SubscribeSensorUpdate([](auto& msg){...})` → `demo_SensorService_proxy_subscribe_sensor_update(&proxy, callback, user_data)`
- 所有结构体需要 `_init()` 初始化、`_destroy()` 释放

## 7. omni-cli 使用示例

### 7.1 查询所有在线服务

```bash
$ ./target/bin/omni-cli list
NAME                HOST            PORT    STATUS
----                ----            ----    ------
SensorService       127.0.0.1       8001    ONLINE
SensorClient       127.0.0.1       8002    ONLINE

Total: 2 services online
```

### 7.2 查询服务详细信息

**基础模式（显示类型名）：**
```bash
$ ./target/bin/omni-cli info SensorService
Service: SensorService
  Host:    127.0.0.1
  Port:    8001
  HostID:  a1b2c3d4e5f6
  Status:  ONLINE

  Interface: SensorService (id=0x1a2b3c4d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x9c0d1e2f)
      - SetThreshold(ControlCommand) -> StatusResponse  (id=0x5e6f7a8b)
      - ResetSensor(int32) -> void  (id=0x3a4b5c6d)

    Published Topics:
      - SensorUpdate  (id=0x7e8f9a0b)
```

**详细模式（展开字段定义）：**
```bash
$ ./target/bin/omni-cli --idl examples/sensor_service.bidl info SensorService
Service: SensorService
  Host:    127.0.0.1
  Port:    8001
  HostID:  a1b2c3d4e5f6
  Status:  ONLINE

  Interface: SensorService (id=0x1a2b3c4d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x9c0d1e2f)
          return:           {
            sensor_id: int32
            temperature: float64
            humidity: float64
            timestamp: int64
            location: string
          }
      - SetThreshold(ControlCommand) -> StatusResponse  (id=0x5e6f7a8b)
          param:           {
            command_type: int32
            target: string
            value: int32
          }
          return:           {
            code: int32
            message: string
          }
      - ResetSensor(int32) -> void  (id=0x3a4b5c6d)
```

### 7.3 调用服务接口

`omni-cli` 当前规则：

- 带 `--idl` 时，`call` 的参数使用 **JSON 输入**
- 不带 `--idl` 时，只能传原始十六进制请求体
- 当前已经验证可正常工作的 JSON I/O 类型：
  - 基础类型（bool/int/float/string）
  - 普通 struct
  - 嵌套 struct
  - 无参返回 struct
- 当前**尚未验证通过**的 JSON I/O 类型：
  - `bytes`
  - `array<...>`
  - 含数组的复杂 struct

#### 7.3.1 通用格式

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService <Method> <JSON参数>
```

#### 7.3.2 每个接口的调用方式

**无参 RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetLatestData
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetSensorCount
```

**基础类型 RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoBool false
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt8 7
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt8 7
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt16 16
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt16 16
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt32 32
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt32 32
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt64 64
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt64 64
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat32 1.5
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat64 2.5
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoString '"hello"'
```

> 注意：字符串参数要按 JSON 字符串传入，因此 shell 参数中需要保留内部双引号，例如 `EchoString '"hello"'`。

**普通 struct RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoStatus '{"code":7,"message":"demo"}'
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoConfig '{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"}'
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService SetThreshold '{"command_type":1,"target":"temperature","value":30}'
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService RequestLatestDataAsync '{"request_id":42,"client_tag":"cli"}'
```

**嵌套 struct RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoEnvelope '{"data":{"sensor_id":10,"temperature":18.5,"humidity":45.5,"timestamp":123456789,"location":"Lab-1"},"config":{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"},"captured_at":{"seconds":123456789,"nanos":321}}'
```

**单向 RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService ResetSensor 1
```

**当前不建议使用 `omni-cli` 调用的接口**

以下接口在当前 `omni-cli` JSON I/O 路径上未验证通过，会编码失败或找不到对应类型：

- `EchoBytes(bytes value)`
- `EchoIdArray(array<int32> value)`
- `EchoLabelArray(array<string> value)`
- `EchoSensorArray(array<SensorData> value)`
- `EchoBundle(SensorArrayBundle value)`

这些接口请使用 `examples/example_cpp/sensor_client.cpp` 或 `examples/example_c/sensor_client.c` 验证。

#### 7.3.3 查询服务信息

```bash
./build-host/target/bin/omni-cli list
./build-host/target/bin/omni-cli info SensorService
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl info SensorService
```

详细使用说明见 [omni-cli 使用指南](omni-tool-usage.md)。

## 8. 完整运行流程

### 8.1 编译

```bash
# 在项目根目录
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 8.2 运行

打开 4 个终端窗口：

**终端1：启动 ServiceManager**
```bash
$ ./target/bin/service_manager --port 9900
[ServiceManager] Listening on 0.0.0.0:9900
[ServiceManager] Waiting for service registrations...
```

**终端2：启动 C++ 服务端**
```bash
$ ./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900
=== SensorService Starting ===
ServiceManager: 127.0.0.1:9900
Connected to ServiceManager
SensorService registered successfully
Starting broadcast loop (Ctrl+C to stop)...

[SensorService] Broadcast: temp=25.32, humidity=60.15
[SensorService] Broadcast: temp=25.48, humidity=59.87
[SensorService] Broadcast: temp=25.61, humidity=60.23
...
```

**终端3：启动 C++ 客户端**
```bash
$ ./target/example/example_cpp_sensor_client --sm-host 127.0.0.1 --sm-port 9900
=== SensorClient (C++ Client) Starting ===
ServiceManager: 127.0.0.1:9900
Connected to ServiceManager
Connected to SensorService

--- Calling GetLatestData ---
Result: sensor_id=1, temp=25.32, humidity=60.15, location=Room-A

--- Calling SetThreshold ---
Result: code=0, message=Threshold set successfully

--- Calling ResetSensor (one-way) ---
ResetSensor sent (no response expected)

--- Subscribing to SensorUpdate topic ---
Subscribed to SensorUpdate

--- Subscribing to SensorService death notification ---
Subscribed to death notification

Waiting for broadcasts and notifications (Ctrl+C to stop)...
============================================================

[Broadcast] SensorUpdate: sensor_id=1, temp=25.00, humidity=60.00, time=1707321601
[Broadcast] SensorUpdate: sensor_id=1, temp=25.15, humidity=59.78, time=1707321602
[Broadcast] SensorUpdate: sensor_id=1, temp=24.89, humidity=60.12, time=1707321603
...
```

说明：客户端会持续等待广播，直到用户手动中断。自动化验证时可使用 `timeout 10s ...` 方式运行。

**终端4：使用 omni-cli**
```bash
# 查询服务列表（端口为运行时动态分配，以下仅示例）
$ ./target/bin/omni-cli list
NAME                HOST            PORT    STATUS
----                ----            ----    ------
SensorService       127.0.0.1       <dynamic> ONLINE

Total: 1 services online

# 查看服务信息（基础模式）
$ ./target/bin/omni-cli info SensorService
Service: SensorService
  Host:    127.0.0.1
  Port:    <dynamic>
  ...

# 调用接口（JSON 模式）
$ ./target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData
Response (status=OK, 42 bytes, 0.85 ms):
  {
    "sensor_id": 1,
    "temperature": 25.32,
    "humidity": 60.15,
    "timestamp": 1707321600,
    "location": "Room-A"
  }
```

### 8.3 运行测试

```bash
# 在 build 目录下运行所有测试
cd build

# 运行单元测试（快速，无需 ServiceManager）
./target/test/test_buffer
./target/test/test_message
./target/test/test_event_loop
./target/test/test_transport
./target/test/test_shm_transport
./target/test/test_service_registry
./target/test/test_heartbeat_death
./target/test/test_topic_manager
./target/test/test_idl_compiler

# 运行集成测试（自动启动 ServiceManager）
./target/test/test_integration           # 基础集成测试
./target/test/test_full_integration      # 完整集成测试（含 SHM 多客户端、广播、死亡通知）

# 或使用 ctest 运行全部
ctest --output-on-failure
```

集成测试会自动 fork 一个 ServiceManager 子进程，测试完成后自动清理。
`test_full_integration` 覆盖以下场景：

| 测试组 | 测试内容 |
|--------|---------|
| 双通道初始化 | 服务同时创建 TCP + SHM，SM 返回 shm_name |
| SHM 自动选择 | 同机客户端自动通过 SHM 调用服务 |
| 多客户端共享 | 3 个客户端并发 + 5 个客户端顺序共享同一块 SHM |
| 广播/订阅 | 发布者通过 SHM 广播数据到订阅者 |
| 死亡通知 | 服务进程崩溃后收到死亡回调 |
| 生命周期 | 服务注销后 SM 查询不到 |

### 8.4 验证死亡通知

在终端2中按 `Ctrl+C` 杀掉 C++ 服务端，观察终端3的输出：

```
[Broadcast] SensorUpdate: sensor_id=1, temp=25.15, humidity=59.78, time=1707321610

!!! [DeathNotify] SensorService has DIED !!!
!!! Connection lost, need to reconnect when service restarts
```

同时 ServiceManager（终端1）会输出：

```
[ServiceManager] Service 'SensorService' heartbeat timeout, marking as dead
[ServiceManager] Sending death notification to 1 subscriber(s)
```

## 9. 跨板通信示例

当服务端和客户端运行在不同机器上时，只需指定不同的 ServiceManager 地址：

**机器1（192.168.1.10）：运行 ServiceManager 和服务端**
```bash
# 启动 SM（监听所有接口）
$ ./target/bin/service_manager --host 0.0.0.0 --port 9900

# 启动服务端
$ ./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900
```

**机器2（192.168.1.11）：运行客户端**
```bash
# 客户端连接到机器1的 SM
$ ./target/example/example_cpp_sensor_client --sm-host 192.168.1.10 --sm-port 9900
```

框架会自动检测到两个服务不在同一台机器上（host_id 不同），自动使用 TCP 传输而非共享内存。
日志中会显示：

```
[OmniRuntime] Connecting to SensorService at 192.168.1.10:8001
[OmniRuntime] Transport: TCP (cross-machine, host_id mismatch)
```

如果在同一台机器上运行，框架会自动打开服务已创建好的共享内存，并通过 UDS 交换 eventfd：

```
[OmniRuntime] Connecting to SensorService via SHM '/binder_SensorService' (same machine)
[OmniRuntime] SHM eventfd exchange via UDS: slot_id=0, req_eventfd=OK, resp_eventfd=OK
```

**注意**：同一个 SHM 可以被多个客户端共享访问。例如同时启动 3 个客户端实例，
它们都会打开同一块 `/binder_SensorService` 共享内存，各自获取不同的 slot（0、1、2），
互不干扰。最多支持 32 个同机客户端。
此场景已在 `test_full_integration` 的 `three_clients_concurrent_invoke` 测试中验证通过。

## 10. 基于 OmniBinder 库构建独立项目

本节介绍如何在已安装 OmniBinder 的环境下，从零开始构建独立的服务端和客户端项目。

### 10.1 前提条件

假设 OmniBinder 已编译并安装到 `build/install` 目录：

```bash
cd omnibinder
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
make install
# 安装目录: build/install/
#   bin_host/  → omni-idlc，以及主机版 omni-cli/service_manager（普通主机构建时）
#   bin_cross/ → 交叉编译版 omni-cli/service_manager（交叉编译时）
#   include/   → omnibinder/*.h
#   lib/       → libomnibinder.so, libomnibinder.a, cmake/OmniBinder/
```

下面示例中，将安装路径记为 `OMNIBINDER_DIR`：

```bash
export OMNIBINDER_DIR=/path/to/omnibinder/build/install
```

### 10.2 编写 IDL 接口定义

无论使用 C++ 还是 C，第一步都是编写 `.bidl` 接口定义文件：

```protobuf
// my_service.bidl
package myapp;

struct Request {
    int32  id;
    string name;
}

struct Response {
    int32  code;
    string message;
}

topic StatusUpdate {
    int32  id;
    string status;
    int64  timestamp;
}

service MyService {
    Response  HandleRequest(Request req);
    void      Notify(int32 event_id);

    publishes StatusUpdate;
}
```

使用 `omni-idlc` 生成代码：

```bash
# 生成 C++ 代码
$OMNIBINDER_DIR/bin_host/omni-idlc --lang=cpp --output=generated/ my_service.bidl

# 生成 C 代码
$OMNIBINDER_DIR/bin_host/omni-idlc --lang=c --output=generated/ my_service.bidl
```

### 10.3 C++ 项目

#### 项目结构

```
my_cpp_project/
├── CMakeLists.txt
├── my_service.bidl
├── server.cpp
└── client.cpp
```

#### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)

# 查找已安装的 OmniBinder
# 方式1: 通过 CMAKE_PREFIX_PATH 指定（推荐）
#   cmake .. -DCMAKE_PREFIX_PATH=/path/to/omnibinder/build/install
# 方式2: 通过 OmniBinder_DIR 指定
#   cmake .. -DOmniBinder_DIR=/path/to/omnibinder/build/install/lib/cmake/OmniBinder
find_package(OmniBinder REQUIRED)

# 服务端
add_executable(my_server server.cpp)
target_link_libraries(my_server PRIVATE OmniBinder::omnibinder_static)

# 客户端
add_executable(my_client client.cpp)
target_link_libraries(my_client PRIVATE OmniBinder::omnibinder_static)

# IDL 代码生成
omnic_generate(
    TARGET my_server
    LANGUAGE cpp
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/my_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)
omnic_generate(
    TARGET my_client
    LANGUAGE cpp
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/my_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)
```

#### server.cpp

```cpp
#include "my_service.bidl.h"
#include <cstdio>
#include <csignal>
#include <ctime>

static volatile bool g_running = true;
void signalHandler(int) { g_running = false; }

class MyServiceImpl : public myapp::MyServiceStub {
public:
    myapp::Response HandleRequest(const myapp::Request& req) override {
        printf("[Server] HandleRequest: id=%d name=%s\n", req.id, req.name.c_str());
        myapp::Response resp;
        resp.code = 0;
        resp.message = "OK";
        return resp;
    }

    void Notify(int32_t event_id) override {
        printf("[Server] Notify: event_id=%d\n", event_id);
    }
};

int main() {
    signal(SIGINT, signalHandler);

    omnibinder::OmniRuntime runtime;
    if (runtime.init("127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }

    MyServiceImpl service;
    runtime.registerService(&service);
    runtime.publishTopic("StatusUpdate");

    printf("Server running (Ctrl+C to stop)...\n");
    while (g_running) {
        runtime.pollOnce(100);
    }

    runtime.unregisterService(&service);
    runtime.stop();
    return 0;
}
```

#### client.cpp

```cpp
#include "my_service.bidl.h"
#include <cstdio>
#include <csignal>

static volatile bool g_running = true;
void signalHandler(int) { g_running = false; }

int main() {
    signal(SIGINT, signalHandler);

    omnibinder::OmniRuntime runtime;
    if (runtime.init("127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }

    myapp::MyServiceProxy proxy(client);
    if (proxy.connect() != 0) {
        fprintf(stderr, "MyService not found\n");
        return 1;
    }

    // RPC 调用
    myapp::Request req;
    req.id = 1;
    req.name = "hello";
    myapp::Response resp = proxy.HandleRequest(req);
    printf("Response: code=%d message=%s\n", resp.code, resp.message.c_str());

    // 订阅广播
    proxy.SubscribeStatusUpdate([](const myapp::StatusUpdate& msg) {
        printf("[Broadcast] id=%d status=%s\n", msg.id, msg.status.c_str());
    });

    // 订阅死亡通知
    proxy.OnServiceDied([]() {
        printf("!!! MyService DIED !!!\n");
    });

    while (g_running) {
        runtime.pollOnce(100);
    }

    runtime.stop();
    return 0;
}
```

#### 编译和运行

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$OMNIBINDER_DIR
make -j$(nproc)

# 运行（需要先启动 ServiceManager）
$OMNIBINDER_DIR/bin/service_manager &
./my_server &
./my_client
```

### 10.4 C 项目

#### 项目结构

```
my_c_project/
├── CMakeLists.txt
├── my_service.bidl
├── server.c
└── client.c
```

#### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(MyCProject LANGUAGES C CXX)  # 需要 CXX 因为 omnibinder 库是 C++ 实现

set(CMAKE_C_STANDARD 99)

find_package(OmniBinder REQUIRED)

# 服务端
add_executable(my_server server.c)
target_link_libraries(my_server PRIVATE OmniBinder::omnibinder_static)

# 客户端
add_executable(my_client client.c)
target_link_libraries(my_client PRIVATE OmniBinder::omnibinder_static)

# IDL 代码生成（C 语言）
omnic_generate(
    TARGET my_server
    LANGUAGE c
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/my_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)
omnic_generate(
    TARGET my_client
    LANGUAGE c
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/my_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)
```

> **注意**：即使源文件是纯 C，`project()` 中仍需声明 `CXX` 语言，因为 `omnibinder_static` 库是 C++ 编译的，链接时需要 C++ 标准库。

#### server.c

```c
#include "my_service.bidl_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile int g_running = 1;
static void signal_handler(int sig) { (void)sig; g_running = 0; }

static void on_handle_request(const myapp_Request* req,
                              myapp_Response* result, void* user_data) {
    (void)user_data;
    printf("[Server] HandleRequest: id=%d name=%.*s\n",
           req->id, req->name_len, req->name);
    result->code = 0;
    result->message = (char*)malloc(3);
    memcpy(result->message, "OK", 3);
    result->message_len = 2;
}

static void on_notify(int32_t event_id, void* user_data) {
    (void)user_data;
    printf("[Server] Notify: event_id=%d\n", event_id);
}

int main(void) {
    signal(SIGINT, signal_handler);

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(client, "127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(client);
        return 1;
    }

    /* 直接绑定生成的 impl 接口 */
    omni_service_t* svc = myapp_MyService_stub_create(NULL);
    omni_runtime_register_service(client, svc);
    omni_runtime_publish_topic(client, "StatusUpdate");

    printf("Server running (Ctrl+C to stop)...\n");
    while (g_running) {
        omni_runtime_poll_once(client, 100);
    }

    omni_runtime_unregister_service(client, svc);
    myapp_MyService_stub_destroy(svc);
    omni_runtime_stop(client);
    omni_runtime_destroy(client);
    return 0;
}
```

#### client.c

```c
#include "my_service.bidl_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile int g_running = 1;
static void signal_handler(int sig) { (void)sig; g_running = 0; }

static void on_status_update(const myapp_StatusUpdate* msg, void* user_data) {
    (void)user_data;
    printf("[Broadcast] id=%d status=%.*s\n", msg->id, msg->status_len, msg->status);
}

static void on_service_died(void* user_data) {
    (void)user_data;
    printf("!!! MyService DIED !!!\n");
}

int main(void) {
    signal(SIGINT, signal_handler);

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(client, "127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(client);
        return 1;
    }

    /* 初始化 Proxy */
    myapp_MyService_proxy proxy;
    myapp_MyService_proxy_init(&proxy, runtime);
    myapp_MyService_proxy_connect(&proxy);

    /* RPC 调用 */
    myapp_Request req;
    myapp_Request_init(&req);
    req.id = 1;
    req.name = (char*)malloc(6);
    memcpy(req.name, "hello", 6);
    req.name_len = 5;

    myapp_Response resp;
    myapp_Response_init(&resp);
    if (myapp_MyService_proxy_handle_request(&proxy, &req, &resp) == 0) {
        printf("Response: code=%d message=%.*s\n", resp.code, resp.message_len, resp.message);
    }
    myapp_Request_destroy(&req);
    myapp_Response_destroy(&resp);

    /* 订阅广播 */
    myapp_MyService_proxy_subscribe_status_update(&proxy, on_status_update, NULL);

    /* 订阅死亡通知 */
    myapp_MyService_proxy_on_service_died(&proxy, on_service_died, NULL);

    while (g_running) {
        omni_runtime_poll_once(client, 100);
    }

    omni_runtime_stop(client);
    omni_runtime_destroy(client);
    return 0;
}
```

#### 编译和运行

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$OMNIBINDER_DIR
make -j$(nproc)

# 运行
$OMNIBINDER_DIR/bin/service_manager &
./my_server &
./my_client
```

### 10.5 不使用 CMake 的场景

如果项目不使用 CMake，可以手动调用 `omni-idlc` 生成代码，然后直接编译链接。

#### C++ 项目

```bash
# 1. 生成代码
$OMNIBINDER_DIR/bin/omni-idlc --lang=cpp --output=. my_service.bidl

# 2. 编译服务端
g++ -std=c++11 -I$OMNIBINDER_DIR/include \
    server.cpp my_service.bidl.cpp \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lpthread -lrt \
    -o my_server

# 3. 编译客户端
g++ -std=c++11 -I$OMNIBINDER_DIR/include \
    client.cpp my_service.bidl.cpp \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lpthread -lrt \
    -o my_client
```

#### C 项目

```bash
# 1. 生成代码
$OMNIBINDER_DIR/bin/omni-idlc --lang=c --output=. my_service.bidl

# 2. 编译服务端（注意：链接时需要 -lstdc++ 因为 omnibinder 库是 C++ 实现）
gcc -std=c99 -I$OMNIBINDER_DIR/include \
    server.c my_service.bidl.c \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lstdc++ -lpthread -lrt \
    -o my_server

# 3. 编译客户端
gcc -std=c99 -I$OMNIBINDER_DIR/include \
    client.c my_service.bidl.c \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lstdc++ -lpthread -lrt \
    -o my_client
```

### 10.6 C++ 与 C 对比速查表

| 功能 | C++ | C |
|------|-----|---|
| 头文件 | `#include "xxx.bidl.h"` | `#include "xxx.bidl_c.h"` |
| 库链接 | `OmniBinder::omnibinder_static` | 同左（需声明 CXX 语言） |
| 实现服务 | 继承 `XxxStub`，重写虚函数 | 实现生成的 `xxx_impl_*` 接口，并调用 `xxx_stub_create(user_data)` |
| 调用服务 | `XxxProxy proxy(client)` | `xxx_proxy proxy; xxx_proxy_init(&proxy, runtime)` |
| RPC 调用 | `proxy.Method(req, &resp)` | `xxx_proxy_method(&proxy, &req, &resp)` |
| 广播 | `stub.BroadcastTopic(msg)` | `xxx_broadcast_topic(client, &msg)` |
| 订阅 | `proxy.SubscribeTopic(lambda)` | `xxx_proxy_subscribe_topic(&proxy, callback, ud)` |
| 死亡通知 | `proxy.OnServiceDied(lambda)` | `xxx_proxy_on_service_died(&proxy, callback, ud)` |
| 字符串 | `std::string` | `char*` + `_len`，需手动 `malloc/free` |
| 结构体生命周期 | 自动（RAII） | 手动 `_init()` / `_destroy()` |
