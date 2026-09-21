/**************************************************************************************************
 * @file        test_system_scenarios.cpp
 * @brief       系统级场景测试 — SM 重启、纯客户端重连、进程崩溃恢复
 * @details     覆盖单元测试无法验证的生产级场景：SM 断线重连、无服务客户端存活、
 *              服务端崩溃恢复等跨进程生命周期场景。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-07-19
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/

#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <omnibinder/proxy_base.h>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <atomic>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SCENARIO_SM_PORT = 19920;

// ─── 场景 1: 纯客户端（无注册服务）SM 重启后自动重连 ───
//
// @brief 纯客户端在 SM 重启后自动重连
// @details 这是之前缺失的测试：客户端未注册任何服务时，sendHeartbeat 的
//          for(local_services_) 循环为空，不会调用 sendToSM，因此重连永远不触发。
//          修复后 sendHeartbeat 开头无条件检查 reconnectServiceManagerIfNeeded。

TEST(SmRestartPureClient, ClientReconnectsAfterSmRestart) {
    // 启动 SM
    TestPid sm_pid = startProcess("./target/bin/service_manager",
        "--port", std::to_string(SCENARIO_SM_PORT).c_str(),
        "--log-level", "3");
    ASSERT_GT(sm_pid, 0);
    ASSERT_TRUE(waitPortReady(SCENARIO_SM_PORT, 30));

    // 创建纯客户端（不注册任何服务，不发布 topic）
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SCENARIO_SM_PORT), 0);

    // 启动 event-loop（在后台线程驱动，模拟真实运行）
    std::atomic<bool> client_running{true};
    std::thread client_thread([&]() {
        while (client_running.load()) {
            runtime.pollOnce(100);
        }
    });
    ScopedThreadJoiner client_join(client_thread, [&]() {
        client_running = false;
        runtime.stop();
    });

    // 验证客户端可以查询自己的运行时信息（证明 SM 连接正常）
    std::vector<RuntimeInfo> runtimes;
    int ret = runtime.listRuntimes(runtimes);
    EXPECT_EQ(ret, 0);
    EXPECT_GE(runtimes.size(), 1u) << "Client should see itself in runtime list";

    // 杀死 SM
    stopProcess(sm_pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 重启 SM
    sm_pid = startProcess("./target/bin/service_manager",
        "--port", std::to_string(SCENARIO_SM_PORT).c_str(),
        "--log-level", "3");
    ASSERT_GT(sm_pid, 0);
    ASSERT_TRUE(waitPortReady(SCENARIO_SM_PORT, 30));

    // 等待客户端自动重连（最多等 10 秒，heartbeat_interval 默认 5 秒）
    bool reconnected = false;
    for (int i = 0; i < 100 && !reconnected; ++i) {
        std::vector<RuntimeInfo> after;
        if (runtime.listRuntimes(after) == 0 && after.size() >= 1u) {
            reconnected = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(reconnected) << "Pure client should reconnect to SM after restart";

    // 清理
    client_running = false;
    runtime.stop();
    client_thread.join();
    stopProcess(sm_pid);
}

// ============================================================
// 注：死亡通知测试（场景 2）需要独立进程 + SIGKILL 才能
// 真正触发 SM 心跳超时检测。当前作为集成测试独立维护。
// ============================================================

// ============================================================
// P4a 话题实例化：服务级 / 实例级订阅、同名 topic 跨服务隔离
// ============================================================

static const uint16_t TOPIC_INSTANCE_SM_PORT = 19921;
static const uint32_t TOPIC_INST_IFACE_ID = fnv1a_32("TopicInstService");
static const uint32_t TOPIC_INST_METHOD_ECHO = fnv1a_32("Echo");
static const char* TOPIC_INST_NAME = "inst_topic";
static const uint32_t TOPIC_INST_ID = fnv1a_32(TOPIC_INST_NAME);

class TopicInstService : public Service {
public:
    explicit TopicInstService(const std::string& name) : Service(name) {
        iface_.interface_id = TOPIC_INST_IFACE_ID;
        iface_.name = name;
        iface_.methods.push_back(MethodInfo(TOPIC_INST_METHOD_ECHO, "Echo"));
    }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }

protected:
    int onInvoke(uint32_t, const Buffer& request, Buffer& response) override {
        if (request.size() > 0 && !response.writeRaw(request.data(), request.size())) {
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }

private:
    InterfaceInfo iface_;
};

struct TopicPublisherContext {
    OmniRuntime runtime;
    TopicInstService service;
    std::string instance_id;
    std::atomic<bool> registered;
    std::atomic<bool> should_stop;
    uint16_t sm_port;

    TopicPublisherContext(const std::string& service_name, const std::string& inst)
        : service(service_name), instance_id(inst), registered(false),
          should_stop(false), sm_port(0) {}
};

static void topicPublisherThread(void* arg) {
    TopicPublisherContext* ctx = static_cast<TopicPublisherContext*>(arg);
    if (ctx->runtime.init("127.0.0.1", ctx->sm_port) != 0) {
        return;
    }
    ctx->service.setInstanceId(ctx->instance_id);
    if (ctx->runtime.registerService(&ctx->service) != 0) {
        ctx->runtime.stop();
        return;
    }
    if (ctx->runtime.publishTopic(TOPIC_INST_NAME, 0x1234u) != 0) {
        ctx->runtime.unregisterService(&ctx->service);
        ctx->runtime.stop();
        return;
    }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(5);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

static void startTopicPublisher(TopicPublisherContext& ctx, std::thread& tid) {
    ctx.sm_port = TOPIC_INSTANCE_SM_PORT;
    tid = std::thread(topicPublisherThread, &ctx);
    for (int i = 0; i < 100 && !ctx.registered; ++i) {
        platform::sleepMs(50);
    }
    if (!ctx.registered) {
        fprintf(stderr, "startTopicPublisher: registration timed out for %s/%s\n",
                ctx.service.serviceName(), ctx.instance_id.c_str());
    }
}

static void stopTopicPublisher(TopicPublisherContext& ctx, std::thread& tid) {
    ctx.should_stop = true;
    if (tid.joinable()) {
        tid.join();
    }
}

static int broadcastTopicUntil(OmniRuntime& pub, OmniRuntime& sub,
                               std::atomic<int>& counter, int32_t value,
                               int want_more, int iterations = 120) {
    Buffer data;
    data.writeInt32(value);
    const int before = counter.load();
    for (int i = 0; i < iterations && (counter.load() - before) < want_more; ++i) {
        pub.broadcast(TOPIC_INST_ID, data);
        sub.pollOnce(20);
        platform::sleepMs(5);
    }
    return counter.load() - before;
}

// 反复广播指定发布者，暴露任何误投递（跑满固定轮次，不关心接收计数）
static void broadcastTopicRepeatedly(OmniRuntime& pub, OmniRuntime& sub, int32_t value) {
    std::atomic<int> ignored(0);
    broadcastTopicUntil(pub, sub, ignored, value, 1 << 30, 120);
}

class TopicInstanceScenario : public ::testing::Test {
protected:
    static TestPid sm_pid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager",
            "--port", std::to_string(TOPIC_INSTANCE_SM_PORT).c_str(),
            "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(TOPIC_INSTANCE_SM_PORT, 30));
    }

    static void TearDownTestSuite() {
        stopProcess(sm_pid_);
    }
};

TestPid TopicInstanceScenario::sm_pid_ = 0;

TEST_F(TopicInstanceScenario, ServiceLevelReceivesAllInstancesAndSource) {
    TopicPublisherContext p1("TopicInstService", "dev-a");
    TopicPublisherContext p2("TopicInstService", "dev-b");
    std::thread t1;
    std::thread t2;
    startTopicPublisher(p1, t1);
    startTopicPublisher(p2, t2);
    ScopedThreadJoiner j1(t1, [&p1]() { p1.should_stop = true; });
    ScopedThreadJoiner j2(t2, [&p2]() { p2.should_stop = true; });
    ASSERT_TRUE(p1.registered);
    ASSERT_TRUE(p2.registered);

    const std::string h1 = p1.runtime.hostId();
    const std::string h2 = p2.runtime.hostId();

    OmniRuntime sub;
    ASSERT_EQ(sub.init("127.0.0.1", TOPIC_INSTANCE_SM_PORT), 0);

    std::atomic<int> count_a(0);
    std::atomic<int> count_b(0);
    std::atomic<bool> source_ok(true);
    int ret = sub.subscribeTopic(
        TopicSubscriptionSelector::service("TopicInstService"), TOPIC_INST_NAME, 0,
        [&count_a, &count_b, &source_ok, &h1, &h2](
            const std::string& host_id, const std::string& instance_id,
            uint32_t, const Buffer&) {
            if (instance_id == "dev-a") {
                count_a++;
                if (host_id != h1) source_ok = false;
            } else if (instance_id == "dev-b") {
                count_b++;
                if (host_id != h2) source_ok = false;
            } else {
                source_ok = false;
            }
        }, nullptr);
    ASSERT_EQ(ret, 0);

    broadcastTopicUntil(p1.runtime, sub, count_a, 11, 1);
    broadcastTopicUntil(p2.runtime, sub, count_b, 22, 1);

    EXPECT_GE(count_a.load(), 1);
    EXPECT_GE(count_b.load(), 1);
    EXPECT_TRUE(source_ok.load());

    stopTopicPublisher(p1, t1);
    stopTopicPublisher(p2, t2);
    sub.stop();
}

TEST_F(TopicInstanceScenario, InstanceLevelReceivesOnlySelectedInstance) {
    TopicPublisherContext p1("TopicInstService", "dev-a");
    TopicPublisherContext p2("TopicInstService", "dev-b");
    std::thread t1;
    std::thread t2;
    startTopicPublisher(p1, t1);
    startTopicPublisher(p2, t2);
    ScopedThreadJoiner j1(t1, [&p1]() { p1.should_stop = true; });
    ScopedThreadJoiner j2(t2, [&p2]() { p2.should_stop = true; });
    ASSERT_TRUE(p1.registered);
    ASSERT_TRUE(p2.registered);

    OmniRuntime sub;
    ASSERT_EQ(sub.init("127.0.0.1", TOPIC_INSTANCE_SM_PORT), 0);

    std::atomic<int> count_a(0);
    std::atomic<int> count_b(0);
    int ret = sub.subscribeTopic(
        TopicSubscriptionSelector::instance("TopicInstService", p1.runtime.hostId(), "dev-a"),
        TOPIC_INST_NAME, 0,
        [&count_a, &count_b](const std::string&, const std::string& instance_id,
                             uint32_t, const Buffer&) {
            if (instance_id == "dev-a") {
                count_a++;
            } else {
                count_b++;
            }
        }, nullptr);
    ASSERT_EQ(ret, 0);

    broadcastTopicUntil(p1.runtime, sub, count_a, 11, 1);
    broadcastTopicRepeatedly(p2.runtime, sub, 22);

    EXPECT_GE(count_a.load(), 1);
    EXPECT_EQ(count_b.load(), 0);

    stopTopicPublisher(p1, t1);
    stopTopicPublisher(p2, t2);
    sub.stop();
}

TEST_F(TopicInstanceScenario, DifferentServicesWithSameTopicDoNotCollide) {
    TopicPublisherContext px("TopicInstSvcX", "x");
    TopicPublisherContext py("TopicInstSvcY", "y");
    std::thread tx;
    std::thread ty;
    startTopicPublisher(px, tx);
    startTopicPublisher(py, ty);
    ScopedThreadJoiner jx(tx, [&px]() { px.should_stop = true; });
    ScopedThreadJoiner jy(ty, [&py]() { py.should_stop = true; });
    ASSERT_TRUE(px.registered);
    ASSERT_TRUE(py.registered);

    OmniRuntime sub;
    ASSERT_EQ(sub.init("127.0.0.1", TOPIC_INSTANCE_SM_PORT), 0);

    std::atomic<int> count_x(0);
    std::atomic<int> count_y(0);
    int ret = sub.subscribeTopic(
        TopicSubscriptionSelector::service("TopicInstSvcX"), TOPIC_INST_NAME, 0,
        [&count_x, &count_y](const std::string&, const std::string& instance_id,
                             uint32_t, const Buffer&) {
            if (instance_id == "x") {
                count_x++;
            } else {
                count_y++;
            }
        }, nullptr);
    ASSERT_EQ(ret, 0);

    broadcastTopicUntil(px.runtime, sub, count_x, 11, 1);
    broadcastTopicRepeatedly(py.runtime, sub, 22);

    EXPECT_GE(count_x.load(), 1);
    EXPECT_EQ(count_y.load(), 0);

    stopTopicPublisher(px, tx);
    stopTopicPublisher(py, ty);
    sub.stop();
}

TEST_F(TopicInstanceScenario, SubscriptionWithoutPublisherSelectorMatchesAnyPublisher) {
    TopicPublisherContext p("TopicInstService", "");
    std::thread t;
    startTopicPublisher(p, t);
    ScopedThreadJoiner j(t, [&p]() { p.should_stop = true; });
    ASSERT_TRUE(p.registered);

    OmniRuntime sub;
    ASSERT_EQ(sub.init("127.0.0.1", TOPIC_INSTANCE_SM_PORT), 0);

    std::atomic<int> received(0);
    int ret = sub.subscribeTopic(TOPIC_INST_NAME,
        [&received](uint32_t, const Buffer&) { received++; }, nullptr);
    ASSERT_EQ(ret, 0);

    broadcastTopicUntil(p.runtime, sub, received, 42, 1);

    EXPECT_GE(received.load(), 1);

    stopTopicPublisher(p, t);
    sub.stop();
}

// ============================================================
// P5 端到端场景：多实例服务身份
//   ① 单板两实例（不同"传感器口"）：各自注册、各自 SHM 数据面、各自广播来源可辨
//   ② 跨板同名（不同 host_id）：并存、按 (host_id, instance_id) 分别绑定控制
//   ③ 崩溃重启：同身份接管、异身份共存
// 三个场景均为真实进程级：ServiceManager + 子进程服务（本二进制 --child-instance 模式）
// ============================================================

static const uint16_t SCEN1_SM_PORT = 19980;
static const uint16_t SCEN2_SM_PORT = 19981;
static const uint16_t SCEN3_SM_PORT = 19982;

static const uint32_t SCEN_IFACE_ID    = fnv1a_32("SysScenarioSvc");
static const uint32_t SCEN_METHOD_ECHO = fnv1a_32("EchoInstance");
static const char*    SCEN_SERVICE_NAME = "SysScenarioSvc";
static const char*    SCEN_TOPIC_NAME   = "SysScenarioUpdate";
static const uint32_t SCEN_TOPIC_ID     = fnv1a_32(SCEN_TOPIC_NAME);

static const char* g_scenario_program_path = NULL;

// 回显自身实例身份的测试服务：客户端据此断言 RPC 命中的是哪一个实例
class SysScenarioService : public Service {
public:
    explicit SysScenarioService(const std::string& name) : Service(name) {
        iface_.interface_id = SCEN_IFACE_ID;
        iface_.name = SCEN_SERVICE_NAME;
        iface_.methods.push_back(MethodInfo(SCEN_METHOD_ECHO, "EchoInstance"));
    }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }

protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        (void)request;
        if (method_id != SCEN_METHOD_ECHO) {
            return static_cast<int>(ErrorCode::ERR_METHOD_NOT_FOUND);
        }
        if (!response.writeString(instanceId())) {
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }

private:
    InterfaceInfo iface_;
};

class SysScenarioProxy : public ServiceProxyBase {
public:
    SysScenarioProxy(OmniRuntime& runtime, const std::string& name)
        : ServiceProxyBase(runtime, name) {}

    int EchoInstance(std::string& out) {
        Buffer req;
        Buffer resp;
        int ret = runtime_.invoke(connectionKey(), SCEN_IFACE_ID, SCEN_METHOD_ECHO, 0,
                                  req, resp, 3000);
        if (ret != 0) return ret;
        Buffer buf(resp.data(), resp.size());
        if (!buf.tryReadString(out)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        return 0;
    }
};

static bool listScenarioInstances(OmniRuntime& rt, std::vector<ServiceInfo>& out) {
    return rt.listServiceInstances(SCEN_SERVICE_NAME, out) == 0;
}

static bool waitScenarioInstanceCount(OmniRuntime& rt, size_t want, int max_attempts = 200) {
    for (int i = 0; i < max_attempts; ++i) {
        std::vector<ServiceInfo> instances;
        if (listScenarioInstances(rt, instances) && instances.size() >= want) {
            return true;
        }
        rt.pollOnce(50);
    }
    return false;
}

static bool findScenarioInstance(OmniRuntime& rt, const std::string& instance_id,
                                 ServiceInfo& out) {
    std::vector<ServiceInfo> instances;
    if (!listScenarioInstances(rt, instances)) return false;
    for (size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].instance_id == instance_id) {
            out = instances[i];
            return true;
        }
    }
    return false;
}

static bool waitScenarioEcho(SysScenarioProxy& proxy, OmniRuntime& rt,
                             const std::string& want, int max_attempts = 200) {
    for (int i = 0; i < max_attempts; ++i) {
        std::string got;
        if (proxy.EchoInstance(got) == 0 && got == want) {
            return true;
        }
        rt.pollOnce(50);
    }
    return false;
}

// 每个用例独立启停 SM 与子进程，TearDown 保证全部回收，杜绝孤儿进程阻塞后续用例
class SystemScenarioTest : public ::testing::Test {
protected:
    TestPid sm_pid_;
    std::vector<TestPid> children_;

    SystemScenarioTest() : sm_pid_(0) {}

    bool startScenarioSm(uint16_t port) {
        sm_pid_ = startProcess("./target/bin/service_manager",
            "--port", std::to_string(port).c_str(), "--log-level", "3");
        if (sm_pid_ <= 0) return false;
        return waitPortReady(port, 30);
    }

    TestPid spawnScenarioChild(uint16_t port, const std::string& host_id,
                               const std::string& instance_id) {
        const char* host_arg = host_id.empty() ? "-" : host_id.c_str();
        TestPid pid = startProcess(g_scenario_program_path, "--child-instance",
                                   std::to_string(port).c_str(),
                                   host_arg, instance_id.c_str());
        if (pid > 0) {
            children_.push_back(pid);
        }
        return pid;
    }

    void stopScenarioChild(TestPid pid) {
        if (pid <= 0) return;
        for (size_t i = 0; i < children_.size(); ++i) {
            if (children_[i] == pid) {
                children_.erase(children_.begin() + static_cast<long>(i));
                break;
            }
        }
        stopProcess(pid);
    }

    void TearDown() override {
        for (size_t i = 0; i < children_.size(); ++i) {
            stopProcess(children_[i]);
        }
        children_.clear();
        if (sm_pid_ > 0) {
            stopProcess(sm_pid_);
            sm_pid_ = 0;
        }
    }
};

// ============================================================
// 场景 1 — 单板两实例（ttyUSB0 / ttyUSB1）
//
// @brief   同服务名、不同 instance_id、同 host_id 的两个实例并存
// @details 断言：两实例同时注册；各自 RPC 命中正确实例；两条独立的 SHM 数据面
//          同时可用（P1 SHM 命名修复）；服务级话题订阅同时收到两实例广播，
//          且回调可按来源实例身份区分。
// ============================================================
TEST_F(SystemScenarioTest, SingleBoardTwoInstancesDistinctShmAndTopicSources) {
    ASSERT_TRUE(startScenarioSm(SCEN1_SM_PORT)) << "failed to start ServiceManager";

    TestPid child_a = spawnScenarioChild(SCEN1_SM_PORT, "", "ttyUSB0");
    TestPid child_b = spawnScenarioChild(SCEN1_SM_PORT, "", "ttyUSB1");
    ASSERT_GT(child_a, 0);
    ASSERT_GT(child_b, 0);

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SCEN1_SM_PORT), 0);
    ASSERT_TRUE(waitScenarioInstanceCount(rt, 2))
        << "两个不同 instance_id 的同名实例必须并存";

    std::vector<ServiceInfo> instances;
    ASSERT_TRUE(listScenarioInstances(rt, instances));
    ASSERT_EQ(instances.size(), 2u);

    ServiceInfo a;
    ServiceInfo b;
    ASSERT_TRUE(findScenarioInstance(rt, "ttyUSB0", a));
    ASSERT_TRUE(findScenarioInstance(rt, "ttyUSB1", b));
    EXPECT_EQ(a.name, SCEN_SERVICE_NAME);
    EXPECT_EQ(b.name, SCEN_SERVICE_NAME);
    EXPECT_EQ(a.host_id, b.host_id) << "同一块板上的两个实例必须共享 host_id";
    EXPECT_EQ(a.host_id, rt.hostId());
    EXPECT_NE(a.instance_id, b.instance_id);

    SysScenarioProxy proxy_a(rt, SCEN_SERVICE_NAME);
    SysScenarioProxy proxy_b(rt, SCEN_SERVICE_NAME);
    ASSERT_EQ(proxy_a.connect(a.host_id, "ttyUSB0"), 0);
    ASSERT_EQ(proxy_b.connect(b.host_id, "ttyUSB1"), 0);
    for (int i = 0; i < 5; ++i) {
        std::string got_a;
        std::string got_b;
        ASSERT_EQ(proxy_a.EchoInstance(got_a), 0);
        EXPECT_EQ(got_a, "ttyUSB0") << "RPC 命中错误的实例";
        ASSERT_EQ(proxy_b.EchoInstance(got_b), 0);
        EXPECT_EQ(got_b, "ttyUSB1") << "RPC 命中错误的实例";
    }

    std::atomic<int> seen_a(0);
    std::atomic<int> seen_b(0);
    std::atomic<bool> source_ok(true);
    const std::string host_a = a.host_id;
    const std::string host_b = b.host_id;
    int sub_ret = rt.subscribeTopic(
        TopicSubscriptionSelector::service(SCEN_SERVICE_NAME), SCEN_TOPIC_NAME, 0,
        [&seen_a, &seen_b, &source_ok, &host_a, &host_b](
            const std::string& host_id, const std::string& instance_id,
            uint32_t, const Buffer&) {
            if (instance_id == "ttyUSB0") {
                seen_a.fetch_add(1, std::memory_order_release);
                if (host_id != host_a) source_ok.store(false, std::memory_order_release);
            } else if (instance_id == "ttyUSB1") {
                seen_b.fetch_add(1, std::memory_order_release);
                if (host_id != host_b) source_ok.store(false, std::memory_order_release);
            } else {
                source_ok.store(false, std::memory_order_release);
            }
        }, NULL);
    ASSERT_EQ(sub_ret, 0);

    for (int i = 0; i < 300
         && (seen_a.load(std::memory_order_acquire) == 0
             || seen_b.load(std::memory_order_acquire) == 0); ++i) {
        rt.pollOnce(20);
    }
    EXPECT_GE(seen_a.load(std::memory_order_acquire), 1)
        << "服务级订阅必须收到 ttyUSB0 的广播";
    EXPECT_GE(seen_b.load(std::memory_order_acquire), 1)
        << "服务级订阅必须收到 ttyUSB1 的广播";
    EXPECT_TRUE(source_ok.load(std::memory_order_acquire))
        << "广播回调携带的来源实例身份必须与发布实例一致";

    RuntimeStats stats;
    ASSERT_EQ(rt.getStats(stats), 0);
    EXPECT_GE(stats.shm_connections, 2u)
        << "两实例必须各自拥有独立的 SHM 数据面（P1 SHM 命名修复）";
    EXPECT_EQ(stats.tcp_connections, 0u) << "同机实例必须走 SHM";

    proxy_a.disconnect();
    proxy_b.disconnect();
    rt.stop();
}

// ============================================================
// 场景 2 — 跨板同名（HMI 分别控制）
//
// @brief   同服务名、不同 host_id 的两实例并存，客户端按 (host_id, instance_id) 绑定
// @details 用 host_id 覆盖模拟两台板卡。断言：listServiceInstances 返回两个
//          含不同 host_id 的实例；客户端分别绑定后 RPC 命中正确实例；跨 host_id
//          走 TCP；自称 board-a 的运行时到 board-a 走 SHM、到 board-b 走 TCP，
//          证明 host_id 决定同机/跨机传输选择。
// ============================================================
TEST_F(SystemScenarioTest, CrossBoardSameNameBindByHostAndInstance) {
    ASSERT_TRUE(startScenarioSm(SCEN2_SM_PORT)) << "failed to start ServiceManager";

    TestPid child_a = spawnScenarioChild(SCEN2_SM_PORT, "board-a", "hmi-a");
    TestPid child_b = spawnScenarioChild(SCEN2_SM_PORT, "board-b", "hmi-b");
    ASSERT_GT(child_a, 0);
    ASSERT_GT(child_b, 0);

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SCEN2_SM_PORT), 0);
    ASSERT_TRUE(waitScenarioInstanceCount(rt, 2))
        << "不同 host_id 的同名实例必须并存（不得跨板顶替）";

    std::vector<ServiceInfo> instances;
    ASSERT_TRUE(listScenarioInstances(rt, instances));
    ASSERT_EQ(instances.size(), 2u);
    ServiceInfo a;
    ServiceInfo b;
    ASSERT_TRUE(findScenarioInstance(rt, "hmi-a", a));
    ASSERT_TRUE(findScenarioInstance(rt, "hmi-b", b));
    EXPECT_EQ(a.host_id, "board-a");
    EXPECT_EQ(b.host_id, "board-b");
    EXPECT_NE(a.host_id, b.host_id) << "两个实例必须暴露不同的 host_id";

    SysScenarioProxy proxy_a(rt, SCEN_SERVICE_NAME);
    SysScenarioProxy proxy_b(rt, SCEN_SERVICE_NAME);
    ASSERT_EQ(proxy_a.connect("board-a", "hmi-a"), 0);
    ASSERT_EQ(proxy_b.connect("board-b", "hmi-b"), 0);
    for (int i = 0; i < 3; ++i) {
        std::string got_a;
        std::string got_b;
        ASSERT_EQ(proxy_a.EchoInstance(got_a), 0);
        EXPECT_EQ(got_a, "hmi-a");
        ASSERT_EQ(proxy_b.EchoInstance(got_b), 0);
        EXPECT_EQ(got_b, "hmi-b");
    }

    RuntimeStats stats;
    ASSERT_EQ(rt.getStats(stats), 0);
    EXPECT_EQ(stats.tcp_connections, 2u) << "跨 host_id 目标必须走 TCP";
    EXPECT_EQ(stats.shm_connections, 0u);

    // host_id 覆盖：本运行时自称 board-a，则到 board-a 同机走 SHM，到 board-b 跨机走 TCP
    OmniRuntime board_a_rt;
    board_a_rt.setHostId("board-a");
    ASSERT_EQ(board_a_rt.init("127.0.0.1", SCEN2_SM_PORT), 0);
    EXPECT_EQ(board_a_rt.hostId(), "board-a")
        << "setHostId 必须在 init 后生效";

    SysScenarioProxy local_a(board_a_rt, SCEN_SERVICE_NAME);
    SysScenarioProxy remote_b(board_a_rt, SCEN_SERVICE_NAME);
    ASSERT_EQ(local_a.connect("board-a", "hmi-a"), 0);
    ASSERT_EQ(remote_b.connect("board-b", "hmi-b"), 0);
    std::string got_local;
    std::string got_remote;
    ASSERT_EQ(local_a.EchoInstance(got_local), 0);
    EXPECT_EQ(got_local, "hmi-a");
    ASSERT_EQ(remote_b.EchoInstance(got_remote), 0);
    EXPECT_EQ(got_remote, "hmi-b");

    RuntimeStats board_stats;
    ASSERT_EQ(board_a_rt.getStats(board_stats), 0);
    EXPECT_EQ(board_stats.shm_connections, 1u)
        << "与目标同 host_id 时必须选择 SHM";
    EXPECT_EQ(board_stats.tcp_connections, 1u)
        << "与目标异 host_id 时必须选择 TCP";

    proxy_a.disconnect();
    proxy_b.disconnect();
    local_a.disconnect();
    remote_b.disconnect();
    board_a_rt.stop();
    rt.stop();
}

// ============================================================
// 场景 3a — 崩溃重启，同身份接管
//
// @brief   实例被杀死后用相同 (name, host_id, instance_id) 重启
// @details 断言：SM 摘除旧实例；重启后该身份只有一条注册（不重复）；此前
//          正在调用的客户端恢复，RPC 再次命中该实例。
// ============================================================
TEST_F(SystemScenarioTest, CrashRestartSameIdentityTakesOverAndClientRecovers) {
    ASSERT_TRUE(startScenarioSm(SCEN3_SM_PORT)) << "failed to start ServiceManager";

    TestPid child = spawnScenarioChild(SCEN3_SM_PORT, "", "restart-0");
    ASSERT_GT(child, 0);

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SCEN3_SM_PORT), 0);
    ASSERT_TRUE(waitScenarioInstanceCount(rt, 1));

    ServiceInfo before;
    ASSERT_TRUE(findScenarioInstance(rt, "restart-0", before));
    const std::string host_id = before.host_id;

    SysScenarioProxy proxy(rt, SCEN_SERVICE_NAME);
    ASSERT_EQ(proxy.connect(host_id, "restart-0"), 0);
    ASSERT_TRUE(waitScenarioEcho(proxy, rt, "restart-0", 60))
        << "崩溃前实例必须可调用";

    stopScenarioChild(child);
    child = 0;

    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        ServiceInfo unused;
        gone = !findScenarioInstance(rt, "restart-0", unused);
        if (!gone) {
            rt.pollOnce(50);
        }
    }
    ASSERT_TRUE(gone) << "SM 必须摘除被杀死实例";

    child = spawnScenarioChild(SCEN3_SM_PORT, "", "restart-0");
    ASSERT_GT(child, 0);

    bool re_registered = false;
    for (int i = 0; i < 200 && !re_registered; ++i) {
        std::vector<ServiceInfo> current;
        if (listScenarioInstances(rt, current) && current.size() == 1u
            && current[0].instance_id == "restart-0") {
            re_registered = true;
        } else {
            rt.pollOnce(50);
        }
    }
    ASSERT_TRUE(re_registered)
        << "同身份重启必须接管：该身份下只保留一条注册";

    std::vector<ServiceInfo> current;
    ASSERT_TRUE(listScenarioInstances(rt, current));
    EXPECT_EQ(current.size(), 1u)
        << "相同 (name, host_id, instance_id) 不得产生第二条注册";

    bool recovered = false;
    for (int i = 0; i < 300 && !recovered; ++i) {
        std::string got;
        if (proxy.EchoInstance(got) == 0 && got == "restart-0") {
            recovered = true;
        } else {
            rt.pollOnce(50);
        }
    }
    EXPECT_TRUE(recovered)
        << "此前正在调用的客户端必须在重启后恢复 RPC";

    proxy.disconnect();
    rt.stop();
}

// ============================================================
// 场景 3b — 崩溃重启，异身份共存（不接管）
//
// @brief   重启使用不同 instance_id 时不得冒用旧身份，且两身份可共存
// @details 断言：原实例被杀死后，以 dev-1 重启不会以 dev-0 身份出现；
//          再拉起 dev-0 后两身份共存，且分别可按 (host_id, instance_id) 寻址。
// ============================================================
TEST_F(SystemScenarioTest, RestartWithDifferentInstanceIdCoexists) {
    ASSERT_TRUE(startScenarioSm(SCEN3_SM_PORT)) << "failed to start ServiceManager";

    TestPid child0 = spawnScenarioChild(SCEN3_SM_PORT, "", "dev-0");
    ASSERT_GT(child0, 0);

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SCEN3_SM_PORT), 0);
    ASSERT_TRUE(waitScenarioInstanceCount(rt, 1));

    ServiceInfo before;
    ASSERT_TRUE(findScenarioInstance(rt, "dev-0", before));
    const std::string host_id = before.host_id;

    stopScenarioChild(child0);
    child0 = 0;

    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        ServiceInfo unused;
        gone = !findScenarioInstance(rt, "dev-0", unused);
        if (!gone) {
            rt.pollOnce(50);
        }
    }
    ASSERT_TRUE(gone);

    TestPid child1 = spawnScenarioChild(SCEN3_SM_PORT, "", "dev-1");
    ASSERT_GT(child1, 0);
    ASSERT_TRUE(waitScenarioInstanceCount(rt, 1));

    ServiceInfo after;
    ASSERT_TRUE(findScenarioInstance(rt, "dev-1", after));
    ServiceInfo stale;
    EXPECT_FALSE(findScenarioInstance(rt, "dev-0", stale))
        << "以不同 instance_id 重启不得冒用旧身份";

    TestPid child0b = spawnScenarioChild(SCEN3_SM_PORT, "", "dev-0");
    ASSERT_GT(child0b, 0);
    ASSERT_TRUE(waitScenarioInstanceCount(rt, 2))
        << "不同 instance_id 必须共存而非互相顶替";

    std::vector<ServiceInfo> instances;
    ASSERT_TRUE(listScenarioInstances(rt, instances));
    EXPECT_EQ(instances.size(), 2u)
        << "不同 (host_id, instance_id) 必须共存";
    ServiceInfo a;
    ServiceInfo b;
    ASSERT_TRUE(findScenarioInstance(rt, "dev-0", a));
    ASSERT_TRUE(findScenarioInstance(rt, "dev-1", b));

    SysScenarioProxy proxy0(rt, SCEN_SERVICE_NAME);
    SysScenarioProxy proxy1(rt, SCEN_SERVICE_NAME);
    ASSERT_EQ(proxy0.connect(host_id, "dev-0"), 0);
    ASSERT_EQ(proxy1.connect(host_id, "dev-1"), 0);
    std::string got0;
    std::string got1;
    ASSERT_EQ(proxy0.EchoInstance(got0), 0);
    EXPECT_EQ(got0, "dev-0");
    ASSERT_EQ(proxy1.EchoInstance(got1), 0);
    EXPECT_EQ(got1, "dev-1");

    proxy0.disconnect();
    proxy1.disconnect();
    rt.stop();
}

// 子进程模式：注册带 instance_id 的服务，回显自身实例身份，并周期广播该身份
static int runScenarioChildInstance(uint16_t port, const std::string& host_id,
                                    const std::string& instance_id) {
    OmniRuntime rt;
    if (!host_id.empty()) {
        rt.setHostId(host_id);
    }
    if (rt.init("127.0.0.1", port) != 0) {
        return 1;
    }

    SysScenarioService service(SCEN_SERVICE_NAME);
    service.setInstanceId(instance_id);
    if (rt.registerService(&service) != 0) {
        rt.stop();
        return 1;
    }
    if (rt.publishTopic(SCEN_TOPIC_NAME) != 0) {
        rt.stop();
        return 1;
    }

    Buffer payload;
    payload.writeString(instance_id);
    while (true) {
        rt.pollOnce(10);
        rt.broadcast(SCEN_TOPIC_ID, payload);
        platform::sleepMs(20);
    }
    return 0;
}

int main(int argc, char** argv) {
    g_scenario_program_path = argv[0];

    if (argc >= 5 && strcmp(argv[1], "--child-instance") == 0) {
        uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
        std::string host_id = argv[3];
        if (host_id == "-") {
            host_id.clear();
        }
        return runScenarioChildInstance(port, host_id, argv[4]);
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
