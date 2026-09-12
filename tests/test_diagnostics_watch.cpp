#include <gtest/gtest.h>

#include "test_common.h"
#include <omnibinder/omnibinder.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace omnibinder;
using namespace omnibinder::test;

namespace {

const uint16_t SM_PORT = 19971;
const char* SERVICE_NAME = "DiagLazyService";
const uint32_t IFACE_ID = fnv1a_32("DiagLazyService");
const uint32_t METHOD_PING = fnv1a_32("Ping");

class DiagLazyService : public Service {
public:
    DiagLazyService() : Service(SERVICE_NAME) {
        setShmConfig(ShmConfig(4 * 1024, 4 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = SERVICE_NAME;
        iface_.methods.push_back(MethodInfo(METHOD_PING, "Ping"));
    }

    const char* serviceName() const override { return SERVICE_NAME; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }

protected:
    int onInvoke(uint32_t method_id, const Buffer&, Buffer& response) override {
        if (method_id != METHOD_PING) {
            return static_cast<int>(ErrorCode::ERR_METHOD_NOT_FOUND);
        }
        return response.writeString("pong") ? 0 : static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

private:
    InterfaceInfo iface_;
};

std::string diagName(pid_t pid) {
    char buf[64];
    snprintf(buf, sizeof(buf), "__diag_pid_%ld", static_cast<long>(pid));
    return std::string(buf);
}

bool listHasService(OmniRuntime& runtime, const std::string& name) {
    runtime.clearServiceCache();
    ServiceInfo info;
    return runtime.lookupService(name, info) == 0;
}

bool listHasPublishedTopic(OmniRuntime& runtime, const std::string& service_name,
                           const std::string& topic) {
    std::vector<std::string> topics;
    if (runtime.queryPublishedTopics(service_name, topics) != 0) {
        return false;
    }
    return std::find(topics.begin(), topics.end(), topic) != topics.end();
}

bool waitForService(OmniRuntime& runtime, const std::string& name, bool expected) {
    for (int i = 0; i < 50; ++i) {
        if (listHasService(runtime, name) == expected) {
            return true;
        }
        runtime.pollOnce(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

uint32_t findPidForService(OmniRuntime& runtime, const std::string& service_name) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        std::vector<RuntimeInfo> runtimes;
        if (runtime.listRuntimes(runtimes) == 0) {
            for (size_t i = 0; i < runtimes.size(); ++i) {
                for (size_t j = 0; j < runtimes[i].services.size(); ++j) {
                    if (runtimes[i].services[j] == service_name) {
                        return runtimes[i].pid;
                    }
                }
            }
        }
        runtime.pollOnce(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

bool waitForRuntimePid(OmniRuntime& runtime, uint32_t pid) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        std::vector<RuntimeInfo> runtimes;
        if (runtime.listRuntimes(runtimes) == 0) {
            for (size_t i = 0; i < runtimes.size(); ++i) {
                if (runtimes[i].pid == pid) {
                    return true;
                }
            }
        }
        runtime.pollOnce(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void runServiceChild() {
    OmniRuntime runtime;
    if (runtime.init("127.0.0.1", SM_PORT) != 0) {
        _exit(2);
    }
    DiagLazyService service;
    if (runtime.registerService(&service) != 0) {
        _exit(3);
    }
    while (true) {
        runtime.pollOnce(50);
    }
}

void runIdleClientChild() {
    OmniRuntime runtime;
    if (runtime.init("127.0.0.1", SM_PORT) != 0) {
        _exit(2);
    }
    while (true) {
        runtime.pollOnce(50);
    }
}

pid_t forkChild(void (*entry)()) {
    pid_t pid = fork();
    if (pid == 0) {
        entry();
        _exit(0);
    }
    return pid;
}

void stopChild(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
}

// ASSERT_* 提前 return 时也必须回收 forked child，避免孤儿进程污染后续用例
struct ChildGuard {
    pid_t pid;
    explicit ChildGuard(pid_t p) : pid(p) {}
    ~ChildGuard() { stopChild(pid); }
};

class DiagnosticsWatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19971", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));
    }

    void TearDown() override {
        stopProcess(sm_pid_);
    }

    TestPid sm_pid_ = 0;
};

} // namespace

TEST_F(DiagnosticsWatchTest, ServicePidWatchDoesNotRegisterHiddenService) {
    pid_t service_pid = forkChild(runServiceChild);
    ASSERT_GT(service_pid, 0);

    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0);
    ASSERT_TRUE(waitForService(watcher, SERVICE_NAME, true));

    const std::string hidden = diagName(service_pid);
    ASSERT_TRUE(waitForService(watcher, hidden, false));

    uint32_t watched_pid = findPidForService(watcher, SERVICE_NAME);
    ASSERT_EQ(watched_pid, static_cast<uint32_t>(service_pid));

    int events = 0;
    ASSERT_EQ(watcher.watchPid(watched_pid, [&events](const Buffer& data) {
        if (data.size() >= DIAG_EVENT_WIRE_HEADER_SIZE
            && data.data()[0] == DIAG_EVENT_REQUEST) {
            ++events;
        }
    }), 0);

    EXPECT_TRUE(waitForService(watcher, hidden, false));

    ASSERT_EQ(watcher.connectService(SERVICE_NAME), 0);
    Buffer request;
    Buffer response;
    ASSERT_EQ(watcher.invoke(SERVICE_NAME, IFACE_ID, METHOD_PING, 0, request, response, 5000), 0);

    for (int i = 0; i < 50 && events == 0; ++i) {
        watcher.pollOnce(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GT(events, 0);

    EXPECT_EQ(watcher.unwatchPid(watched_pid), 0);
    EXPECT_TRUE(waitForService(watcher, hidden, false));
    watcher.stop();
    stopChild(service_pid);
}

/* @brief SM 重连后 watcher 必须重放 WATCH_START
 * @details 否则 SM 侧 watcher 关系丢失，unwatch 无法停止目标诊断数据发布，监控静默失效。 */
TEST_F(DiagnosticsWatchTest, WatchIsReplayedAfterSmRestart) {
    pid_t service_pid = forkChild(runServiceChild);
    ASSERT_GT(service_pid, 0);
    ChildGuard child_guard(service_pid);

    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0);
    ASSERT_TRUE(waitForService(watcher, SERVICE_NAME, true));

    const std::string diag_topic = diagName(service_pid);
    uint32_t watched_pid = findPidForService(watcher, SERVICE_NAME);
    ASSERT_EQ(watched_pid, static_cast<uint32_t>(service_pid));

    int events = 0;
    ASSERT_EQ(watcher.watchPid(watched_pid, [&events](const Buffer& data) {
        if (data.size() >= DIAG_EVENT_WIRE_HEADER_SIZE
            && data.data()[0] == DIAG_EVENT_REQUEST) {
            ++events;
        }
    }), 0);

    // 等待目标处理 WATCH_START 并发布 diag topic，避免 Ping 早于 watch 生效
    bool topic_ready = false;
    for (int i = 0; i < 50 && !topic_ready; ++i) {
        watcher.pollOnce(20);
        topic_ready = listHasPublishedTopic(watcher, SERVICE_NAME, diag_topic);
    }
    ASSERT_TRUE(topic_ready);

    ASSERT_EQ(watcher.connectService(SERVICE_NAME), 0);
    for (int attempt = 0; attempt < 20 && events == 0; ++attempt) {
        Buffer req, resp;
        ASSERT_EQ(watcher.invoke(SERVICE_NAME, IFACE_ID, METHOD_PING, 0, req, resp, 5000), 0);
        for (int i = 0; i < 20 && events == 0; ++i) {
            watcher.pollOnce(20);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ASSERT_GT(events, 0);

    stopProcess(sm_pid_);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19971", "--log-level", "3");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    // 等待 watcher 与目标 runtime 各自重连（心跳周期 3s）并重建诊断链路
    const int baseline = events;
    bool topic_back = false;
    for (int i = 0; i < 200 && !topic_back; ++i) {
        watcher.pollOnce(50);
        topic_back = listHasPublishedTopic(watcher, SERVICE_NAME, diag_topic);
    }
    ASSERT_TRUE(topic_back);

    bool resumed = false;
    for (int attempt = 0; attempt < 60 && !resumed; ++attempt) {
        if (watcher.connectService(SERVICE_NAME) == 0) {
            Buffer req, resp;
            if (watcher.invoke(SERVICE_NAME, IFACE_ID, METHOD_PING, 0, req, resp, 3000) == 0) {
                for (int i = 0; i < 20 && events == baseline; ++i) {
                    watcher.pollOnce(20);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (events > baseline) resumed = true;
            }
        }
        if (!resumed) {
            watcher.pollOnce(50);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    EXPECT_TRUE(resumed);

    // 至少经过一个心跳周期，确保失败的 WATCH_START 重放已兜底重试
    for (int i = 0; i < 70; ++i) {
        watcher.pollOnce(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 重放成功的可观测证据：SM 侧 watcher 关系存在，WATCH_STOP 才能到达目标并
    // 使其停止发布 diag topic
    EXPECT_EQ(watcher.unwatchPid(watched_pid), 0);
    bool topic_gone = false;
    for (int i = 0; i < 100 && !topic_gone; ++i) {
        watcher.pollOnce(20);
        topic_gone = !listHasPublishedTopic(watcher, SERVICE_NAME, diag_topic);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(topic_gone);

    watcher.stop();
}

TEST_F(DiagnosticsWatchTest, ClientPidWatchRegistersAndCleansHiddenService) {
    pid_t client_pid = forkChild(runIdleClientChild);
    ASSERT_GT(client_pid, 0);

    OmniRuntime watcher;
    ASSERT_EQ(watcher.init("127.0.0.1", SM_PORT), 0);

    const std::string hidden = diagName(client_pid);
    ASSERT_TRUE(waitForRuntimePid(watcher, static_cast<uint32_t>(client_pid)));
    ASSERT_FALSE(listHasService(watcher, hidden));

    ASSERT_EQ(watcher.watchPid(static_cast<uint32_t>(client_pid), [](const Buffer&) {}), 0);
    EXPECT_TRUE(waitForService(watcher, hidden, true));

    EXPECT_EQ(watcher.unwatchPid(static_cast<uint32_t>(client_pid)), 0);
    EXPECT_TRUE(waitForService(watcher, hidden, false));

    watcher.stop();
    stopChild(client_pid);
}
