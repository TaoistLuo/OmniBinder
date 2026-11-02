// 最简单的 C++ 客户端示例：调用一个 RPC，并订阅一个 topic。

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

    demo::ExampleServiceProxy proxy(runtime);
    std::thread runtime_thread([&runtime]() { runtime.start(); });
    while (g_running && !proxy.isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_running) {
        runtime.stop();
        runtime_thread.join();
        return 0;
    }

    common::Input input;
    input.value = 42;
    input.text = "world";
    omnibinder::Result<common::Output> result = proxy.Echo(input);
    if (!result.ok()) {
        std::fprintf(stderr, "Echo failed: %d\n", result.error());
    } else {
        std::printf("Echo => code=%d message=%s\n",
                    result.value().code, result.value().message.c_str());
    }

    proxy.SubscribeUpdate([](const demo::Update& update) {
        std::printf("Update => value=%d text=%s\n", update.value, update.text.c_str());
    });

    std::printf("waiting for Update...\n");
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    runtime.stop();
    runtime_thread.join();
    return 0;
}
