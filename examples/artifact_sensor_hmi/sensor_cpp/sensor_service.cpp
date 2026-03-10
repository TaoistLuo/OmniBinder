#include "sensor_hmi_service.bidl.h"

#include <omnibinder/error.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <string>

static volatile bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

static int64_t nowUs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

class SensorControlServiceImpl : public sensor_hmi::SensorControlServiceStub {
public:
    SensorControlServiceImpl()
        : enabled_(true), sampling_interval_ms_(1000), current_value_(24.5), mode_("normal") {
    }

    sensor_hmi::SensorSnapshot GetSnapshot() override {
        sensor_hmi::SensorSnapshot snapshot;
        fillSnapshot(snapshot);
        std::printf("[sensor_cpp] GetSnapshot -> enabled=%d interval=%d mode=%s value=%.2f\n",
                    enabled_ ? 1 : 0, sampling_interval_ms_, mode_.c_str(), current_value_);
        return snapshot;
    }

    hmi_common::OperationResult ApplyControl(const sensor_hmi::SensorControlRequest& request) override {
        enabled_ = request.enabled;
        if (request.sampling_interval_ms > 0) {
            sampling_interval_ms_ = request.sampling_interval_ms;
        }
        if (!request.mode.empty()) {
            mode_ = request.mode;
        }

        hmi_common::OperationResult result;
        result.code = 0;
        result.message = std::string("applied: enabled=") + (enabled_ ? "true" : "false")
            + ", interval_ms=" + std::to_string(sampling_interval_ms_)
            + ", mode=" + mode_;
        const int64_t server_delay_us = nowUs() - request.request_time_us;
        std::printf("[sensor_cpp] ApplyControl -> %s server_delay=%lld us\n",
                    result.message.c_str(), static_cast<long long>(server_delay_us));
        return result;
    }

    void TriggerCalibration(int32_t sensor_id) override {
        current_value_ = 20.0;
        mode_ = "calibrating";
        std::printf("[sensor_cpp] TriggerCalibration sensor_id=%d\n", sensor_id);
    }

    sensor_hmi::RpcProbeResult MeasureRpcLatency(int64_t request_time_us) override {
        sensor_hmi::RpcProbeResult result;
        fillSnapshot(result.snapshot);
        result.request_time_us = request_time_us;
        result.server_handle_time_us = nowUs();
        result.response_time_us = nowUs();
        std::printf("[sensor_cpp] MeasureRpcLatency -> request_age=%lld us\n",
                    static_cast<long long>(result.server_handle_time_us - request_time_us));
        return result;
    }

    void tick() {
        if (!enabled_) {
            return;
        }
        current_value_ += (std::rand() % 100 - 50) / 100.0;
        if (current_value_ < -40.0) current_value_ = -40.0;
        if (current_value_ > 125.0) current_value_ = 125.0;
        if (mode_ == "calibrating") {
            mode_ = "normal";
        }
    }

    void publishStatus() {
        sensor_hmi::SensorStatusTopic topic;
        fillSnapshot(topic.snapshot);
        topic.publish_time_us = nowUs();
        BroadcastSensorStatusTopic(topic);
        std::printf("[sensor_cpp] Broadcast -> enabled=%d interval=%d mode=%s value=%.2f\n",
                    enabled_ ? 1 : 0, sampling_interval_ms_, mode_.c_str(), current_value_);
    }

private:
    void fillSnapshot(sensor_hmi::SensorSnapshot& snapshot) const {
        snapshot.sensor_id = 101;
        snapshot.enabled = enabled_;
        snapshot.sampling_interval_ms = sampling_interval_ms_;
        snapshot.current_value = current_value_;
        snapshot.mode = mode_;
        snapshot.timestamp_us = nowUs();
    }

    bool enabled_;
    int32_t sampling_interval_ms_;
    double current_value_;
    std::string mode_;
};

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
    std::srand(static_cast<unsigned>(std::time(NULL)));

    omnibinder::OmniRuntime runtime;
    int ret = runtime.init(sm_host, sm_port);
    if (ret != 0) {
        std::fprintf(stderr, "sensor_cpp: init failed: %s\n",
                     omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    SensorControlServiceImpl service;
    ret = runtime.registerService(&service);
    if (ret != 0) {
        std::fprintf(stderr, "sensor_cpp: register failed: %s\n",
                     omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        runtime.stop();
        return 1;
    }

    ret = runtime.publishTopic("SensorStatusTopic");
    if (ret != 0) {
        std::fprintf(stderr, "sensor_cpp: publishTopic failed: %s\n",
                     omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        runtime.unregisterService(&service);
        runtime.stop();
        return 1;
    }

    std::printf("sensor_cpp ready. Kill this process to test HMI death notification.\n");

    int loops = 0;
    while (g_running) {
        runtime.pollOnce(100);
        if (++loops >= 10) {
            loops = 0;
            service.tick();
            service.publishStatus();
        }
    }

    runtime.unregisterService(&service);
    runtime.stop();
    return 0;
}
