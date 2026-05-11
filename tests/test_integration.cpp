// test_integration.cpp - End-to-end integration test
//
// This test starts a ServiceManager process, then uses OmniRuntime to:
// 1. Register a service (in a background thread)
// 2. Discover the service from another client
// 3. Invoke methods on the service
// 4. List services / query interfaces
// 5. Unregister and verify cleanup

#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder.h>
#include <pthread.h>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19901;
static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("TestService");

// ============================================================
// A simple test service
// ============================================================
class TestService : public Service {
public:
    TestService() : Service("TestService"), invoke_count_(0) {
        setShmConfig(ShmConfig(4 * 1024, 4 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = "TestService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }

    const char* serviceName() const override { return "TestService"; }
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
        } else {
            if (!response.writeInt32(-1)) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }

private:
    InterfaceInfo iface_;
    int invoke_count_;
};

// ============================================================
// Server thread context
// ============================================================
struct ServerContext {
    OmniRuntime runtime;
    TestService service;
    volatile bool registered;
    volatile bool should_stop;
    uint16_t sm_port;

    ServerContext() : registered(false), should_stop(false), sm_port(0) {}
};

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);

    int ret = ctx->runtime.init("127.0.0.1", ctx->sm_port);
    if (ret != 0) {
        fprintf(stderr, "Server: failed to init runtime\n");
        return NULL;
    }

    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) {
        fprintf(stderr, "Server: failed to register service\n");
        ctx->runtime.stop();
        return NULL;
    }

    ctx->registered = true;

    // Event loop: process incoming requests
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(50);
    }

    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

// ============================================================
// Test fixture
// ============================================================
class IntegrationTest : public ::testing::Test {
protected:
    static pid_t sm_pid_;
    static ServerContext* server_ctx_;
    static pthread_t server_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19901", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        // Start server in background thread
        server_ctx_ = new ServerContext();
        server_ctx_->sm_port = SM_PORT;
        ASSERT_EQ(pthread_create(&server_tid_, NULL, serverThread, server_ctx_), 0);

        // Wait for service to be registered
        for (int i = 0; i < 50 && !server_ctx_->registered; i++) {
            usleep(100000);
        }
        ASSERT_TRUE(server_ctx_->registered);
    }

    static void TearDownTestSuite() {
        if (server_ctx_) {
            server_ctx_->should_stop = true;
            pthread_join(server_tid_, NULL);

            // Verify service is gone after unregister
            usleep(200000);
            OmniRuntime runtime;
            if (runtime.init("127.0.0.1", SM_PORT) == 0) {
                ServiceInfo info;
                EXPECT_NE(runtime.lookupService("TestService", info), 0);
                runtime.stop();
            }

            delete server_ctx_;
            server_ctx_ = nullptr;
        }
        stopProcess(sm_pid_);
    }
};

pid_t IntegrationTest::sm_pid_ = 0;
ServerContext* IntegrationTest::server_ctx_ = nullptr;
pthread_t IntegrationTest::server_tid_ = 0;

// ============================================================
// Tests
// ============================================================

TEST_F(IntegrationTest, ConnectToSm) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    runtime.stop();
}

TEST_F(IntegrationTest, ListServices) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    std::vector<ServiceInfo> services;
    ASSERT_EQ(runtime.listServices(services), 0);
    ASSERT_GE(services.size(), 1u);

    bool found = false;
    for (size_t i = 0; i < services.size(); i++) {
        if (services[i].name == "TestService") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
    runtime.stop();
}

TEST_F(IntegrationTest, LookupService) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    ServiceInfo info;
    ASSERT_EQ(runtime.lookupService("TestService", info), 0);
    EXPECT_EQ(info.name, "TestService");
    EXPECT_EQ(info.port, server_ctx_->service.port());
    EXPECT_FALSE(info.host_id.empty());
    EXPECT_EQ(info.shm_config.req_ring_capacity, 4u * 1024);
    EXPECT_EQ(info.shm_config.resp_ring_capacity, 4u * 1024);
    ASSERT_EQ(info.interfaces.size(), 1u);
    EXPECT_EQ(info.interfaces[0].name, "TestService");
    EXPECT_EQ(info.interfaces[0].methods.size(), 2u);
    runtime.stop();
}

TEST_F(IntegrationTest, LookupNonExistent) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    ServiceInfo info;
    EXPECT_NE(runtime.lookupService("NonExistentService", info), 0);
    runtime.stop();
}

TEST_F(IntegrationTest, QueryInterfaces) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    std::vector<InterfaceInfo> ifaces;
    ASSERT_EQ(runtime.queryInterfaces("TestService", ifaces), 0);
    ASSERT_EQ(ifaces.size(), 1u);
    EXPECT_EQ(ifaces[0].name, "TestService");
    EXPECT_EQ(ifaces[0].interface_id, IFACE_ID);

    bool found_add = false, found_echo = false;
    for (size_t i = 0; i < ifaces[0].methods.size(); i++) {
        if (ifaces[0].methods[i].name == "Add") found_add = true;
        if (ifaces[0].methods[i].name == "Echo") found_echo = true;
    }
    EXPECT_TRUE(found_add);
    EXPECT_TRUE(found_echo);
    runtime.stop();
}

TEST_F(IntegrationTest, InvokeAdd) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    Buffer request;
    request.writeInt32(17);
    request.writeInt32(25);

    Buffer response;
    ASSERT_EQ(runtime.invoke("TestService", IFACE_ID, METHOD_ADD,
                        request, response, 5000), 0);
    ASSERT_GE(response.size(), 4u);

    EXPECT_EQ(mustRead<int32_t>(response, &Buffer::tryReadInt32), 42);
    runtime.stop();
}

TEST_F(IntegrationTest, InvokeEcho) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    Buffer request;
    request.writeString("Hello OmniBinder!");

    Buffer response;
    ASSERT_EQ(runtime.invoke("TestService", IFACE_ID, METHOD_ECHO,
                        request, response, 5000), 0);
    EXPECT_GT(response.size(), 0u);

    std::string echo = mustReadString(response);
    EXPECT_EQ(echo, "Hello OmniBinder!");
    runtime.stop();
}

TEST_F(IntegrationTest, MultipleInvocations) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    for (int i = 0; i < 10; i++) {
        Buffer request;
        request.writeInt32(i);
        request.writeInt32(100);

        Buffer response;
        ASSERT_EQ(runtime.invoke("TestService", IFACE_ID, METHOD_ADD,
                            request, response, 5000), 0);
        EXPECT_EQ(mustRead<int32_t>(response, &Buffer::tryReadInt32), i + 100);
    }

    runtime.stop();
}
