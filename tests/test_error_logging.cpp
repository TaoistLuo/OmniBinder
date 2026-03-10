#include <omnibinder/omnibinder.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace omnibinder;

#define TEST(name) printf("  TEST %-34s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)

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
    void onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ECHO && request.size() > 0) {
            response.writeRaw(request.data(), request.size());
        }
    }

private:
    InterfaceInfo iface_;
};

struct ServerContext {
    OmniRuntime runtime;
    LogService service;
    volatile bool registered;
    volatile bool should_stop;
    ServerContext() : registered(false), should_stop(false) {}
};

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) {
        return NULL;
    }
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) {
        return NULL;
    }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(50);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

static pid_t startSM(uint16_t port) {
    char kill_cmd[128];
    snprintf(kill_cmd, sizeof(kill_cmd), "pkill -f 'service_manager --port %u' >/dev/null 2>&1 || true", port);
    int kill_rc = system(kill_cmd);
    (void)kill_rc;
    usleep(100000);

    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        execl("./target/bin/service_manager", "service_manager", "--port", port_str, "--log-level", "0", (char*)NULL);
        _exit(1);
    }
    return pid;
}

static void stopSM(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid) {
                return;
            }
            usleep(100000);
        }
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
}

static bool waitSMReady(uint16_t port, int retries) {
    for (int i = 0; i < retries; ++i) {
        OmniRuntime probe;
        if (probe.init("127.0.0.1", port) == 0) {
            probe.stop();
            return true;
        }
        usleep(100000);
    }
    return false;
}

static std::string readFile(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return std::string();
    }
    std::string out;
    char buf[1024];
    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    fclose(fp);
    return out;
}

int main() {
    printf("=== Error Logging Tests ===\n\n");

    const char* log_path = "/tmp/omnibinder_error_logging_test.log";
    unlink(log_path);
    FILE* log_fp = freopen(log_path, "w", stderr);
    assert(log_fp != NULL);
    setLogLevel(LogLevel::LOG_DEBUG);

    TEST(sm_connect_failure_log);
    {
        OmniRuntime bad_runtime;
        int ret = bad_runtime.init("127.0.0.1", 19999);
        assert(ret != 0);
        PASS();
    }

    pid_t sm_pid = startSM(SM_PORT);
    assert(sm_pid > 0);
    bool sm_ready = waitSMReady(SM_PORT, 30);
    assert(sm_ready);

    ServerContext server_ctx;
    pthread_t server_tid = 0;
    int create_ret = pthread_create(&server_tid, NULL, serverThread, &server_ctx);
    assert(create_ret == 0);
    for (int i = 0; i < 50 && !server_ctx.registered; ++i) {
        usleep(100000);
    }
    assert(server_ctx.registered);

    TEST(rpc_timeout_and_reconnect_logs);
    {
        OmniRuntime runtime;
        assert(runtime.init("127.0.0.1", SM_PORT) == 0);

        Buffer req;
        Buffer resp;
        req.writeRaw("bad", 3);
        int ret = runtime.invoke("MissingService", IFACE_ID, METHOD_ECHO, req, resp, 1000);
        assert(ret != 0);

        stopSM(sm_pid);
        usleep(300000);
        sm_pid = startSM(SM_PORT);
        assert(sm_pid > 0);
        sm_ready = waitSMReady(SM_PORT, 30);
        assert(sm_ready);

        bool recovered = false;
        for (int i = 0; i < 80 && !recovered; ++i) {
            Buffer ok_req;
            Buffer ok_resp;
            const char* payload = "reconnect-ok";
            ok_req.writeRaw(payload, strlen(payload));
            ret = runtime.invoke("LogService", IFACE_ID, METHOD_ECHO, ok_req, ok_resp, 3000);
            if (ret == 0) {
                std::string result(reinterpret_cast<const char*>(ok_resp.data()), ok_resp.size());
                if (result == payload) {
                    recovered = true;
                }
            }
            usleep(100000);
        }
        assert(recovered);
        runtime.stop();
        PASS();
    }

    fflush(stderr);

    server_ctx.should_stop = true;
    pthread_join(server_tid, NULL);
    stopSM(sm_pid);

    std::string logs = readFile(log_path);
    TEST(log_keywords_present);
    {
        assert(logs.find("sm_connect_failed") != std::string::npos ||
               logs.find("sm_connect_timeout") != std::string::npos);
        assert(logs.find("sm_connection_lost") != std::string::npos);
        assert(logs.find("sm_reconnect_begin") != std::string::npos);
        assert(logs.find("sm_reconnect_success") != std::string::npos);
        PASS();
    }

    printf("\nAll error logging tests passed.\n");
    return 0;
}
