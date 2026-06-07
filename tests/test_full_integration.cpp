// test_full_integration.cpp - Comprehensive integration tests (GTest)
//
// Tests:
// 1. Service init creates both TCP + SHM simultaneously
// 2. SM lookup returns shm_name, client auto-selects SHM for same machine
// 3. Multiple clients share the same SHM (multi-client)
// 4. Broadcast / subscribe (pub/sub)
// 5. Death notification
// 6. Service unregister cleanup

#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("CalcService");
static const uint16_t SM_PORT = 19902;

static const char* g_program_path = nullptr;

// ============================================================
// Test service: CalcService
// ============================================================
class CalcService : public Service {
public:
    CalcService() : Service("CalcService"), invoke_count_(0) {
        setShmConfig(ShmConfig(8 * 1024, 12 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = "CalcService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "CalcService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
    int invokeCount() const { return invoke_count_; }
protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        invoke_count_++;
        if (method_id == METHOD_ADD) {
            Buffer req(request.data(), request.size());
            int32_t a = mustRead<int32_t>(req, &Buffer::tryReadInt32);
            int32_t b = mustRead<int32_t>(req, &Buffer::tryReadInt32);
            if (!response.writeInt32(a + b)) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        } else if (method_id == METHOD_ECHO) {
            if (request.size() > 0) {
                if (!response.writeRaw(request.data(), request.size())) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
            }
        }
        return 0;
    }
private:
    InterfaceInfo iface_;
    std::atomic<int> invoke_count_;
};

// ============================================================
// Server thread
// ============================================================
struct ServerContext {
    OmniRuntime runtime;
    CalcService service;
    volatile bool registered;
    volatile bool should_stop;
    uint16_t sm_port;
    ServerContext() : registered(false), should_stop(false), sm_port(0) {}
};

static void serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", ctx->sm_port);
    if (ret != 0) { fprintf(stderr, "Server: init failed (%d)\n", ret); return; }
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) { fprintf(stderr, "Server: register failed (%d)\n", ret); ctx->runtime.stop(); return; }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(20);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

// ============================================================
// Test fixture
// ============================================================
class FullIntegrationTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static ServerContext srv_;
    static std::thread srv_tid_;

    static void SetUpTestSuite() {
        unlink("/dev/shm/binder_CalcService");

        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19902", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        srv_.sm_port = SM_PORT;
        srv_tid_ = std::thread(serverThread, &srv_);

        for (int i = 0; i < 50 && !srv_.registered; i++) std::this_thread::sleep_for(std::chrono::microseconds(100000));
        ASSERT_TRUE(srv_.registered);
    }

    static void TearDownTestSuite() {
        srv_.should_stop = true;
        srv_tid_.join();

        // Verify service is gone after unregister
        std::this_thread::sleep_for(std::chrono::microseconds(200000));
        OmniRuntime c;
        if (c.init("127.0.0.1", SM_PORT) == 0) {
            ServiceInfo info;
            EXPECT_NE(c.lookupService("CalcService", info), 0);
            c.stop();
        }

        stopProcess(sm_pid_);
    }
};

TestPid FullIntegrationTest::sm_pid_ = 0;
ServerContext FullIntegrationTest::srv_;
std::thread FullIntegrationTest::srv_tid_;

// ============================================================
// Test Group 1: Dual-channel initialization
// ============================================================

TEST_F(FullIntegrationTest, ServiceHasTcpPort) {
    ASSERT_GT(srv_.service.port(), 0);
}

TEST_F(FullIntegrationTest, ServiceHasShmInRegistry) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    ServiceInfo info;
    ASSERT_EQ(c.lookupService("CalcService", info), 0);
    ASSERT_FALSE(info.shm_name.empty());
    EXPECT_NE(info.shm_name.find("binder_CalcService"), std::string::npos);
    ASSERT_EQ(info.shm_config.req_ring_capacity, 8u * 1024);
    ASSERT_EQ(info.shm_config.resp_ring_capacity, 12u * 1024);
    c.stop();
}

TEST_F(FullIntegrationTest, ServiceRegistryHostIsRoutable) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    ServiceInfo info;
    ASSERT_EQ(c.lookupService("CalcService", info), 0);
    ASSERT_FALSE(info.host.empty());
    EXPECT_NE(info.host, "0.0.0.0");
    c.stop();
}

TEST_F(FullIntegrationTest, SmReturnsHostId) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    ServiceInfo info;
    ASSERT_EQ(c.lookupService("CalcService", info), 0);
    ASSERT_FALSE(info.host_id.empty());
    EXPECT_EQ(info.host_id, c.hostId());
    c.stop();
}

// ============================================================
// Test Group 2: SHM auto-selection (same machine)
// ============================================================

TEST_F(FullIntegrationTest, InvokeViaShmAdd) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    Buffer req; req.writeInt32(100); req.writeInt32(200);
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ADD, req, resp, 5000), 0) << "invoke add failed";
    EXPECT_EQ(mustRead<int32_t>(resp, &Buffer::tryReadInt32), 300);
    c.stop();
}

TEST_F(FullIntegrationTest, InvokeViaShmEcho) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    Buffer req; req.writeString("SHM echo test");
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ECHO, req, resp, 5000), 0) << "invoke echo failed";
    EXPECT_EQ(mustReadString(resp), "SHM echo test");
    c.stop();
}

TEST_F(FullIntegrationTest, InterfaceMismatchIsRejected) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    int before = srv_.service.invokeCount();
    Buffer req; req.writeInt32(1); req.writeInt32(2);
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    EXPECT_EQ(c.invoke("CalcService", IFACE_ID + 1, METHOD_ADD, req, resp, 5000),
              static_cast<int>(ErrorCode::ERR_INTERFACE_NOT_FOUND));
    EXPECT_EQ(srv_.service.invokeCount(), before);
    c.stop();
}

TEST_F(FullIntegrationTest, InterfaceMismatchOnewayIsRejected) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    int before = srv_.service.invokeCount();
    Buffer req; req.writeInt32(7); req.writeInt32(8);
    ASSERT_EQ(c.connectService("CalcService"), 0);
    EXPECT_EQ(c.invokeOneWay("CalcService", IFACE_ID + 1, METHOD_ADD, req), 0);
    for (int i = 0; i < 10; ++i) {
        srv_.runtime.pollOnce(20);
        std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
    EXPECT_EQ(srv_.service.invokeCount(), before);
    c.stop();
}

// ============================================================
// Test Group 3: Multi-client SHM sharing
// ============================================================

TEST_F(FullIntegrationTest, ThreeClientsConcurrentInvoke) {
    struct ClientResult {
        int ret;
        int32_t result;
        volatile bool done;
    };

    ClientResult results[3];
    std::thread tids[3];

    struct ThreadArg {
        uint16_t port;
        int32_t a, b;
        ClientResult* result;
    };

    ThreadArg args[3];
    for (int i = 0; i < 3; i++) {
        args[i].port = SM_PORT;
        args[i].a = (i + 1) * 10;
        args[i].b = (i + 1) * 20;
        args[i].result = &results[i];
        results[i].ret = -1;
        results[i].result = 0;
        results[i].done = false;
    }

    auto clientFn = [](void* arg) {
        ThreadArg* a = static_cast<ThreadArg*>(arg);
        OmniRuntime c;
        if (c.init("127.0.0.1", a->port) != 0) {
            a->result->done = true;
            return;
        }
        Buffer req; req.writeInt32(a->a); req.writeInt32(a->b);
        Buffer resp;
        c.connectService("CalcService");
        a->result->ret = c.invoke("CalcService", IFACE_ID, METHOD_ADD, req, resp, 5000);
        if (a->result->ret == 0) {
            a->result->result = mustRead<int32_t>(resp, &Buffer::tryReadInt32);
        }
        c.stop();
        a->result->done = true;
    };

    for (int i = 0; i < 3; i++) {
        tids[i] = std::thread(clientFn, &args[i]);
    }
    for (int i = 0; i < 3; i++) {
        tids[i].join();
    }

    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(results[i].done) << "client thread did not finish";
        ASSERT_EQ(results[i].ret, 0) << "concurrent invoke failed";
        EXPECT_EQ(results[i].result, args[i].a + args[i].b) << "unexpected concurrent invoke result";
    }
}

TEST_F(FullIntegrationTest, SequentialMultiClientInvoke) {
    for (int c = 0; c < 5; c++) {
        OmniRuntime runtime;
        ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0) << "sequential client init failed";
        for (int i = 0; i < 5; i++) {
            Buffer req; req.writeInt32(c * 100 + i); req.writeInt32(1);
            Buffer resp;
            ASSERT_EQ(runtime.connectService("CalcService"), 0);
            int ret = runtime.invoke("CalcService", IFACE_ID, METHOD_ADD, req, resp, 5000);
            ASSERT_EQ(ret, 0) << "sequential invoke failed";
            ASSERT_EQ(mustRead<int32_t>(resp, &Buffer::tryReadInt32), c * 100 + i + 1);
        }
        runtime.stop();
    }
}

// ============================================================
// Test Group 4: Broadcast / Subscribe
// ============================================================

TEST_F(FullIntegrationTest, PublishAndSubscribeTopic) {
    static const char* TOPIC_NAME = "calc_result";
    uint32_t topic_id = fnv1a_32(TOPIC_NAME);

    // Server publishes topic
    int ret = srv_.runtime.publishTopic(TOPIC_NAME);
    ASSERT_EQ(ret, 0) << "publishTopic failed";

    // Subscriber
    OmniRuntime sub;
    ret = sub.init("127.0.0.1", SM_PORT);
    ASSERT_EQ(ret, 0) << "subscriber init failed";

    volatile bool received = false;
    volatile int32_t received_value = 0;

    ret = sub.subscribeTopic(TOPIC_NAME,
        [&received, &received_value](uint32_t tid, const Buffer& data) {
            (void)tid;
            Buffer buf(data.data(), data.size());
            received_value = mustRead<int32_t>(buf, &Buffer::tryReadInt32);
            received = true;
        });
    ASSERT_EQ(ret, 0) << "subscribeTopic failed";

    // Wait for subscription to propagate
    for (int i = 0; i < 20; i++) {
        sub.pollOnce(50);
        srv_.runtime.pollOnce(10);
    }

    // Broadcast data
    Buffer bdata;
    bdata.writeInt32(42);
    ret = srv_.runtime.broadcast(topic_id, bdata);
    ASSERT_EQ(ret, 0) << "broadcast failed";

    // Wait for subscriber to receive
    for (int i = 0; i < 50 && !received; i++) {
        sub.pollOnce(50);
        srv_.runtime.pollOnce(10);
    }

    ASSERT_TRUE(received) << "topic callback not received";
    EXPECT_EQ(received_value, 42);

    ret = sub.unsubscribeTopic(TOPIC_NAME);
    ASSERT_EQ(ret, 0) << "unsubscribeTopic failed";

    received = false;
    received_value = 0;
    Buffer second_broadcast;
    second_broadcast.writeInt32(84);
    ret = srv_.runtime.broadcast(topic_id, second_broadcast);
    ASSERT_EQ(ret, 0) << "second broadcast failed";

    for (int i = 0; i < 30 && !received; i++) {
        sub.pollOnce(50);
        srv_.runtime.pollOnce(10);
    }

    EXPECT_FALSE(received) << "topic callback should not be delivered after unsubscribe";
    sub.stop();
}

// ============================================================
// Test Group 5: Death notification
// ============================================================

TEST_F(FullIntegrationTest, DeathNotificationOnServiceCrash) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", SM_PORT);
    TestPid child = startProcess(g_program_path, "--child-death", port_str, "EphemeralService");
    ASSERT_GT(child, 0) << "Failed to start ephemeral service child";

    // Parent: wait for EphemeralService to appear
    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0) << "watcher init failed";

    bool found = false;
    for (int i = 0; i < 50; i++) {
        ServiceInfo info;
        if (watcher.lookupService("EphemeralService", info) == 0) {
            found = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
    }
    ASSERT_TRUE(found) << "EphemeralService not found";

    // Subscribe to death
    volatile bool death_received = false;
    int ret = watcher.subscribeServiceDeath("EphemeralService",
        [&death_received](const std::string& name) {
            (void)name;
            death_received = true;
        });
    ASSERT_EQ(ret, 0) << "subscribeServiceDeath failed";

    stopProcess(child);

    // Wait for death notification (SM heartbeat timeout ~10s, wait up to 15s)
    for (int i = 0; i < 150 && !death_received; i++) {
        watcher.pollOnce(100);
    }

    ASSERT_TRUE(death_received) << "death notification not received";
    watcher.stop();
}

TEST_F(FullIntegrationTest, UnsubscribeServiceDeathStopsCallback) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", SM_PORT);
    TestPid child = startProcess(g_program_path, "--child-death", port_str, "EphemeralService2");
    ASSERT_GT(child, 0) << "Failed to start ephemeral service2 child";

    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0) << "watcher init failed";

    bool found = false;
    for (int i = 0; i < 50; i++) {
        ServiceInfo info;
        if (watcher.lookupService("EphemeralService2", info) == 0) {
            found = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
    }
    ASSERT_TRUE(found) << "EphemeralService2 not found";

    volatile bool death_received = false;
    int ret = watcher.subscribeServiceDeath("EphemeralService2",
        [&death_received](const std::string& name) {
            (void)name;
            death_received = true;
        });
    ASSERT_EQ(ret, 0) << "subscribeServiceDeath failed";

    ret = watcher.unsubscribeServiceDeath("EphemeralService2");
    ASSERT_EQ(ret, 0) << "unsubscribeServiceDeath failed";

    stopProcess(child);

    for (int i = 0; i < 80 && !death_received; i++) {
        watcher.pollOnce(100);
    }

    EXPECT_FALSE(death_received) << "death callback should not be delivered after unsubscribe";
    watcher.stop();
}

// ============================================================
// Test Group 6: Lifecycle
// ============================================================

TEST_F(FullIntegrationTest, InvokeCountAccumulated) {
    ASSERT_GT(srv_.service.invokeCount(), 0);
}

int main(int argc, char** argv) {
    g_program_path = argv[0];

    if (argc >= 4 && strcmp(argv[1], "--child-death") == 0) {
        uint16_t port = (uint16_t)atoi(argv[2]);
        const char* svc_name = argv[3];

        class EphService : public Service {
        public:
            explicit EphService(const std::string& n) : Service(n) {
                iface_.interface_id = fnv1a_32(n.c_str());
                iface_.name = n;
            }
            const char* serviceName() const override { return iface_.name.c_str(); }
            const InterfaceInfo& interfaceInfo() const override { return iface_; }
        protected:
            int onInvoke(uint32_t, const Buffer&, Buffer&) override { return 0; }
        private:
            InterfaceInfo iface_;
        };

        OmniRuntime c;
        if (c.init("127.0.0.1", port) != 0) return 1;
        EphService svc(svc_name);
        if (c.registerService(&svc) != 0) { c.stop(); return 1; }
        while (true) { c.pollOnce(50); }
        return 0;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
