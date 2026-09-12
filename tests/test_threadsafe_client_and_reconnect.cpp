#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19931;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("ThreadSafeService");

class ThreadSafeService : public Service {
public:
    ThreadSafeService() : Service("ThreadSafeService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "ThreadSafeService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "ThreadSafeService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ECHO && request.size() > 0) {
            if (!response.writeRaw(request.data(), request.size())) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }
private:
    InterfaceInfo iface_;
};

struct TsClientServerCtx {
    OmniRuntime runtime;
    ThreadSafeService service;
    volatile bool registered;
    volatile bool should_stop;
    TsClientServerCtx() : registered(false), should_stop(false) {}
};

struct ClientThreadArg {
    OmniRuntime* runtime;
    int thread_index;
    int iterations;
    volatile bool done;
    int failures;
    ClientThreadArg() : runtime(NULL), thread_index(0), iterations(0), done(false), failures(0) {}
};

static void tsClientServerThread(void* arg) {
    TsClientServerCtx* ctx = static_cast<TsClientServerCtx*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) return;
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) return;
    ctx->registered = true;
    while (!ctx->should_stop) ctx->runtime.pollOnce(50);
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

static void concurrentInvokeThread(void* arg) {
    ClientThreadArg* ctx = static_cast<ClientThreadArg*>(arg);
    for (int i = 0; i < ctx->iterations; ++i) {
        Buffer req, resp;
        char payload[64];
        snprintf(payload, sizeof(payload), "thread-%d-iter-%d", ctx->thread_index, i);
        req.writeRaw(payload, strlen(payload));
        int ret = ctx->runtime->invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, 0, req, resp, 5000);
        if (ret != 0) { ctx->failures++; continue; }
        std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
        if (result != payload) ctx->failures++;
    }
    ctx->done = true;
}

class ThreadsafeClientAndReconnectTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static TsClientServerCtx* server_ctx_;
    static std::thread server_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19931", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new TsClientServerCtx();
        server_tid_ = std::thread(tsClientServerThread, server_ctx_);
        for (int i = 0; i < 50 && !server_ctx_->registered; ++i) std::this_thread::sleep_for(std::chrono::microseconds(100000));
        ASSERT_TRUE(server_ctx_->registered);
    }

    static void TearDownTestSuite() {
        server_ctx_->should_stop = true;
        server_tid_.join();
        delete server_ctx_;
        stopProcess(sm_pid_);
    }
};

TestPid ThreadsafeClientAndReconnectTest::sm_pid_ = 0;
TsClientServerCtx* ThreadsafeClientAndReconnectTest::server_ctx_ = nullptr;
std::thread ThreadsafeClientAndReconnectTest::server_tid_;

TEST_F(ThreadsafeClientAndReconnectTest, StopBeforeRunReturns) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    // stop 在 run 之前调用（文档允许）：run 必须立即返回，不得重新武装 loop
    runtime.stop();
    std::atomic<bool> returned(false);
    std::thread t([&runtime, &returned]() {
        runtime.run();
        returned.store(true);
    });
    for (int i = 0; i < 300 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
    EXPECT_TRUE(returned.load());
    // 幂等 stop：即使上一步失败也保证 join 不挂起
    runtime.stop();
    t.join();
    EXPECT_FALSE(runtime.isRunning());
}

TEST_F(ThreadsafeClientAndReconnectTest, StopDuringRunExits) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    std::atomic<bool> returned(false);
    std::thread t([&runtime, &returned]() {
        runtime.run();
        returned.store(true);
    });
    for (int i = 0; i < 300 && !runtime.isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
    runtime.stop();
    t.join();
    EXPECT_TRUE(returned.load());
    EXPECT_FALSE(runtime.isRunning());
}

TEST_F(ThreadsafeClientAndReconnectTest, ConcurrentApiDuringStartDoesNotHang) {
    // 反复制造 init→run 的 owner 认领窗口与 API 调用交叠：
    // 修复前可能内联执行与 loop 驱动并发/挂起
    for (int iter = 0; iter < 5; ++iter) {
        OmniRuntime runtime;
        ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

        std::atomic<bool> run_returned(false);
        std::atomic<bool> api_done(false);
        std::thread driver([&runtime, &run_returned]() {
            runtime.run();
            run_returned.store(true);
        });
        std::thread api([&runtime, &api_done]() {
            RuntimeStats stats;
            runtime.getStats(stats);
            runtime.setRegisterHost("race-host");
            (void)runtime.getRegisterHost();
            api_done.store(true);
        });

        api.join();
        runtime.stop();
        driver.join();
        EXPECT_TRUE(api_done.load());
        EXPECT_TRUE(run_returned.load());
    }
}

TEST_F(ThreadsafeClientAndReconnectTest, RegisterHostConcurrentGetSet) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    std::thread driver([&runtime]() { runtime.run(); });
    std::this_thread::sleep_for(std::chrono::microseconds(100000));

    std::atomic<bool> stop_flag(false);
    std::thread setter([&runtime, &stop_flag]() {
        int i = 0;
        while (!stop_flag.load()) {
            runtime.setRegisterHost("host-" + std::to_string(i++));
        }
    });
    std::thread getter([&runtime, &stop_flag]() {
        while (!stop_flag.load()) {
            (void)runtime.getRegisterHost();
        }
    });

    std::this_thread::sleep_for(std::chrono::microseconds(300000));
    stop_flag.store(true);
    setter.join();
    getter.join();
    runtime.stop();
    driver.join();
    EXPECT_FALSE(runtime.getRegisterHost().empty());
}

TEST_F(ThreadsafeClientAndReconnectTest, ConcurrentInvokeSameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(runtime.connectService("ThreadSafeService"), 0);

    std::thread tids[6];
    ClientThreadArg args[6];
    for (int i = 0; i < 6; ++i) {
        args[i].runtime = &runtime;
        args[i].thread_index = i;
        args[i].iterations = 40;
        tids[i] = std::thread(concurrentInvokeThread, &args[i]);
    }
    for (int i = 0; i < 6; ++i) {
        tids[i].join();
        EXPECT_TRUE(args[i].done);
        EXPECT_EQ(args[i].failures, 0);
    }
    runtime.stop();
}

TEST_F(ThreadsafeClientAndReconnectTest, SmRestartRecoverySameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    stopProcess(sm_pid_);
    std::this_thread::sleep_for(std::chrono::microseconds(300000));
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19931", "--log-level", "3");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    bool recovered = false;
    for (int i = 0; i < 80 && !recovered; ++i) {
        Buffer req, resp;
        const char* payload = "after-restart";
        req.writeRaw(payload, strlen(payload));
        if (runtime.connectService("ThreadSafeService") != 0) continue;
        int ret = runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, 0, req, resp, 3000);
        if (ret == 0) {
            std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
            if (result == payload) recovered = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
    }
    EXPECT_TRUE(recovered);
    runtime.stop();
}

TEST_F(ThreadsafeClientAndReconnectTest, ServerRestartRecoverySameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    Buffer warm_req, warm_resp;
    const char* warm_payload = "before-server-restart";
    warm_req.writeRaw(warm_payload, strlen(warm_payload));
    ASSERT_EQ(runtime.connectService("ThreadSafeService"), 0);
    ASSERT_EQ(runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, 0, warm_req, warm_resp, 3000), 0);

    server_ctx_->should_stop = true;
    server_tid_.join();
    delete server_ctx_;

    server_ctx_ = new TsClientServerCtx();
    server_tid_ = std::thread(tsClientServerThread, server_ctx_);
    for (int i = 0; i < 80 && !server_ctx_->registered; ++i) std::this_thread::sleep_for(std::chrono::microseconds(100000));
    ASSERT_TRUE(server_ctx_->registered);

    bool server_recovered = false;
    for (int i = 0; i < 80 && !server_recovered; ++i) {
        runtime.clearServiceCache();
        runtime.closeAllConnections();
        if (runtime.connectService("ThreadSafeService") != 0) continue;

        Buffer req, resp;
        const char* payload = "after-server-restart";
        req.writeRaw(payload, strlen(payload));
        int ret = runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, 0, req, resp, 3000);
        if (ret == 0) {
            std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
            if (result == payload) server_recovered = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
    }
    EXPECT_TRUE(server_recovered);
    runtime.stop();
}
