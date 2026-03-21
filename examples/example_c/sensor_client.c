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

static char* dup_cstr(const char* s, uint32_t* len_out) {
    size_t len = strlen(s);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    if (len_out) *len_out = (uint32_t)len;
    return out;
}

static void fill_status(common_StatusResponse* resp, int32_t code, const char* msg) {
    resp->code = code;
    resp->message = dup_cstr(msg, &resp->message_len);
}

static void fill_config(demo_SensorConfig* cfg) {
    demo_SensorConfig_init(cfg);
    cfg->enabled = 1;
    cfg->sample_rate_hz = 25;
    cfg->label = dup_cstr("sensor-main", &cfg->label_len);
}

static void fill_envelope(demo_SensorEnvelope* env) {
    demo_SensorEnvelope_init(env);
    env->data.sensor_id = 10;
    env->data.temperature = 18.5;
    env->data.humidity = 45.5;
    env->data.timestamp = 123456789;
    env->data.location = dup_cstr("Lab-1", &env->data.location_len);
    fill_config(&env->config);
    env->captured_at.seconds = 123456789;
    env->captured_at.nanos = 321;
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

    bundle->blobs.count = 1;
    bundle->blobs.data = (uint8_t**)malloc(sizeof(uint8_t*));
    bundle->blobs.lens = (uint32_t*)malloc(sizeof(uint32_t));
    bundle->blobs.lens[0] = 3;
    bundle->blobs.data[0] = (uint8_t*)malloc(3);
    memset(bundle->blobs.data[0], 0xAA, 3);

    bundle->samples.count = 1;
    bundle->samples.data = (struct demo_SensorData*)malloc(sizeof(struct demo_SensorData));
    demo_SensorData_init(&bundle->samples.data[0]);
    bundle->samples.data[0].sensor_id = 77;
    bundle->samples.data[0].temperature = 20.5;
    bundle->samples.data[0].humidity = 30.5;
    bundle->samples.data[0].timestamp = 123;
    bundle->samples.data[0].location = dup_cstr("bundle", &bundle->samples.data[0].location_len);
}

static void on_sensor_update(const demo_SensorUpdate* msg, void* user_data) {
    (void)user_data;
    printf("[SensorUpdate] sensor_id=%d temp=%.2f humidity=%.2f\n",
           msg->data.sensor_id, msg->data.temperature, msg->data.humidity);
}

static void on_async_result(const demo_AsyncResultReady* msg, void* user_data) {
    (void)user_data;
    printf("[AsyncResultReady] request_id=%d tag=%.*s code=%d temp=%.2f\n",
           msg->result.request_id,
           msg->result.client_tag_len,
           msg->result.client_tag,
           msg->result.status.code,
           msg->result.data.temperature);
}

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

    omni_runtime_t* runtime = omni_runtime_create();
    if (omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    demo_SensorService_proxy proxy;
    demo_SensorService_proxy_init(&proxy, runtime);
    if (demo_SensorService_proxy_connect(&proxy) != 0) {
        fprintf(stderr, "SensorService not found\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    printf("--- Primitive RPCs ---\n");
    uint8_t b = 0;
    int8_t i8 = 0;
    int16_t i16 = 0;
    uint16_t u16 = 0;
    uint8_t u8 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    demo_SensorService_proxy_echo_bool(&proxy, 0, &b);
    demo_SensorService_proxy_echo_int8(&proxy, 7, &i8);
    demo_SensorService_proxy_echo_int16(&proxy, 16, &i16);
    demo_SensorService_proxy_echo_uint16(&proxy, 16, &u16);
    demo_SensorService_proxy_echo_uint8(&proxy, 7, &u8);
    demo_SensorService_proxy_echo_int32(&proxy, 32, &i32);
    demo_SensorService_proxy_echo_uint32(&proxy, 32, &u32);
    demo_SensorService_proxy_echo_int64(&proxy, 64, &i64);
    demo_SensorService_proxy_echo_uint64(&proxy, 64, &u64);
    demo_SensorService_proxy_echo_float32(&proxy, 1.5f, &f32);
    demo_SensorService_proxy_echo_float64(&proxy, 2.5, &f64);
    printf("EchoBool(false) => %u\n", b);
    printf("EchoInt8(7) => %d\n", (int)i8);
    printf("EchoInt16 => %d, EchoUInt16 => %u\n", (int)i16, (unsigned)u16);
    printf("EchoUInt8 => %u, EchoInt32 => %d, EchoUInt32 => %u\n", u8, i32, u32);
    printf("EchoInt64 => %lld, EchoUInt64 => %llu\n", (long long)i64, (unsigned long long)u64);
    printf("EchoFloat32 => %.2f, EchoFloat64 => %.2f\n", f32, f64);

    char* out_str = NULL;
    uint32_t out_str_len = 0;
    demo_SensorService_proxy_echo_string(&proxy, "hello", 5, &out_str, &out_str_len);
    printf("EchoString => %.*s\n", out_str_len, out_str);
    free(out_str);

    uint8_t in_bytes[3] = {0x11, 0x11, 0x11};
    uint8_t* out_bytes = NULL;
    uint32_t out_bytes_len = 0;
    demo_SensorService_proxy_echo_bytes(&proxy, in_bytes, 3, &out_bytes, &out_bytes_len);
    printf("EchoBytes => size=%u\n\n", out_bytes_len);
    free(out_bytes);

    printf("--- Custom / Nested Struct RPCs ---\n");
    common_StatusResponse status_in;
    common_StatusResponse status_out;
    common_StatusResponse_init(&status_in);
    common_StatusResponse_init(&status_out);
    fill_status(&status_in, 7, "demo");
    demo_SensorService_proxy_echo_status(&proxy, &status_in, &status_out);
    printf("EchoStatus => code=%d message=%.*s\n", status_out.code, status_out.message_len, status_out.message);

    demo_SensorConfig cfg_in;
    demo_SensorConfig cfg_out;
    fill_config(&cfg_in);
    demo_SensorConfig_init(&cfg_out);
    demo_SensorService_proxy_echo_config(&proxy, &cfg_in, &cfg_out);
    printf("EchoConfig => enabled=%d rate=%d label=%.*s\n",
           cfg_out.enabled, cfg_out.sample_rate_hz, cfg_out.label_len, cfg_out.label);

    demo_SensorEnvelope env_in;
    demo_SensorEnvelope env_out;
    fill_envelope(&env_in);
    demo_SensorEnvelope_init(&env_out);
    demo_SensorService_proxy_echo_envelope(&proxy, &env_in, &env_out);
    printf("EchoEnvelope => location=%.*s nested_label=%.*s nanos=%d\n",
           env_out.data.location_len, env_out.data.location,
           env_out.config.label_len, env_out.config.label,
           env_out.captured_at.nanos);
    demo_SensorEnvelope_destroy(&env_in);
    demo_SensorEnvelope_destroy(&env_out);

    printf("\n--- Array RPCs ---\n");
    demo_int32_t_array ids_in;
    demo_int32_t_array ids_out;
    demo_int32_t_array_init(&ids_in);
    demo_int32_t_array_init(&ids_out);
    ids_in.count = 2;
    ids_in.data = (int32_t*)malloc(sizeof(int32_t) * 2);
    ids_in.data[0] = 10;
    ids_in.data[1] = 20;
    demo_SensorService_proxy_echo_id_array(&proxy, &ids_in, &ids_out);
    printf("EchoIdArray => size=%u tail=%d\n", ids_out.count, ids_out.data[ids_out.count - 1]);

    demo_string_array labels_in;
    demo_string_array labels_out;
    demo_string_array_init(&labels_in);
    demo_string_array_init(&labels_out);
    labels_in.count = 2;
    labels_in.data = (char**)malloc(sizeof(char*) * 2);
    labels_in.lens = (uint32_t*)malloc(sizeof(uint32_t) * 2);
    labels_in.data[0] = dup_cstr("l1", &labels_in.lens[0]);
    labels_in.data[1] = dup_cstr("l2", &labels_in.lens[1]);
    demo_SensorService_proxy_echo_label_array(&proxy, &labels_in, &labels_out);
    printf("EchoLabelArray => size=%u tail=%.*s\n",
           labels_out.count,
           labels_out.lens[labels_out.count - 1],
           labels_out.data[labels_out.count - 1]);

    demo_demo_SensorData_array sensor_arr_in;
    demo_demo_SensorData_array sensor_arr_out;
    demo_demo_SensorData_array_init(&sensor_arr_in);
    demo_demo_SensorData_array_init(&sensor_arr_out);
    sensor_arr_in.count = 1;
    sensor_arr_in.data = (struct demo_SensorData*)malloc(sizeof(struct demo_SensorData));
    demo_SensorData_init(&sensor_arr_in.data[0]);
    sensor_arr_in.data[0].sensor_id = 88;
    sensor_arr_in.data[0].temperature = 21.5;
    sensor_arr_in.data[0].humidity = 31.5;
    sensor_arr_in.data[0].timestamp = 321;
    sensor_arr_in.data[0].location = dup_cstr("array-item", &sensor_arr_in.data[0].location_len);
    demo_SensorService_proxy_echo_sensor_array(&proxy, &sensor_arr_in, &sensor_arr_out);
    printf("EchoSensorArray => first.location=%.*s\n",
           sensor_arr_out.data[0].location_len,
           sensor_arr_out.data[0].location);

    demo_SensorArrayBundle bundle_in;
    demo_SensorArrayBundle bundle_out;
    fill_bundle(&bundle_in);
    demo_SensorArrayBundle_init(&bundle_out);
    demo_SensorService_proxy_echo_bundle(&proxy, &bundle_in, &bundle_out);
    printf("EchoBundle => ids=%u labels=%u blobs=%u samples=%u\n\n",
           bundle_out.ids.count, bundle_out.labels.count, bundle_out.blobs.count, bundle_out.samples.count);
    demo_int32_t_array_destroy(&ids_in);
    demo_int32_t_array_destroy(&ids_out);
    demo_string_array_destroy(&labels_in);
    demo_string_array_destroy(&labels_out);
    demo_demo_SensorData_array_destroy(&sensor_arr_in);
    demo_demo_SensorData_array_destroy(&sensor_arr_out);
    demo_SensorArrayBundle_destroy(&bundle_in);
    demo_SensorArrayBundle_destroy(&bundle_out);

    printf("--- Existing RPCs ---\n");
    demo_SensorData data;
    demo_SensorData_init(&data);
    demo_SensorService_proxy_get_latest_data(&proxy, &data);
    printf("GetLatestData => sensor_id=%d temp=%.2f humidity=%.2f location=%.*s\n",
           data.sensor_id, data.temperature, data.humidity, data.location_len, data.location);
    demo_SensorData_destroy(&data);

    demo_ControlCommand cmd;
    demo_ControlCommand_init(&cmd);
    cmd.command_type = 1;
    cmd.target = dup_cstr("temperature", &cmd.target_len);
    cmd.value = 30;
    common_StatusResponse resp;
    common_StatusResponse_init(&resp);
    demo_SensorService_proxy_set_threshold(&proxy, &cmd, &resp);
    printf("SetThreshold => code=%d message=%.*s\n", resp.code, resp.message_len, resp.message);
    int32_t count = 0;
    demo_SensorService_proxy_get_sensor_count(&proxy, &count);
    printf("GetSensorCount => %d\n", count);
    demo_ControlCommand_destroy(&cmd);
    common_StatusResponse_destroy(&resp);

    demo_SensorService_proxy_subscribe_sensor_update(&proxy, on_sensor_update, NULL);
    demo_SensorService_proxy_subscribe_async_result_ready(&proxy, on_async_result, NULL);
    demo_SensorService_proxy_on_service_died(&proxy, on_service_died, NULL);

    demo_AsyncRequest async_req;
    demo_AsyncRequest_init(&async_req);
    async_req.request_id = 42;
    async_req.client_tag = dup_cstr("c-client", &async_req.client_tag_len);
    common_StatusResponse ack;
    common_StatusResponse_init(&ack);
    demo_SensorService_proxy_request_latest_data_async(&proxy, &async_req, &ack);
    printf("RequestLatestDataAsync => code=%d message=%.*s\n", ack.code, ack.message_len, ack.message);
    demo_AsyncRequest_destroy(&async_req);
    common_StatusResponse_destroy(&ack);

    printf("\nWaiting for broadcasts / async callbacks (Ctrl+C to stop)...\n\n");
    while (g_running) {
        omni_runtime_poll_once(runtime, 100);
    }

    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
    return 0;
}
