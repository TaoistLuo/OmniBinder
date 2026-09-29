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
> `examples/artifact_examples/`。该目录是独立 CMake 工程，通过
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

struct NameValue {
    string name;
    int32  value;
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
mkdir -p generated/

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

add_executable(example_cpp_sensor_server sensor_server.cpp)
target_link_libraries(example_cpp_sensor_server PRIVATE omnibinder_static)

# IDL 代码生成（C++）
if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_cpp_sensor_server
        LANGUAGE cpp
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
endif()
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

add_executable(example_cpp_sensor_client sensor_client.cpp)
target_link_libraries(example_cpp_sensor_client PRIVATE omnibinder_static)

# 客户端使用同一份 IDL 生成的 Proxy
if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_cpp_sensor_client
        LANGUAGE cpp
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
endif()
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

bool bool_out = false;
proxy.EchoBool(false, bool_out);

int32_t int_out = 0;
proxy.EchoInt32(32, int_out);

common::StatusResponse status_out;
proxy.EchoStatus(status, status_out);

demo::SensorEnvelope envelope_out;
proxy.EchoEnvelope(envelope, envelope_out);

std::vector<int32_t> ids_out;
proxy.EchoIdArray(ids, ids_out);

demo::SensorArrayBundle bundle_out;
proxy.EchoBundle(bundle, bundle_out);

proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) { ... });
proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& msg) { ... });

demo::AsyncRequest req;
req.request_id = 42;
req.client_tag = "cpp-client";
common::StatusResponse ack;
proxy.RequestLatestDataAsync(req, ack);
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

if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_c_sensor_server
        LANGUAGE c
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
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
- `service.BroadcastSensorUpdate(msg)` → `demo_SensorService_broadcast_sensor_update(runtime, &msg)`
- `std::string` → `char*` + `_len` 字段，需要手动用 `omni_malloc()` / `omni_free()` 管理
- 结构体需要 `_init()` / `_destroy()` 管理生命周期
- 内存申请/释放的完整规则（谁分配、谁释放、借用有效期、常见错误）见 [内存使用说明](memory-guide.md)

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
- `proxy.GetLatestData(data)` 使用 C++ 输出引用 → `demo_SensorService_proxy_get_latest_data(&proxy, &data)` 使用 C 输出指针
- `proxy.SubscribeSensorUpdate([](auto& msg){...})` → `demo_SensorService_proxy_subscribe_sensor_update(&proxy, callback, user_data)`
- 所有结构体需要 `_init()` 初始化、`_destroy()` 释放

## 7. omni-cli 使用示例

### 7.1 查询所有在线服务

```bash
$ ./build/target/bin/omni-cli list
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            (default)            a1b2c3d4e5f6                     127.0.0.1:8001        ONLINE

Total: 1 service instances online
```

### 7.2 查询服务详细信息

**基础模式（显示类型名）：**
```bash
$ ./build/target/bin/omni-cli info SensorService
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            (default)            a1b2c3d4e5f6                     127.0.0.1:8001        ONLINE

Instance: instance_id="" (default) host_id="a1b2c3d4e5f6" endpoint=127.0.0.1:8001

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      - EchoBool(bool) -> bool  (id=0x8889447c)
      - EchoInt8(int8_t) -> int8_t  (id=0x80e488bf)
      - EchoUInt8(uint8_t) -> uint8_t  (id=0x154e42fc)
      - EchoInt16(int16_t) -> int16_t  (id=0xded4e5f6)
      - EchoUInt16(uint16_t) -> uint16_t  (id=0xc6499dc3)
      - EchoInt32(int32_t) -> int32_t  (id=0xead97608)
      - EchoUInt32(uint32_t) -> uint32_t  (id=0x5a4f03ed)
      - EchoInt64(int64_t) -> int64_t  (id=0x5ed1dddf)
      - EchoUInt64(uint64_t) -> uint64_t  (id=0x564231ae)
      - EchoFloat32(float) -> float  (id=0xe6a4ec77)
      - EchoFloat64(double) -> double  (id=0x4e98c43c)
      - EchoString(std::string) -> std::string  (id=0x721b4145)
      - EchoBytes(std::vector<uint8_t>) -> std::vector<uint8_t>  (id=0x27327787)
      - EchoStatus(common::StatusResponse) -> common::StatusResponse  (id=0xfba0408e)
      - EchoConfig(SensorConfig) -> SensorConfig  (id=0x2b5df382)
      - EchoEnvelope(SensorEnvelope) -> SensorEnvelope  (id=0x37a58ab8)
      - EchoIdArray(std::vector<int32_t>) -> std::vector<int32_t>  (id=0x22c9b2f2)
      - EchoLabelArray(std::vector<std::string>) -> std::vector<std::string>  (id=0x05410ce3)
      - EchoSensorArray(std::vector<SensorData>) -> std::vector<SensorData>  (id=0x9991eadb)
      - EchoBundle(SensorArrayBundle) -> SensorArrayBundle  (id=0xa4e21414)
      - GetLatestData() -> SensorData  (id=0x61564c6c)
      - SetThreshold(ControlCommand) -> common::StatusResponse  (id=0x2214744e)
      - ResetSensor(int32_t) -> void  (id=0x7f2b95ee)
      - GetSensorCount() -> int32_t  (id=0xd1719a20)
      - RequestLatestDataAsync(AsyncRequest) -> common::StatusResponse  (id=0xad1829b3)
```

**详细模式（展开字段定义）：**
```bash
$ ./build/target/bin/omni-cli --idl examples/sensor_service.bidl info SensorService
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            (default)            a1b2c3d4e5f6                     127.0.0.1:8001        ONLINE

Instance: instance_id="" (default) host_id="a1b2c3d4e5f6" endpoint=127.0.0.1:8001

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      - EchoBool(bool) -> bool  (id=0x8889447c)
      - EchoInt32(int32_t) -> int32_t  (id=0xead97608)
      - GetLatestData() -> SensorData  (id=0x61564c6c)
          return:           {
            sensor_id: int32
            temperature: float64
            humidity: float64
            timestamp: int64
            location: string
          }
      - SetThreshold(ControlCommand) -> common::StatusResponse  (id=0x2214744e)
          param:           {
            command_type: int32
            target: string
            value: int32
          }
          return:           {
            code: int32
            message: string
          }
      - ResetSensor(int32_t) -> void  (id=0x7f2b95ee)
      ...（其余方法同样逐条列出；带 --idl 时结构体参数/返回值按字段展开）
```

### 7.3 调用服务接口

`omni-cli` 当前规则：

- 带 `--idl` 时，`call` 的参数使用 **JSON 输入**
- 不带 `--idl` 时，只能传原始十六进制请求体
- 当前 JSON I/O 支持：
  - 基础类型（bool/int/float/string）
  - 普通 struct
  - 嵌套 struct
  - 无参返回 struct
  - 不含 `bytes` 的数组及嵌套复杂 struct
- 当前 JSON codec 不支持 `bytes`，因此也不支持包含 `bytes` 的数组或 struct；struct 输入必须包含全部声明字段

#### 7.3.1 通用格式

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService <Method> <参数>
```

#### 7.3.2 每个接口的调用方式

**无参 RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetLatestData
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetSensorCount
```

**基础类型 RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoBool false
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt8 7
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt8 7
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt16 16
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt16 16
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt32 32
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt32 32
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt64 64
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt64 64
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat32 1.5
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat64 2.5
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoString hello
```

> 注意：字符串基础类型使用原始命令行参数，例如 `EchoString hello`；保留内部双引号会把引号本身作为字符串内容。

**普通 struct RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoStatus '{"code":7,"message":"demo"}'
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoConfig '{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"}'
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService SetThreshold '{"command_type":1,"target":"temperature","value":30}'
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService RequestLatestDataAsync '{"request_id":42,"client_tag":"cli"}'
```

**嵌套 struct RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoEnvelope '{"data":{"sensor_id":10,"temperature":18.5,"humidity":45.5,"timestamp":123456789,"location":"Lab-1"},"config":{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"},"captured_at":{"seconds":123456789,"nanos":321}}'
```

**单向 RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService ResetSensor 1
```

**当前不建议使用 `omni-cli` 调用的接口**

以下接口包含 `bytes`，当前 JSON codec 会拒绝；其他不含 `bytes` 的数组可直接用 `[` 开头的 JSON 参数调用：

- `EchoBytes(bytes value)`
- `EchoBundle(SensorArrayBundle value)`

这些接口请使用 `examples/example_cpp/sensor_client.cpp` 或 `examples/example_c/sensor_client.c` 验证。

#### 7.3.3 查询服务信息

```bash
./build/target/bin/omni-cli list
./build/target/bin/omni-cli info SensorService
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl info SensorService
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
[INFO][ServiceManager] Listening on 0.0.0.0:9900
```

**终端2：启动 C++ 服务端**
```bash
$ ./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900 --register-host 192.168.1.10
=== SensorService (C++ Server) Starting ===
ServiceManager: 127.0.0.1:9900
RegisterHost: 192.168.1.10
SensorService registered on port <port> (host_id=<host_id>, instance_id=)
Demo service is ready. Press Ctrl+C to stop.

[SensorService] GetLatestData: temp=25.0 humidity=60.0
[Broadcast] temp=25.32 humidity=60.15
[SensorService] GetLatestData: temp=25.32 humidity=60.15
[Broadcast] temp=25.48 humidity=59.87
...
```
> 服务端每轮广播前会先调用 `GetLatestData()` 打印当前值（`[SensorService] GetLatestData:`），再打印 `[Broadcast]`。

**终端3：启动 C++ 客户端**
```bash
$ ./target/example/example_cpp_sensor_client --sm-host 127.0.0.1 --sm-port 9900
=== SensorClient (C++ Client) Starting ===
ServiceManager: 127.0.0.1:9900
Discovered 1 SensorService instance(s):
  [0] host_id=<host_id> instance_id= host=127.0.0.1 port=<port>
Connected to SensorService

--- Primitive RPCs ---
EchoBool(false) => 1
EchoInt8(7) => 8
EchoUInt8(7) => 8
EchoInt16(16) => 18
EchoUInt16(16) => 18
EchoInt32(32) => 35
EchoUInt32(32) => 35
EchoInt64(64) => 68
EchoUInt64(64) => 68
EchoFloat32(1.5) => 2.00
EchoFloat64(2.5) => 2.75
EchoString(hello) => hello_echo
EchoBytes([3 bytes]) => size=4

--- Custom / Nested Struct RPCs ---
EchoStatus => code=107 message=demo_echo
EchoConfig => enabled=0 rate=30 label=sensor-main_server
EchoEnvelope => location=Lab-1 nested_label=sensor-main_nested nanos=322

--- Array RPCs ---
EchoIdArray => size=3 tail=2
EchoLabelArray => size=3 tail=tail
EchoSensorArray => first.location=Lab-1_echo
EchoBundle => ids=3 labels=3 blobs=3 samples=2

--- Existing RPCs ---
GetLatestData => sensor_id=1 temp=25.32 humidity=60.15 location=Room-A
SetThreshold => code=0 message=OK
GetSensorCount => 3

--- Topic subscriptions ---

--- Async RPC via topic reply ---
RequestLatestDataAsync => code=0 message=accepted

Waiting for broadcasts / async callbacks (Ctrl+C to stop)...

[SensorUpdate] id=1 temp=25.48 humidity=59.87
[AsyncResultReady] request_id=42 tag=cpp-client code=0 temp=25.48
...
```
> 各 `Echo*` 返回值由示例服务端实现决定（如 `EchoInt32` 返回 `value + 3`）；上例数值仅示意格式，实际随广播数据变化。

说明：客户端会持续等待广播，直到用户手动中断。自动化验证时可使用 `timeout 10s ...` 方式运行。

**终端4：使用 omni-cli**
```bash
# 查询服务列表（端口为运行时动态分配，以下仅示例）
$ ./target/bin/omni-cli list
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            (default)            <host_id>                        127.0.0.1:<dynamic>   ONLINE

Total: 1 service instances online

# 查看服务信息（基础模式）
$ ./target/bin/omni-cli info SensorService
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            (default)            <host_id>                        127.0.0.1:<dynamic>   ONLINE

Instance: instance_id="" (default) host_id="<host_id>" endpoint=127.0.0.1:<dynamic>

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      ...

# 调用接口（JSON 模式）
$ ./target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData
No instance selector given; using deterministic first instance: instance_id="" (default) host_id="<host_id>" endpoint=127.0.0.1:<dynamic>
  Hint: use --instance-id <id> [--host-id <id>] to target another instance.
Calling SensorService.GetLatestData() ...
  interface_id = 0xa112646d
  method_id    = 0x61564c6c
Response (status=OK, 38 bytes, 0.85 ms):
  {
    "humidity": 60.15,
    "location": "Room-A",
    "sensor_id": 1,
    "temperature": 25.32,
    "timestamp": 1707321600
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
| 双通道初始化 | 服务同时创建 TCP listener + SHM UDS listener，SM 返回 host_id/shm_config |
| SHM 自动选择 | 同机客户端自动创建 per-client SHM 并通过 UDS 握手 |
| 多客户端独立 SHM | 3 个客户端并发 + 5 个客户端顺序使用各自独立 SHM ring |
| 广播/订阅 | 发布者通过 SHM 广播数据到订阅者 |
| 死亡通知 | 服务进程崩溃后收到死亡回调 |
| 生命周期 | 服务注销后 SM 查询不到 |

### 8.4 验证死亡通知

在终端2中按 `Ctrl+C` 杀掉 C++ 服务端，观察终端3的输出：

```
[SensorUpdate] id=1 temp=25.15 humidity=59.78

!!! [DeathNotify] SensorService has DIED !!!

```

同时 ServiceManager（终端1）会输出：

```
[WARN][HeartbeatMonitor] Service SensorService timed out (missed 1 heartbeats)
[WARN][ServiceManager] Service timed out: SensorService (host_id=<host_id>, instance_id=)
[INFO][DeathNotifier] Instance SensorService (host_id=<host_id>, instance_id=) died, name_gone=1, notifying 1 subscribers
```

### 8.5 单板运行两个同名服务实例

要在同一块板上运行多个**同名**服务，必须为每个实例声明**不同的 `instance_id`**：实例身份为 `(service_name, host_id, instance_id)`，相同身份会被视为同一实例而互相顶替（后来者接管）。下面以 C++ 示例演示两个 `SensorService` 实例。

**终端1：启动 ServiceManager**
```bash
$ ./target/bin/service_manager --port 9900
```

**终端2：启动实例 A（`--instance-id ttyUSB0`）**
```bash
$ ./target/example/example_cpp_sensor_server --instance-id ttyUSB0
=== SensorService (C++ Server) Starting ===
ServiceManager: 127.0.0.1:9900
InstanceId: ttyUSB0
SensorService registered on port <A> (host_id=<host>, instance_id=ttyUSB0)
```

**终端3：启动实例 B（`--instance-id ttyUSB1`）**
```bash
$ ./target/example/example_cpp_sensor_server --instance-id ttyUSB1
...
SensorService registered on port <B> (host_id=<host>, instance_id=ttyUSB1)
```

此时 ServiceManager（终端1）会输出两条注册日志，两个实例并存：

```
Registered service: SensorService (handle=..., fd=..., host=127.0.0.1, port=<A>, host_id=<host>, instance_id=ttyUSB0)
Registered service: SensorService (handle=..., fd=..., host=127.0.0.1, port=<B>, host_id=<host>, instance_id=ttyUSB1)
```

> 若两个实例都使用默认空 `instance_id`，第二条日志会变成
> `Service instance taken over by newer registration: SensorService (... instance_id=)`，
> 即后者顶替前者，registry 中只剩一个实例。
>
> **已知边界（顶替 churn）**：顶替是针对**相同身份** `(service_name, host_id, instance_id)` 的。
> 如果两个**都还在运行**的进程声明了完全相同的身份，SM 会让后来者接管并关闭先来者的控制连接；
> 被顶替的活实例会自动重连并重新注册，于是两者可能反复互相顶替（takeover churn）。
> 这是已接受的边界行为，不是缺陷——运维必须保证同板同名服务各自声明**不同**的 `instance_id`，
> 绝不能让两个活实例使用完全相同的身份。

**终端4：客户端先列出实例，再绑定指定实例**
```bash
$ ./target/example/example_cpp_sensor_client --instance ttyUSB1
=== SensorClient (C++ Client) Starting ===
ServiceManager: 127.0.0.1:9900
Discovered 2 SensorService instance(s):
  [0] host_id=<host> instance_id=ttyUSB0 host=127.0.0.1 port=<A>
  [1] host_id=<host> instance_id=ttyUSB1 host=127.0.0.1 port=<B>
Connected to SensorService instance_id=ttyUSB1
...
```

不带 `--instance` 时客户端按服务名连接，命中 SM 注册表实例键字典序第一实例。

C 示例参数一致（服务端 `--instance-id`、客户端 `--instance`），底层走运行时实例 API：

```c
/* 服务端：注册前声明实例标识 */
omni_service_t* svc = demo_SensorService_stub_create(runtime);
omni_service_set_instance_id(svc, "ttyUSB0");
omni_runtime_register_service(runtime, svc);

/* 客户端：枚举实例后绑定指定实例 */
omni_runtime_list_instances(runtime, "SensorService", on_instance, &lookup);
demo_SensorService_proxy_connect_instance(&proxy, lookup.found_host_id, lookup.found_instance_id);
```

> 跨板部署时 `host_id` 会随实例身份一起参与寻址，请确保每块板 `host_id` 唯一：克隆镜像会共享 `/etc/machine-id`，需在启动时通过 `setHostId()` 显式设置各设备唯一标识（MAC / 芯片 ID / 序列号）。

## 9. 跨板通信示例

当服务端和客户端运行在不同机器上时，需要同时区分两类地址：

- `--sm-host` / `runtime.init(sm_host, sm_port)`：当前进程连接哪个 `ServiceManager`
- `setRegisterHost(...)`：服务注册到 `ServiceManager` 时写入的可达地址

跨机调用时，客户端最终连接的是服务注册到 `ServiceManager` 的 `ServiceInfo.host`，
而不是服务自己连接 `ServiceManager` 时使用的 `sm_host`。

**机器1（192.168.1.10）：运行 ServiceManager 和服务端**
```bash
# 启动 SM（监听所有接口）
$ ./target/bin/service_manager --host 0.0.0.0 --port 9900

# 启动服务端（连接本机 SM，但显式把 192.168.1.10 注册给其它机器访问）
$ ./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900 --register-host 192.168.1.10
```

```cpp
omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);
runtime.setRegisterHost("192.168.1.10");

MySensorService service;
runtime.registerService(&service);
```

**机器2（192.168.1.11）：运行客户端**
```bash
# 客户端连接到机器1的 SM
$ ./target/example/example_cpp_sensor_client --sm-host 192.168.1.10 --sm-port 9900
```

框架会自动检测到两个服务不在同一台机器上（host_id 不同），自动使用 TCP 传输而非共享内存。
其中客户端拿到的目标地址来自服务注册时写入的 `registerHost`。
同步 `invoke()` 在 TCP 路径上会先把完整请求写入 socket，再开始等待 reply。
因此如果跨机链路存在明显背压，大请求可能先在发送阶段触发 timeout，
这时更应该检查链路带宽、对端读取速度和单次请求体积，而不是把问题理解为 reply timeout。
日志中会显示：

```
[INFO][TcpConnection] Connected to 192.168.1.10:<port> (fd=<n>)
[INFO][TransportSelector] Connected to SensorService via TCP at 192.168.1.10:<port> (fd=<n>)
```

如果在同一台机器上运行，每个客户端会创建自己独立的共享内存，并通过 UDS 与服务端交换 eventfd：

```
[INFO][TransportSelector] Connected to SensorService via SHM (same machine)
[INFO][ShmClientConnection] Client connected: shm='/binder_SensorService_<hash>', event_fd=<n>, notify_fd=<n>
```

SHM 不可用时会自动回退到 TCP，并输出：

```
[WARN][TransportSelector] data_connect_fallback service=SensorService transport=SHM fallback=TCP reason=connect_failed
```

**注意**：per-client SHM 模型中，每个客户端创建独立的共享内存段，名称格式为
`/binder_<服务名前缀>_<FNV-1a 哈希>`（`host_id` 与 `instance_id` 参与哈希计算但不进入名称）。
客户端通过 UDS 将 SHM 名称发送给服务端，服务端回传两个 eventfd：
一个用于通知客户端响应已就绪，另一个供客户端通知服务端新请求到达。

每个客户端各自拥有独立的 SHM ring，互不共享资源，避免了同一共享内存段上的竞争条件。
此场景已在 `test_full_integration` 的 `ThreeClientsConcurrentInvoke` 测试中验证通过。

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
#   lib/       → libomnibinder.a, cmake/OmniBinder/
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
# 输出目录必须预先存在
mkdir -p generated/

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
    service.PublishStatusUpdate();

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

    myapp::MyServiceProxy proxy(runtime);
    if (proxy.connect() != 0) {
        fprintf(stderr, "MyService not found\n");
        return 1;
    }

    // RPC 调用
    myapp::Request req;
    req.id = 1;
    req.name = "hello";
    myapp::Response resp;
    proxy.HandleRequest(req, resp);
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
# 普通主机构建安装时使用 bin_host；交叉部署到目标板时通常使用 bin_cross。
$OMNIBINDER_DIR/bin_host/service_manager &
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

void myapp_MyService_impl_handle_request(const myapp_Request* req,
                                         myapp_Response* result, void* user_data) {
    (void)user_data;
    printf("[Server] HandleRequest: id=%d name=%.*s\n",
           req->id, req->name_len, req->name);
    result->code = 0;
    result->message = (char*)omni_malloc(3);
    memcpy(result->message, "OK", 3);
    result->message_len = 2;
}

void myapp_MyService_impl_notify(int32_t event_id, void* user_data) {
    (void)user_data;
    printf("[Server] Notify: event_id=%d\n", event_id);
}

int main(void) {
    signal(SIGINT, signal_handler);

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, "127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    /* 直接绑定生成的 impl 接口 */
    omni_service_t* svc = myapp_MyService_stub_create(runtime);
    omni_runtime_register_service(runtime, svc);
    omni_runtime_publish_topic(runtime, "StatusUpdate", myapp_StatusUpdate_TOPIC_IDL_HASH);

    printf("Server running (Ctrl+C to stop)...\n");
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
    }

    omni_runtime_unregister_service(runtime, svc);
    myapp_MyService_stub_destroy(svc);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
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
    if (omni_runtime_init(runtime, "127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
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
    req.name = (char*)omni_malloc(6);
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
        omni_runtime_poll_once(runtime, 100);
    }

    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
```

#### 编译和运行

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$OMNIBINDER_DIR
make -j$(nproc)

# 运行
$OMNIBINDER_DIR/bin_host/service_manager &
./my_server &
./my_client
```

### 10.5 不使用 CMake 的场景

如果项目不使用 CMake，可以手动调用 `omni-idlc` 生成代码，然后直接编译链接。

#### C++ 项目

```bash
# 1. 生成代码
$OMNIBINDER_DIR/bin_host/omni-idlc --lang=cpp --output=. my_service.bidl

# 2. 编译服务端（Linux 示例，其他平台调整编译器和链接选项）
${CXX:-g++} -std=c++11 -I$OMNIBINDER_DIR/include \
    server.cpp my_service.bidl.cpp \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lpthread -lrt \
    -o my_server

# 3. 编译客户端
${CXX:-g++} -std=c++11 -I$OMNIBINDER_DIR/include \
    client.cpp my_service.bidl.cpp \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lpthread -lrt \
    -o my_client
```

#### C 项目

```bash
# 1. 生成代码
$OMNIBINDER_DIR/bin_host/omni-idlc --lang=c --output=. my_service.bidl

# 2. 编译服务端（注意：链接时需要 -lstdc++ 因为 omnibinder 库是 C++ 实现）
${CC:-gcc} -std=c99 -I$OMNIBINDER_DIR/include \
    server.c my_service.bidl.c \
    -L$OMNIBINDER_DIR/lib -lomnibinder -lstdc++ -lpthread -lrt \
    -o my_server

# 3. 编译客户端
${CC:-gcc} -std=c99 -I$OMNIBINDER_DIR/include \
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
| 调用服务 | `XxxProxy proxy(runtime)` | `xxx_proxy proxy; xxx_proxy_init(&proxy, runtime)` |
| RPC 调用 | `proxy.Method(req, resp)` | `xxx_proxy_method(&proxy, &req, &resp)` |
| 广播 | `stub.BroadcastTopic(msg)` | `xxx_broadcast_topic(runtime, &msg)` |
| 订阅 | `proxy.SubscribeTopic(lambda)` | `xxx_proxy_subscribe_topic(&proxy, callback, ud)` |
| 死亡通知 | `proxy.OnServiceDied(lambda)` | `xxx_proxy_on_service_died(&proxy, callback, ud)` |
| 字符串 | `std::string` | `char*` + `_len`，手动 `omni_malloc()` / `omni_free()`（详见[内存使用说明](memory-guide.md)） |
| 结构体生命周期 | 自动（RAII） | 手动 `_init()` / `_destroy()` |
