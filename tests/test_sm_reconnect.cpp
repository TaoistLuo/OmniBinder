#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <thread>
#include <chrono>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19916;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("ReconnectService");

class ReconnectService : public Service {
public:
    ReconnectService() : Service("ReconnectService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "ReconnectService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "ReconnectService"; }
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

struct SmReconnectServerCtx {
    OmniRuntime runtime;
    ReconnectService service;
    volatile bool registered;
    volatile bool should_stop;
    SmReconnectServerCtx() : registered(false), should_stop(false) {}
};

static void smReconnectServerThread(void* arg) {
    SmReconnectServerCtx* ctx = static_cast<SmReconnectServerCtx*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) return;
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) return;
    ctx->registered = true;
    while (!ctx->should_stop) ctx->runtime.pollOnce(50);
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

class SmReconnectTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static SmReconnectServerCtx* server_ctx_;
    static std::thread server_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19916", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new SmReconnectServerCtx();
        server_tid_ = std::thread(smReconnectServerThread, server_ctx_);
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

TestPid SmReconnectTest::sm_pid_ = 0;
SmReconnectServerCtx* SmReconnectTest::server_ctx_ = nullptr;
std::thread SmReconnectTest::server_tid_;

TEST_F(SmReconnectTest, InitialLookupAndInvoke) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    Buffer req, resp;
    const char* msg = "before-restart";
    req.writeRaw(msg, strlen(msg));
        ASSERT_EQ(runtime.connectService("ReconnectService"), 0);
        ASSERT_EQ(runtime.invoke("ReconnectService", IFACE_ID, METHOD_ECHO, req, resp, 3000), 0);
    EXPECT_EQ(resp.size(), strlen(msg));

    runtime.stop();
}

TEST_F(SmReconnectTest, SmRestartRecovery) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    stopProcess(sm_pid_);
    std::this_thread::sleep_for(std::chrono::microseconds(300000));
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19916", "--log-level", "3");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    bool recovered = false;
    for (int i = 0; i < 80 && !recovered; ++i) {
        runtime.pollOnce(100);
        ServiceInfo info;
        if (runtime.lookupService("ReconnectService", info) == 0) {
            Buffer req, resp;
            const char* msg = "after-restart";
            req.writeRaw(msg, strlen(msg));
            ASSERT_EQ(runtime.connectService("ReconnectService"), 0);
            if (runtime.invoke("ReconnectService", IFACE_ID, METHOD_ECHO, req, resp, 3000) == 0 &&
                resp.size() == strlen(msg)) {
                recovered = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
    }
    EXPECT_TRUE(recovered);
    runtime.stop();
}
