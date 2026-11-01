#include "sensor_service.bidl_c.h"
#include "app_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void runtime_entry(void* arg) {
    omni_runtime_start((omni_runtime_t*)arg);
}

/* 实例发现回调的上下文：打印全部实例，并记录 --instance 指定实例的 host_id */
typedef struct instance_lookup {
    const char* want_instance_id;
    char found_host_id[128];
    char found_instance_id[128];
    int found;
} instance_lookup;

static void on_instance(const omni_service_instance_t* info, void* user_data) {
    instance_lookup* lookup = (instance_lookup*)user_data;
    printf("  host_id=%s instance_id=%s host=%s port=%u\n",
           info->host_id, info->instance_id, info->host, info->port);
    if (lookup && lookup->want_instance_id && !lookup->found
        && strcmp(info->instance_id, lookup->want_instance_id) == 0) {
        snprintf(lookup->found_host_id, sizeof(lookup->found_host_id), "%s", info->host_id);
        snprintf(lookup->found_instance_id, sizeof(lookup->found_instance_id), "%s", info->instance_id);
        lookup->found = 1;
    }
}

/* 请求输入是借用语义：框架只读取、不接管内存，因此可直接指向字符串字面量。
 * 注意：这类借用输入结构不要调用 *_destroy()，否则会尝试释放字面量。 */
static void fill_status(common_StatusResponse* resp, int32_t code, const char* msg) {
    resp->code = code;
    resp->message = (char*)msg;
}

static void fill_config(demo_SensorConfig* cfg) {
    demo_SensorConfig_init(cfg);
    cfg->enabled = 1;
    cfg->sample_rate_hz = 25;
    cfg->label = (char*)"sensor-main";
}

static void fill_envelope(demo_SensorEnvelope* env) {
    demo_SensorEnvelope_init(env);
    env->data.sensor_id = 10;
    env->data.temperature = 18.5;
    env->data.humidity = 45.5;
    env->data.timestamp = 123456789;
    env->data.location = (char*)"Lab-1";
    fill_config(&env->config);
    env->captured_at.seconds = 123456789;
    env->captured_at.nanos = 321;
}

static void fill_bundle(demo_SensorArrayBundle* bundle) {
    static int32_t ids[2] = {1, 2};
    static char* labels[2] = {(char*)"alpha", (char*)"beta"};
    static uint8_t blob[3] = {0xAA, 0xAA, 0xAA};
    static uint8_t* blobs[1] = {blob};
    static uint32_t blob_lens[1] = {3};
    static demo_SensorData samples[1];

    demo_SensorArrayBundle_init(bundle);
    bundle->ids.count = 2;
    bundle->ids.data = ids;

    bundle->labels.count = 2;
    bundle->labels.data = labels;

    bundle->blobs.count = 1;
    bundle->blobs.data = blobs;
    bundle->blobs.lens = blob_lens;

    bundle->samples.count = 1;
    bundle->samples.data = samples;
    samples[0].sensor_id = 77;
    samples[0].temperature = 20.5;
    samples[0].humidity = 30.5;
    samples[0].timestamp = 123;
    samples[0].location = (char*)"bundle";
}

static void on_sensor_update(const demo_SensorUpdate* msg, void* user_data) {
    (void)user_data;
    printf("[SensorUpdate] sensor_id=%d temp=%.2f humidity=%.2f\n",
           msg->data.sensor_id, msg->data.temperature, msg->data.humidity);
}

static void on_async_result(const demo_AsyncResultReady* msg, void* user_data) {
    (void)user_data;
    printf("[AsyncResultReady] request_id=%d tag=%s code=%d temp=%.2f\n",
           msg->result.request_id,
           msg->result.client_tag,
           msg->result.status.code,
           msg->result.data.temperature);
}

static void on_service_died(void* user_data) {
    (void)user_data;
    printf("\n!!! [DeathNotify] SensorService has DIED !!!\n\n");
}

static void on_service_state(uint64_t watch_id, const char* service_name,
                             const char* host_id, const char* instance_id,
                             int online, void* user_data) {
    (void)watch_id;
    (void)user_data;
    printf("[Service] %s host_id=%s instance_id=%s %s\n",
           service_name, host_id, instance_id, online ? "ONLINE" : "OFFLINE");
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    const char* instance_id = NULL;
    uint16_t sm_port = 9900;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--instance") == 0 && i + 1 < argc) instance_id = argv[++i];
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

    app_thread_t* runtime_thread = app_thread_create(runtime_entry, runtime);
    if (!runtime_thread) {
        fprintf(stderr, "Failed to create runtime thread\n");
        demo_SensorService_proxy_destroy(&proxy);
        omni_runtime_destroy(runtime);
        return 1;
    }

    while (g_running && !omni_runtime_is_ready(runtime)) {
        app_sleep_ms(100);
    }

    uint64_t service_watch_id = 0;
    omni_service_watch_request_t watch_request = { "SensorService", NULL, NULL };
    if (omni_runtime_watch_services(runtime, &watch_request, 1,
            on_service_state, NULL, &service_watch_id) != 0) {
        fprintf(stderr, "Failed to watch SensorService state\n");
    }

    instance_lookup lookup;
    memset(&lookup, 0, sizeof(lookup));
    lookup.want_instance_id = instance_id;
    printf("Discovered SensorService instance(s):\n");
    omni_runtime_list_instances(runtime, "SensorService", on_instance, &lookup);

    if (instance_id) {
        int waiting_instance_printed = 0;
        while (g_running && !lookup.found) {
            if (!waiting_instance_printed) {
                printf("Waiting for SensorService instance_id='%s'...\n", instance_id);
                waiting_instance_printed = 1;
            }
            app_sleep_ms(500);
            memset(&lookup, 0, sizeof(lookup));
            lookup.want_instance_id = instance_id;
            omni_runtime_list_instances(runtime, "SensorService", on_instance, &lookup);
        }
        if (!g_running) {
            omni_runtime_stop(runtime);
            app_thread_join(runtime_thread);
            demo_SensorService_proxy_destroy(&proxy);
            omni_runtime_destroy(runtime);
            return 0;
        }
        if (demo_SensorService_proxy_set_instance(&proxy,
                lookup.found_host_id, lookup.found_instance_id) != 0) {
            fprintf(stderr, "SensorService instance not found\n");
            omni_runtime_stop(runtime);
            app_thread_join(runtime_thread);
            demo_SensorService_proxy_destroy(&proxy);
            omni_runtime_destroy(runtime);
            return 1;
        }
        printf("Using SensorService instance_id=%s\n", instance_id);
    } else {
        printf("Using SensorService default instance\n");
    }

    int waiting_connection_printed = 0;
    for (;;) {
        if (!g_running) break;
        int connected = 0;
        if (instance_id) {
            if (omni_runtime_connect_service_instance(runtime, "SensorService",
                    lookup.found_host_id, lookup.found_instance_id) == 0) {
                connected = omni_runtime_is_service_instance_connected(
                    runtime, "SensorService", lookup.found_host_id, lookup.found_instance_id);
            }
        } else {
            if (omni_runtime_connect_service(runtime, "SensorService") == 0) {
                connected = omni_runtime_is_service_connected(runtime, "SensorService");
            }
        }
        if (connected) break;
        if (!waiting_connection_printed) {
            printf("Waiting for SensorService data connection...\n");
            waiting_connection_printed = 1;
        }
        app_sleep_ms(500);
    }
    if (!g_running) {
        if (service_watch_id != 0) omni_runtime_unwatch_services(runtime, service_watch_id);
        omni_runtime_stop(runtime);
        app_thread_join(runtime_thread);
        demo_SensorService_proxy_destroy(&proxy);
        omni_runtime_destroy(runtime);
        return 0;
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
    /* 调用布尔回显 RPC，确认基础 bool 参数与返回值传递正常。 */
    demo_SensorService_proxy_echo_bool(&proxy, 0, &b);
    /* 调用 int8 回显 RPC，演示有符号 8 位整数的请求/响应。 */
    demo_SensorService_proxy_echo_int8(&proxy, 7, &i8);
    /* 调用 int16 回显 RPC，演示有符号 16 位整数的请求/响应。 */
    demo_SensorService_proxy_echo_int16(&proxy, 16, &i16);
    /* 调用 uint16 回显 RPC，演示无符号 16 位整数的请求/响应。 */
    demo_SensorService_proxy_echo_uint16(&proxy, 16, &u16);
    /* 调用 uint8 回显 RPC，演示无符号 8 位整数的请求/响应。 */
    demo_SensorService_proxy_echo_uint8(&proxy, 7, &u8);
    /* 调用 int32 回显 RPC，演示常见整型参数的调用方式。 */
    demo_SensorService_proxy_echo_int32(&proxy, 32, &i32);
    /* 调用 uint32 回显 RPC，演示无符号 32 位整数的请求/响应。 */
    demo_SensorService_proxy_echo_uint32(&proxy, 32, &u32);
    /* 调用 int64 回显 RPC，演示 64 位有符号整数的调用方式。 */
    demo_SensorService_proxy_echo_int64(&proxy, 64, &i64);
    /* 调用 uint64 回显 RPC，演示 64 位无符号整数的调用方式。 */
    demo_SensorService_proxy_echo_uint64(&proxy, 64, &u64);
    /* 调用 float32 回显 RPC，演示单精度浮点数的传输。 */
    demo_SensorService_proxy_echo_float32(&proxy, 1.5f, &f32);
    /* 调用 float64 回显 RPC，演示双精度浮点数的传输。 */
    demo_SensorService_proxy_echo_float64(&proxy, 2.5, &f64);
    printf("EchoBool(false) => %u\n", b);
    printf("EchoInt8(7) => %d\n", (int)i8);
    printf("EchoInt16 => %d, EchoUInt16 => %u\n", (int)i16, (unsigned)u16);
    printf("EchoUInt8 => %u, EchoInt32 => %d, EchoUInt32 => %u\n", u8, i32, u32);
    printf("EchoInt64 => %lld, EchoUInt64 => %llu\n", (long long)i64, (unsigned long long)u64);
    printf("EchoFloat32 => %.2f, EchoFloat64 => %.2f\n", f32, f64);

    char* out_str = NULL;
    /* 调用字符串回显 RPC，演示 C 标准字符串传输。 */
    demo_SensorService_proxy_echo_string(&proxy, "hello", &out_str);
    printf("EchoString => %s\n", out_str);
    omni_free(out_str);

    uint8_t in_bytes[3] = {0x11, 0x11, 0x11};
    uint8_t* out_bytes = NULL;
    uint32_t out_bytes_len = 0;
    /* 调用字节数组回显 RPC，演示 bytes 的传输。 */
    demo_SensorService_proxy_echo_bytes(&proxy, in_bytes, 3, &out_bytes, &out_bytes_len);
    printf("EchoBytes => size=%u\n\n", out_bytes_len);
    omni_free(out_bytes);

    printf("--- Custom / Nested Struct RPCs ---\n");
    common_StatusResponse status_in;
    common_StatusResponse status_out;
    common_StatusResponse_init(&status_in);
    common_StatusResponse_init(&status_out);
    fill_status(&status_in, 7, "demo");
    /* 调用结构体回显 RPC，演示跨包结构体 common_StatusResponse 的传输。 */
    demo_SensorService_proxy_echo_status(&proxy, &status_in, &status_out);
    printf("EchoStatus => code=%d message=%s\n", status_out.code, status_out.message);

    demo_SensorConfig cfg_in;
    demo_SensorConfig cfg_out;
    fill_config(&cfg_in);
    demo_SensorConfig_init(&cfg_out);
    /* 调用自定义配置结构体回显 RPC，演示普通 struct 参数与返回值。 */
    demo_SensorService_proxy_echo_config(&proxy, &cfg_in, &cfg_out);
    printf("EchoConfig => enabled=%d rate=%d label=%s\n",
           cfg_out.enabled, cfg_out.sample_rate_hz, cfg_out.label);

    demo_SensorEnvelope env_in;
    demo_SensorEnvelope env_out;
    fill_envelope(&env_in);
    demo_SensorEnvelope_init(&env_out);
    /* 调用嵌套结构体回显 RPC，演示多层 struct 组合的序列化。 */
    demo_SensorService_proxy_echo_envelope(&proxy, &env_in, &env_out);
    printf("EchoEnvelope => location=%s nested_label=%s nanos=%d\n",
           env_out.data.location,
           env_out.config.label,
           env_out.captured_at.nanos);
    demo_SensorEnvelope_destroy(&env_out);

    printf("\n--- Array RPCs ---\n");
    int32_t ids_buf[2] = {10, 20};
    demo_int32_t_array ids_in;
    demo_int32_t_array ids_out;
    demo_int32_t_array_init(&ids_in);
    demo_int32_t_array_init(&ids_out);
    ids_in.count = 2;
    ids_in.data = ids_buf;
    /* 调用整数数组回显 RPC，演示 array<int32> 的传输。 */
    demo_SensorService_proxy_echo_id_array(&proxy, &ids_in, &ids_out);
    printf("EchoIdArray => size=%u tail=%d\n", ids_out.count, ids_out.data[ids_out.count - 1]);

    char* labels_buf[2] = {(char*)"l1", (char*)"l2"};
    demo_string_array labels_in;
    demo_string_array labels_out;
    demo_string_array_init(&labels_in);
    demo_string_array_init(&labels_out);
    labels_in.count = 2;
    labels_in.data = labels_buf;
    /* 调用字符串数组回显 RPC，演示 array<string> 的传输。 */
    demo_SensorService_proxy_echo_label_array(&proxy, &labels_in, &labels_out);
    printf("EchoLabelArray => size=%u tail=%s\n",
           labels_out.count,
           labels_out.data[labels_out.count - 1]);

    demo_SensorData sensor_buf[1];
    demo_SensorData_init(&sensor_buf[0]);
    demo_demo_SensorData_array sensor_arr_in;
    demo_demo_SensorData_array sensor_arr_out;
    demo_demo_SensorData_array_init(&sensor_arr_in);
    demo_demo_SensorData_array_init(&sensor_arr_out);
    sensor_arr_in.count = 1;
    sensor_arr_in.data = sensor_buf;
    sensor_buf[0].sensor_id = 88;
    sensor_buf[0].temperature = 21.5;
    sensor_buf[0].humidity = 31.5;
    sensor_buf[0].timestamp = 321;
    sensor_buf[0].location = (char*)"array-item";
    /* 调用结构体数组回显 RPC，演示 array<struct> 的传输。 */
    demo_SensorService_proxy_echo_sensor_array(&proxy, &sensor_arr_in, &sensor_arr_out);
    printf("EchoSensorArray => first.location=%s\n",
           sensor_arr_out.data[0].location);

    demo_SensorArrayBundle bundle_in;
    demo_SensorArrayBundle bundle_out;
    fill_bundle(&bundle_in);
    demo_SensorArrayBundle_init(&bundle_out);
    /* 调用组合结构体回显 RPC，演示结构体内部嵌套多个数组字段。 */
    demo_SensorService_proxy_echo_bundle(&proxy, &bundle_in, &bundle_out);
    printf("EchoBundle => ids=%u labels=%u blobs=%u samples=%u\n\n",
           bundle_out.ids.count, bundle_out.labels.count, bundle_out.blobs.count, bundle_out.samples.count);
    demo_int32_t_array_destroy(&ids_out);
    demo_string_array_destroy(&labels_out);
    demo_demo_SensorData_array_destroy(&sensor_arr_out);
    demo_SensorArrayBundle_destroy(&bundle_out);

    printf("--- Existing RPCs ---\n");
    demo_SensorData data;
    demo_SensorData_init(&data);
    /* 调用查询类 RPC，获取服务端当前最新的传感器数据。 */
    demo_SensorService_proxy_get_latest_data(&proxy, &data);
    printf("GetLatestData => sensor_id=%d temp=%.2f humidity=%.2f location=%s\n",
           data.sensor_id, data.temperature, data.humidity, data.location);
    demo_SensorData_destroy(&data);

    demo_ControlCommand cmd;
    demo_ControlCommand_init(&cmd);
    cmd.command_type = 1;
    cmd.target = (char*)"temperature";
    cmd.value = 30;
    common_StatusResponse resp;
    common_StatusResponse_init(&resp);
    /* 调用控制类 RPC，请求服务端修改阈值配置。 */
    demo_SensorService_proxy_set_threshold(&proxy, &cmd, &resp);
    printf("SetThreshold => code=%d message=%s\n", resp.code, resp.message);
    int32_t count = 0;
    /* 调用统计类 RPC，获取服务端维护的传感器数量。 */
    demo_SensorService_proxy_get_sensor_count(&proxy, &count);
    printf("GetSensorCount => %d\n", count);
    common_StatusResponse_destroy(&resp);

    /* 订阅普通广播话题：这是客户端订阅服务发布的消息，不是向服务注册业务回调。 */
    demo_SensorService_proxy_subscribe_sensor_update(&proxy, on_sensor_update, NULL);
    /* 订阅异步结果话题：服务端通过 topic 回推异步任务结果。 */
    demo_SensorService_proxy_subscribe_async_result_ready(&proxy, on_async_result, NULL);
    /* 数据面连接断开回调：感知已连接服务异常退出，不是向服务注册业务回调。 */
    demo_SensorService_proxy_on_service_died(&proxy, on_service_died, NULL);

    demo_AsyncRequest async_req;
    demo_AsyncRequest_init(&async_req);
    async_req.request_id = 42;
    async_req.client_tag = (char*)"c-client";
    common_StatusResponse ack;
    common_StatusResponse_init(&ack);
    /* 调用异步请求 RPC，请求被接受后，真正结果会通过 AsyncResultReady 话题回推。 */
    demo_SensorService_proxy_request_latest_data_async(&proxy, &async_req, &ack);
    printf("RequestLatestDataAsync => code=%d message=%s\n", ack.code, ack.message);
    common_StatusResponse_destroy(&ack);

    printf("\nWaiting for broadcasts / async callbacks (Ctrl+C to stop)...\n\n");
    while (g_running) {
        app_sleep_ms(100);
    }

    if (service_watch_id != 0) {
        omni_runtime_unwatch_services(runtime, service_watch_id);
    }
    omni_runtime_stop(runtime);
    app_thread_join(runtime_thread);
    demo_SensorService_proxy_destroy(&proxy);
    omni_runtime_destroy(runtime);
    return 0;
}
