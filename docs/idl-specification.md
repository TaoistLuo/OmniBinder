# OmniBinder IDL 语法规范

## 1. 概述

OmniBinder IDL（Interface Definition Language）使用类 protobuf 风格的语法来定义服务接口、
数据结构和广播话题。IDL 文件扩展名为 `.bidl`，通过 `omni-idlc` 编译器生成 C 和 C++ 代码。

## 2. 文件结构

一个 `.bidl` 文件的基本结构：

```
// 文件注释

package <包名>;

import "path/to/dependency.bidl";   // 可选，导入其他 IDL 文件

// 结构体定义
struct <名称> {
    <类型> <字段名>;
    ...
}

// 话题定义
topic <名称> {
    <类型> <字段名>;
    ...
}

// 服务定义
service <名称> {
    <返回类型> <方法名>(<参数类型> <参数名>);
    ...
    publishes <话题名>;
    ...
}
```

### 2.1 导入声明（import）

`import` 语句必须出现在 `package` 声明之后、任何类型定义之前。

#### 2.1.1 语法

```
import "<路径>";
```

#### 2.1.2 路径解析规则

- **相对路径**：相对于当前 `.bidl` 文件所在目录解析
  ```
  import "common_types.bidl";          // 同目录
  import "../shared/common.bidl";      // 上级目录
  import "sub/types.bidl";             // 子目录
  ```
- **绝对路径**：直接使用指定的绝对路径
  ```
  import "/opt/idl/common_types.bidl"; // 绝对路径
  ```

#### 2.1.3 跨包类型引用

导入文件后，使用 `包名.类型名` 语法引用被导入包中的类型：

```
package demo;

import "common_types.bidl";   // common_types.bidl 中 package 为 common

struct MapData {
    common.Point center;       // 跨包引用：包名.类型名
    float32 zoom_level;
}

service MapService {
    common.StatusResponse UpdateLocation(common.Point p);
}
```

#### 2.1.4 代码生成映射

跨包类型在生成代码中的映射：

| IDL 类型 | C++ 生成代码 | C 生成代码 |
|----------|-------------|-----------|
| `common.Point` | `common::Point` | `common_Point` |
| `common.StatusResponse` | `common::StatusResponse` | `common_StatusResponse` |

生成的头文件会自动包含被导入文件的头文件：

- C++: `#include "common_types.bidl.h"`
- C: `#include "common_types.bidl_c.h"`

#### 2.1.5 规则与限制

- 不允许循环导入（A import B，B import A）
- 同一文件不会被重复加载（多个文件 import 同一依赖时只解析一次）
- 不同文件的 package 名必须唯一（不允许两个文件声明相同的 package）
- 编译主文件时，所有被导入的文件也会自动生成对应的代码文件

## 3. 词法规则

### 3.1 注释

```
// 单行注释

/* 多行注释
   可以跨越多行 */
```

### 3.2 标识符

- 由字母、数字、下划线组成
- 必须以字母或下划线开头
- 区分大小写
- 推荐命名风格：
  - 结构体/服务/话题名：PascalCase（如 `SensorData`）
  - 字段名/方法名：snake_case（如 `sensor_id`）或 camelCase（如 `sensorId`）

### 3.3 关键字

以下为保留关键字，不能用作标识符：

```
package  import   struct  service  topic  publishes
bool     int8     uint8   int16    uint16
int32    uint32   int64   uint64
float32  float64  string  bytes
array
```

## 4. 类型系统

### 4.1 基础类型

| IDL 类型 | 说明 | C 类型 | C++ 类型 | 大小 |
|----------|------|--------|----------|------|
| `bool` | 布尔值 | `uint8_t` | `bool` | 1 byte |
| `int8` | 有符号8位整数 | `int8_t` | `int8_t` | 1 byte |
| `uint8` | 无符号8位整数 | `uint8_t` | `uint8_t` | 1 byte |
| `int16` | 有符号16位整数 | `int16_t` | `int16_t` | 2 bytes |
| `uint16` | 无符号16位整数 | `uint16_t` | `uint16_t` | 2 bytes |
| `int32` | 有符号32位整数 | `int32_t` | `int32_t` | 4 bytes |
| `uint32` | 无符号32位整数 | `uint32_t` | `uint32_t` | 4 bytes |
| `int64` | 有符号64位整数 | `int64_t` | `int64_t` | 8 bytes |
| `uint64` | 无符号64位整数 | `uint64_t` | `uint64_t` | 8 bytes |
| `float32` | 32位浮点数 | `float` | `float` | 4 bytes |
| `float64` | 64位浮点数 | `double` | `double` | 8 bytes |
| `string` | 字符串 | `char*` + `uint32_t` | `std::string` | 变长 |
| `bytes` | 字节数组 | `uint8_t*` + `uint32_t` | `std::vector<uint8_t>` | 变长 |

### 4.2 数组类型

使用 `array<T>` 语法定义数组：

```
struct Example {
    array<int32>      ids;          // 整数数组
    array<string>     names;        // 字符串数组
    array<SensorData> sensors;      // 结构体数组
}
```

| IDL 类型 | C 类型 | C++ 类型 |
|----------|--------|----------|
| `array<T>` | `T*` + `uint32_t count` | `std::vector<T>` |

### 4.3 自定义结构体类型

在同一个 package 内定义的 struct 可以作为字段类型使用：

```
struct Point {
    float32 x;
    float32 y;
}

struct Line {
    Point start;    // 使用自定义结构体作为字段类型
    Point end;
}
```

**限制**：不支持循环引用（结构体 A 包含 B，B 又包含 A）。

## 5. 结构体定义（struct）

### 5.1 语法

```
struct <名称> {
    <类型> <字段名>;
    <类型> <字段名>;
    ...
}
```

### 5.2 示例

```
struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    int64   timestamp;
    string  location;
    bytes   raw_data;
}

struct DeviceInfo {
    string          name;
    string          model;
    uint32          firmware_version;
    array<string>   capabilities;
}
```

### 5.3 生成的 C++ 代码

```cpp
namespace demo {

struct SensorData {
    int32_t sensor_id;
    double temperature;
    double humidity;
    int64_t timestamp;
    std::string location;
    std::vector<uint8_t> raw_data;

    SensorData();  // 默认构造函数（所有字段零初始化）

    // 序列化到 Buffer
    bool serialize(omnibinder::Buffer& buf) const;

    // 从 Buffer 反序列化
    bool deserialize(omnibinder::Buffer& buf);

    // 计算序列化后的大小
    size_t serializedSize() const;
};

} // namespace demo
```

### 5.4 生成的 C 代码

```c
typedef struct demo_SensorData {
    int32_t sensor_id;
    double temperature;
    double humidity;
    int64_t timestamp;
    char* location;
    uint32_t location_len;
    uint8_t* raw_data;
    uint32_t raw_data_len;
} demo_SensorData;

/* 初始化（零初始化所有字段） */
void demo_SensorData_init(demo_SensorData* self);

/* 释放动态分配的内存 */
void demo_SensorData_destroy(demo_SensorData* self);

/* 序列化 */
int demo_SensorData_serialize(const demo_SensorData* self,
                               omnibinder_buffer_t* buf);

/* 反序列化 */
int demo_SensorData_deserialize(demo_SensorData* self,
                                 omnibinder_buffer_t* buf);
```

## 6. 话题定义（topic）

话题用于广播/订阅模式，定义广播消息的数据结构。

### 6.1 语法

```
topic <名称> {
    <类型> <字段名>;
    ...
}
```

### 6.2 示例

```
topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic SystemAlert {
    int32  alert_level;
    string message;
    string source;
}
```

### 6.3 说明

- topic 在序列化层面与 struct 完全相同
- topic 的特殊之处在于它会在 service 中通过 `publishes` 声明
- 每个 topic 会生成一个唯一的 topic_id（基于包名+话题名的哈希值）

## 7. 服务定义（service）

### 7.1 语法

```
service <名称> {
    // 方法定义
    <返回类型> <方法名>(<参数类型> <参数名>);

    // 无参数方法
    <返回类型> <方法名>();

    // 无返回值方法（单向调用）
    void <方法名>(<参数类型> <参数名>);

    // 声明发布的话题
    publishes <话题名>;
}
```

### 7.2 示例

```
service SensorService {
    // 请求-响应方法
    StatusResponse SetThreshold(ControlCommand cmd);
    SensorData     GetLatestData();

    // 单向调用（无返回值）
    void ResetSensor(int32 sensor_id);

    // 声明发布的话题
    publishes SensorUpdate;
    publishes SystemAlert;
}
```

### 7.3 方法规则

- 每个方法最多一个参数（如需多个参数，定义一个 struct 包装）
- 返回类型可以是 `void`（单向调用，不等待响应）
- 返回类型可以是基础类型或自定义 struct
- 参数类型可以是基础类型或自定义 struct

### 7.4 生成的 C++ 代码 — Stub（服务端骨架）

```cpp
namespace demo {

class SensorServiceStub : public omnibinder::Service {
public:
    SensorServiceStub();
    virtual ~SensorServiceStub();

    // ---- 用户需要实现的纯虚函数 ----
    virtual StatusResponse SetThreshold(const ControlCommand& cmd) = 0;
    virtual SensorData GetLatestData() = 0;
    virtual void ResetSensor(int32_t sensor_id) = 0;

    // ---- 广播方法（框架提供） ----
    void BroadcastSensorUpdate(const SensorUpdate& msg);
    void BroadcastSystemAlert(const SystemAlert& msg);

    // ---- 服务名和接口信息（框架使用） ----
    const char* serviceName() const override;
    const omnibinder::InterfaceInfo& interfaceInfo() const override;

protected:
    // 框架内部调用，分发请求到对应方法
    void onInvoke(uint32_t method_id,
                  const omnibinder::Buffer& request,
                  omnibinder::Buffer& response) override;
};

} // namespace demo
```

### 7.5 生成的 C++ 代码 — Proxy（客户端代理）

```cpp
namespace demo {

class SensorServiceProxy {
public:
    explicit SensorServiceProxy(omnibinder::OmniRuntime& runtime);
    ~SensorServiceProxy();

    // 连接到远程服务（通过 ServiceManager 查询地址后直连）
    int connect();

    // 断开连接
    void disconnect();

    // 检查连接状态
    bool isConnected() const;

    // ---- 远程调用方法 ----
    StatusResponse SetThreshold(const ControlCommand& cmd);
    SensorData GetLatestData();
    void ResetSensor(int32_t sensor_id);

    // ---- 订阅广播话题 ----
    void SubscribeSensorUpdate(
        const std::function<void(const SensorUpdate&)>& callback);
    void SubscribeSystemAlert(
        const std::function<void(const SystemAlert&)>& callback);

    // ---- 订阅死亡通知 ----
    void OnServiceDied(const std::function<void()>& callback);

private:
    omnibinder::OmniRuntime& runtime_;
    omnibinder::ServiceConnection* connection_;
    // ...
};

} // namespace demo
```

### 7.6 生成的 C 代码

```c
/* ---- Stub 回调函数类型 ---- */
typedef demo_StatusResponse (*demo_SensorService_SetThreshold_fn)(
    void* user_data, const demo_ControlCommand* cmd);
typedef demo_SensorData (*demo_SensorService_GetLatestData_fn)(
    void* user_data);
typedef void (*demo_SensorService_ResetSensor_fn)(
    void* user_data, int32_t sensor_id);

/* Stub 回调表 */
typedef struct demo_SensorService_callbacks {
    demo_SensorService_SetThreshold_fn  on_set_threshold;
    demo_SensorService_GetLatestData_fn on_get_latest_data;
    demo_SensorService_ResetSensor_fn   on_reset_sensor;
    void* user_data;
} demo_SensorService_callbacks;

/* 注册服务 */
int demo_SensorService_register(
    omni_runtime_t* runtime,
    const demo_SensorService_callbacks* callbacks);

/* 广播 */
int demo_SensorService_broadcast_SensorUpdate(
    omni_runtime_t* runtime,
    const demo_SensorUpdate* msg);

/* ---- Proxy 函数 ---- */
typedef struct demo_SensorService_proxy demo_SensorService_proxy;

demo_SensorService_proxy* demo_SensorService_proxy_create(
    omni_runtime_t* runtime);
void demo_SensorService_proxy_destroy(demo_SensorService_proxy* proxy);
int demo_SensorService_proxy_connect(demo_SensorService_proxy* proxy);

/* 远程调用 */
int demo_SensorService_proxy_SetThreshold(
    demo_SensorService_proxy* proxy,
    const demo_ControlCommand* cmd,
    demo_StatusResponse* result);

int demo_SensorService_proxy_GetLatestData(
    demo_SensorService_proxy* proxy,
    demo_SensorData* result);
```

## 8. ID 生成规则

### 8.1 interface_id

基于服务名生成，使用 FNV-1a 32位哈希：

```
interface_id = fnv1a_32(package_name + "." + service_name)
```

### 8.2 method_id

基于方法名生成：

```
method_id = fnv1a_32(method_name)
```

### 8.3 topic_id

基于话题名生成：

```
topic_id = fnv1a_32(package_name + "." + topic_name)
```

### 8.4 FNV-1a 哈希算法

```cpp
uint32_t fnv1a_32(const char* str) {
    uint32_t hash = 0x811c9dc5;  // FNV offset basis
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 0x01000193;      // FNV prime
    }
    return hash;
}
```

## 9. 完整 IDL 示例

### 9.1 基础类型文件

```
// common_types.bidl
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

### 9.2 使用 import 的服务文件

```
// sensor_service.bidl
// 传感器服务接口定义

package sensor;

import "common_types.bidl";

// ---- 数据结构 ----

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

struct SensorConfig {
    int32   sensor_id;
    float64 min_threshold;
    float64 max_threshold;
    int32   sample_interval_ms;
}

struct SensorList {
    array<SensorData> sensors;
    int32             total_count;
}

// ---- 广播话题 ----

topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic SensorAlert {
    int32   sensor_id;
    int32   alert_level;
    string  description;
    int64   timestamp;
}

// ---- 服务接口 ----

service SensorService {
    // 获取传感器数据
    SensorData     GetSensorData(int32 sensor_id);
    SensorList     GetAllSensors();

    // 控制命令（使用跨包类型）
    common.StatusResponse SetConfig(SensorConfig config);
    common.StatusResponse SendCommand(ControlCommand cmd);

    // 单向通知
    void ResetSensor(int32 sensor_id);

    // 广播话题
    publishes SensorUpdate;
    publishes SensorAlert;
}
```

## 10. omni-idlc 编译器使用

### 10.1 命令行用法

```bash
# 生成 C++ 代码
omni-idlc --lang=cpp --output=./generated sensor_service.bidl

# 生成 C 代码
omni-idlc --lang=c --output=./generated sensor_service.bidl

# 同时生成 C 和 C++ 代码
omni-idlc --lang=all --output=./generated sensor_service.bidl
```

### 10.2 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--lang=<cpp\|c\|all>` | 目标语言 | `cpp` |
| `--output=<dir>` | 输出目录 | 当前目录 |
| `--dep-file=<file>` | 生成 Makefile 格式依赖文件 | 不生成 |
| `--header-only` | 仅生成头文件（C++） | 否 |

### 10.3 生成的文件

```bash
# 输入: sensor_service.bidl
# --lang=cpp 输出:
#   sensor_service.bidl.h
#   sensor_service.bidl.cpp
#
# --lang=c 输出:
#   sensor_service.bidl.h
#   sensor_service.bidl.c
```

### 10.4 CMake 集成

```cmake
# 在 CMakeLists.txt 中使用
find_package(OmniBinder REQUIRED)

# 自动编译 IDL 文件并生成代码
omnic_generate(
    TARGET my_service
    LANGUAGE cpp
    FILES sensor_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_executable(my_service main.cpp)
target_link_libraries(my_service omnibinder)
```

`omnic_generate` CMake 函数会：
1. 在构建时自动调用 `omni-idlc` 编译 `.bidl` 文件
2. 将生成的源文件添加到目标的源文件列表
3. 将生成的头文件目录添加到目标的 include 路径
