#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <pthread.h>
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

static void* tsClientServerThread(void* arg) {
    TsClientServerCtx* ctx = static_cast<TsClientServerCtx*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) return NULL;
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) return NULL;
    ctx->registered = true;
    while (!ctx->should_stop) ctx->runtime.pollOnce(50);
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

static void* concurrentInvokeThread(void* arg) {
    ClientThreadArg* ctx = static_cast<ClientThreadArg*>(arg);
    for (int i = 0; i < ctx->iterations; ++i) {
        Buffer req, resp;
        char payload[64];
        snprintf(payload, sizeof(payload), "thread-%d-iter-%d", ctx->thread_index, i);
        req.writeRaw(payload, strlen(payload));
        int ret = ctx->runtime->invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, req, resp, 5000);
        if (ret != 0) { ctx->failures++; continue; }
        std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
        if (result != payload) ctx->failures++;
    }
    ctx->done = true;
    return NULL;
}

class ThreadsafeClientAndReconnectTest : public ::testing::Test {
protected:
    static pid_t sm_pid_;
    static TsClientServerCtx* server_ctx_;
    static pthread_t server_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19931", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new TsClientServerCtx();
        ASSERT_EQ(pthread_create(&server_tid_, NULL, tsClientServerThread, server_ctx_), 0);
        for (int i = 0; i < 50 && !server_ctx_->registered; ++i) usleep(100000);
        ASSERT_TRUE(server_ctx_->registered);
    }

    static void TearDownTestSuite() {
        server_ctx_->should_stop = true;
        pthread_join(server_tid_, NULL);
        delete server_ctx_;
        stopProcess(sm_pid_);
    }
};

pid_t ThreadsafeClientAndReconnectTest::sm_pid_ = 0;
TsClientServerCtx* ThreadsafeClientAndReconnectTest::server_ctx_ = nullptr;
pthread_t ThreadsafeClientAndReconnectTest::server_tid_ = 0;

TEST_F(ThreadsafeClientAndReconnectTest, ConcurrentInvokeSameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(runtime.connectService("ThreadSafeService"), 0);

    pthread_t tids[6];
    ClientThreadArg args[6];
    for (int i = 0; i < 6; ++i) {
        args[i].runtime = &runtime;
        args[i].thread_index = i;
        args[i].iterations = 40;
        ASSERT_EQ(pthread_create(&tids[i], NULL, concurrentInvokeThread, &args[i]), 0);
    }
    for (int i = 0; i < 6; ++i) {
        pthread_join(tids[i], NULL);
        EXPECT_TRUE(args[i].done);
        EXPECT_EQ(args[i].failures, 0);
    }
    runtime.stop();
}

TEST_F(ThreadsafeClientAndReconnectTest, SmRestartRecoverySameClient) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    stopProcess(sm_pid_);
    usleep(300000);
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19931", "--log-level", "3");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    bool recovered = false;
    for (int i = 0; i < 80 && !recovered; ++i) {
        Buffer req, resp;
        const char* payload = "after-restart";
        req.writeRaw(payload, strlen(payload));
        if (runtime.connectService("ThreadSafeService") != 0) continue;
        int ret = runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, req, resp, 3000);
        if (ret == 0) {
            std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
            if (result == payload) recovered = true;
        }
        usleep(100000);
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
    ASSERT_EQ(runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, warm_req, warm_resp, 3000), 0);

    server_ctx_->should_stop = true;
    pthread_join(server_tid_, NULL);
    delete server_ctx_;

    server_ctx_ = new TsClientServerCtx();
    ASSERT_EQ(pthread_create(&server_tid_, NULL, tsClientServerThread, server_ctx_), 0);
    for (int i = 0; i < 80 && !server_ctx_->registered; ++i) usleep(100000);
    ASSERT_TRUE(server_ctx_->registered);

    bool server_recovered = false;
    for (int i = 0; i < 80 && !server_recovered; ++i) {
        runtime.clearServiceCache();
        runtime.closeAllConnections();
        if (runtime.connectService("ThreadSafeService") != 0) continue;

        Buffer req, resp;
        const char* payload = "after-server-restart";
        req.writeRaw(payload, strlen(payload));
        int ret = runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, req, resp, 3000);
        if (ret == 0) {
            std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
            if (result == payload) server_recovered = true;
        }
        usleep(100000);
    }
    EXPECT_TRUE(server_recovered);
    runtime.stop();
}
