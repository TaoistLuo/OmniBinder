# OmniBinder 使用示例

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

```protobuf
// examples/sensor_service.bidl
// 传感器服务接口定义

package demo;

import "common_types.bidl";  // 导入公共类型

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

topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

service SensorService {
    SensorData              GetLatestData();
    common.StatusResponse   SetThreshold(ControlCommand cmd);  // 使用导入的类型
    void                    ResetSensor(int32 sensor_id);

    publishes SensorUpdate;
}
```

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

### 3.2 sensor_server.cpp

```cpp
// examples/example_cpp/sensor_server.cpp
// C++ 传感器服务端 - 提供数据查询接口，定时广播传感器数据

#include "sensor_service.bidl.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <csignal>

// 全局标志，用于优雅退出
static volatile bool g_running = true;

void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// ============================================================
// 实现传感器服务
// 继承 IDL 生成的 SensorServiceStub，实现纯虚函数
// ============================================================
class MySensorService : public demo::SensorServiceStub {
public:
    MySensorService()
        : current_temperature_(25.0)
        , current_humidity_(60.0)
        , threshold_value_(0)
    {
    }

    // 实现 GetLatestData 接口
    demo::SensorData GetLatestData() override {
        demo::SensorData data;
        data.sensor_id = 1;
        data.temperature = current_temperature_;
        data.humidity = current_humidity_;
        data.timestamp = static_cast<int64_t>(time(NULL));
        data.location = "Room-A";

        printf("[SensorService] GetLatestData called: temp=%.1f, humidity=%.1f\n",
               data.temperature, data.humidity);
        return data;
    }

    // 实现 SetThreshold 接口（返回 common.StatusResponse）
    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override {
        printf("[SensorService] SetThreshold called: type=%d, target=%s, value=%d\n",
               cmd.command_type, cmd.target.c_str(), cmd.value);

        threshold_value_ = cmd.value;

        common::StatusResponse resp;  // 使用 common 包中的类型
        resp.code = 0;
        resp.message = "Threshold set successfully";
        return resp;
    }

    // 实现 ResetSensor 接口（单向调用，无返回值）
    void ResetSensor(int32_t sensor_id) override {
        printf("[SensorService] ResetSensor called: sensor_id=%d\n", sensor_id);
        current_temperature_ = 25.0;
        current_humidity_ = 60.0;
    }

    // 模拟传感器数据变化
    void updateSensorData() {
        // 模拟温度和湿度的随机波动
        current_temperature_ += (rand() % 100 - 50) / 100.0;
        current_humidity_ += (rand() % 100 - 50) / 100.0;

        // 限制范围
        if (current_temperature_ < -20.0) current_temperature_ = -20.0;
        if (current_temperature_ > 50.0) current_temperature_ = 50.0;
        if (current_humidity_ < 0.0) current_humidity_ = 0.0;
        if (current_humidity_ > 100.0) current_humidity_ = 100.0;
    }

    double temperature() const { return current_temperature_; }
    double humidity() const { return current_humidity_; }

private:
    double current_temperature_;
    double current_humidity_;
    int32_t threshold_value_;
};

// ============================================================
// 主函数
// ============================================================
int main(int argc, char* argv[]) {
    // 解析命令行参数
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) {
            sm_host = argv[++i];
        } else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = static_cast<uint16_t>(atoi(argv[++i]));
        }
    }

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    srand(static_cast<unsigned>(time(NULL)));

    printf("=== SensorService Starting ===\n");
    printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    // 1. 创建 OmniRuntime 并连接 ServiceManager
    omnibinder::OmniRuntime runtime;
    int ret = runtime.init(sm_host, sm_port);
    if (ret != 0) {
        printf("Failed to connect to ServiceManager: %s\n",
               omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }
    printf("Connected to ServiceManager\n");

    // 2. 创建并注册服务
    //    registerService() 内部会：
    //    a) 创建 TCP 监听（端口自动分配）
    //    b) 创建共享内存 "/binder_SensorService"（多客户端共享）
    //    c) 创建 eventfd（1 个 req_eventfd + 32 个 resp_eventfd）
    //    d) 创建 UDS 监听 "/tmp/binder_binder_SensorService.sock"（用于 eventfd 交换）
    //    e) 将 {name, host, port, host_id, shm_name} 注册到 SM
    MySensorService service;
    ret = runtime.registerService(&service);
    if (ret != 0) {
        printf("Failed to register service: %s\n",
               omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }
    printf("SensorService registered successfully\n");

    // 3. 主循环：定时广播传感器数据
    printf("Starting broadcast loop (Ctrl+C to stop)...\n\n");

    while (g_running) {
        // 处理事件（非阻塞，100ms 超时）
        runtime.pollOnce(100);

        // 模拟传感器数据更新
        service.updateSensorData();

        // 构造广播消息
        demo::SensorUpdate update;
        update.data.sensor_id = 1;
        update.data.temperature = service.temperature();
        update.data.humidity = service.humidity();
        update.data.timestamp = static_cast<int64_t>(time(NULL));
        update.data.location = "Room-A";
        update.publish_time = static_cast<int64_t>(time(NULL));

        // 广播传感器数据
        service.BroadcastSensorUpdate(update);

        printf("[SensorService] Broadcast: temp=%.2f, humidity=%.2f\n",
               update.data.temperature, update.data.humidity);

        // 等待1秒
        // 注意：实际项目中应使用 EventLoop 的定时器，这里简化处理
        for (int i = 0; i < 9 && g_running; i++) {
            runtime.pollOnce(100);
        }
    }

    // 4. 清理退出
    printf("\nShutting down...\n");
    runtime.unregisterService(&service);
    runtime.stop();
    printf("SensorService stopped\n");

    return 0;
}
```

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

```cpp
// examples/example_cpp/sensor_client.cpp
// C++ 传感器客户端 - 调用 SensorService 接口，订阅广播，监听死亡通知

#include "sensor_service.bidl.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>

static volatile bool g_running = true;

void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) {
            sm_host = argv[++i];
        } else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = static_cast<uint16_t>(atoi(argv[++i]));
        }
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    printf("=== SensorClient (C++ Client) Starting ===\n");
    printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    // 1. 创建 OmniRuntime 并连接 ServiceManager
    omnibinder::OmniRuntime runtime;
    int ret = runtime.init(sm_host, sm_port);
    if (ret != 0) {
        printf("Failed to connect to ServiceManager: %s\n",
               omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }
    printf("Connected to ServiceManager\n");

    // 2. 创建 SensorService 的远程代理
    demo::SensorServiceProxy proxy(client);

    // 连接到远程 SensorService
    // 内部流程：
    //   a) 通过 SM 查询 SensorService 的 {host, port, host_id, shm_name}
    //   b) 比较 host_id：同机 -> 打开已有 SHM，获取 slot_id
    //      -> 通过 UDS 连接交换 eventfd（SCM_RIGHTS）
    //      -> 注册 resp_eventfd 到 EventLoop
    //      跨机 -> TCP 连接
    //   c) 同一个 SHM 可被多个客户端共享（最多 32 个）
    ret = proxy.connect();
    if (ret != 0) {
        printf("Failed to connect to SensorService: %s\n",
               omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }
    printf("Connected to SensorService\n\n");

    // 3. 调用远程接口：GetLatestData
    printf("--- Calling GetLatestData ---\n");
    demo::SensorData data = proxy.GetLatestData();
    printf("Result: sensor_id=%d, temp=%.2f, humidity=%.2f, location=%s\n\n",
           data.sensor_id, data.temperature, data.humidity,
           data.location.c_str());

    // 4. 调用远程接口：SetThreshold
    printf("--- Calling SetThreshold ---\n");
    demo::ControlCommand cmd;
    cmd.command_type = 1;
    cmd.target = "temperature";
    cmd.value = 30;

    common::StatusResponse resp = proxy.SetThreshold(cmd);  // 返回 common::StatusResponse
    printf("Result: code=%d, message=%s\n\n", resp.code, resp.message.c_str());

    // 5. 调用远程接口：ResetSensor（单向调用）
    printf("--- Calling ResetSensor (one-way) ---\n");
    proxy.ResetSensor(1);
    printf("ResetSensor sent (no response expected)\n\n");

    // 6. 订阅广播话题：SensorUpdate
    printf("--- Subscribing to SensorUpdate topic ---\n");
    proxy.SubscribeSensorUpdate(
        [](const demo::SensorUpdate& update) {
            printf("[Broadcast] SensorUpdate: sensor_id=%d, temp=%.2f, "
                   "humidity=%.2f, time=%lld\n",
                   update.data.sensor_id,
                   update.data.temperature,
                   update.data.humidity,
                   (long long)update.publish_time);
        }
    );
    printf("Subscribed to SensorUpdate\n\n");

    // 7. 订阅死亡通知
    printf("--- Subscribing to SensorService death notification ---\n");
    proxy.OnServiceDied(
        []() {
            printf("\n!!! [DeathNotify] SensorService has DIED !!!\n");
            printf("!!! Connection lost, need to reconnect when service restarts\n\n");
        }
    );
    printf("Subscribed to death notification\n\n");

    // 8. 进入事件循环，等待广播和通知
    printf("Waiting for broadcasts and notifications (Ctrl+C to stop)...\n");
    printf("============================================================\n\n");

    while (g_running) {
        runtime.pollOnce(100);
    }

    // 9. 清理退出
    printf("\nShutting down...\n");
    proxy.disconnect();
    runtime.stop();
    printf("SensorClient stopped\n");

    return 0;
}
```

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

### 5.2 sensor_server.c

```c
// examples/example_c/sensor_server.c
// C 传感器服务端 - 使用 omni-idlc 生成的 C 代码实现 OmniBinder 服务

#include "sensor_service.bidl_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile int g_running = 1;
static double g_temp = 25.0;
static double g_hum = 60.0;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- 服务方法实现 ---- */

static void on_get_latest_data(demo_SensorData* result, void* user_data) {
    (void)user_data;
    result->sensor_id = 1;
    result->temperature = g_temp;
    result->humidity = g_hum;
    result->timestamp = (int64_t)time(NULL);
    result->location = (char*)malloc(7);
    if (result->location) {
        memcpy(result->location, "Room-A", 7);
        result->location_len = 6;
    }
    printf("[SensorService] GetLatestData: temp=%.1f humidity=%.1f\n", g_temp, g_hum);
}

static void on_set_threshold(const demo_ControlCommand* cmd,
                             common_StatusResponse* result, void* user_data) {  // 返回 common_StatusResponse
    (void)user_data;
    printf("[SensorService] SetThreshold: type=%d target=%.*s value=%d\n",
           cmd->command_type, cmd->target_len, cmd->target, cmd->value);
    result->code = 0;
    result->message = (char*)malloc(3);
    if (result->message) {
        memcpy(result->message, "OK", 3);
        result->message_len = 2;
    }
}

static void on_reset_sensor(int32_t sensor_id, void* user_data) {
    (void)user_data;
    g_temp = 25.0;
    g_hum = 60.0;
    printf("[SensorService] ResetSensor: id=%d\n", sensor_id);
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand((unsigned)time(NULL));

    /* 创建客户端连接 */
    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    /* 创建服务（使用生成的回调表） */
    demo_SensorService_callbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.GetLatestData = on_get_latest_data;
    cbs.SetThreshold = on_set_threshold;
    cbs.ResetSensor = on_reset_sensor;
    cbs.user_data = NULL;

    omni_service_t* svc = demo_SensorService_stub_create(&cbs);
    omni_runtime_register_service(runtime, svc);

    /* 发布话题 */
    omni_runtime_publish_topic(runtime, "SensorUpdate");

    /* 主循环：定时广播 */
    int counter = 0;
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
        if (++counter >= 10) {
            counter = 0;
            g_temp += (rand() % 100 - 50) / 100.0;
            g_hum += (rand() % 100 - 50) / 100.0;

            demo_SensorUpdate msg;
            demo_SensorUpdate_init(&msg);
            msg.data.sensor_id = 1;
            msg.data.temperature = g_temp;
            msg.data.humidity = g_hum;
            msg.data.timestamp = (int64_t)time(NULL);
            msg.data.location = (char*)malloc(7);
            if (msg.data.location) {
                memcpy(msg.data.location, "Room-A", 7);
                msg.data.location_len = 6;
            }
            msg.publish_time = (int64_t)time(NULL);

            demo_SensorService_broadcast_sensor_update(runtime, &msg);
            demo_SensorUpdate_destroy(&msg);
        }
    }

    omni_runtime_unregister_service(runtime, svc);
    demo_SensorService_stub_destroy(svc);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
```

与 C++ 版本的关键区别：
- 继承 Stub 基类 → 填充 `demo_SensorService_callbacks` 回调表
- `service.BroadcastSensorUpdate(msg)` → `demo_SensorService_broadcast_sensor_update(client, &msg)`
- `std::string` → `char*` + `_len` 字段，需要手动 `malloc/free`
- 结构体需要 `_init()` / `_destroy()` 管理生命周期

## 6. C 客户端

纯 C 语言实现的客户端，功能与 C++ 客户端等价。

### 6.1 sensor_client.c

```c
// examples/example_c/sensor_client.c
// C 传感器客户端 - 使用 omni-idlc 生成的 C 代码调用 OmniBinder 服务

#include "sensor_service.bidl_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- 广播回调 ---- */
static void on_sensor_update(const demo_SensorUpdate* msg, void* user_data) {
    (void)user_data;
    printf("[Broadcast] sensor_id=%d temp=%.2f humidity=%.2f\n",
           msg->data.sensor_id, msg->data.temperature, msg->data.humidity);
}

/* ---- 死亡通知回调 ---- */
static void on_service_died(void* user_data) {
    (void)user_data;
    printf("\n!!! [DeathNotify] SensorService has DIED !!!\n\n");
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 创建客户端连接 */
    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    /* 初始化 Proxy */
    demo_SensorService_proxy proxy;
    demo_SensorService_proxy_init(&proxy, runtime);
    demo_SensorService_proxy_connect(&proxy);

    /* 调用 GetLatestData */
    demo_SensorData data;
    demo_SensorData_init(&data);
    if (demo_SensorService_proxy_get_latest_data(&proxy, &data) == 0) {
        printf("Result: sensor_id=%d temp=%.2f humidity=%.2f location=%.*s\n",
               data.sensor_id, data.temperature, data.humidity,
               data.location_len, data.location);
    }
    demo_SensorData_destroy(&data);

    /* 调用 SetThreshold */
    demo_ControlCommand cmd;
    demo_ControlCommand_init(&cmd);
    cmd.command_type = 1;
    cmd.target = (char*)malloc(12);
    if (cmd.target) {
        memcpy(cmd.target, "temperature", 12);
        cmd.target_len = 11;
    }
    cmd.value = 30;

    common_StatusResponse resp;  // 使用 common_StatusResponse
    common_StatusResponse_init(&resp);
    if (demo_SensorService_proxy_set_threshold(&proxy, &cmd, &resp) == 0) {
        printf("Result: code=%d message=%.*s\n", resp.code, resp.message_len, resp.message);
    }
    demo_ControlCommand_destroy(&cmd);
    common_StatusResponse_destroy(&resp);

    /* 订阅广播 */
    demo_SensorService_proxy_subscribe_sensor_update(&proxy, on_sensor_update, NULL);

    /* 订阅死亡通知 */
    demo_SensorService_proxy_on_service_died(&proxy, on_service_died, NULL);

    /* 事件循环 */
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
    }

    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
```

与 C++ 版本的关键区别：
- `SensorServiceProxy proxy(client)` → `demo_SensorService_proxy proxy; demo_SensorService_proxy_init(&proxy, runtime)`
- `proxy.GetLatestData()` 返回值 → `demo_SensorService_proxy_get_latest_data(&proxy, &data)` 通过指针输出
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

**Hex 模式（不指定 IDL）：**
```bash
$ ./target/bin/omni-cli call SensorService GetLatestData
Calling SensorService.GetLatestData() ...
  interface_id = 0x1a2b3c4d
  method_id    = 0x9c0d1e2f
Response (status=OK, 42 bytes, 1.23 ms):
  Hex: 01 00 00 00 00 00 00 00 80 39 40 ...
```

**JSON 模式（指定 IDL）：**
```bash
$ ./target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData
Calling SensorService.GetLatestData() ...
  interface_id = 0x1a2b3c4d
  method_id    = 0x9c0d1e2f
Response (status=OK, 42 bytes, 0.85 ms):
  {
    "sensor_id": 1,
    "temperature": 25.50,
    "humidity": 60.20,
    "timestamp": 1707321600,
    "location": "Room-A"
  }

$ ./target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"temperature","value":30}'
Calling SensorService.SetThreshold() ...
  interface_id = 0x1a2b3c4d
  method_id    = 0x5e6f7a8b
  request (19 bytes)
Response (status=OK, 35 bytes, 0.89 ms):
  {
    "code": 0,
    "message": "Threshold set successfully"
  }
```

**说明：**
- 不带 `--idl` 参数时使用 hex 格式（向后兼容）
- 带 `--idl` 参数时支持 JSON 输入输出
- 所有调用都显示耗时统计

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
#   bin/     → service_manager, omni-cli, omni-idlc
#   include/ → omnibinder/*.h
#   lib/     → libomnibinder.so, libomnibinder.a, cmake/OmniBinder/
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
$OMNIBINDER_DIR/bin/omni-idlc --lang=cpp --output=generated/ my_service.bidl

# 生成 C 代码
$OMNIBINDER_DIR/bin/omni-idlc --lang=c --output=generated/ my_service.bidl
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

    /* 填充回调表 */
    myapp_MyService_callbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.HandleRequest = on_handle_request;
    cbs.Notify = on_notify;
    cbs.user_data = NULL;

    omni_service_t* svc = myapp_MyService_stub_create(&cbs);
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
| 实现服务 | 继承 `XxxStub`，重写虚函数 | 填充 `xxx_callbacks` 回调表 |
| 调用服务 | `XxxProxy proxy(client)` | `xxx_proxy proxy; xxx_proxy_init(&proxy, runtime)` |
| RPC 调用 | `auto resp = proxy.Method(req)` | `xxx_proxy_method(&proxy, &req, &resp)` |
| 广播 | `stub.BroadcastTopic(msg)` | `xxx_broadcast_topic(client, &msg)` |
| 订阅 | `proxy.SubscribeTopic(lambda)` | `xxx_proxy_subscribe_topic(&proxy, callback, ud)` |
| 死亡通知 | `proxy.OnServiceDied(lambda)` | `xxx_proxy_on_service_died(&proxy, callback, ud)` |
| 字符串 | `std::string` | `char*` + `_len`，需手动 `malloc/free` |
| 结构体生命周期 | 自动（RAII） | 手动 `_init()` / `_destroy()` |
