#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <omnibinder/log.h>
#include <pthread.h>

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
    volatile bool registered;
    volatile bool should_stop;
    ErrorLogServerCtx() : registered(false), should_stop(false) {}
};

static void* errorLogServerThread(void* arg) {
    ErrorLogServerCtx* ctx = static_cast<ErrorLogServerCtx*>(arg);
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
    static pid_t sm_pid_;
    static ErrorLogServerCtx* server_ctx_;
    static pthread_t server_tid_;
    static const char* log_path_;

    static void SetUpTestSuite() {
        log_path_ = "/tmp/omnibinder_error_logging_test.log";
        unlink(log_path_);
        FILE* log_fp = freopen(log_path_, "w", stderr);
        ASSERT_NE(log_fp, nullptr);
        setLogLevel(omnibinder::LOG_DEBUG);

        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19933", "--log-level", "0");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new ErrorLogServerCtx();
        ASSERT_EQ(pthread_create(&server_tid_, NULL, errorLogServerThread, server_ctx_), 0);
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

pid_t ErrorLoggingTest::sm_pid_ = 0;
ErrorLogServerCtx* ErrorLoggingTest::server_ctx_ = nullptr;
pthread_t ErrorLoggingTest::server_tid_ = 0;
const char* ErrorLoggingTest::log_path_ = nullptr;

TEST_F(ErrorLoggingTest, SmConnectFailureLog) {
    OmniRuntime bad_runtime;
    int ret = bad_runtime.init("127.0.0.1", 19999);
    EXPECT_NE(ret, 0);
}

TEST_F(ErrorLoggingTest, RpcTimeoutAndReconnectLogs) {
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SM_PORT), 0);

    Buffer req, resp;
    req.writeRaw("bad", 3);
    int ret = runtime.invoke("MissingService", IFACE_ID, METHOD_ECHO, req, resp, 1000);
    EXPECT_NE(ret, 0);

    kill(sm_pid_, SIGKILL);
    int wstatus = 0;
    waitpid(sm_pid_, &wstatus, 0);
    usleep(100000);
    for (int i = 0; i < 30; ++i) {
        runtime.pollOnce(50);
    }
    sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19933", "--log-level", "0");
    ASSERT_GT(sm_pid_, 0);
    ASSERT_TRUE(waitPortReady(SM_PORT, 30));

    bool recovered = false;
    for (int i = 0; i < 80 && !recovered; ++i) {
        if (runtime.connectService("LogService") != 0) {
            usleep(100000);
            continue;
        }
        Buffer ok_req, ok_resp;
        const char* payload = "reconnect-ok";
        ok_req.writeRaw(payload, strlen(payload));
        ret = runtime.invoke("LogService", IFACE_ID, METHOD_ECHO, ok_req, ok_resp, 3000);
        if (ret == 0) {
            std::string result(reinterpret_cast<const char*>(ok_resp.data()), ok_resp.size());
            if (result == payload) recovered = true;
        }
        usleep(100000);
    }
    EXPECT_TRUE(recovered);
    runtime.stop();
}

TEST_F(ErrorLoggingTest, LogKeywordsPresent) {
    fflush(stderr);
    std::string logs = readFile(log_path_);
    EXPECT_TRUE(logs.find("sm_connect_failed") != std::string::npos ||
                logs.find("sm_connect_timeout") != std::string::npos);
    EXPECT_TRUE(logs.find("sm_connection_lost") != std::string::npos);
    EXPECT_TRUE(logs.find("sm_reconnect_begin") != std::string::npos);
    EXPECT_TRUE(logs.find("sm_reconnect_success") != std::string::npos);
}
