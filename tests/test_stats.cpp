#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <thread>
#include <utility>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 9900;

class StatsTestService : public Service {
public:
    StatsTestService() : Service("TestService") {}

    const char* serviceName() const override { return "TestService"; }

    const InterfaceInfo& interfaceInfo() const override {
        static InterfaceInfo info;
        static bool initialized = false;
        if (!initialized) {
            info.interface_id = 0x12345678;
            info.name = "ITestService";
            MethodInfo m;
            m.method_id = 0x00000001;
            m.name = "echo";
            info.methods.push_back(m);
            initialized = true;
        }
        return info;
    }

    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == 0x00000001) {
            Buffer temp(request.data(), request.size());
            int32_t value = mustRead<int32_t>(temp, &Buffer::tryReadInt32);
            if (!response.writeInt32(value)) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }
};

class StatsTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 5));
    }

    static void TearDownTestSuite() {
        stopProcess(sm_pid_);
    }
};

TestPid StatsTest::sm_pid_ = 0;

TEST_F(StatsTest, InvokeUpdatesRpcCount) {
    OmniRuntime server_rt;
    ASSERT_EQ(server_rt.init("127.0.0.1", SM_PORT), 0);
    StatsTestService svc;
    ASSERT_EQ(server_rt.registerService(&svc), 0);

    volatile bool server_should_stop = false;
    auto serverLoop = [](void* arg) {
        auto* ctx = static_cast<std::pair<OmniRuntime*, volatile bool*>*>(arg);
        while (!*ctx->second) ctx->first->pollOnce(50);
    };
    std::pair<OmniRuntime*, volatile bool*> server_ctx(&server_rt, &server_should_stop);
    std::thread server_tid(serverLoop, &server_ctx);

    OmniRuntime client_rt;
    ASSERT_EQ(client_rt.init("127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(client_rt.connectService("TestService"), 0);

    for (int i = 0; i < 3; i++) {
        Buffer req, resp;
        req.writeInt32(i);
        int ret = client_rt.invoke("TestService", 0x12345678, 0x00000001, req, resp, 5000);
        EXPECT_EQ(ret, 0);
    }

    RuntimeStats stats;
    ASSERT_EQ(client_rt.getStats(stats), 0);
    EXPECT_GE(stats.total_rpc_calls, 3u);
    EXPECT_GE(stats.total_rpc_success, 3u);

    server_should_stop = true;
    server_tid.join();
    client_rt.stop();
    server_rt.stop();
}

TEST_F(StatsTest, FailedInvokeUpdatesFailureCount) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    Buffer req, resp;
    req.writeRaw("bad", 3);
    int ret = runtime.invoke("MissingService", 0x12345678, 0x00000001, req, resp, 1000);
    EXPECT_NE(ret, 0);

    RuntimeStats stats;
    ASSERT_EQ(runtime.getStats(stats), 0);
    EXPECT_GE(stats.total_rpc_calls, 1u);
    EXPECT_GE(stats.total_rpc_failures, 1u);

    runtime.stop();
}

TEST_F(StatsTest, ResetClearsStats) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    runtime.resetStats();
    RuntimeStats stats;
    ASSERT_EQ(runtime.getStats(stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 0u);

    runtime.stop();
}
