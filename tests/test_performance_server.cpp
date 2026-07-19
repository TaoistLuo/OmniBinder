#include "perf_service.bidl.h"
#include <omnibinder/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>

static std::atomic<bool> g_running(true);
static void signalHandler(int) { g_running.store(false); }

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class PerfServiceImpl : public perf::PerfServiceStub {
public:
    PerfServiceImpl() {
        setShmConfig(omnibinder::ShmConfig(64 * 1024, 64 * 1024));
    }

    std::vector<uint8_t> EchoBytes(const std::vector<uint8_t>& data) override {
        return data;
    }

    int32_t EchoInt32(int32_t value) override {
        return value + 1;
    }

    perf::PerfData EchoStruct(const perf::PerfData& data) override {
        perf::PerfData out = data;
        out.id += 1;
        out.value += 1.0;
        out.tag += "_echo";
        return out;
    }

    int32_t Add(const perf::AddParams& params) override {
        return params.a + params.b;
    }
};

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    uint16_t sm_port = 19910;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = static_cast<uint16_t>(atoi(argv[++i]));
        }
    }

    omnibinder::setLogLevel(omnibinder::LOG_ERROR);

    omnibinder::OmniRuntime runtime;
    if (runtime.init("127.0.0.1", sm_port) != 0) {
        fprintf(stderr, "SERVER: init failed\n");
        return 1;
    }

    PerfServiceImpl service;
    if (runtime.registerService(&service) != 0) {
        fprintf(stderr, "SERVER: register failed\n");
        return 1;
    }
    runtime.publishTopic("PerfTopic");

    printf("SERVER_READY port=%u\n", service.port());
    fflush(stdout);

    const int sizes[] = {0, 64, 256, 1024, 4096, 8192};
    const int n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    int size_idx = 0;
    uint32_t seq = 0;
    int64_t next_us = nowUs();
    const int64_t topic_interval_us = 500;

    while (g_running.load()) {
        int64_t now = nowUs();
        if (now >= next_us) {
            ++seq;
            perf::PerfTopic msg;
            msg.seq = static_cast<int32_t>(seq);
            int sz = sizes[size_idx];
            if (sz > 0) msg.payload.assign(static_cast<size_t>(sz), static_cast<uint8_t>(0xCD));
            // 时间戳在 payload 构造之后、Broadcast 之前，确保只测量传输延迟
            msg.send_time_us = nowUs();
            service.BroadcastPerfTopic(msg);
            size_idx = (size_idx + 1) % n_sizes;
            next_us += topic_interval_us;
            if (next_us <= now - topic_interval_us * n_sizes) {
                next_us = now + topic_interval_us;
            }
        }
        runtime.pollOnce(0);
    }

    runtime.unregisterService(&service);
    runtime.stop();
    return 0;
}
