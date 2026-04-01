#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <unistd.h>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19930;
static std::atomic<int> success_count(0);
static std::atomic<int> failure_count(0);

static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = 0x12345678;

class ThreadSafetyTestService : public Service {
public:
    ThreadSafetyTestService() : Service("TestService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "TestService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "TestService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ECHO) {
            if (!response.writeRaw(request.data(), request.size())) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }
private:
    InterfaceInfo iface_;
};

class ThreadSafetyTest : public ::testing::Test {
protected:
    static pid_t sm_pid_;
    static OmniRuntime* server_rt_;
    static ThreadSafetyTestService* svc_;
    static std::thread* server_thread_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19930", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        svc_ = new ThreadSafetyTestService();
        server_rt_ = new OmniRuntime();
        ASSERT_EQ(server_rt_->init("127.0.0.1", SM_PORT), 0);
        ASSERT_EQ(server_rt_->registerService(svc_), 0);
        server_thread_ = new std::thread([]() { server_rt_->run(); });
        usleep(500000);
    }

    static void TearDownTestSuite() {
        server_rt_->stop();
        server_thread_->join();
        delete server_thread_;
        delete svc_;
        delete server_rt_;
        stopProcess(sm_pid_);
    }
};

pid_t ThreadSafetyTest::sm_pid_ = 0;
OmniRuntime* ThreadSafetyTest::server_rt_ = nullptr;
ThreadSafetyTestService* ThreadSafetyTest::svc_ = nullptr;
std::thread* ThreadSafetyTest::server_thread_ = nullptr;

static void concurrentLookup(OmniRuntime& runtime, int thread_id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        ServiceInfo info;
        int ret = runtime.lookupService("TestService", info);
        if (ret == 0) success_count++;
        else failure_count++;
        std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
    }
}

static void concurrentInvoke(OmniRuntime& runtime, int thread_id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        Buffer request, response;
        request.writeInt32(thread_id * 1000 + i);
        int ret = runtime.invoke("TestService", IFACE_ID, METHOD_ECHO, request, response, 5000);
        if (ret == 0) success_count++;
        else failure_count++;
        std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
    }
}

TEST_F(ThreadSafetyTest, ConcurrentLookup) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    std::thread rt_thread([&runtime]() { runtime.run(); });
    usleep(200000);
    success_count = 0;
    failure_count = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(concurrentLookup, std::ref(runtime), i, 100);
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failure_count.load(), 0);
    runtime.stop();
    rt_thread.join();
}

TEST_F(ThreadSafetyTest, ConcurrentInvoke) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    std::thread rt_thread([&runtime]() { runtime.run(); });
    usleep(200000);
    success_count = 0;
    failure_count = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(concurrentInvoke, std::ref(runtime), i, 50);
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failure_count.load(), 0);
    runtime.stop();
    rt_thread.join();
}
