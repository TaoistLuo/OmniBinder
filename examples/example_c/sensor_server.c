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

static char* dup_cstr(const char* s, uint32_t* len_out) {
    size_t len = strlen(s);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    if (len_out) *len_out = (uint32_t)len;
    return out;
}

static void fill_sensor_data(demo_SensorData* data, int32_t id, const char* location) {
    data->sensor_id = id;
    data->temperature = g_temp;
    data->humidity = g_hum;
    data->timestamp = (int64_t)time(NULL);
    data->location = dup_cstr(location, &data->location_len);
}

static void fill_status(common_StatusResponse* resp, int32_t code, const char* msg) {
    resp->code = code;
    resp->message = dup_cstr(msg, &resp->message_len);
}

static void fill_config(demo_SensorConfig* cfg, uint8_t enabled, int32_t rate, const char* label) {
    cfg->enabled = enabled;
    cfg->sample_rate_hz = rate;
    cfg->label = dup_cstr(label, &cfg->label_len);
}

static void fill_sensor_data_array(demo_demo_SensorData_array* arr) {
    demo_demo_SensorData_array_init(arr);
    arr->count = 1;
    arr->data = (struct demo_SensorData*)malloc(sizeof(struct demo_SensorData));
    demo_SensorData_init(&arr->data[0]);
    fill_sensor_data(&arr->data[0], 77, "bundle");
}

static void fill_bundle(demo_SensorArrayBundle* bundle) {
    demo_SensorArrayBundle_init(bundle);

    bundle->ids.count = 2;
    bundle->ids.data = (int32_t*)malloc(sizeof(int32_t) * 2);
    bundle->ids.data[0] = 1;
    bundle->ids.data[1] = 2;

    bundle->labels.count = 2;
    bundle->labels.data = (char**)malloc(sizeof(char*) * 2);
    bundle->labels.lens = (uint32_t*)malloc(sizeof(uint32_t) * 2);
    bundle->labels.data[0] = dup_cstr("alpha", &bundle->labels.lens[0]);
    bundle->labels.data[1] = dup_cstr("beta", &bundle->labels.lens[1]);

    bundle->blobs.count = 2;
    bundle->blobs.data = (uint8_t**)malloc(sizeof(uint8_t*) * 2);
    bundle->blobs.lens = (uint32_t*)malloc(sizeof(uint32_t) * 2);
    bundle->blobs.lens[0] = 3;
    bundle->blobs.data[0] = (uint8_t*)malloc(3);
    memset(bundle->blobs.data[0], 0xAA, 3);
    bundle->blobs.lens[1] = 2;
    bundle->blobs.data[1] = (uint8_t*)malloc(2);
    memset(bundle->blobs.data[1], 0xBB, 2);

    fill_sensor_data_array(&bundle->samples);
}

void demo_SensorService_impl_echo_bool(uint8_t value, uint8_t* result, void* user_data) {
    (void)user_data;
    *result = value ? 0 : 1;
}

void demo_SensorService_impl_echo_int8(int8_t value, int8_t* result, void* user_data) { (void)user_data; *result = (int8_t)(value + 1); }
void demo_SensorService_impl_echo_uint8(uint8_t value, uint8_t* result, void* user_data) { (void)user_data; *result = (uint8_t)(value + 1); }
void demo_SensorService_impl_echo_int16(int16_t value, int16_t* result, void* user_data) { (void)user_data; *result = (int16_t)(value + 2); }
void demo_SensorService_impl_echo_uint16(uint16_t value, uint16_t* result, void* user_data) { (void)user_data; *result = (uint16_t)(value + 2); }
void demo_SensorService_impl_echo_int32(int32_t value, int32_t* result, void* user_data) { (void)user_data; *result = value + 3; }
void demo_SensorService_impl_echo_uint32(uint32_t value, uint32_t* result, void* user_data) { (void)user_data; *result = value + 3; }
void demo_SensorService_impl_echo_int64(int64_t value, int64_t* result, void* user_data) { (void)user_data; *result = value + 4; }
void demo_SensorService_impl_echo_uint64(uint64_t value, uint64_t* result, void* user_data) { (void)user_data; *result = value + 4; }
void demo_SensorService_impl_echo_float32(float value, float* result, void* user_data) { (void)user_data; *result = value + 0.5f; }
void demo_SensorService_impl_echo_float64(double value, double* result, void* user_data) { (void)user_data; *result = value + 0.25; }

void demo_SensorService_impl_echo_string(const char* value, uint32_t value_len, char** result, uint32_t* result_len, void* user_data) {
    (void)user_data;
    const char* suffix = "_echo";
    uint32_t suffix_len = 5;
    *result = (char*)malloc(value_len + suffix_len + 1);
    memcpy(*result, value, value_len);
    memcpy(*result + value_len, suffix, suffix_len + 1);
    *result_len = value_len + suffix_len;
}

void demo_SensorService_impl_echo_bytes(const uint8_t* value, uint32_t value_len, uint8_t** result, uint32_t* result_len, void* user_data) {
    (void)user_data;
    *result = (uint8_t*)malloc(value_len + 1);
    memcpy(*result, value, value_len);
    (*result)[value_len] = (uint8_t)(value_len & 0xFF);
    *result_len = value_len + 1;
}

void demo_SensorService_impl_echo_status(const common_StatusResponse* value, common_StatusResponse* result, void* user_data) {
    (void)user_data;
    fill_status(result, value->code + 100, "demo_echo");
}

void demo_SensorService_impl_echo_config(const demo_SensorConfig* value, demo_SensorConfig* result, void* user_data) {
    (void)user_data;
    fill_config(result, value->enabled ? 0 : 1, value->sample_rate_hz + 5, "sensor-main_server");
}

void demo_SensorService_impl_echo_envelope(const demo_SensorEnvelope* value, demo_SensorEnvelope* result, void* user_data) {
    (void)user_data;
    demo_SensorEnvelope_init(result);
    fill_sensor_data(&result->data, value->data.sensor_id, "Lab-1");
    result->data.temperature = value->data.temperature + 1.0;
    result->data.humidity = value->data.humidity + 2.0;
    fill_config(&result->config, value->config.enabled ? 0 : 1, value->config.sample_rate_hz + 5, "sensor-main_nested");
    result->captured_at.seconds = value->captured_at.seconds;
    result->captured_at.nanos = value->captured_at.nanos + 1;
}

void demo_SensorService_impl_echo_id_array(const demo_int32_t_array* value, demo_int32_t_array* result, void* user_data) {
    (void)user_data;
    demo_int32_t_array_init(result);
    result->count = value->count + 1;
    result->data = (int32_t*)malloc(sizeof(int32_t) * result->count);
    memcpy(result->data, value->data, sizeof(int32_t) * value->count);
    result->data[value->count] = (int32_t)value->count;
}

void demo_SensorService_impl_echo_label_array(const demo_string_array* value, demo_string_array* result, void* user_data) {
    uint32_t i;
    (void)user_data;
    demo_string_array_init(result);
    result->count = value->count + 1;
    result->data = (char**)malloc(sizeof(char*) * result->count);
    result->lens = (uint32_t*)malloc(sizeof(uint32_t) * result->count);
    for (i = 0; i < value->count; ++i) {
        result->data[i] = dup_cstr(value->data[i], &result->lens[i]);
    }
    result->data[value->count] = dup_cstr("tail", &result->lens[value->count]);
}

void demo_SensorService_impl_echo_sensor_array(const demo_demo_SensorData_array* value, demo_demo_SensorData_array* result, void* user_data) {
    uint32_t i;
    (void)user_data;
    demo_demo_SensorData_array_init(result);
    result->count = value->count;
    result->data = (struct demo_SensorData*)malloc(sizeof(struct demo_SensorData) * result->count);
    for (i = 0; i < result->count; ++i) {
        demo_SensorData_init(&result->data[i]);
        fill_sensor_data(&result->data[i], value->data[i].sensor_id, "echo_location");
    }
}

void demo_SensorService_impl_echo_bundle(const demo_SensorArrayBundle* value, demo_SensorArrayBundle* result, void* user_data) {
    (void)user_data;
    fill_bundle(result);
    if (value->ids.count > 0) {
        result->ids.data[0] = value->ids.data[0];
    }
}

void demo_SensorService_impl_get_latest_data(demo_SensorData* result, void* user_data) {
    (void)user_data;
    fill_sensor_data(result, 1, "Room-A");
    printf("[SensorService] GetLatestData: temp=%.1f humidity=%.1f\n", g_temp, g_hum);
}

void demo_SensorService_impl_set_threshold(const demo_ControlCommand* cmd,
                                           common_StatusResponse* result, void* user_data) {
    (void)user_data;
    printf("[SensorService] SetThreshold: type=%d target=%.*s value=%d\n",
           cmd->command_type, cmd->target_len, cmd->target, cmd->value);
    fill_status(result, 0, "OK");
}

void demo_SensorService_impl_reset_sensor(int32_t sensor_id, void* user_data) {
    (void)user_data;
    g_temp = 25.0;
    g_hum = 60.0;
    printf("[SensorService] ResetSensor: id=%d\n", sensor_id);
}

void demo_SensorService_impl_get_sensor_count(int32_t* result, void* user_data) {
    (void)user_data;
    *result = 3;
}

void demo_SensorService_impl_request_latest_data_async(const demo_AsyncRequest* request,
                                                       common_StatusResponse* result, void* user_data) {
    omni_runtime_t* runtime = (omni_runtime_t*)user_data;
    demo_AsyncResultReady ready;
    demo_AsyncResultReady_init(&ready);
    ready.result.request_id = request->request_id;
    ready.result.client_tag = dup_cstr(request->client_tag, &ready.result.client_tag_len);
    fill_status(&ready.result.status, 0, "async_ok");
    fill_sensor_data(&ready.result.data, request->request_id, "AsyncRoom");
    ready.publish_time = (int64_t)time(NULL);
    demo_SensorService_broadcast_async_result_ready(runtime, &ready);
    demo_AsyncResultReady_destroy(&ready);
    fill_status(result, 0, "accepted");
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

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    omni_service_t* svc = demo_SensorService_stub_create(runtime);
    if (omni_runtime_register_service(runtime, svc) != 0) {
        fprintf(stderr, "Failed to register service\n");
        demo_SensorService_stub_destroy(svc);
        omni_runtime_destroy(runtime);
        return 1;
    }

    omni_runtime_publish_topic(runtime, "SensorUpdate");
    omni_runtime_publish_topic(runtime, "AsyncResultReady");

    printf("SensorService registered on port %u\n\n", omni_service_port(svc));

    int counter = 0;
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
        if (++counter >= 10) {
            counter = 0;
            g_temp += (rand() % 100 - 50) / 100.0;
            g_hum += (rand() % 100 - 50) / 100.0;

            demo_SensorUpdate msg;
            demo_SensorUpdate_init(&msg);
            fill_sensor_data(&msg.data, 1, "Room-A");
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
