#include "sensor_hmi_service.bidl_c.h"

#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile int g_running = 1;
static int g_enabled = 1;
static int32_t g_sampling_interval_ms = 1000;
static double g_current_value = 24.5;
static char g_mode[32] = "normal";

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static char* dup_text(const char* text) {
    size_t len = strlen(text);
    char* copy = (char*)malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, text, len + 1);
    }
    return copy;
}

static void fill_snapshot(sensor_hmi_SensorSnapshot* snapshot) {
    sensor_hmi_SensorSnapshot_init(snapshot);
    snapshot->sensor_id = 101;
    snapshot->enabled = g_enabled;
    snapshot->sampling_interval_ms = g_sampling_interval_ms;
    snapshot->current_value = g_current_value;
    snapshot->mode = dup_text(g_mode);
    if (snapshot->mode != NULL) {
        snapshot->mode_len = (uint32_t)strlen(g_mode);
    }
    snapshot->timestamp_us = now_us();
}

static void on_get_snapshot(sensor_hmi_SensorSnapshot* result, void* user_data) {
    (void)user_data;
    fill_snapshot(result);
    printf("[sensor_c] GetSnapshot -> enabled=%d interval=%d mode=%s value=%.2f\n",
           g_enabled, g_sampling_interval_ms, g_mode, g_current_value);
}

static void on_apply_control(const sensor_hmi_SensorControlRequest* request,
                             hmi_common_OperationResult* result,
                             void* user_data) {
    (void)user_data;
    g_enabled = request->enabled;
    if (request->sampling_interval_ms > 0) {
        g_sampling_interval_ms = request->sampling_interval_ms;
    }
    if (request->mode != NULL && request->mode_len > 0) {
        size_t copy_len = request->mode_len < sizeof(g_mode) - 1 ? request->mode_len : sizeof(g_mode) - 1;
        memcpy(g_mode, request->mode, copy_len);
        g_mode[copy_len] = '\0';
    }

    hmi_common_OperationResult_init(result);
    result->code = 0;
    {
        char message[128];
        snprintf(message, sizeof(message), "applied: enabled=%s interval_ms=%d mode=%s",
                 g_enabled ? "true" : "false", g_sampling_interval_ms, g_mode);
        result->message = dup_text(message);
        if (result->message != NULL) {
            result->message_len = (uint32_t)strlen(message);
        }
        printf("[sensor_c] ApplyControl -> %s server_delay=%lld us\n",
               message, (long long)(now_us() - request->request_time_us));
    }
}

static void on_trigger_calibration(int32_t sensor_id, void* user_data) {
    (void)user_data;
    g_current_value = 20.0;
    snprintf(g_mode, sizeof(g_mode), "%s", "calibrating");
    printf("[sensor_c] TriggerCalibration sensor_id=%d\n", sensor_id);
}

static void on_measure_rpc_latency(int64_t request_time_us,
                                   sensor_hmi_RpcProbeResult* result,
                                   void* user_data) {
    (void)user_data;
    sensor_hmi_RpcProbeResult_init(result);
    fill_snapshot(&result->snapshot);
    result->request_time_us = request_time_us;
    result->server_handle_time_us = now_us();
    result->response_time_us = now_us();
    printf("[sensor_c] MeasureRpcLatency -> request_age=%lld us\n",
           (long long)(result->server_handle_time_us - request_time_us));
}

static void tick_value(void) {
    if (!g_enabled) {
        return;
    }
    g_current_value += (rand() % 100 - 50) / 100.0;
    if (g_current_value < -40.0) g_current_value = -40.0;
    if (g_current_value > 125.0) g_current_value = 125.0;
    if (strcmp(g_mode, "calibrating") == 0) {
        snprintf(g_mode, sizeof(g_mode), "%s", "normal");
    }
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    int i;
    int loops = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) {
            sm_host = argv[++i];
        } else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = (uint16_t)atoi(argv[++i]);
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand((unsigned)time(NULL));

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "sensor_c: init failed\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    sensor_hmi_SensorControlService_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.GetSnapshot = on_get_snapshot;
    callbacks.ApplyControl = on_apply_control;
    callbacks.TriggerCalibration = on_trigger_calibration;
    callbacks.MeasureRpcLatency = on_measure_rpc_latency;
    callbacks.user_data = NULL;

    omni_service_t* service = sensor_hmi_SensorControlService_stub_create(&callbacks);
    if (omni_runtime_register_service(runtime, service) != 0) {
        fprintf(stderr, "sensor_c: register failed\n");
        sensor_hmi_SensorControlService_stub_destroy(service);
        omni_runtime_destroy(runtime);
        return 1;
    }

    if (omni_runtime_publish_topic(runtime, "SensorStatusTopic") != 0) {
        fprintf(stderr, "sensor_c: publishTopic failed\n");
        omni_runtime_unregister_service(runtime, service);
        sensor_hmi_SensorControlService_stub_destroy(service);
        omni_runtime_stop(runtime);
        omni_runtime_destroy(runtime);
        return 1;
    }

    printf("sensor_c ready. Kill this process to test HMI death notification.\n");
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
        if (++loops >= 10) {
            sensor_hmi_SensorStatusTopic topic;
            loops = 0;
            tick_value();
            sensor_hmi_SensorStatusTopic_init(&topic);
            fill_snapshot(&topic.snapshot);
            topic.publish_time_us = now_us();
            sensor_hmi_SensorControlService_broadcast_sensor_status_topic(runtime, &topic);
            printf("[sensor_c] Broadcast -> enabled=%d interval=%d mode=%s value=%.2f\n",
                   g_enabled, g_sampling_interval_ms, g_mode, g_current_value);
            sensor_hmi_SensorStatusTopic_destroy(&topic);
        }
    }

    omni_runtime_unregister_service(runtime, service);
    sensor_hmi_SensorControlService_stub_destroy(service);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
