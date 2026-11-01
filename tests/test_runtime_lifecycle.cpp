#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder.h>
#include <omnibinder/proxy_base.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <mutex>

using namespace omnibinder;
using namespace omnibinder::test;

namespace {

const uint16_t LIFE_SM_PORT = 19985;
const uint32_t LIFE_IFACE_ID = fnv1a_32("LifecycleService");
const uint32_t LIFE_METHOD_ECHO = fnv1a_32("Echo");

class LifecycleService : public Service {
public:
    LifecycleService() : Service("LifecycleService"), started(false), stopped(false) {
        iface_.interface_id = LIFE_IFACE_ID;
        iface_.name = "LifecycleService";
        iface_.methods.push_back(MethodInfo(LIFE_METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "LifecycleService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
    std::atomic<bool> started;
    std::atomic<bool> stopped;
    std::thread::id start_thread;
    std::thread::id stop_thread;

protected:
    void onStart() override {
        start_thread = std::this_thread::get_id();
        started.store(true, std::memory_order_release);
    }
    void onStop() override {
        stop_thread = std::this_thread::get_id();
        stopped.store(true, std::memory_order_release);
    }
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id != LIFE_METHOD_ECHO) {
            return static_cast<int>(ErrorCode::ERR_METHOD_NOT_FOUND);
        }
        int32_t value = 0;
        Buffer req(request.data(), request.size());
        if (!req.tryReadInt32(value)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        if (!response.writeInt32(value + 1)) {
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }

private:
    InterfaceInfo iface_;
};

class LifecycleProxy : public ServiceProxyBase {
public:
    explicit LifecycleProxy(OmniRuntime& runtime)
        : ServiceProxyBase(runtime, "LifecycleService") {}
    int Echo(int32_t value, int32_t& out) {
        Buffer req;
        Buffer resp;
        if (!req.writeInt32(value)) {
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        int ret = runtime_.invoke(connectionKey(), LIFE_IFACE_ID,
                                  LIFE_METHOD_ECHO, 0, req, resp, 3000);
        if (ret != 0) return ret;
        Buffer view(resp.data(), resp.size());
        if (!view.tryReadInt32(out)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        return 0;
    }
};

static bool waitUntil(const std::function<bool()>& predicate, int attempts,
                      int sleep_ms = 100) {
    for (int i = 0; i < attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    return predicate();
}

} // namespace

TEST(RuntimeLifecycleTest, InitOnlyCreatesResources) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", 19984), 0);
    EXPECT_FALSE(runtime.isRunning());
    EXPECT_FALSE(runtime.isReady());
    runtime.stop();
}

TEST(RuntimeLifecycleTest, RejectsTickWithBlockingPoll) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", 19983), 0);
    EXPECT_EQ(runtime.start(-1, []() {}), static_cast<int>(ErrorCode::ERR_INVALID_PARAM));
    EXPECT_FALSE(runtime.isRunning());
}

TEST(RuntimeLifecycleTest, TickCallbackRunsOnOwnerAndStops) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", 19983), 0);
    std::atomic<int> ticks(0);
    std::thread::id tick_thread;
    std::thread::id main_thread = std::this_thread::get_id();
    int start_ret = 1;
    std::thread owner([&]() {
        start_ret = runtime.start(1, [&runtime, &ticks, &tick_thread]() {
            tick_thread = std::this_thread::get_id();
            if (++ticks >= 3) runtime.stop();
        });
    });
    owner.join();
    EXPECT_EQ(start_ret, 0);
    EXPECT_GE(ticks.load(), 3);
    EXPECT_NE(tick_thread, main_thread);
}

TEST(RuntimeLifecycleTest, SpinPollModeStops) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", 19983), 0);
    std::thread owner([&runtime]() {
        EXPECT_EQ(runtime.start(0, LoopTickCallback()), 0);
    });
    EXPECT_TRUE(waitUntil([&runtime]() { return runtime.isRunning(); }, 30));
    runtime.stop();
    owner.join();
    EXPECT_FALSE(runtime.isRunning());
}

TEST(RuntimeLifecycleTest, StartWaitsForLateServiceManager) {
    TestPid sm = 0;
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", LIFE_SM_PORT), 0);

    LifecycleService service;
    ASSERT_EQ(runtime.registerService(&service), 0);

    std::thread owner([&runtime]() { runtime.start(); });
    EXPECT_TRUE(waitUntil([&runtime]() { return runtime.isRunning(); }, 30));
    EXPECT_FALSE(runtime.isReady());
    EXPECT_TRUE(waitUntil([&service]() {
        return service.started.load(std::memory_order_acquire);
    }, 30));

    sm = startProcess("./target/bin/service_manager", "--port", "19985", "--log-level", "3");
    ASSERT_GT(sm, 0);
    ASSERT_TRUE(waitPortReady(LIFE_SM_PORT, 30));
    EXPECT_TRUE(waitUntil([&runtime]() { return runtime.isReady(); }, 80));

    runtime.stop();
    owner.join();
    stopProcess(sm);
}

TEST(RuntimeLifecycleTest, StartAutoConnectsDeclaredProxy) {
    TestPid sm = startProcess("./target/bin/service_manager", "--port", "19986", "--log-level", "3");
    ASSERT_GT(sm, 0);
    ASSERT_TRUE(waitPortReady(19986, 30));

    OmniRuntime server;
    ASSERT_EQ(server.init("127.0.0.1", 19986), 0);
    LifecycleService service;
    ASSERT_EQ(server.registerService(&service), 0);
    std::thread server_owner([&server]() { server.start(); });
    ASSERT_TRUE(waitUntil([&server]() { return server.isReady(); }, 80));

    OmniRuntime client;
    ASSERT_EQ(client.init("127.0.0.1", 19986), 0);
    LifecycleProxy proxy(client);
    std::thread client_owner([&client]() { client.start(); });
    ASSERT_TRUE(waitUntil([&proxy]() { return proxy.isConnected(); }, 80));

    int32_t value = 0;
    ASSERT_EQ(proxy.Echo(41, value), 0);
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(service.started.load(std::memory_order_acquire));
    EXPECT_NE(service.start_thread, std::this_thread::get_id())
        << "onStart must run on the start() owner thread, not registerService caller";

    client.stop();
    client_owner.join();
    server.stop();
    server_owner.join();
    EXPECT_TRUE(service.stopped.load(std::memory_order_acquire));
    stopProcess(sm);
}
