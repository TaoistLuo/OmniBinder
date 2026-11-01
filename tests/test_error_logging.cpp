#include <gtest/gtest.h>
#include "test_common.h"
#include "runtime_thread.h"
#include "platform/platform_test.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <omnibinder/log.h>
#include <thread>
#include <chrono>
#include <atomic>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19933;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("LogService");

class LogService : public Service {
public:
    LogService() : Service("LogService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "LogService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "LogService"; }
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

struct ErrorLogServerCtx {
    OmniRuntime runtime;
    LogService service;
    std::atomic<bool> started;
    ErrorLogServerCtx() : started(false) {}
};

static void errorLogServerThread(void* arg) {
    ErrorLogServerCtx* ctx = static_cast<ErrorLogServerCtx*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) return;
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) return;
    ctx->started.store(true);
    // 新生命周期：业务线程调用 start() 阻塞驱动 event-loop，stop() 后返回。
    ctx->runtime.start();
}

static std::string readFile(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return std::string();
    std::string out;
    char buf[1024];
    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break;
        out.append(buf, n);
    }
    fclose(fp);
    return out;
}

class ErrorLoggingTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static ErrorLogServerCtx* server_ctx_;
    static std::thread server_tid_;
    static const char* log_path_;

    static void SetUpTestSuite() {
        // 相对路径：ctest WORKING_DIRECTORY 为构建根目录，避免硬编码 /tmp
        log_path_ = "omnibinder_error_logging_test.log";
        platform::removeFile(log_path_);
        FILE* log_fp = freopen(log_path_, "w", stderr);
        ASSERT_NE(log_fp, nullptr);
        setLogLevel(omnibinder::LOG_DEBUG);

        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19933", "--log-level", "0");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new ErrorLogServerCtx();
        server_tid_ = std::thread(errorLogServerThread, server_ctx_);
        // start() 阻塞在业务线程；isReady() 表示 SM 已连接且服务已激活。
        for (int i = 0; i < 200 && !server_ctx_->runtime.isReady(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(server_ctx_->runtime.isReady());
    }

    static void TearDownTestSuite() {
        if (server_ctx_) {
            server_ctx_->runtime.stop();
        }
        if (server_tid_.joinable()) {
            server_tid_.join();
        }
        delete server_ctx_;
        server_ctx_ = nullptr;
        stopProcess(sm_pid_);
        sm_pid_ = 0;
        fflush(stderr);
        platform::removeFile(log_path_);
    }
};

TestPid ErrorLoggingTest::sm_pid_ = 0;
ErrorLogServerCtx* ErrorLoggingTest::server_ctx_ = nullptr;
std::thread ErrorLoggingTest::server_tid_;
const char* ErrorLoggingTest::log_path_ = nullptr;

TEST_F(ErrorLoggingTest, SmConnectFailureLog) {
    OmniRuntime bad_runtime;
    // 新生命周期：init 只创建本地资源，SM 不可达在首次访问/start 时才暴露。
    ASSERT_EQ(bad_runtime.init("127.0.0.1", 19999), 0);
    ServiceInfo info;
    EXPECT_NE(bad_runtime.lookupService("NoService", info), 0);
    bad_runtime.stop();
}

TEST_F(ErrorLoggingTest, RpcTimeoutAndReconnectLogs) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    RuntimeThread owner(runtime);

    Buffer req, resp;
    req.writeRaw("bad", 3);
    int ret = runtime.invoke("MissingService", IFACE_ID, METHOD_ECHO, 0, req, resp, 1000);
    EXPECT_NE(ret, 0);

    stopProcess(sm_pid_);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19933", "--log-level", "0");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    bool recovered = false;
    for (int i = 0; i < 100 && !recovered; ++i) {
        if (runtime.connectService("LogService") == 0) {
            Buffer ok_req, ok_resp;
            const char* payload = "reconnect-ok";
            ok_req.writeRaw(payload, strlen(payload));
            ret = runtime.invoke("LogService", IFACE_ID, METHOD_ECHO, 0, ok_req, ok_resp, 3000);
            if (ret == 0) {
                std::string result(reinterpret_cast<const char*>(ok_resp.data()), ok_resp.size());
                if (result == payload) recovered = true;
            }
        }
        if (!recovered) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    EXPECT_TRUE(recovered);
    owner.stop();
}

TEST_F(ErrorLoggingTest, LogKeywordsPresent) {
    // 本用例自证日志：自行制造连接失败与 SM 掉线/重连，不依赖其它用例的产出
    {
        OmniRuntime bad_runtime;
        ASSERT_EQ(bad_runtime.init("127.0.0.1", 19999), 0);
        // 触发一次 SM 访问以产生 sm_connect_failed/timeout 日志。
        ServiceInfo info;
        bad_runtime.lookupService("NoService", info);
        bad_runtime.stop();
    }

    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);
    RuntimeThread owner(runtime);

    stopProcess(sm_pid_);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19933", "--log-level", "0");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    // 等待 runtime 重连并恢复服务发现，确保 reconnect_success 已落日志。
    bool reconnected = false;
    for (int i = 0; i < 100 && !reconnected; ++i) {
        ServiceInfo info;
        if (runtime.lookupService("LogService", info) == 0) {
            reconnected = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    EXPECT_TRUE(reconnected);
    owner.stop();

    fflush(stderr);
    std::string logs = readFile(log_path_);
    EXPECT_TRUE(logs.find("sm_connect_failed") != std::string::npos ||
                logs.find("sm_connect_timeout") != std::string::npos);
    EXPECT_TRUE(logs.find("sm_connection_lost") != std::string::npos);
    EXPECT_TRUE(logs.find("sm_reconnect_begin") != std::string::npos);
    EXPECT_TRUE(logs.find("sm_reconnect_success") != std::string::npos);
}
