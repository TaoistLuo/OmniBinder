// test_full_integration.cpp - 综合集成测试（GTest）
//
// 测试内容：
// 1. 服务初始化同时创建 TCP + SHM
// 2. SM lookup 返回 shm_name，客户端对同机服务自动选择 SHM
// 3. 多客户端共享同一个 SHM
// 4. 广播 / 订阅（pub/sub）
// 5. 服务状态 watch
// 6. 服务注销清理

#include <gtest/gtest.h>
#include "test_common.h"
#include "runtime_thread.h"
#include <omnibinder/omnibinder.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("CalcService");
static const uint16_t SM_PORT = 19902;

static const char* g_program_path = nullptr;

// 校验同机数据面确实走了 SHM：否则 SHM 故障静默降级 TCP 时用例照过，掩盖缺陷
static void expectShmTransport(OmniRuntime& c) {
    RuntimeStats stats;
    EXPECT_EQ(c.getStats(stats), 0);
    EXPECT_GE(stats.shm_connections, 1u) << "同机调用必须走 SHM";
    EXPECT_EQ(stats.tcp_connections, 0u) << "同机调用不得回退 TCP";
}

static bool waitReady(OmniRuntime& runtime, int attempts = 100) {
    for (int i = 0; i < attempts; ++i) {
        if (runtime.isReady()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return runtime.isReady();
}

// ============================================================
// 测试服务：CalcService
// ============================================================
class CalcService : public Service {
public:
    CalcService() : Service("CalcService"), invoke_count_(0) {
        setShmConfig(ShmConfig(256 * 1024, 256 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = "CalcService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
        // start() 激活服务阶段统一发布，避免测试线程提前驱动 runtime
        declarePublishedTopic("zeta/topic", 0);
        declarePublishedTopic("alpha/topic", 0);
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
// 回归测试服务：接口序列化后不足 32 字节（name="A"、零方法）
// 用于验证 decodeCountedVector 不再用固定的 /32 上限误判小接口
// ============================================================
class ShortIfaceService : public Service {
public:
    ShortIfaceService() : Service("ShortIfaceSvc") {
        iface_.interface_id = fnv1a_32("A");
        iface_.name = "A";  // 4(id) + 4(len) + 1("A") + 2(method_count) = 11 字节
    }
    const char* serviceName() const override { return "ShortIfaceSvc"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    int onInvoke(uint32_t, const Buffer&, Buffer&) override { return 0; }
private:
    InterfaceInfo iface_;
};

// ============================================================
// 服务端线程
// ============================================================
struct ServerContext {
    OmniRuntime runtime;
    CalcService service;
    std::atomic<bool> registered;
    uint16_t sm_port;
    ServerContext() : registered(false), sm_port(0) {}
};

static void serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", ctx->sm_port);
    if (ret != 0) { fprintf(stderr, "Server: init failed (%d)\n", ret); return; }
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) { fprintf(stderr, "Server: register failed (%d)\n", ret); return; }
    // 服务和已声明 topic 在 start() 激活阶段注册/发布；start() 阻塞直到主线程 stop()
    ctx->runtime.start();
}

// ============================================================
// 测试 fixture
// ============================================================
class FullIntegrationTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static ServerContext srv_;
    static std::thread srv_tid_;

    static void SetUpTestSuite() {
        // 旧全局 SHM 命名（binder_<service>）已废弃，当前为 per-client SHM；
        // 如仍有残留由平台层 shmUnlink 负责，测试不再硬编码 /dev/shm 路径。

        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19902", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        srv_.sm_port = SM_PORT;
        srv_tid_ = std::thread(serverThread, &srv_);

        for (int i = 0; i < 100 && !srv_.registered; i++) {
            ServiceInfo info;
            if (waitReady(srv_.runtime)
                && srv_.runtime.lookupService("CalcService", info) == 0) {
                std::vector<std::string> topics;
                if (srv_.runtime.queryPublishedTopics("CalcService", topics) == 0
                    && std::find(topics.begin(), topics.end(), "alpha/topic") != topics.end()
                    && std::find(topics.begin(), topics.end(), "zeta/topic") != topics.end()) {
                    srv_.registered = true;
                }
            }
            if (!srv_.registered) {
                std::this_thread::sleep_for(std::chrono::microseconds(100000));
            }
        }
        ASSERT_TRUE(srv_.registered);
    }

    static void TearDownTestSuite() {
        if (srv_tid_.joinable()) {
            // 先注销再 stop：验证注销路径确实把服务从 SM 移除
            srv_.runtime.unregisterService(&srv_.service);
            srv_.runtime.stop();
            srv_tid_.join();
        }

        // 验证注销后服务已消失
        std::this_thread::sleep_for(std::chrono::microseconds(200000));
        OmniRuntime c;
        if (c.init("127.0.0.1", SM_PORT) == 0) {
            test::RuntimeThread owner(c);
            waitReady(c);
            ServiceInfo info;
            EXPECT_NE(c.lookupService("CalcService", info), 0);
            owner.stop();
        }

        stopProcess(sm_pid_);
    }
};

TestPid FullIntegrationTest::sm_pid_ = 0;
ServerContext FullIntegrationTest::srv_;
std::thread FullIntegrationTest::srv_tid_;

// ============================================================
// 测试组 1：双通道初始化
// ============================================================

TEST_F(FullIntegrationTest, ServiceHasTcpPort) {
    // 经线程安全 API 获取该服务端口：Service::port() 由 owner 线程在 registerService
    // 内写入，主测试线程直读会与 owner 线程构成数据竞争（TSan 可捕获）。改用注册表查询
    ServiceInfo info;
    ASSERT_EQ(srv_.runtime.lookupService("CalcService", info), 0);
    ASSERT_GT(info.port, 0);
}

TEST_F(FullIntegrationTest, ListServices) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    std::vector<ServiceInfo> services;
    ASSERT_EQ(c.listServices(services), 0);
    ASSERT_GE(services.size(), 1u);
    bool found = false;
    for (size_t i = 0; i < services.size(); ++i) {
        if (services[i].name == "CalcService") { found = true; break; }
    }
    EXPECT_TRUE(found);
    owner.stop();
}

TEST_F(FullIntegrationTest, LookupNonExistent) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    ServiceInfo info;
    EXPECT_NE(c.lookupService("NonExistentService", info), 0);
    owner.stop();
}

TEST_F(FullIntegrationTest, QueryInterfaces) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    std::vector<InterfaceInfo> ifaces;
    ASSERT_EQ(c.queryInterfaces("CalcService", ifaces), 0);
    ASSERT_EQ(ifaces.size(), 1u);
    EXPECT_EQ(ifaces[0].name, "CalcService");
    EXPECT_EQ(ifaces[0].interface_id, IFACE_ID);
    bool found_add = false, found_echo = false;
    for (size_t i = 0; i < ifaces[0].methods.size(); ++i) {
        if (ifaces[0].methods[i].name == "Add") found_add = true;
        if (ifaces[0].methods[i].name == "Echo") found_echo = true;
    }
    EXPECT_TRUE(found_add);
    EXPECT_TRUE(found_echo);
    owner.stop();
}

// ============================================================
// 回归 #5：queryInterfaces 不得误拒小接口
//
// @brief  序列化后不足 32 字节的接口必须能通过 queryInterfaces 取回
// @details 修复前 decodeCountedVector 用固定的 count > remaining/32 上限校验，
//          11 字节的接口（count=1）被判为 ERR_DESERIALIZE；修复后按接口单元
//          最小长度（InterfaceInfo=10）校验，正常返回。
// ============================================================
TEST_F(FullIntegrationTest, QuerySmallSerializedInterfaceSucceeds) {
    OmniRuntime provider;
    ASSERT_EQ(provider.init("127.0.0.1", SM_PORT), 0);

    ShortIfaceService svc;
    ASSERT_EQ(provider.registerService(&svc), 0);
    test::RuntimeThread owner(provider);
    ASSERT_TRUE(waitReady(provider));

    std::vector<InterfaceInfo> ifaces;
    ASSERT_EQ(provider.queryInterfaces("ShortIfaceSvc", ifaces), 0);
    ASSERT_EQ(ifaces.size(), 1u);
    EXPECT_EQ(ifaces[0].name, "A");
    EXPECT_EQ(ifaces[0].interface_id, fnv1a_32("A"));
    EXPECT_TRUE(ifaces[0].methods.empty());

    owner.stop();
}

TEST_F(FullIntegrationTest, QueryPublishedTopics) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    std::vector<std::string> topics(1, "stale");
    ASSERT_EQ(c.queryPublishedTopics("CalcService", topics), 0);
    // 其它用例可能在同一服务上再发布话题（如 calc_result），故断言包含而非精确计数
    EXPECT_NE(std::find(topics.begin(), topics.end(), "alpha/topic"), topics.end());
    EXPECT_NE(std::find(topics.begin(), topics.end(), "zeta/topic"), topics.end());
    EXPECT_EQ(std::find(topics.begin(), topics.end(), "stale"), topics.end());
    EXPECT_EQ(c.queryPublishedTopics("NonExistentService", topics),
              static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND));
    EXPECT_TRUE(topics.empty());
    owner.stop();
}

TEST_F(FullIntegrationTest, ServiceRegistryHostIsRoutable) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    ServiceInfo info;
    ASSERT_EQ(c.lookupService("CalcService", info), 0);
    ASSERT_FALSE(info.host.empty());
    EXPECT_NE(info.host, "0.0.0.0");
    owner.stop();
}

TEST_F(FullIntegrationTest, SmReturnsHostId) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    ServiceInfo info;
    ASSERT_EQ(c.lookupService("CalcService", info), 0);
    ASSERT_FALSE(info.host_id.empty());
    EXPECT_EQ(info.host_id, c.hostId());
    owner.stop();
}

// ============================================================
// 测试组 2：SHM 自动选择（同机）
// ============================================================

TEST_F(FullIntegrationTest, InvokeViaShmAdd) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    Buffer req; req.writeInt32(100); req.writeInt32(200);
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ADD, 0, req, resp, 5000), 0) << "invoke add failed";
    EXPECT_EQ(mustRead<int32_t>(resp, &Buffer::tryReadInt32), 300);
    expectShmTransport(c);
    owner.stop();
}

TEST_F(FullIntegrationTest, InvokeViaShmEcho) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    Buffer req; req.writeString("SHM echo test");
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ECHO, 0, req, resp, 5000), 0) << "invoke echo failed";
    EXPECT_EQ(mustReadString(resp), "SHM echo test");
    expectShmTransport(c);
    owner.stop();
}

TEST_F(FullIntegrationTest, InvokeViaShmLargeEchoThenSmall) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    ASSERT_EQ(c.connectService("CalcService"), 0);

    const size_t large_size = 100 * 1024;
    Buffer large_request;
    std::vector<uint8_t> expected(large_size);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>((i * 29u + (i >> 7)) & 0xffu);
    }
    ASSERT_TRUE(large_request.writeRaw(expected.data(), expected.size()));
    Buffer large_response;
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ECHO, 0,
                       large_request, large_response, 5000), 0);
    ASSERT_EQ(large_response.size(), expected.size());
    EXPECT_EQ(memcmp(large_response.data(), expected.data(), expected.size()), 0);

    Buffer small_request;
    ASSERT_TRUE(small_request.writeString("after-large"));
    Buffer small_response;
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ECHO, 0,
                       small_request, small_response, 5000), 0);
    EXPECT_EQ(mustReadString(small_response), "after-large");
    expectShmTransport(c);
    owner.stop();
}

TEST_F(FullIntegrationTest, InterfaceMismatchIsRejected) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    int before = srv_.service.invokeCount();
    Buffer req; req.writeInt32(1); req.writeInt32(2);
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    EXPECT_EQ(c.invoke("CalcService", IFACE_ID + 1, METHOD_ADD, 0, req, resp, 5000),
              static_cast<int>(ErrorCode::ERR_INTERFACE_NOT_FOUND));
    EXPECT_EQ(srv_.service.invokeCount(), before);
    owner.stop();
}

TEST_F(FullIntegrationTest, InterfaceMismatchOnewayIsRejected) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0) << "client init failed";
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    int before = srv_.service.invokeCount();
    Buffer req; req.writeInt32(7); req.writeInt32(8);
    ASSERT_EQ(c.connectService("CalcService"), 0);
    EXPECT_EQ(c.invokeOneWay("CalcService", IFACE_ID + 1, METHOD_ADD, 0, req), 0);
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
    EXPECT_EQ(srv_.service.invokeCount(), before);
    owner.stop();
}

// ============================================================
// 测试组 3：多客户端共享 SHM
// ============================================================

TEST_F(FullIntegrationTest, ThreeClientsConcurrentInvoke) {
    struct ClientResult {
        int ret;
        int32_t result;
        std::atomic<bool> done;
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
        test::RuntimeThread owner(c);
        waitReady(c);
        Buffer req; req.writeInt32(a->a); req.writeInt32(a->b);
        Buffer resp;
        c.connectService("CalcService");
        a->result->ret = c.invoke("CalcService", IFACE_ID, METHOD_ADD, 0, req, resp, 5000);
        if (a->result->ret == 0) {
            a->result->result = mustRead<int32_t>(resp, &Buffer::tryReadInt32);
        }
        owner.stop();
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
        test::RuntimeThread owner(runtime);
        ASSERT_TRUE(waitReady(runtime));
        for (int i = 0; i < 5; i++) {
            Buffer req; req.writeInt32(c * 100 + i); req.writeInt32(1);
            Buffer resp;
            ASSERT_EQ(runtime.connectService("CalcService"), 0);
            int ret = runtime.invoke("CalcService", IFACE_ID, METHOD_ADD, 0, req, resp, 5000);
            ASSERT_EQ(ret, 0) << "sequential invoke failed";
            ASSERT_EQ(mustRead<int32_t>(resp, &Buffer::tryReadInt32), c * 100 + i + 1);
        }
        owner.stop();
    }
}

// ============================================================
// 测试组 4：广播 / 订阅
// ============================================================

TEST_F(FullIntegrationTest, PublishAndSubscribeTopic) {
    static const char* TOPIC_NAME = "calc_result";
    uint32_t topic_id = fnv1a_32(TOPIC_NAME);

    // 服务端发布 topic
    int ret = srv_.runtime.publishTopic(TOPIC_NAME);
    ASSERT_EQ(ret, 0) << "publishTopic failed";

    // 订阅者
    OmniRuntime sub;
    ret = sub.init("127.0.0.1", SM_PORT);
    ASSERT_EQ(ret, 0) << "subscriber init failed";
    test::RuntimeThread owner(sub);
    ASSERT_TRUE(waitReady(sub));

    std::atomic<bool> received(false);
    std::atomic<int32_t> received_value(0);

    ret = sub.subscribeTopic(TOPIC_NAME,
        [&received, &received_value](uint32_t tid, const Buffer& data) {
            (void)tid;
            Buffer buf(data.data(), data.size());
            received_value = mustRead<int32_t>(buf, &Buffer::tryReadInt32);
            received = true;
        }, nullptr);
    ASSERT_EQ(ret, 0) << "subscribeTopic failed";

    // 反复广播直至订阅者收到（owner 线程自行处理发布者通知与数据面连接）
    Buffer bdata;
    bdata.writeInt32(42);
    for (int i = 0; i < 100 && !received.load(); i++) {
        ASSERT_EQ(srv_.runtime.broadcast(topic_id, bdata), 0) << "broadcast failed";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ASSERT_TRUE(received.load()) << "topic callback not received";
    EXPECT_EQ(received_value.load(), 42);

    ret = sub.unsubscribeTopic(TOPIC_NAME);
    ASSERT_EQ(ret, 0) << "unsubscribeTopic failed";

    received = false;
    received_value = 0;
    Buffer second_broadcast;
    second_broadcast.writeInt32(84);
    for (int i = 0; i < 30 && !received.load(); i++) {
        ASSERT_EQ(srv_.runtime.broadcast(topic_id, second_broadcast), 0)
            << "second broadcast failed";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_FALSE(received.load()) << "topic callback should not be delivered after unsubscribe";
    owner.stop();
}

// ============================================================
// 回归 #3：同话题多选择器订阅，取消其一不得拆掉另一条仍生效的绑定
//
// @brief  服务级 + 实例级订阅同一发布者实例，取消实例级后服务级仍收到广播
// @details 两种选择器命中同一发布者时共用同一条数据面连接。修复前
//          clearTopicSubscriptionBindings 在 erase 之后用 find 复查同话题，
//          恒为 false，于取消实例级订阅时把整条连接回收，服务级订阅静默失效。
// ============================================================
TEST_F(FullIntegrationTest, SameTopicMultiSelectorUnsubscribeKeepsServiceLevel) {
    static const char* TOPIC_NAME = "regression/multi_selector";
    uint32_t topic_id = fnv1a_32(TOPIC_NAME);
    ASSERT_EQ(srv_.runtime.publishTopic(TOPIC_NAME), 0);

    OmniRuntime sub;
    ASSERT_EQ(sub.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(sub);
    ASSERT_TRUE(waitReady(sub));

    ServiceInfo pub_info;
    ASSERT_EQ(sub.lookupService("CalcService", pub_info), 0);

    const TopicSubscriptionSelector service_selector =
        TopicSubscriptionSelector::service("CalcService");
    const TopicSubscriptionSelector instance_selector =
        TopicSubscriptionSelector::instance("CalcService", pub_info.host_id, pub_info.instance_id);

    std::atomic<int> service_hits(0);
    std::atomic<int> instance_hits(0);

    ASSERT_EQ(sub.subscribeTopic(service_selector, TOPIC_NAME, 0,
        [&service_hits](const std::string&, const std::string&, uint32_t, const Buffer&) {
            service_hits.fetch_add(1, std::memory_order_release);
        }, nullptr), 0);
    ASSERT_EQ(sub.subscribeTopic(instance_selector, TOPIC_NAME, 0,
        [&instance_hits](const std::string&, const std::string&, uint32_t, const Buffer&) {
            instance_hits.fetch_add(1, std::memory_order_release);
        }, nullptr), 0);

    // 反复广播直至 SM 发布者通知到达并建立数据面直连
    Buffer first;
    first.writeInt32(1);
    for (int i = 0; i < 100
         && (service_hits.load(std::memory_order_acquire) == 0
             || instance_hits.load(std::memory_order_acquire) == 0); ++i) {
        ASSERT_EQ(srv_.runtime.broadcast(topic_id, first), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_GE(service_hits.load(std::memory_order_acquire), 1)
        << "service-level subscription must receive before unsubscribe";
    ASSERT_GE(instance_hits.load(std::memory_order_acquire), 1)
        << "instance-level subscription must receive before unsubscribe";

    // 仅取消实例级选择器
    ASSERT_EQ(sub.unsubscribeTopic(instance_selector, TOPIC_NAME), 0);
    const int service_before = service_hits.load(std::memory_order_acquire);
    const int instance_before = instance_hits.load(std::memory_order_acquire);

    Buffer second;
    second.writeInt32(2);
    for (int i = 0; i < 100
         && service_hits.load(std::memory_order_acquire) == service_before; ++i) {
        ASSERT_EQ(srv_.runtime.broadcast(topic_id, second), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_GT(service_hits.load(std::memory_order_acquire), service_before)
        << "service-level subscription must survive instance-level unsubscribe";
    EXPECT_EQ(instance_hits.load(std::memory_order_acquire), instance_before)
        << "instance-level subscription must stop after unsubscribe";

    owner.stop();
}

// ============================================================
// 测试组 5：服务状态 watch
// ============================================================

TEST_F(FullIntegrationTest, DeathNotificationOnServiceCrash) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", SM_PORT);
    TestPid child = startProcess(g_program_path, "--child-death", port_str, "EphemeralService");
    ASSERT_GT(child, 0) << "Failed to start ephemeral service child";

    // 父进程：等待 EphemeralService 出现
    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0) << "watcher init failed";
    test::RuntimeThread owner(watcher);
    ASSERT_TRUE(waitReady(watcher));

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

    // 观察服务状态：初始 online 忽略，只关心服务崩溃后的 offline
    std::atomic<bool> death_received(false);
    uint64_t watch_id = 0;
    std::vector<ServiceWatchRequest> filters;
    filters.push_back(ServiceWatchRequest("EphemeralService", "", ""));
    int ret = watcher.watchServices(filters,
        [&death_received](uint64_t, const ServiceIdentity&, bool online) {
            if (!online) death_received = true;
        }, watch_id);
    ASSERT_EQ(ret, 0) << "watchServices failed";
    ASSERT_NE(watch_id, 0u);

    stopProcess(child);

    // 等待服务离线通知（SM 心跳超时约 10s，最多等 15s）
    for (int i = 0; i < 150 && !death_received.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(death_received.load()) << "death notification not received";
    owner.stop();
}

TEST_F(FullIntegrationTest, UnwatchServiceStopsCallback) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", SM_PORT);
    TestPid child = startProcess(g_program_path, "--child-death", port_str, "EphemeralService2");
    ASSERT_GT(child, 0) << "Failed to start ephemeral service2 child";

    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0) << "watcher init failed";
    test::RuntimeThread owner(watcher);
    ASSERT_TRUE(waitReady(watcher));

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

    std::atomic<bool> death_received(false);
    uint64_t watch_id = 0;
    std::vector<ServiceWatchRequest> filters;
    filters.push_back(ServiceWatchRequest("EphemeralService2", "", ""));
    int ret = watcher.watchServices(filters,
        [&death_received](uint64_t, const ServiceIdentity&, bool online) {
            if (!online) death_received = true;
        }, watch_id);
    ASSERT_EQ(ret, 0) << "watchServices failed";

    ret = watcher.unwatchServices(watch_id);
    ASSERT_EQ(ret, 0) << "unwatchServices failed";

    stopProcess(child);

    for (int i = 0; i < 80 && !death_received.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_FALSE(death_received.load()) << "death callback should not be delivered after unsubscribe";
    owner.stop();
}

TEST_F(FullIntegrationTest, WatchInitialOfflineForMissingService) {
    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(watcher);
    ASSERT_TRUE(waitReady(watcher));

    std::atomic<int> offline(0);
    std::string seen_name;
    std::string seen_host;
    std::string seen_instance;
    uint64_t watch_id = 0;
    std::vector<ServiceWatchRequest> filters;
    filters.push_back(ServiceWatchRequest("NeverRegisteredService", "host-A", "id-1"));
    ASSERT_EQ(watcher.watchServices(filters,
        [&](uint64_t, const ServiceIdentity& identity, bool online) {
            if (!online) {
                seen_name = identity.name;
                seen_host = identity.host_id;
                seen_instance = identity.instance_id;
                offline.fetch_add(1);
            }
        }, watch_id), 0);
    for (int i = 0; i < 50 && offline.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(offline.load(), 1);
    EXPECT_EQ(seen_name, "NeverRegisteredService");
    EXPECT_EQ(seen_host, "host-A");
    EXPECT_EQ(seen_instance, "id-1");
    watcher.unwatchServices(watch_id);
    owner.stop();
}

TEST_F(FullIntegrationTest, WatchTakeoverEmitsOfflineThenOnline) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", SM_PORT);
    TestPid first = startProcess(g_program_path, "--child-death", port_str, "TakeoverService");
    ASSERT_GT(first, 0);

    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(watcher);
    ASSERT_TRUE(waitReady(watcher));
    for (int i = 0; i < 50; ++i) {
        ServiceInfo info;
        if (watcher.lookupService("TakeoverService", info) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::atomic<int> online(0);
    std::atomic<int> offline(0);
    uint64_t watch_id = 0;
    std::vector<ServiceWatchRequest> filters;
    filters.push_back(ServiceWatchRequest("TakeoverService", "", ""));
    ASSERT_EQ(watcher.watchServices(filters,
        [&](uint64_t, const ServiceIdentity&, bool is_online) {
            if (is_online) online.fetch_add(1);
            else offline.fetch_add(1);
        }, watch_id), 0);
    for (int i = 0; i < 50 && online.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_GE(online.load(), 1);

    TestPid second = startProcess(g_program_path, "--child-death", port_str, "TakeoverService");
    ASSERT_GT(second, 0);
    for (int i = 0; i < 100; ++i) {
        if (offline.load() >= 1 && online.load() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_GE(offline.load(), 1);
    EXPECT_GE(online.load(), 2) << "takeover must emit offline then online";

    stopProcess(second);
    stopProcess(first);
    watcher.unwatchServices(watch_id);
    owner.stop();
}

TEST_F(FullIntegrationTest, WatchRejectsOverlappingFilters) {
    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(watcher);
    ASSERT_TRUE(waitReady(watcher));

    std::vector<ServiceWatchRequest> filters;
    filters.push_back(ServiceWatchRequest("AnyService", "", ""));
    filters.push_back(ServiceWatchRequest("AnyService", "host-A", ""));
    uint64_t watch_id = 0;
    EXPECT_EQ(watcher.watchServices(filters,
        [](uint64_t, const ServiceIdentity&, bool) {}, watch_id),
        static_cast<int>(ErrorCode::ERR_INVALID_PARAM));
    EXPECT_EQ(watch_id, 0u);
    owner.stop();
}

// ============================================================
// 测试组 6：生命周期
// ============================================================

TEST_F(FullIntegrationTest, InvokeCountAccumulated) {
    OmniRuntime c;
    ASSERT_EQ(c.init("127.0.0.1", SM_PORT), 0);
    test::RuntimeThread owner(c);
    ASSERT_TRUE(waitReady(c));
    const int before = srv_.service.invokeCount();
    Buffer req; req.writeInt32(3); req.writeInt32(4);
    Buffer resp;
    ASSERT_EQ(c.connectService("CalcService"), 0);
    ASSERT_EQ(c.invoke("CalcService", IFACE_ID, METHOD_ADD, 0, req, resp, 5000), 0);
    EXPECT_GT(srv_.service.invokeCount(), before);
    owner.stop();
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
        if (c.registerService(&svc) != 0) return 1;
        c.start();  // 阻塞驱动 owner event-loop，由父进程终止
        return 0;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
