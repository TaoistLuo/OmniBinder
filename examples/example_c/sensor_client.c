/**
 * sensor_client.c - 纯 C 语言传感器客户端示例
 *
 * 演示如何使用 omni-idlc 生成的 C 代码调用 OmniBinder 服务。
 * 编译前需要 omni-idlc 从 sensor_service.bidl 生成 C 代码。
 */

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
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== SensorClient (C Client) Starting ===\n");
    printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    /* 创建客户端连接 */
    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }
    printf("Connected to ServiceManager\n\n");

    /* 初始化 Proxy */
    demo_SensorService_proxy proxy;
    demo_SensorService_proxy_init(&proxy, runtime);
    if (demo_SensorService_proxy_connect(&proxy) != 0) {
        fprintf(stderr, "SensorService not found\n");
        omni_runtime_destroy(runtime);
        return 1;
    }
    printf("Connected to SensorService\n\n");

    /* 调用 GetLatestData */
    printf("--- Calling GetLatestData ---\n");
    demo_SensorData data;
    demo_SensorData_init(&data);
    if (demo_SensorService_proxy_get_latest_data(&proxy, &data) == 0) {
        printf("Result: sensor_id=%d temp=%.2f humidity=%.2f location=%.*s\n\n",
               data.sensor_id, data.temperature, data.humidity,
               data.location_len, data.location);
    } else {
        printf("Call failed\n\n");
    }
    demo_SensorData_destroy(&data);

    /* 调用 SetThreshold */
    printf("--- Calling SetThreshold ---\n");
    demo_ControlCommand cmd;
    demo_ControlCommand_init(&cmd);
    cmd.command_type = 1;
    cmd.target = (char*)malloc(12);
    if (cmd.target) {
        memcpy(cmd.target, "temperature", 12);
        cmd.target_len = 11;
    }
    cmd.value = 30;

    common_StatusResponse resp;
    common_StatusResponse_init(&resp);
    if (demo_SensorService_proxy_set_threshold(&proxy, &cmd, &resp) == 0) {
        printf("Result: code=%d message=%.*s\n\n", resp.code, resp.message_len, resp.message);
    } else {
        printf("Call failed\n\n");
    }
    demo_ControlCommand_destroy(&cmd);
    common_StatusResponse_destroy(&resp);

    /* 订阅广播 */
    printf("--- Subscribing to SensorUpdate ---\n");
    demo_SensorService_proxy_subscribe_sensor_update(&proxy, on_sensor_update, NULL);

    /* 订阅死亡通知 */
    demo_SensorService_proxy_on_service_died(&proxy, on_service_died, NULL);

    printf("Waiting for broadcasts (Ctrl+C to stop)...\n");
    printf("============================================================\n\n");

    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
    }

    printf("\nShutting down...\n");
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
