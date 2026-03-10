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

#define TEST(name) printf("  TEST %-36s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)

static const uint16_t SM_PORT = 19932;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("StatsService");

class StatsService : public Service {
public:
    StatsService() : Service("StatsService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "StatsService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }

    const char* serviceName() const override { return "StatsService"; }
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
    StatsService service;
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
        execl("./target/bin/service_manager", "service_manager", "--port", port_str, "--log-level", "3", (char*)NULL);
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

int main() {
    printf("=== Runtime Stats Tests ===\n\n");

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

    OmniRuntime runtime;
    assert(runtime.init("127.0.0.1", SM_PORT) == 0);

    TEST(initial_stats_zero);
    {
        RuntimeStats stats;
        assert(runtime.getStats(stats) == 0);
        assert(stats.total_rpc_calls == 0);
        assert(stats.total_rpc_success == 0);
        assert(stats.total_rpc_failures == 0);
        PASS();
    }

    TEST(successful_calls_update_stats);
    {
        for (int i = 0; i < 5; ++i) {
            Buffer req;
            Buffer resp;
            const char* payload = "stats-ok";
            req.writeRaw(payload, strlen(payload));
            assert(runtime.invoke("StatsService", IFACE_ID, METHOD_ECHO, req, resp, 3000) == 0);
        }

        RuntimeStats stats;
        assert(runtime.getStats(stats) == 0);
        assert(stats.total_rpc_calls == 5);
        assert(stats.total_rpc_success == 5);
        assert(stats.total_rpc_failures == 0);
        assert(stats.active_connections >= 1);
        assert(stats.tcp_connections + stats.shm_connections >= 1);
        PASS();
    }

    TEST(failed_call_updates_stats);
    {
        Buffer req;
        Buffer resp;
        req.writeRaw("bad", 3);
        int ret = runtime.invoke("MissingService", IFACE_ID, METHOD_ECHO, req, resp, 1000);
        assert(ret != 0);

        RuntimeStats stats;
        assert(runtime.getStats(stats) == 0);
        assert(stats.total_rpc_calls == 6);
        assert(stats.total_rpc_success == 5);
        assert(stats.total_rpc_failures >= 1);
        PASS();
    }

    TEST(reset_stats_clears_counters);
    {
        runtime.resetStats();
        RuntimeStats stats;
        assert(runtime.getStats(stats) == 0);
        assert(stats.total_rpc_calls == 0);
        assert(stats.total_rpc_success == 0);
        assert(stats.total_rpc_failures == 0);
        PASS();
    }

    runtime.stop();
    server_ctx.should_stop = true;
    pthread_join(server_tid, NULL);
    stopSM(sm_pid);

    printf("\nAll runtime stats tests passed.\n");
    return 0;
}
