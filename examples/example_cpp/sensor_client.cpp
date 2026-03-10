// C++ 传感器客户端示例（使用 bidl 生成的 Proxy）
// 编译前需要 omni-idlc 从 sensor_service.bidl 生成 C++ 代码

#include "sensor_service.bidl.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>

static volatile bool g_running = true;
void signalHandler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    printf("=== SensorClient (C++ Client) Starting ===\n");
    printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    omnibinder::OmniRuntime runtime;
    if (runtime.init(sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }
    printf("Connected to ServiceManager\n\n");

    // 使用生成的 Proxy 连接 SensorService
    demo::SensorServiceProxy proxy(runtime);
    if (proxy.connect() != 0) {
        fprintf(stderr, "SensorService not found\n");
        return 1;
    }
    printf("Connected to SensorService\n\n");

    // 调用 GetLatestData — 类型安全，无需手动序列化
    printf("--- Calling GetLatestData ---\n");
    demo::SensorData data = proxy.GetLatestData();
    printf("Result: sensor_id=%d temp=%.2f humidity=%.2f location=%s\n\n",
           data.sensor_id, data.temperature, data.humidity, data.location.c_str());

    // 调用 SetThreshold — 类型安全
    printf("--- Calling SetThreshold ---\n");
    demo::ControlCommand cmd;
    cmd.command_type = 1;
    cmd.target = "temperature";
    cmd.value = 30;
    common::StatusResponse resp = proxy.SetThreshold(cmd);
    printf("Result: code=%d message=%s\n\n", resp.code, resp.message.c_str());

    // 订阅广播 — 回调直接接收反序列化后的结构体
    printf("--- Subscribing to SensorUpdate ---\n");
    proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
        printf("[Broadcast] sensor_id=%d temp=%.2f humidity=%.2f\n",
               msg.data.sensor_id, msg.data.temperature, msg.data.humidity);
    });

    // 订阅死亡通知
    proxy.OnServiceDied([]() {
        printf("\n!!! [DeathNotify] SensorService has DIED !!!\n\n");
    });

    printf("Waiting for broadcasts (Ctrl+C to stop)...\n");
    printf("============================================================\n\n");

    while (g_running) {
        runtime.pollOnce(100);
    }

    printf("\nShutting down...\n");
    runtime.stop();
    return 0;
}
