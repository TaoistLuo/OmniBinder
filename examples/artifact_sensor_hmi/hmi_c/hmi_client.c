#include "sensor_hmi_service.bidl_c.h"

#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_running = 1;
static volatile int g_service_died = 0;
static const int k_rpc_interval_ms = 1000;

static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static char* dup_text(const char* text) {
    size_t len = strlen(text);
    char* copy = (char*)malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, text, len + 1);
    }
    return copy;
}

static void on_sensor_status_topic(const sensor_hmi_SensorStatusTopic* topic, void* user_data) {
    (void)user_data;
    printf("[hmi_c] broadcast -> enabled=%d interval=%d mode=%.*s value=%.2f pubsub_latency=%lld us snapshot_age=%lld us\n",
           topic->snapshot.enabled,
           topic->snapshot.sampling_interval_ms,
           topic->snapshot.mode_len,
           topic->snapshot.mode != NULL ? topic->snapshot.mode : "",
           topic->snapshot.current_value,
           (long long)(now_us() - topic->publish_time_us),
           (long long)(now_us() - topic->snapshot.timestamp_us));
}

static void on_service_died(void* user_data) {
    (void)user_data;
    g_service_died = 1;
    printf("[hmi_c] death notification: SensorControlService died, waiting for library-managed recovery\n");
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    int i;
    int service_online = 1;
    int64_t last_rpc_ms = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) {
            sm_host = argv[++i];
        } else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = (uint16_t)atoi(argv[++i]);
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "hmi_c: init failed\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    sensor_hmi_SensorControlService_proxy proxy;
    sensor_hmi_SensorControlService_proxy_init(&proxy, runtime);

    if (sensor_hmi_SensorControlService_proxy_connect(&proxy) != 0) {
        fprintf(stderr, "hmi_c: initial connect failed\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    sensor_hmi_SensorControlService_proxy_subscribe_sensor_status_topic(&proxy, on_sensor_status_topic, NULL);
    sensor_hmi_SensorControlService_proxy_on_service_died(&proxy, on_service_died, NULL);

    {
        sensor_hmi_SensorSnapshot snapshot;
        sensor_hmi_SensorSnapshot_init(&snapshot);
        if (sensor_hmi_SensorControlService_proxy_get_snapshot(&proxy, &snapshot) == 0) {
            printf("[hmi_c] initial snapshot -> enabled=%d interval=%d mode=%.*s value=%.2f snapshot_age=%lld us\n",
                   snapshot.enabled,
                   snapshot.sampling_interval_ms,
                   snapshot.mode_len,
                   snapshot.mode != NULL ? snapshot.mode : "",
                   snapshot.current_value,
                   (long long)(now_us() - snapshot.timestamp_us));
        }
        sensor_hmi_SensorSnapshot_destroy(&snapshot);
    }

    {
        sensor_hmi_SensorControlRequest request;
        hmi_common_OperationResult result;
        int64_t request_time_us = now_us();
        sensor_hmi_SensorControlRequest_init(&request);
        hmi_common_OperationResult_init(&result);
        request.enabled = 1;
        request.sampling_interval_ms = 250;
        request.mode = dup_text("boost");
        request.request_time_us = request_time_us;
        if (request.mode != NULL) {
            request.mode_len = 5;
        }
        if (sensor_hmi_SensorControlService_proxy_apply_control(&proxy, &request, &result) == 0) {
            printf("[hmi_c] ApplyControl -> code=%d message=%.*s rpc_roundtrip=%lld us\n",
                   result.code,
                   result.message_len,
                   result.message != NULL ? result.message : "",
                   (long long)(now_us() - request_time_us));
        }
        sensor_hmi_SensorControlRequest_destroy(&request);
        hmi_common_OperationResult_destroy(&result);
    }

    sensor_hmi_SensorControlService_proxy_trigger_calibration(&proxy, 101);
    printf("[hmi_c] TriggerCalibration sent\n");

    printf("hmi_c running. It will stay alive after death notification and rely on library recovery.\n");
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);

        if (now_us() - last_rpc_ms >= (int64_t)k_rpc_interval_ms * 1000) {
            sensor_hmi_RpcProbeResult result;
            int64_t request_time_us = now_us();
            sensor_hmi_RpcProbeResult_init(&result);
            if (sensor_hmi_SensorControlService_proxy_measure_rpc_latency(&proxy, request_time_us, &result) == 0
                && result.request_time_us == request_time_us
                && result.server_handle_time_us > 0
                && result.snapshot.timestamp_us > 0) {
                if (!service_online) {
                    printf("[hmi_c] service recovered, RPC path is available again\n");
                }
                service_online = 1;
                g_service_died = 0;
                printf("[hmi_c] rpc probe -> roundtrip=%lld us server_queue=%lld us snapshot_age=%lld us value=%.2f\n",
                       (long long)(now_us() - request_time_us),
                       (long long)(result.server_handle_time_us - result.request_time_us),
                       (long long)(now_us() - result.snapshot.timestamp_us),
                       result.snapshot.current_value);
            } else {
                if (service_online || g_service_died) {
                    printf("[hmi_c] rpc probe failed, waiting for service recovery\n");
                }
                service_online = 0;
            }
            sensor_hmi_RpcProbeResult_destroy(&result);
            last_rpc_ms = now_us();
        }
    }

    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
