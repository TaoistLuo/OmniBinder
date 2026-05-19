#include "sensor_service.bidl.h"
#include <omnibinder/runtime.h>
#include "platform/platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static volatile bool g_running = true;
static void signalHandler(int) { g_running = false; }

static demo::SensorConfig makeConfig() {
    demo::SensorConfig cfg;
    cfg.enabled = true;
    cfg.sample_rate_hz = 25;
    cfg.label = "sensor-main";
    return cfg;
}

static demo::SensorEnvelope makeEnvelope() {
    demo::SensorEnvelope env;
    env.data.sensor_id = 10;
    env.data.temperature = 18.5;
    env.data.humidity = 45.5;
    env.data.timestamp = 123456789;
    env.data.location = "Lab-1";
    env.config = makeConfig();
    env.captured_at.seconds = 123456789;
    env.captured_at.nanos = 321;
    return env;
}

static demo::SensorArrayBundle makeBundle() {
    demo::SensorArrayBundle bundle;
    bundle.ids.push_back(1);
    bundle.ids.push_back(2);
    bundle.labels.push_back("alpha");
    bundle.labels.push_back("beta");
    bundle.blobs.push_back(std::vector<uint8_t>(3, 0xAA));
    bundle.blobs.push_back(std::vector<uint8_t>(2, 0xBB));
    bundle.samples.push_back(makeEnvelope().data);
    return bundle;
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
    }

    omnibinder::platform::setupSignalHandlers(signalHandler);

    std::printf("=== SensorClient (C++ Client) Starting ===\n");
    std::printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    omnibinder::OmniRuntime runtime;
    if (runtime.init(sm_host, sm_port) != 0) {
        std::fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }

    demo::SensorServiceProxy proxy(runtime);
    if (proxy.connect() != 0) {
        std::fprintf(stderr, "SensorService not found\n");
        return 1;
    }

    std::printf("Connected to SensorService\n\n");

    std::printf("--- Primitive RPCs ---\n");
    // 调用布尔回显 RPC，确认基础 bool 参数与返回值传递正常。
    bool echo_bool = false;
    proxy.EchoBool(false, &echo_bool);
    std::printf("EchoBool(false) => %d\n", echo_bool);
    // 调用 int8 回显 RPC，演示有符号 8 位整数的请求/响应。
    int8_t echo_i8 = 0;
    proxy.EchoInt8(7, &echo_i8);
    std::printf("EchoInt8(7) => %d\n", (int)echo_i8);
    // 调用 uint8 回显 RPC，演示无符号 8 位整数的请求/响应。
    uint8_t echo_u8 = 0;
    proxy.EchoUInt8(7, &echo_u8);
    std::printf("EchoUInt8(7) => %u\n", (unsigned)echo_u8);
    // 调用 int16 回显 RPC，演示有符号 16 位整数的请求/响应。
    int16_t echo_i16 = 0;
    proxy.EchoInt16(16, &echo_i16);
    std::printf("EchoInt16(16) => %d\n", (int)echo_i16);
    // 调用 uint16 回显 RPC，演示无符号 16 位整数的请求/响应。
    uint16_t echo_u16 = 0;
    proxy.EchoUInt16(16, &echo_u16);
    std::printf("EchoUInt16(16) => %u\n", (unsigned)echo_u16);
    // 调用 int32 回显 RPC，演示最常见整型参数的调用方式。
    int32_t echo_i32 = 0;
    proxy.EchoInt32(32, &echo_i32);
    std::printf("EchoInt32(32) => %d\n", echo_i32);
    // 调用 uint32 回显 RPC，演示无符号 32 位整数的请求/响应。
    uint32_t echo_u32 = 0;
    proxy.EchoUInt32(32, &echo_u32);
    std::printf("EchoUInt32(32) => %u\n", echo_u32);
    // 调用 int64 回显 RPC，演示 64 位有符号整数的调用方式。
    int64_t echo_i64 = 0;
    proxy.EchoInt64(64, &echo_i64);
    std::printf("EchoInt64(64) => %lld\n", (long long)echo_i64);
    // 调用 uint64 回显 RPC，演示 64 位无符号整数的调用方式。
    uint64_t echo_u64 = 0;
    proxy.EchoUInt64(64, &echo_u64);
    std::printf("EchoUInt64(64) => %llu\n", (unsigned long long)echo_u64);
    // 调用 float32 回显 RPC，演示单精度浮点数的传输。
    float echo_f32 = 0.0f;
    proxy.EchoFloat32(1.5f, &echo_f32);
    std::printf("EchoFloat32(1.5) => %.2f\n", echo_f32);
    // 调用 float64 回显 RPC，演示双精度浮点数的传输。
    double echo_f64 = 0.0;
    proxy.EchoFloat64(2.5, &echo_f64);
    std::printf("EchoFloat64(2.5) => %.2f\n", echo_f64);
    // 调用字符串回显 RPC，演示 std::string 的序列化与反序列化。
    std::string echo_string;
    proxy.EchoString("hello", &echo_string);
    std::printf("EchoString(hello) => %s\n", echo_string.c_str());
    // 调用字节数组回显 RPC，演示 bytes / std::vector<uint8_t> 的传输。
    std::vector<uint8_t> bytes(3, 0x11);
    std::vector<uint8_t> out_bytes;
    proxy.EchoBytes(bytes, &out_bytes);
    std::printf("EchoBytes([3 bytes]) => size=%zu\n\n", out_bytes.size());

    std::printf("--- Custom / Nested Struct RPCs ---\n");
    // 调用结构体回显 RPC，演示跨包结构体 common::StatusResponse 的传输。
    common::StatusResponse status;
    status.code = 7;
    status.message = "demo";
    common::StatusResponse status_out;
    proxy.EchoStatus(status, &status_out);
    std::printf("EchoStatus => code=%d message=%s\n", status_out.code, status_out.message.c_str());

    // 调用自定义配置结构体回显 RPC，演示普通 struct 参数与返回值。
    demo::SensorConfig cfg_out;
    proxy.EchoConfig(makeConfig(), &cfg_out);
    std::printf("EchoConfig => enabled=%d rate=%d label=%s\n",
               cfg_out.enabled, cfg_out.sample_rate_hz, cfg_out.label.c_str());

    // 调用嵌套结构体回显 RPC，演示多层 struct 组合的序列化。
    demo::SensorEnvelope env_out;
    proxy.EchoEnvelope(makeEnvelope(), &env_out);
    std::printf("EchoEnvelope => location=%s nested_label=%s nanos=%d\n",
               env_out.data.location.c_str(), env_out.config.label.c_str(), env_out.captured_at.nanos);

    std::printf("\n--- Array RPCs ---\n");
    // 调用整数数组回显 RPC，演示 array<int32> 的传输。
    std::vector<int32_t> ids;
    ids.push_back(10);
    ids.push_back(20);
    std::vector<int32_t> ids_out;
    proxy.EchoIdArray(ids, &ids_out);
    std::printf("EchoIdArray => size=%zu tail=%d\n", ids_out.size(), ids_out.back());

    // 调用字符串数组回显 RPC，演示 array<string> 的传输。
    std::vector<std::string> labels;
    labels.push_back("l1");
    labels.push_back("l2");
    std::vector<std::string> labels_out;
    proxy.EchoLabelArray(labels, &labels_out);
    std::printf("EchoLabelArray => size=%zu tail=%s\n", labels_out.size(), labels_out.back().c_str());

    // 调用结构体数组回显 RPC，演示 array<struct> 的传输。
    std::vector<demo::SensorData> samples;
    samples.push_back(makeEnvelope().data);
    std::vector<demo::SensorData> samples_out;
    proxy.EchoSensorArray(samples, &samples_out);
    std::printf("EchoSensorArray => first.location=%s\n", samples_out[0].location.c_str());

    // 调用组合结构体回显 RPC，演示结构体内部再嵌套多个数组字段的场景。
    demo::SensorArrayBundle bundle_out;
    proxy.EchoBundle(makeBundle(), &bundle_out);
    std::printf("EchoBundle => ids=%zu labels=%zu blobs=%zu samples=%zu\n\n",
               bundle_out.ids.size(), bundle_out.labels.size(),
               bundle_out.blobs.size(), bundle_out.samples.size());

    std::printf("--- Existing RPCs ---\n");
    // 调用查询类 RPC，获取服务端当前最新的传感器数据。
    demo::SensorData data;
    proxy.GetLatestData(&data);
    std::printf("GetLatestData => sensor_id=%d temp=%.2f humidity=%.2f location=%s\n",
               data.sensor_id, data.temperature, data.humidity, data.location.c_str());

    // 调用控制类 RPC，请求服务端修改阈值配置。
    demo::ControlCommand cmd;
    cmd.command_type = 1;
    cmd.target = "temperature";
    cmd.value = 30;
    common::StatusResponse resp;
    proxy.SetThreshold(cmd, &resp);
    std::printf("SetThreshold => code=%d message=%s\n", resp.code, resp.message.c_str());
    // 调用统计类 RPC，获取服务端维护的传感器数量。
    int32_t sensor_count = 0;
    proxy.GetSensorCount(&sensor_count);
    std::printf("GetSensorCount => %d\n\n", sensor_count);

    std::printf("--- Topic subscriptions ---\n");
    // 订阅普通广播话题：这是客户端订阅服务发布的消息，不是向服务注册业务回调。
    proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
        std::printf("[SensorUpdate] id=%d temp=%.2f humidity=%.2f\n",
                   msg.data.sensor_id, msg.data.temperature, msg.data.humidity);
    });
    // 订阅异步结果话题：服务端通过 topic 回推异步任务结果。
    // 调用异步请求 RPC，请求被接受后，真正结果会通过 AsyncResultReady 话题回推。
    proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& ready) {
        std::printf("[AsyncResultReady] request_id=%d tag=%s code=%d temp=%.2f\n",
                   ready.result.request_id,
                   ready.result.client_tag.c_str(),
                   ready.result.status.code,
                   ready.result.data.temperature);
    });

    std::printf("\n--- Async RPC via topic reply ---\n");
    
    demo::AsyncRequest async_req;
    async_req.request_id = 42;
    async_req.client_tag = "cpp-client";
    common::StatusResponse ack;
    proxy.RequestLatestDataAsync(async_req, &ack);
    std::printf("RequestLatestDataAsync => code=%d message=%s\n", ack.code, ack.message.c_str());

    // 注册服务死亡通知：这是客户端侧连接状态通知，不是向服务注册业务回调。
    proxy.OnServiceDied([]() {
        std::printf("\n!!! [DeathNotify] SensorService has DIED !!!\n\n");
    });

    std::printf("\nWaiting for broadcasts / async callbacks (Ctrl+C to stop)...\n\n");
    while (g_running) {
        runtime.pollOnce(100);
    }

    runtime.stop();
    return 0;
}
