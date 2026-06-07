#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <thread>
#include <chrono>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19932;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("StatsService");

class StatsService : public Service {
public:
    StatsService() : Service("StatsService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "StatsService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "StatsService"; }
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

struct RuntimeStatsServerCtx {
    OmniRuntime runtime;
    StatsService service;
    volatile bool registered;
    volatile bool should_stop;
    RuntimeStatsServerCtx() : registered(false), should_stop(false) {}
};

static void rtStatsServerThread(void* arg) {
    RuntimeStatsServerCtx* ctx = static_cast<RuntimeStatsServerCtx*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) return;
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) return;
    ctx->registered = true;
    while (!ctx->should_stop) ctx->runtime.pollOnce(50);
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

class RuntimeStatsTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static RuntimeStatsServerCtx* server_ctx_;
    static std::thread server_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19932", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new RuntimeStatsServerCtx();
        server_tid_ = std::thread(rtStatsServerThread, server_ctx_);
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

TestPid RuntimeStatsTest::sm_pid_ = 0;
RuntimeStatsServerCtx* RuntimeStatsTest::server_ctx_ = nullptr;
std::thread RuntimeStatsTest::server_tid_;

TEST_F(RuntimeStatsTest, InitialStatsZero) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    RuntimeStats stats;
    ASSERT_EQ(runtime.getStats(stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 0u);
    EXPECT_EQ(stats.total_rpc_success, 0u);
    EXPECT_EQ(stats.total_rpc_failures, 0u);
    runtime.stop();
}

TEST_F(RuntimeStatsTest, SuccessfulCallsUpdateStats) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    for (int i = 0; i < 5; ++i) {
        Buffer req, resp;
        const char* payload = "stats-ok";
        req.writeRaw(payload, strlen(payload));
        ASSERT_EQ(runtime.connectService("StatsService"), 0);
        ASSERT_EQ(runtime.invoke("StatsService", IFACE_ID, METHOD_ECHO, req, resp, 3000), 0);
    }
    RuntimeStats stats;
    ASSERT_EQ(runtime.getStats(stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 5u);
    EXPECT_EQ(stats.total_rpc_success, 5u);
    EXPECT_EQ(stats.total_rpc_failures, 0u);
    EXPECT_GE(stats.active_connections, 1u);
    EXPECT_GE(stats.tcp_connections + stats.shm_connections, 1u);
    runtime.stop();
}

TEST_F(RuntimeStatsTest, FailedCallUpdatesStats) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    Buffer req, resp;
    req.writeRaw("bad", 3);
    int ret = runtime.invoke("MissingService", IFACE_ID, METHOD_ECHO, req, resp, 1000);
    EXPECT_NE(ret, 0);

    RuntimeStats stats;
    ASSERT_EQ(runtime.getStats(stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 1u);
    EXPECT_EQ(stats.total_rpc_success, 0u);
    EXPECT_GE(stats.total_rpc_failures, 1u);
    runtime.stop();
}

TEST_F(RuntimeStatsTest, ResetStatsClearsCounters) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    runtime.resetStats();
    RuntimeStats stats;
    ASSERT_EQ(runtime.getStats(stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 0u);
    EXPECT_EQ(stats.total_rpc_success, 0u);
    EXPECT_EQ(stats.total_rpc_failures, 0u);
    runtime.stop();
}
