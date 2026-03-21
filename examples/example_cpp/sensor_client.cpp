#include "sensor_service.bidl.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
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

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

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
    std::printf("EchoBool(false) => %d\n", proxy.EchoBool(false));
    std::printf("EchoInt8(7) => %d\n", (int)proxy.EchoInt8(7));
    std::printf("EchoUInt8(7) => %u\n", (unsigned)proxy.EchoUInt8(7));
    std::printf("EchoInt16(16) => %d\n", (int)proxy.EchoInt16(16));
    std::printf("EchoUInt16(16) => %u\n", (unsigned)proxy.EchoUInt16(16));
    std::printf("EchoInt32(32) => %d\n", proxy.EchoInt32(32));
    std::printf("EchoUInt32(32) => %u\n", proxy.EchoUInt32(32));
    std::printf("EchoInt64(64) => %lld\n", (long long)proxy.EchoInt64(64));
    std::printf("EchoUInt64(64) => %llu\n", (unsigned long long)proxy.EchoUInt64(64));
    std::printf("EchoFloat32(1.5) => %.2f\n", proxy.EchoFloat32(1.5f));
    std::printf("EchoFloat64(2.5) => %.2f\n", proxy.EchoFloat64(2.5));
    std::printf("EchoString(hello) => %s\n", proxy.EchoString("hello").c_str());
    std::vector<uint8_t> bytes(3, 0x11);
    std::vector<uint8_t> out_bytes = proxy.EchoBytes(bytes);
    std::printf("EchoBytes([3 bytes]) => size=%zu\n\n", out_bytes.size());

    std::printf("--- Custom / Nested Struct RPCs ---\n");
    common::StatusResponse status;
    status.code = 7;
    status.message = "demo";
    common::StatusResponse status_out = proxy.EchoStatus(status);
    std::printf("EchoStatus => code=%d message=%s\n", status_out.code, status_out.message.c_str());

    demo::SensorConfig cfg_out = proxy.EchoConfig(makeConfig());
    std::printf("EchoConfig => enabled=%d rate=%d label=%s\n",
               cfg_out.enabled, cfg_out.sample_rate_hz, cfg_out.label.c_str());

    demo::SensorEnvelope env_out = proxy.EchoEnvelope(makeEnvelope());
    std::printf("EchoEnvelope => location=%s nested_label=%s nanos=%d\n",
               env_out.data.location.c_str(), env_out.config.label.c_str(), env_out.captured_at.nanos);

    std::printf("\n--- Array RPCs ---\n");
    std::vector<int32_t> ids;
    ids.push_back(10);
    ids.push_back(20);
    std::vector<int32_t> ids_out = proxy.EchoIdArray(ids);
    std::printf("EchoIdArray => size=%zu tail=%d\n", ids_out.size(), ids_out.back());

    std::vector<std::string> labels;
    labels.push_back("l1");
    labels.push_back("l2");
    std::vector<std::string> labels_out = proxy.EchoLabelArray(labels);
    std::printf("EchoLabelArray => size=%zu tail=%s\n", labels_out.size(), labels_out.back().c_str());

    std::vector<demo::SensorData> samples;
    samples.push_back(makeEnvelope().data);
    std::vector<demo::SensorData> samples_out = proxy.EchoSensorArray(samples);
    std::printf("EchoSensorArray => first.location=%s\n", samples_out[0].location.c_str());

    demo::SensorArrayBundle bundle_out = proxy.EchoBundle(makeBundle());
    std::printf("EchoBundle => ids=%zu labels=%zu blobs=%zu samples=%zu\n\n",
               bundle_out.ids.size(), bundle_out.labels.size(),
               bundle_out.blobs.size(), bundle_out.samples.size());

    std::printf("--- Existing RPCs ---\n");
    demo::SensorData data = proxy.GetLatestData();
    std::printf("GetLatestData => sensor_id=%d temp=%.2f humidity=%.2f location=%s\n",
               data.sensor_id, data.temperature, data.humidity, data.location.c_str());

    demo::ControlCommand cmd;
    cmd.command_type = 1;
    cmd.target = "temperature";
    cmd.value = 30;
    common::StatusResponse resp = proxy.SetThreshold(cmd);
    std::printf("SetThreshold => code=%d message=%s\n", resp.code, resp.message.c_str());
    std::printf("GetSensorCount => %d\n\n", proxy.GetSensorCount());

    std::printf("--- Topic subscriptions ---\n");
    proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
        std::printf("[SensorUpdate] id=%d temp=%.2f humidity=%.2f\n",
                   msg.data.sensor_id, msg.data.temperature, msg.data.humidity);
    });
    proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& ready) {
        std::printf("[AsyncResultReady] request_id=%d tag=%s code=%d temp=%.2f\n",
                   ready.result.request_id,
                   ready.result.client_tag.c_str(),
                   ready.result.status.code,
                   ready.result.data.temperature);
    });

    std::printf("\n--- Callback-like async RPC via topic ---\n");
    demo::AsyncRequest async_req;
    async_req.request_id = 42;
    async_req.client_tag = "cpp-client";
    common::StatusResponse ack = proxy.RequestLatestDataAsync(async_req);
    std::printf("RequestLatestDataAsync => code=%d message=%s\n", ack.code, ack.message.c_str());

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
