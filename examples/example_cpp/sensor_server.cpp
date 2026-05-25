#include "sensor_service.bidl.h"
#include <omnibinder/runtime.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

static volatile bool g_running = true;
static void signalHandler(int) { g_running = false; }

class MySensorService : public demo::SensorServiceStub {
public:
    MySensorService() : temp_(25.0), hum_(60.0), config_enabled_(true), sample_rate_hz_(10) {}

    bool EchoBool(bool value) override { return !value; }
    int8_t EchoInt8(int8_t value) override { return static_cast<int8_t>(value + 1); }
    uint8_t EchoUInt8(uint8_t value) override { return static_cast<uint8_t>(value + 1); }
    int16_t EchoInt16(int16_t value) override { return static_cast<int16_t>(value + 2); }
    uint16_t EchoUInt16(uint16_t value) override { return static_cast<uint16_t>(value + 2); }
    int32_t EchoInt32(int32_t value) override { return value + 3; }
    uint32_t EchoUInt32(uint32_t value) override { return value + 3; }
    int64_t EchoInt64(int64_t value) override { return value + 4; }
    uint64_t EchoUInt64(uint64_t value) override { return value + 4; }
    float EchoFloat32(float value) override { return value + 0.5f; }
    double EchoFloat64(double value) override { return value + 0.25; }
    std::string EchoString(const std::string& value) override { return value + "_echo"; }

    std::vector<uint8_t> EchoBytes(const std::vector<uint8_t>& value) override {
        std::vector<uint8_t> out = value;
        out.push_back(static_cast<uint8_t>(value.size() & 0xFF));
        return out;
    }

    common::StatusResponse EchoStatus(const common::StatusResponse& value) override {
        common::StatusResponse out = value;
        out.code += 100;
        out.message += "_echo";
        return out;
    }

    demo::SensorConfig EchoConfig(const demo::SensorConfig& value) override {
        demo::SensorConfig out = value;
        out.enabled = !value.enabled;
        out.sample_rate_hz += 5;
        out.label += "_server";
        return out;
    }

    demo::SensorEnvelope EchoEnvelope(const demo::SensorEnvelope& value) override {
        demo::SensorEnvelope out = value;
        out.data.temperature += 1.0;
        out.data.humidity += 2.0;
        out.config.label += "_nested";
        out.captured_at.nanos += 1;
        return out;
    }

    std::vector<int32_t> EchoIdArray(const std::vector<int32_t>& value) override {
        std::vector<int32_t> out = value;
        out.push_back(static_cast<int32_t>(value.size()));
        return out;
    }

    std::vector<std::string> EchoLabelArray(const std::vector<std::string>& value) override {
        std::vector<std::string> out = value;
        out.push_back("tail");
        return out;
    }

    std::vector<demo::SensorData> EchoSensorArray(const std::vector<demo::SensorData>& value) override {
        std::vector<demo::SensorData> out = value;
        if (!out.empty()) {
            out[0].location += "_echo";
        }
        return out;
    }

    demo::SensorArrayBundle EchoBundle(const demo::SensorArrayBundle& value) override {
        demo::SensorArrayBundle out = value;
        out.ids.push_back(999);
        out.labels.push_back("bundle_tail");
        out.blobs.push_back(std::vector<uint8_t>(1, 0xEE));
        out.samples.push_back(makeSensorData(77, "bundle"));
        return out;
    }

    demo::SensorData GetLatestData() override {
        demo::SensorData data = makeSensorData(1, "Room-A");
        data.temperature = temp_;
        data.humidity = hum_;
        std::printf("[SensorService] GetLatestData: temp=%.1f humidity=%.1f\n", temp_, hum_);
        return data;
    }

    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override {
        std::printf("[SensorService] SetThreshold: type=%d target=%s value=%d\n",
                   cmd.command_type, cmd.target.c_str(), cmd.value);
        common::StatusResponse resp;
        resp.code = 0;
        resp.message = "OK";
        return resp;
    }

    void ResetSensor(int32_t sensor_id) override {
        temp_ = 25.0;
        hum_ = 60.0;
        std::printf("[SensorService] ResetSensor: id=%d\n", sensor_id);
    }

    int32_t GetSensorCount() override {
        return 3;
    }

    common::StatusResponse RequestLatestDataAsync(const demo::AsyncRequest& request) override {
        demo::AsyncResultReady ready;
        ready.result.request_id = request.request_id;
        ready.result.client_tag = request.client_tag;
        ready.result.status.code = 0;
        ready.result.status.message = "async_ok";
        ready.result.data = makeSensorData(request.request_id, "AsyncRoom");
        ready.publish_time = static_cast<int64_t>(time(NULL));
        BroadcastAsyncResultReady(ready);

        common::StatusResponse ack;
        ack.code = 0;
        ack.message = "accepted";
        return ack;
    }

    void updateData() {
        temp_ += (rand() % 100 - 50) / 100.0;
        hum_ += (rand() % 100 - 50) / 100.0;
    }

private:
    demo::SensorData makeSensorData(int32_t id, const std::string& location) const {
        demo::SensorData data;
        data.sensor_id = id;
        data.temperature = temp_;
        data.humidity = hum_;
        data.timestamp = static_cast<int64_t>(time(NULL));
        data.location = location;
        return data;
    }

    double temp_;
    double hum_;
    bool config_enabled_;
    int32_t sample_rate_hz_;
};

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    const char* register_host = NULL;
    uint16_t sm_port = 9900;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--register-host") == 0 && i + 1 < argc) register_host = argv[++i];
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    srand((unsigned)time(NULL));

    std::printf("=== SensorService (C++ Server) Starting ===\n");
    std::printf("ServiceManager: %s:%u\n", sm_host, sm_port);
    if (register_host) {
        std::printf("RegisterHost: %s\n", register_host);
    }

    omnibinder::OmniRuntime runtime;
    if (register_host) {
        runtime.setRegisterHost(register_host);
    }
    if (runtime.init(sm_host, sm_port) != 0) {
        std::fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }
    MySensorService service;
    if (runtime.registerService(&service) != 0) {
        std::fprintf(stderr, "Failed to register service\n");
        return 1;
    }

    runtime.publishTopic("SensorUpdate");
    runtime.publishTopic("AsyncResultReady");

    std::printf("SensorService registered on port %u\n", service.port());
    std::printf("Demo service is ready. Press Ctrl+C to stop.\n\n");

    int counter = 0;
    while (g_running) {
        runtime.pollOnce(100);
        if (++counter >= 10) {
            counter = 0;
            service.updateData();

            demo::SensorUpdate msg;
            msg.data = service.GetLatestData();
            msg.publish_time = static_cast<int64_t>(time(NULL));
            service.BroadcastSensorUpdate(msg);

            std::printf("[Broadcast] temp=%.2f humidity=%.2f\n",
                       msg.data.temperature, msg.data.humidity);
        }
    }

    std::printf("\nShutting down...\n");
    runtime.unregisterService(&service);
    runtime.stop();
    return 0;
}
