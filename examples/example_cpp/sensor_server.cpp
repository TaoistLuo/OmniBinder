// C++ 传感器服务端示例（使用 bidl 生成的 Stub）
// 编译前需要 omni-idlc 从 sensor_service.bidl 生成 C++ 代码

#include "sensor_service.bidl.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <cstring>

static volatile bool g_running = true;
void signalHandler(int) { g_running = false; }

// 继承生成的 Stub，只需实现业务逻辑
class MySensorService : public demo::SensorServiceStub {
public:
    MySensorService() : temp_(25.0), hum_(60.0) {}

    demo::SensorData GetLatestData() override {
        demo::SensorData data;
        data.sensor_id = 1;
        data.temperature = temp_;
        data.humidity = hum_;
        data.timestamp = static_cast<int64_t>(time(NULL));
        data.location = "Room-A";
        printf("[SensorService] GetLatestData: temp=%.1f humidity=%.1f\n", temp_, hum_);
        return data;
    }

    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override {
        printf("[SensorService] SetThreshold: type=%d target=%s value=%d\n",
               cmd.command_type, cmd.target.c_str(), cmd.value);
        common::StatusResponse resp;
        resp.code = 0;
        resp.message = "OK";
        return resp;
    }

    void ResetSensor(int32_t sensor_id) override {
        temp_ = 25.0;
        hum_ = 60.0;
        printf("[SensorService] ResetSensor: id=%d\n", sensor_id);
    }

    void updateData() {
        temp_ += (rand() % 100 - 50) / 100.0;
        hum_ += (rand() % 100 - 50) / 100.0;
    }

    double temp_, hum_;
};

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) sm_host = argv[++i];
        else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) sm_port = (uint16_t)atoi(argv[++i]);
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    srand((unsigned)time(NULL));

    printf("=== SensorService (C++ Server) Starting ===\n");
    printf("ServiceManager: %s:%u\n", sm_host, sm_port);

    omnibinder::OmniRuntime runtime;
    if (runtime.init(sm_host, sm_port) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }
    printf("Connected to ServiceManager\n");

    MySensorService service;
    if (runtime.registerService(&service) != 0) {
        fprintf(stderr, "Failed to register service\n");
        return 1;
    }
    printf("SensorService registered on port %u\n\n", service.port());

    // 发布话题
    runtime.publishTopic("SensorUpdate");

    printf("Broadcasting sensor data (Ctrl+C to stop)...\n\n");
    int counter = 0;
    while (g_running) {
        runtime.pollOnce(100);
        if (++counter >= 10) {
            counter = 0;
            service.updateData();

            // 使用生成的 BroadcastSensorUpdate 方法
            demo::SensorUpdate msg;
            msg.data.sensor_id = 1;
            msg.data.temperature = service.temp_;
            msg.data.humidity = service.hum_;
            msg.data.timestamp = static_cast<int64_t>(time(NULL));
            msg.data.location = "Room-A";
            msg.publish_time = static_cast<int64_t>(time(NULL));
            service.BroadcastSensorUpdate(msg);

            printf("[Broadcast] temp=%.2f humidity=%.2f\n", service.temp_, service.hum_);
        }
    }

    printf("\nShutting down...\n");
    runtime.unregisterService(&service);
    runtime.stop();
    return 0;
}
