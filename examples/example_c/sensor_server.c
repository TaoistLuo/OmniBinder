/**
 * sensor_server.c - 纯 C 语言传感器服务端示例
 *
 * 演示如何使用 omni-idlc 生成的 C 代码实现一个 OmniBinder 服务端。
 * 编译前需要 omni-idlc 从 sensor_service.bidl 生成 C 代码。
 */

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
                             common_StatusResponse* result, void* user_data) {
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
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand((unsigned)time(NULL));

    printf("=== SensorService (C Server) Starting ===\n");
    printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    /* 创建客户端连接 */
    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }
    printf("Connected to ServiceManager\n");

    /* 创建服务（使用生成的回调表） */
    demo_SensorService_callbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.GetLatestData = on_get_latest_data;
    cbs.SetThreshold = on_set_threshold;
    cbs.ResetSensor = on_reset_sensor;
    cbs.user_data = NULL;

    omni_service_t* svc = demo_SensorService_stub_create(&cbs);
    if (omni_runtime_register_service(runtime, svc) != 0) {
        fprintf(stderr, "Failed to register service\n");
        demo_SensorService_stub_destroy(svc);
        omni_runtime_destroy(runtime);
        return 1;
    }
    printf("SensorService registered on port %u\n\n", omni_service_port(svc));

    /* 发布话题 */
    omni_runtime_publish_topic(runtime, "SensorUpdate");

    printf("Broadcasting sensor data (Ctrl+C to stop)...\n\n");

    int counter = 0;
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);

        if (++counter >= 10) {
            counter = 0;

            /* 更新模拟数据 */
            g_temp += (rand() % 100 - 50) / 100.0;
            g_hum += (rand() % 100 - 50) / 100.0;

            /* 使用生成的广播辅助函数 */
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
            printf("[Broadcast] temp=%.2f humidity=%.2f\n", g_temp, g_hum);

            demo_SensorUpdate_destroy(&msg);
        }
    }

    printf("\nShutting down...\n");
    omni_runtime_unregister_service(runtime, svc);
    demo_SensorService_stub_destroy(svc);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
