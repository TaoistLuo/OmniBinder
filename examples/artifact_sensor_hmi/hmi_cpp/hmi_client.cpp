#include "sensor_hmi_service.bidl.h"

#include <omnibinder/error.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

static constexpr int64_t kRpcIntervalUs = 1000 * 1000;

static volatile bool g_running = true;
static volatile bool g_service_died = false;

static int64_t nowUs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

static void signalHandler(int) {
    g_running = false;
}

static void logSnapshot(const char* prefix, const sensor_hmi::SensorSnapshot& snapshot) {
    std::printf("%s enabled=%d interval=%d mode=%s value=%.2f snapshot_age=%lld us\n",
                prefix,
                snapshot.enabled ? 1 : 0,
                snapshot.sampling_interval_ms,
                snapshot.mode.c_str(),
                snapshot.current_value,
                static_cast<long long>(nowUs() - snapshot.timestamp_us));
}

static void installSubscriptions(sensor_hmi::SensorControlServiceProxy& proxy) {
    proxy.SubscribeSensorStatusTopic([](const sensor_hmi::SensorStatusTopic& topic) {
        const int64_t receive_time_us = nowUs();
        const int64_t publish_latency_us = receive_time_us - topic.publish_time_us;
        std::printf("[hmi_cpp] broadcast -> enabled=%d interval=%d mode=%s value=%.2f pubsub_latency=%lld us snapshot_age=%lld us\n",
                    topic.snapshot.enabled ? 1 : 0,
                    topic.snapshot.sampling_interval_ms,
                    topic.snapshot.mode.c_str(),
                    topic.snapshot.current_value,
                    static_cast<long long>(publish_latency_us),
                    static_cast<long long>(receive_time_us - topic.snapshot.timestamp_us));
    });

    proxy.OnServiceDied([]() {
        g_service_died = true;
        std::printf("[hmi_cpp] death notification: SensorControlService died, waiting for library-managed recovery\n");
    });
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) {
            sm_host = argv[++i];
        } else if (std::strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    omnibinder::OmniRuntime runtime;
    int ret = runtime.init(sm_host, sm_port);
    if (ret != 0) {
        std::fprintf(stderr, "hmi_cpp: init failed: %s\n",
                     omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    sensor_hmi::SensorControlServiceProxy proxy(runtime);
    bool service_online = false;
    int64_t last_rpc_ms = 0;

    ret = proxy.connect();
    if (ret != 0) {
        std::fprintf(stderr, "hmi_cpp: initial connect failed\n");
        runtime.stop();
        return 1;
    }

    installSubscriptions(proxy);

    sensor_hmi::SensorSnapshot before = proxy.GetSnapshot();
    logSnapshot("[hmi_cpp] initial snapshot ->", before);

    sensor_hmi::SensorControlRequest request;
    request.enabled = true;
    request.sampling_interval_ms = 250;
    request.mode = "boost";
    request.request_time_us = nowUs();
    hmi_common::OperationResult result = proxy.ApplyControl(request);
    std::printf("[hmi_cpp] ApplyControl -> code=%d message=%s rpc_roundtrip=%lld us\n",
                result.code, result.message.c_str(),
                static_cast<long long>(nowUs() - request.request_time_us));

    proxy.TriggerCalibration(101);
    std::printf("[hmi_cpp] TriggerCalibration sent\n");

    service_online = true;
    std::printf("hmi_cpp running. It will stay alive after death notification and rely on library recovery.\n");
    while (g_running) {
        runtime.pollOnce(100);

        const int64_t now = nowUs();
        if (now - last_rpc_ms >= kRpcIntervalUs) {
            const int64_t request_time_us = now;
            sensor_hmi::RpcProbeResult rpc_result = proxy.MeasureRpcLatency(request_time_us);
            const bool valid_probe = rpc_result.request_time_us == request_time_us
                && rpc_result.server_handle_time_us > 0
                && rpc_result.response_time_us >= rpc_result.server_handle_time_us
                && rpc_result.snapshot.timestamp_us > 0;

            if (valid_probe) {
                const int64_t rpc_roundtrip_us = nowUs() - request_time_us;
                if (!service_online) {
                    std::printf("[hmi_cpp] service recovered, RPC path is available again\n");
                }
                service_online = true;
                g_service_died = false;
                std::printf("[hmi_cpp] rpc probe -> roundtrip=%lld us server_queue=%lld us snapshot_age=%lld us value=%.2f\n",
                            static_cast<long long>(rpc_roundtrip_us),
                            static_cast<long long>(rpc_result.server_handle_time_us - rpc_result.request_time_us),
                            static_cast<long long>(nowUs() - rpc_result.snapshot.timestamp_us),
                            rpc_result.snapshot.current_value);
            } else {
                if (service_online || g_service_died) {
                    std::printf("[hmi_cpp] rpc probe failed, waiting for service recovery\n");
                }
                service_online = false;
            }
            last_rpc_ms = now;
        }
    }

    runtime.stop();
    return 0;
}
