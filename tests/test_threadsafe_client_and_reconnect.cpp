#include <gtest/gtest.h>
#include "test_common.h"
#include "runtime_thread.h"
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
    std::thread thread;
    TsClientServerCtx() {}
};

static bool waitUntilReady(OmniRuntime& runtime, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (runtime.isReady()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return runtime.isReady();
}

static bool startTsServer(TsClientServerCtx* ctx) {
    if (ctx->runtime.init("127.0.0.1", SM_PORT) != 0) return false;
    if (ctx->runtime.registerService(&ctx->service) != 0) return false;
    ctx->thread = std::thread([ctx]() { ctx->runtime.start(); });
    if (!waitUntilReady(ctx->runtime, 100)) return false;
    for (int i = 0; i < 100 && ctx->service.port() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ctx->service.port() != 0;
}

static void stopTsServer(TsClientServerCtx* ctx) {
    ctx->runtime.stop();
    if (ctx->thread.joinable()) ctx->thread.join();
}

struct ClientThreadArg {
    OmniRuntime* runtime;
    int thread_index;
    int iterations;
    std::atomic<bool> done;
    int failures;
    ClientThreadArg() : runtime(NULL), thread_index(0), iterations(0), done(false), failures(0) {}
};

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

static bool waitForService(OmniRuntime& runtime, const std::string& name, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        ServiceInfo info;
        if (runtime.lookupService(name, info) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

class ThreadsafeClientAndReconnectTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static TsClientServerCtx* server_ctx_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19931", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new TsClientServerCtx();
        ASSERT_TRUE(startTsServer(server_ctx_));
    }

    static void TearDownTestSuite() {
        if (server_ctx_) {
            stopTsServer(server_ctx_);
            delete server_ctx_;
            server_ctx_ = nullptr;
        }
        stopProcess(sm_pid_);
    }
};

TestPid ThreadsafeClientAndReconnectTest::sm_pid_ = 0;
TsClientServerCtx* ThreadsafeClientAndReconnectTest::server_ctx_ = nullptr;

TEST_F(ThreadsafeClientAndReconnectTest, StartAfterStopReturnsImmediately) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    // stop 在 start 之前调用：start 必须立即返回，不得重新武装 loop
    runtime.stop();
    std::atomic<bool> returned(false);
    std::thread t([&runtime, &returned]() {
        runtime.start();
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

TEST_F(ThreadsafeClientAndReconnectTest, StopDuringStartExits) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    std::atomic<bool> returned(false);
    std::thread t([&runtime, &returned]() {
        runtime.start();
        returned.store(true);
    });
    for (int i = 0; i < 300 && !runtime.isRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
    // 若 start() 立即返回（loop 未真正运行），本用例会空过，故必须先确认在运行中
    ASSERT_TRUE(runtime.isRunning());
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
            runtime.start();
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
    std::thread driver([&runtime]() { runtime.start(); });
    std::this_thread::sleep_for(std::chrono::microseconds(100000));

    std::atomic<bool> stop_flag(false);
    std::atomic<int> set_count(0);
    std::thread setter([&runtime, &stop_flag, &set_count]() {
        int i = 0;
        while (!stop_flag.load()) {
            runtime.setRegisterHost("host-" + std::to_string(i++));
            set_count.fetch_add(1, std::memory_order_relaxed);
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
    EXPECT_GT(set_count.load(), 0) << "setter 必须真正执行过，否则并发语义未被覆盖";
}

TEST_F(ThreadsafeClientAndReconnectTest, ConcurrentInvokeSameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(runtime);
    ASSERT_TRUE(waitUntilReady(runtime, 100));
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
        EXPECT_EQ(args[i].failures, 0);
    }
    owner.stop();
}

TEST_F(ThreadsafeClientAndReconnectTest, SmRestartRecoverySameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(runtime);
    ASSERT_TRUE(waitUntilReady(runtime, 100));

    stopProcess(sm_pid_);
    std::this_thread::sleep_for(std::chrono::microseconds(300000));
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19931", "--log-level", "3");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));
    ASSERT_TRUE(waitForService(runtime, "ThreadSafeService", 80));

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
    owner.stop();
}

TEST_F(ThreadsafeClientAndReconnectTest, ServerRestartRecoverySameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(runtime);
    ASSERT_TRUE(waitUntilReady(runtime, 100));

    ASSERT_TRUE(waitForService(runtime, "ThreadSafeService", 80));

    Buffer warm_req, warm_resp;
    const char* warm_payload = "before-server-restart";
    warm_req.writeRaw(warm_payload, strlen(warm_payload));
    ASSERT_EQ(runtime.connectService("ThreadSafeService"), 0);
    ASSERT_EQ(runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, 0, warm_req, warm_resp, 3000), 0);

    stopTsServer(server_ctx_);
    delete server_ctx_;

    server_ctx_ = new TsClientServerCtx();
    ASSERT_TRUE(startTsServer(server_ctx_));

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
    owner.stop();
}
