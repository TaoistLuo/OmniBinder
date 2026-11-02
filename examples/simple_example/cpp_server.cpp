// 最简单的 C++ 服务端示例：注册服务、实现一个 RPC、周期发布一个 topic。

#include "example.bidl.h"

#include <omnibinder/runtime.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static volatile std::sig_atomic_t g_running = 1;

static void signalHandler(int) {
    g_running = 0;
}

class ExampleService : public demo::ExampleServiceStub {
public:
    common::Output Echo(const common::Input& input) override {
        common::Output output;
        output.code = 0;
        output.message = "hello " + input.text;
        return output;
    }
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

    omnibinder::OmniRuntime runtime;
    if (runtime.init(sm_host, sm_port) != 0) {
        std::fprintf(stderr, "init failed\n");
        return 1;
    }

    ExampleService service;
    if (runtime.registerService(&service) != 0) {
        std::fprintf(stderr, "register failed\n");
        return 1;
    }

    std::thread runtime_thread([&runtime]() { runtime.start(); });
    for (int i = 0; i < 100 && g_running && !runtime.isReady(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!g_running) {
        runtime.stop();
        runtime_thread.join();
        return 0;
    }

    std::printf("ExampleService ready on port %u\n", service.port());
    int counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        demo::Update update;
        update.value = ++counter;
        update.text = "tick";
        service.BroadcastUpdate(update);
        std::printf("publish Update value=%d text=%s\n", update.value, update.text.c_str());
    }

    runtime.stop();
    runtime_thread.join();
    return 0;
}
