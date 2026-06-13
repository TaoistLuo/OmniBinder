// test_heartbeat_reconnect.cpp - Heartbeat, auto-reconnect, and OnServiceDied integration tests
//
// Tests:
// 1. HeartbeatTimeoutDetected     — proxy-side heartbeat detects service death quickly
// 2. AutoReconnectAfterDeath       — auto-reconnect restores connection after service restart
// 3. OnServiceDiedCallbackFires    — SM death notification fires proxy callback
// 4. DirectDisconnectTriggersReconnect — explicit disconnect/connect cycle works with heartbeat
// 5. DisconnectCleansUpState       — disconnect() cleans up, subsequent connect() is safe

#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder.h>
#include <omnibinder/proxy_base.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT        = 19950;
static const uint32_t METHOD_ECHO    = fnv1a_32("Echo");
static const uint32_t IFACE_ID       = fnv1a_32("HrtbtReconnectSvc");
static const char*    SERVICE_NAME   = "HrtbtReconnectSvc";

// ============================================================
// Test service — simple Echo back
// ============================================================
class HrtbtTestService : public Service {
public:
    explicit HrtbtTestService(const std::string& name)
        : Service(name) {
        iface_.interface_id = IFACE_ID;
        iface_.name         = SERVICE_NAME;
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return SERVICE_NAME; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ECHO && request.size() > 0) {
            if (!response.writeRaw(request.data(), request.size()))
                return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }
private:
    InterfaceInfo iface_;
};

// ============================================================
// Program path (saved from argv[0] for startProcess)
// ============================================================
static const char* g_program_path = nullptr;

// ============================================================
// Wait helpers — pollOnce-based, no sleep()
// ============================================================
static bool waitServiceRegistered(OmniRuntime& rt, const std::string& name,
                                   int max_attempts = 50) {
    for (int i = 0; i < max_attempts; ++i) {
        ServiceInfo info;
        if (rt.lookupService(name, info) == 0) return true;
        rt.pollOnce(50);
    }
    return false;
}

static bool tryEchoRpc(OmniRuntime& rt, const std::string& svc_name,
                        const std::string& payload) {
    Buffer req;
    req.writeString(payload);
    Buffer resp;
    int ret = rt.invoke(svc_name, IFACE_ID, METHOD_ECHO, 0, req, resp, 3000);
    if (ret != 0) return false;
    std::string result;
    Buffer resp_buf(resp.data(), resp.size());
    if (!resp_buf.tryReadString(result)) return false;
    return (result == payload);
}

// ============================================================
// Test fixture
// ============================================================
class HeartbeatReconnectTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager",
                                "--port", "19950", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0) << "Failed to start ServiceManager process";
        ASSERT_TRUE(waitPortReady(SM_PORT, 30))
            << "Timed out waiting for ServiceManager port " << SM_PORT;
    }

    static void TearDownTestSuite() {
        if (sm_pid_ > 0) {
            stopProcess(sm_pid_);
            sm_pid_ = 0;
        }
    }
};

TestPid HeartbeatReconnectTest::sm_pid_ = 0;

// ============================================================
// Test 1 — HeartbeatTimeoutDetected
//   Start service → connect proxy → start heartbeat (200 ms
//   interval / 500 ms timeout) → kill service → verify
//   OnServiceDied callback fires within 2 s.
// ============================================================
TEST_F(HeartbeatReconnectTest, HeartbeatTimeoutDetected) {
    TestPid svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0) << "Failed to start service child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SM_PORT), 0) << "Client runtime init failed";
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME))
        << "Service not registered with ServiceManager";

    ServiceProxyBase proxy(rt, SERVICE_NAME);
    ASSERT_EQ(proxy.connect(), 0) << "Proxy connect() failed";
    ASSERT_TRUE(proxy.isConnected())
        << "Proxy should report connected after successful connect()";

    std::atomic<bool> death_fired(false);
    proxy.OnServiceDied([&death_fired]() {
        death_fired.store(true, std::memory_order_release);
    });
    proxy.startHeartbeat(200, 500);

    for (int i = 0; i < 5; ++i) rt.pollOnce(50);

    stopProcess(svc_pid);

    for (int i = 0; i < 40 && !death_fired.load(std::memory_order_acquire); ++i) {
        rt.pollOnce(50);
    }

    ASSERT_TRUE(death_fired.load(std::memory_order_acquire))
        << "OnServiceDied callback should have fired within ~2 s "
        << "(heartbeat 200 ms interval, 500 ms timeout)";

    rt.stop();
}

// ============================================================
// Test 2 — AutoReconnectAfterDeath
//   Start service → connect proxy (auto-reconnect on by
//   default) + heartbeat → kill service → verify death →
//   restart service → verify reconnect + RPC works.
// ============================================================
TEST_F(HeartbeatReconnectTest, AutoReconnectAfterDeath) {
    TestPid svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0) << "Failed to start service child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SM_PORT), 0) << "Client runtime init failed";
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME))
        << "Service not registered with ServiceManager";
    ASSERT_EQ(rt.connectService(SERVICE_NAME), 0) << "connectService failed";

    ASSERT_TRUE(tryEchoRpc(rt, SERVICE_NAME, "pre-death"))
        << "Initial RPC should succeed while service is alive";

    ServiceProxyBase proxy(rt, SERVICE_NAME);
    ASSERT_EQ(proxy.connect(), 0) << "Proxy connect() failed";
    ASSERT_TRUE(proxy.isConnected());

    std::atomic<bool> death_fired(false);
    proxy.OnServiceDied([&death_fired]() {
        death_fired.store(true, std::memory_order_release);
    });
    proxy.startHeartbeat(200, 500);
    for (int i = 0; i < 5; ++i) rt.pollOnce(50);

    stopProcess(svc_pid);

    for (int i = 0; i < 40 && !death_fired.load(std::memory_order_acquire); ++i) {
        rt.pollOnce(50);
    }
    ASSERT_TRUE(death_fired.load(std::memory_order_acquire))
        << "Death should be detected via heartbeat after kill";

    svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0) << "Restarting service process failed";

    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME, 80))
        << "Restarted service should re-register with ServiceManager";

    bool reconnected = false;
    for (int i = 0; i < 60 && !reconnected; ++i) {
        rt.pollOnce(100);
        reconnected = tryEchoRpc(rt, SERVICE_NAME, "post-restart");
    }

    EXPECT_TRUE(reconnected)
        << "RPC should succeed after auto-reconnect restores the data channel";
    EXPECT_TRUE(rt.isServiceConnected(SERVICE_NAME))
        << "Runtime should report service as connected after auto-reconnect";

    stopProcess(svc_pid);
    rt.stop();
}

// ============================================================
// Test 3 — OnServiceDiedCallbackFires
//   Connect proxy (no explicit heartbeat) → register
//   OnServiceDied → kill service → SM detects death and
//   notifies → verify callback fires.
// ============================================================
TEST_F(HeartbeatReconnectTest, OnServiceDiedCallbackFires) {
    TestPid svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0) << "Failed to start service child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SM_PORT), 0) << "Client runtime init failed";
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME))
        << "Service not registered with ServiceManager";

    ServiceProxyBase proxy(rt, SERVICE_NAME);
    ASSERT_EQ(proxy.connect(), 0) << "Proxy connect() failed";
    ASSERT_TRUE(proxy.isConnected());

    std::atomic<bool> death_fired(false);
    proxy.OnServiceDied([&death_fired]() {
        death_fired.store(true, std::memory_order_release);
    });

    stopProcess(svc_pid);

    for (int i = 0; i < 150 && !death_fired.load(std::memory_order_acquire); ++i) {
        rt.pollOnce(100);
    }

    ASSERT_TRUE(death_fired.load(std::memory_order_acquire))
        << "OnServiceDied callback should fire when SM detects service death "
        << "(via SM-side heartbeat / lost TCP control channel)";

    rt.stop();
}

// ============================================================
// Test 4 — DirectDisconnectTriggersReconnect
//   Connect proxy + start heartbeat → explicitly disconnect()
//   → connect() again → restart heartbeat → verify connection
//   is re-established and heartbeat still detects death.
// ============================================================
TEST_F(HeartbeatReconnectTest, DirectDisconnectTriggersReconnect) {
    TestPid svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0) << "Failed to start service child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SM_PORT), 0) << "Client runtime init failed";
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME));

    ServiceProxyBase proxy(rt, SERVICE_NAME);
    ASSERT_EQ(proxy.connect(), 0) << "Initial connect failed";
    ASSERT_TRUE(proxy.isConnected());
    proxy.startHeartbeat(200, 500);
    for (int i = 0; i < 5; ++i) rt.pollOnce(50);

    proxy.disconnect();
    ASSERT_FALSE(proxy.isConnected())
        << "isConnected() should be false after explicit disconnect()";

    ASSERT_EQ(proxy.connect(), 0)
        << "Re-connect after explicit disconnect should succeed";
    ASSERT_TRUE(proxy.isConnected())
        << "isConnected() should be true after re-connect";
    proxy.startHeartbeat(200, 500);
    for (int i = 0; i < 5; ++i) rt.pollOnce(50);

    std::atomic<bool> death_fired(false);
    proxy.OnServiceDied([&death_fired]() {
        death_fired.store(true, std::memory_order_release);
    });

    stopProcess(svc_pid);

    for (int i = 0; i < 40 && !death_fired.load(std::memory_order_acquire); ++i) {
        rt.pollOnce(50);
    }
    ASSERT_TRUE(death_fired.load(std::memory_order_acquire))
        << "Heartbeat should still detect death after explicit disconnect/connect cycle";

    rt.stop();
}

// ============================================================
// Test 5 — DisconnectCleansUpState
//   Connect proxy → disconnect() → verify isConnected() false
//   → connect() again → verify no stale heartbeat timers (no
//   crash), isConnected() true.
// ============================================================
TEST_F(HeartbeatReconnectTest, DisconnectCleansUpState) {
    TestPid svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0) << "Failed to start service child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SM_PORT), 0) << "Client runtime init failed";
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME));

    ServiceProxyBase proxy(rt, SERVICE_NAME);
    ASSERT_EQ(proxy.connect(), 0) << "Initial connect failed";
    ASSERT_TRUE(proxy.isConnected());

    proxy.disconnect();
    ASSERT_FALSE(proxy.isConnected())
        << "isConnected() should return false after disconnect()";

    ASSERT_EQ(proxy.connect(), 0)
        << "Re-connect after disconnect should succeed without crash";
    ASSERT_TRUE(proxy.isConnected())
        << "isConnected() should be true after re-connect";
    proxy.startHeartbeat(200, 500);
    for (int i = 0; i < 5; ++i) rt.pollOnce(50);

    std::atomic<bool> death_fired(false);
    proxy.OnServiceDied([&death_fired]() {
        death_fired.store(true, std::memory_order_release);
    });

    stopProcess(svc_pid);

    for (int i = 0; i < 40 && !death_fired.load(std::memory_order_acquire); ++i) {
        rt.pollOnce(50);
    }
    ASSERT_TRUE(death_fired.load(std::memory_order_acquire))
        << "Heartbeat should detect death after disconnect/reconnect, "
        << "confirming no stale heartbeat state";

    rt.stop();
}

int main(int argc, char** argv) {
    g_program_path = argv[0];

    if (argc >= 3 && strcmp(argv[1], "--child-service") == 0) {
        uint16_t port = (uint16_t)atoi(argv[2]);
        OmniRuntime rt;
        if (rt.init("127.0.0.1", port) != 0) return 1;
        HrtbtTestService svc(SERVICE_NAME);
        if (rt.registerService(&svc) != 0) { rt.stop(); return 1; }
        while (true) { rt.pollOnce(100); }
        return 0;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
