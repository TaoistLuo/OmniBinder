// test_heartbeat_reconnect.cpp - 心跳、自动重连与 OnServiceDied 集成测试
//
// 测试内容：
// 1. HeartbeatTimeoutDetected     — proxy 侧心跳快速检测服务死亡
// 2. AutoReconnectAfterDeath       — 自动重连在服务重启后恢复连接
// 3. OnServiceDiedCallbackFires    — SM 死亡通知触发 proxy 回调
// 4. DirectDisconnectTriggersReconnect — 显式 disconnect/connect 循环在心跳下正常工作
// 5. DisconnectCleansUpState       — disconnect() 完成清理，后续 connect() 安全
// 6. DisconnectCancelsPendingReconnect — 显式 disconnect 取消已排队的重连定时器
// 7. TimeoutDisconnectTriggersReregistration — 阻塞的 runtime 在 SM 关闭超时控制连接后重新注册
//    （不会永久漂移）
// 8. StopHeartbeatKeepsSmLiveness — stopHeartbeat 只影响数据面检测，不会造成 SM 超时 → 重连循环
// 9. PureClientConnectionNotClosedByPeerTimeout — 从未注册过任何服务的连接不会被心跳超时规则关闭

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
// 测试服务 — 简单 Echo 回显
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
// 程序路径（从 argv[0] 保存，供 startProcess 使用）
// ============================================================
static const char* g_program_path = nullptr;

// ============================================================
// 等待辅助函数 — 基于 pollOnce，不用 sleep()
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
// 测试 fixture
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
// Heartbeat 生命周期 fixture — 专用短心跳超时 SM 实例，
// 使超时→断开→重注册循环可在数秒内跑完，而非生产默认的 10s x 3。
// ============================================================
static const uint16_t HB_LIFECYCLE_SM_PORT = 19966;

class HeartbeatLifecycleTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    TestPid child_pid_;

    void SetUp() override {
        child_pid_ = 0;
    }

    void TearDown() override {
        if (child_pid_ > 0) {
            stopProcess(child_pid_);
            child_pid_ = 0;
        }
    }

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager",
                                "--port", "19966",
                                "--heartbeat-timeout", "500");
        ASSERT_GT(sm_pid_, 0) << "Failed to start lifecycle ServiceManager";
        ASSERT_TRUE(waitPortReady(HB_LIFECYCLE_SM_PORT, 30))
            << "Timed out waiting for ServiceManager port " << HB_LIFECYCLE_SM_PORT;
    }

    static void TearDownTestSuite() {
        if (sm_pid_ > 0) {
            stopProcess(sm_pid_);
            sm_pid_ = 0;
        }
    }
};

TestPid HeartbeatLifecycleTest::sm_pid_ = 0;

/* @brief 查询 SM 注册状态（绕过 runtime 的 lookupService 缓存）
 * @param[in] rt OmniRuntime 实例
 * @param[in] name 服务名称
 * @return 1=在册，0=不在册，-1=查询失败
 */
static int smServiceState(OmniRuntime& rt, const std::string& name) {
    std::vector<ServiceInfo> services;
    if (rt.listServices(services) != 0) {
        return -1;
    }
    for (size_t i = 0; i < services.size(); ++i) {
        if (services[i].name == name) {
            return 1;
        }
    }
    return 0;
}

// 手工控制面客户端：可在同一条 TCP 连接上注册多个服务并单独控制心跳，
// 用于验证"仍有存活服务时不得关闭连接"的规则。
class RawControlClient {
public:
    RawControlClient() : fd_(platform::INVALID_SOCKET_FD), seq_(1) {}

    ~RawControlClient() {
        close();
    }

    bool connectTo(const std::string& host, uint16_t port) {
        fd_ = platform::createTcpSocket();
        if (fd_ == platform::INVALID_SOCKET_FD) {
            return false;
        }
        platform::setNonBlocking(fd_);
        int ret = platform::connectSocket(fd_, host, port);
        if (ret == 1) {
            return platform::waitSocketWritable(fd_, 5000);
        }
        return ret == 0;
    }

    bool sendRegister(const std::string& name) {
        Message msg(MessageType::MSG_REGISTER, seq_++);
        ServiceInfo info;
        info.name = name;
        info.host = "127.0.0.1";
        info.port = 1;
        serializeServiceInfo(info, msg.payload);
        return send(msg);
    }

    bool sendHeartbeat(const std::string& name) {
        Message msg(MessageType::MSG_HEARTBEAT, 0);
        msg.payload.writeString(name);
        return send(msg);
    }

    void drain() {
        uint8_t tmp[256];
        while (platform::socketRecv(fd_, tmp, sizeof(tmp)) > 0) {}
    }

    /* @brief 探测连接接收状态
     * @return >0 有数据；-1 would-block（连接仍存活）；0 EOF（对端关闭）
     */
    int recvProbe() {
        uint8_t tmp[1];
        return platform::socketRecv(fd_, tmp, sizeof(tmp));
    }

private:
    bool send(Message& msg) {
        Buffer buf;
        if (!msg.serialize(buf)) {
            return false;
        }
        return platform::socketSendAll(fd_, buf.data(), buf.size(), 2000, NULL);
    }

    void close() {
        if (fd_ != platform::INVALID_SOCKET_FD) {
            platform::closeSocket(fd_);
            fd_ = platform::INVALID_SOCKET_FD;
        }
    }

    platform::SocketFd fd_;
    uint32_t seq_;
};

// ============================================================
// Test 7 — TimeoutDisconnectTriggersReregistration
//
// @brief   超时断开后触发重新注册
// @details 子进程注册后阻塞 owner-loop，超过 SM 超时窗口。SM 必须丢弃过期注册并关闭
//          控制连接（C2），恢复后的 runtime 重连并通过 restoreControlPlaneState 重新注册。
//          父 runtime 是纯客户端，必须保持自己的连接。
// ============================================================
TEST_F(HeartbeatLifecycleTest, TimeoutDisconnectTriggersReregistration) {
    child_pid_ = startProcess(g_program_path, "--child-blocked-service", "19966", "3500");
    ASSERT_GT(child_pid_, 0) << "Failed to start blocked service child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", HB_LIFECYCLE_SM_PORT), 0);
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME))
        << "Service should register before the owner loop blocks";

    bool observed_timeout = false;
    for (int i = 0; i < 60 && !observed_timeout; ++i) {
        if (smServiceState(rt, SERVICE_NAME) == 0) {
            observed_timeout = true;
            break;
        }
        rt.pollOnce(100);
    }
    ASSERT_TRUE(observed_timeout)
        << "SM should drop the service once heartbeats stop";

    RuntimeStats stats;
    ASSERT_EQ(rt.getStats(stats), 0);
    EXPECT_EQ(stats.sm_reconnect_attempts, 0u)
        << "Pure-client connection must not be closed by the timeout rule";

    bool recovered = false;
    for (int i = 0; i < 120 && !recovered; ++i) {
        recovered = (smServiceState(rt, SERVICE_NAME) == 1);
        if (!recovered) {
            rt.pollOnce(50);
        }
    }
    ASSERT_TRUE(recovered)
        << "Runtime must re-register after the timeout-induced disconnect";

    rt.stop();
}

// ============================================================
// Test 8 — StopHeartbeatKeepsSmLiveness
//
// @brief   stopHeartbeat 后保持 SM 存活
// @details 子进程停掉自己注册服务的心跳监控。由于 stopHeartbeat 只取消数据面检测，
//          SM 控制面心跳继续发送，服务跨多个超时窗口保持注册（无重连循环）。
// ============================================================
TEST_F(HeartbeatLifecycleTest, StopHeartbeatKeepsSmLiveness) {
    child_pid_ = startProcess(g_program_path, "--child-owns-heartbeat-stop", "19966");
    ASSERT_GT(child_pid_, 0) << "Failed to start heartbeat-stop child";

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", HB_LIFECYCLE_SM_PORT), 0);
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME));

    int missing = 0;
    for (int i = 0; i < 60; ++i) {
        if (smServiceState(rt, SERVICE_NAME) == 0) {
            ++missing;
        }
        rt.pollOnce(100);
    }
    EXPECT_EQ(missing, 0)
        << "stopHeartbeat must not stop SM control-plane liveness "
        << "(service disappearing implies a timeout/reconnect loop)";

    RuntimeStats stats;
    ASSERT_EQ(rt.getStats(stats), 0);
    EXPECT_EQ(stats.sm_reconnect_attempts, 0u);

    rt.stop();
}

// ============================================================
// Test 9 — PureClientConnectionNotClosedByPeerTimeout
//
// @brief   纯客户端连接不被对端超时关闭
// @details 从未注册任何服务的 runtime 与一个会超时的对端共享 SM。只允许关闭对端连接；
//          纯客户端必须保持可用，一次重连都不发生。
// ============================================================
TEST_F(HeartbeatLifecycleTest, PureClientConnectionNotClosedByPeerTimeout) {
    child_pid_ = startProcess(g_program_path, "--child-blocked-service", "19966", "3500");
    ASSERT_GT(child_pid_, 0) << "Failed to start blocked service child";

    OmniRuntime client;
    ASSERT_EQ(client.init("127.0.0.1", HB_LIFECYCLE_SM_PORT), 0);
    ASSERT_TRUE(waitServiceRegistered(client, SERVICE_NAME));

    bool observed_timeout = false;
    for (int i = 0; i < 60 && !observed_timeout; ++i) {
        if (smServiceState(client, SERVICE_NAME) == 0) {
            observed_timeout = true;
            break;
        }
        client.pollOnce(100);
    }
    ASSERT_TRUE(observed_timeout);

    RuntimeStats stats;
    ASSERT_EQ(client.getStats(stats), 0);
    EXPECT_EQ(stats.sm_reconnect_attempts, 0u)
        << "Never-registered connection must survive a peer's timeout close";

    EXPECT_NE(smServiceState(client, SERVICE_NAME), -1)
        << "Pure client should still get a normal SM reply after the peer close";
    ASSERT_EQ(client.getStats(stats), 0);
    EXPECT_EQ(stats.sm_reconnect_attempts, 0u);

    client.stop();
}

// ============================================================
// Test 1 — HeartbeatTimeoutDetected
//
// @brief   心跳超时被快速检测
// @details 启动服务 → 连接 proxy → 启动心跳（200 ms 间隔 / 500 ms 超时）
//          → 杀死服务 → 验证 OnServiceDied 回调在 2 s 内触发。
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
//
// @brief   服务死亡后自动重连
// @details 启动服务 → 连接 proxy（默认开启自动重连）+ 心跳 → 杀死服务 → 验证死亡
//          → 重启服务 → 验证重连且 RPC 可用。
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
//
// @brief   OnServiceDied 回调触发
// @details 连接 proxy（不显式启动心跳）→ 注册 OnServiceDied → 杀死服务
//          → SM 检测死亡并通知 → 验证回调触发。
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
//
// @brief   显式断开后触发重连
// @details 连接 proxy + 启动心跳 → 显式 disconnect() → 再次 connect()
//          → 重启心跳 → 验证连接重建且心跳仍能检测死亡。
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
//
// @brief   disconnect 清理状态
// @details 连接 proxy → disconnect() → 验证 isConnected() 为 false → 再次 connect()
//          → 验证无残留心跳定时器（不崩溃）、isConnected() 为 true。
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

TEST_F(HeartbeatReconnectTest, DisconnectCancelsPendingReconnect) {
    TestPid svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0);

    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", SM_PORT), 0);
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME));

    ServiceProxyBase proxy(rt, SERVICE_NAME);
    ASSERT_EQ(proxy.connect(), 0);
    proxy.setReconnectInterval(5000);

    std::atomic<bool> death_fired(false);
    proxy.OnServiceDied([&death_fired]() {
        death_fired.store(true, std::memory_order_release);
    });
    stopProcess(svc_pid);
    for (int i = 0; i < 150 && !death_fired.load(std::memory_order_acquire); ++i) {
        rt.pollOnce(100);
    }
    ASSERT_TRUE(death_fired.load(std::memory_order_acquire));

    // 现在有一个 5 秒后触发的重连排队。显式 disconnect 必须取消该定时器并移除重连/心跳配置。
    ASSERT_EQ(rt.disconnectService(SERVICE_NAME), 0);
    svc_pid = startProcess(g_program_path, "--child-service", "19950");
    ASSERT_GT(svc_pid, 0);
    ASSERT_TRUE(waitServiceRegistered(rt, SERVICE_NAME, 80));
    for (int i = 0; i < 60; ++i) rt.pollOnce(100);
    EXPECT_FALSE(rt.isServiceConnected(SERVICE_NAME));

    stopProcess(svc_pid);
    rt.stop();
}

// ============================================================
// Test 10 — ConnectionWithSiblingServiceSurvivesTimeout
//
// @brief   有兄弟服务存活的连接可跨过超时
// @details 一条控制连接注册 A 和 B，但只为 A 发心跳。B 超时时 SM 必须保留连接（A 仍存活）。
//          当 A 也停止心跳后，连接已无存活服务，必须关闭。
// ============================================================
TEST_F(HeartbeatLifecycleTest, ConnectionWithSiblingServiceSurvivesTimeout) {
    OmniRuntime rt;
    ASSERT_EQ(rt.init("127.0.0.1", HB_LIFECYCLE_SM_PORT), 0);

    RawControlClient raw;
    ASSERT_TRUE(raw.connectTo("127.0.0.1", HB_LIFECYCLE_SM_PORT));
    ASSERT_TRUE(raw.sendRegister("SiblingA"));
    ASSERT_TRUE(raw.sendRegister("SiblingB"));
    raw.drain();

    bool both_registered = false;
    for (int i = 0; i < 50 && !both_registered; ++i) {
        both_registered = (smServiceState(rt, "SiblingA") == 1)
                          && (smServiceState(rt, "SiblingB") == 1);
        if (!both_registered) {
            rt.pollOnce(50);
        }
    }
    ASSERT_TRUE(both_registered) << "Both sibling services should register";

    bool sibling_b_gone = false;
    for (int i = 0; i < 30 && !sibling_b_gone; ++i) {
        raw.sendHeartbeat("SiblingA");
        sibling_b_gone = (smServiceState(rt, "SiblingB") == 0)
                         && (smServiceState(rt, "SiblingA") == 1);
        if (!sibling_b_gone) {
            platform::sleepMs(200);
        }
    }
    ASSERT_TRUE(sibling_b_gone) << "B should time out while A keeps heartbeating";

    raw.drain();
    EXPECT_NE(raw.recvProbe(), 0)
        << "Connection with a live sibling service must not be closed";

    bool connection_closed = false;
    for (int i = 0; i < 30 && !connection_closed; ++i) {
        platform::sleepMs(200);
        connection_closed = (raw.recvProbe() == 0);
    }
    EXPECT_TRUE(connection_closed)
        << "Connection must close once no live service remains";

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

    if (argc >= 3 && strcmp(argv[1], "--child-owns-heartbeat-stop") == 0) {
        uint16_t port = (uint16_t)atoi(argv[2]);
        OmniRuntime rt;
        if (rt.init("127.0.0.1", port) != 0) return 1;
        rt.setHeartbeatInterval(200);
        HrtbtTestService svc(SERVICE_NAME);
        if (rt.registerService(&svc) != 0) { rt.stop(); return 1; }
        // C2b 回归：对本进程已注册服务调用 stopHeartbeat 只影响数据面检测，
        // 不得停掉向 SM 的控制面注册心跳（否则会超时→断开→重注册循环）
        rt.startHeartbeat(SERVICE_NAME, 100, 300);
        rt.stopHeartbeat(SERVICE_NAME);
        while (true) { rt.pollOnce(100); }
        return 0;
    }

    if (argc >= 4 && strcmp(argv[1], "--child-blocked-service") == 0) {
        uint16_t port = (uint16_t)atoi(argv[2]);
        int block_ms = atoi(argv[3]);
        OmniRuntime rt;
        if (rt.init("127.0.0.1", port) != 0) return 1;
        rt.setHeartbeatInterval(200);
        HrtbtTestService svc(SERVICE_NAME);
        if (rt.registerService(&svc) != 0) { rt.stop(); return 1; }
        // 阻塞 owner-loop 超过 SM 超时窗口，模拟卡死；恢复后由 C2 的连接断开
        // 驱动 reconnect + restoreControlPlaneState 重新注册
        if (block_ms > 0) platform::sleepMs(static_cast<uint32_t>(block_ms));
        while (true) { rt.pollOnce(100); }
        return 0;
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
