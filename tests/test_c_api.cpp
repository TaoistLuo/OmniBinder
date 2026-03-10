#include <omnibinder/omnibinder_c.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST(name) printf("  TEST %-38s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)
#define FAIL(msg) printf("FAIL: %s\n", msg); fflush(stdout)
#define REQUIRE(cond, msg) do { \
    if (!(cond)) { \
        FAIL(msg); \
        return 1; \
    } \
} while (0)

static const uint16_t SM_PORT = 19941;
static const uint32_t IFACE_ID = 0x11223344u;
static const uint32_t METHOD_ECHO = 0x55667788u;

struct ServerContext {
    omni_runtime_t* runtime;
    omni_service_t* service;
    volatile bool registered;
    volatile bool should_stop;
    ServerContext() : runtime(NULL), service(NULL), registered(false), should_stop(false) {}
};

static void cEchoCallback(uint32_t method_id, const omni_buffer_t* request,
                          omni_buffer_t* response, void* user_data) {
    (void)user_data;
    if (method_id != METHOD_ECHO) {
        return;
    }
    uint32_t len = 0;
    char* payload = omni_buffer_read_string(const_cast<omni_buffer_t*>(request), &len);
    if (payload == NULL) {
        return;
    }
    omni_buffer_write_string(response, payload, len);
    free(payload);
}

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = omni_runtime_init(ctx->runtime, "127.0.0.1", SM_PORT);
    if (ret != 0) {
        return NULL;
    }

    ret = omni_runtime_register_service(ctx->runtime, ctx->service);
    if (ret != 0) {
        return NULL;
    }
    ctx->registered = true;

    while (!ctx->should_stop) {
        omni_runtime_poll_once(ctx->runtime, 20);
    }

    omni_runtime_unregister_service(ctx->runtime, ctx->service);
    omni_runtime_stop(ctx->runtime);
    return NULL;
}

static pid_t startSM(uint16_t port) {
    char kill_cmd[128];
    std::snprintf(kill_cmd, sizeof(kill_cmd), "pkill -f 'service_manager --port %u' >/dev/null 2>&1 || true", port);
    int kill_rc = system(kill_cmd);
    (void)kill_rc;
    usleep(100000);

    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%u", port);
        const char* paths[] = {
            "./build/target/bin/service_manager",
            "./target/bin/service_manager",
            "./service_manager/service_manager",
            "../service_manager/service_manager",
            "service_manager",
            NULL
        };
        for (int i = 0; paths[i] != NULL; ++i) {
            execl(paths[i], "service_manager", "--port", port_str, "--log-level", "3", (char*)NULL);
        }
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
        omni_runtime_t* probe = omni_runtime_create();
        assert(probe != NULL);
        if (omni_runtime_init(probe, "127.0.0.1", port) == 0) {
            omni_runtime_stop(probe);
            omni_runtime_destroy(probe);
            return true;
        }
        omni_runtime_destroy(probe);
        usleep(100000);
    }
    return false;
}

int main() {
    printf("=== C API Tests ===\n\n");

    TEST(buffer_roundtrip_and_hash);
    {
        omni_buffer_t* buf = omni_buffer_create();
        REQUIRE(buf != NULL, "buffer create failed");
        omni_buffer_write_int32(buf, 1234);
        omni_buffer_write_string(buf, "hello-c-api", 11);
        REQUIRE(omni_buffer_size(buf) > 0, "buffer remained empty after writes");
        omni_buffer_reset(buf);
        int32_t value = omni_buffer_read_int32(buf);
        REQUIRE(value == 1234, "buffer int roundtrip failed");
        uint32_t len = 0;
        char* text = omni_buffer_read_string(buf, &len);
        REQUIRE(text != NULL, "buffer string read returned null");
        REQUIRE(len == 11, "buffer string length mismatch");
        REQUIRE(std::strcmp(text, "hello-c-api") == 0, "buffer string content mismatch");
        free(text);
        REQUIRE(omni_fnv1a_32("Echo") != 0, "fnv hash returned zero");
        omni_buffer_destroy(buf);
        PASS();
    }

    pid_t sm_pid = startSM(SM_PORT);
    REQUIRE(sm_pid > 0, "failed to start service manager");
    REQUIRE(waitSMReady(SM_PORT, 30), "service manager did not become ready");

    ServerContext server_ctx;
    server_ctx.runtime = omni_runtime_create();
    server_ctx.service = omni_service_create("CService", IFACE_ID, cEchoCallback, NULL);
    REQUIRE(server_ctx.runtime != NULL, "server runtime create failed");
    REQUIRE(server_ctx.service != NULL, "server service create failed");
    omni_service_add_method(server_ctx.service, METHOD_ECHO, "Echo");

    pthread_t server_tid = 0;
    int pthread_ret = pthread_create(&server_tid, NULL, serverThread, &server_ctx);
    REQUIRE(pthread_ret == 0, "server thread create failed");
    for (int i = 0; i < 50 && !server_ctx.registered; ++i) {
        usleep(100000);
    }
    REQUIRE(server_ctx.registered, "server failed to register service");

    TEST(c_api_register_invoke_and_stats);
    {
        omni_runtime_t* runtime = omni_runtime_create();
        REQUIRE(runtime != NULL, "runtime create failed");
        int init_ret = omni_runtime_init(runtime, "127.0.0.1", SM_PORT);
        REQUIRE(init_ret == 0, "runtime init failed");

        omni_buffer_t* req = omni_buffer_create();
        omni_buffer_t* resp = omni_buffer_create();
        REQUIRE(req != NULL, "request buffer create failed");
        REQUIRE(resp != NULL, "response buffer create failed");
        omni_buffer_write_string(req, "rpc-through-c-api", 17);

        int ret = omni_runtime_invoke(runtime, "CService", IFACE_ID, METHOD_ECHO, req, resp, 3000);
        REQUIRE(ret == 0, "c api invoke failed");

        uint32_t len = 0;
        char* echoed = omni_buffer_read_string(resp, &len);
        REQUIRE(echoed != NULL, "invoke response string was null");
        REQUIRE(len == 17, "invoke response length mismatch");
        REQUIRE(std::strcmp(echoed, "rpc-through-c-api") == 0, "invoke response content mismatch");
        free(echoed);

        omni_runtime_stats_t stats;
        int stats_ret = omni_runtime_get_stats(runtime, &stats);
        REQUIRE(stats_ret == 0, "get stats failed");
        REQUIRE(stats.total_rpc_calls == 1, "stats rpc call count mismatch");
        REQUIRE(stats.total_rpc_success == 1, "stats rpc success count mismatch");
        REQUIRE(stats.total_rpc_failures == 0, "stats rpc failure count mismatch");

        omni_runtime_reset_stats(runtime);
        stats_ret = omni_runtime_get_stats(runtime, &stats);
        REQUIRE(stats_ret == 0, "get stats after reset failed");
        REQUIRE(stats.total_rpc_calls == 0, "stats reset did not clear rpc calls");

        omni_buffer_destroy(req);
        omni_buffer_destroy(resp);
        omni_runtime_stop(runtime);
        omni_runtime_destroy(runtime);
        PASS();
    }

    server_ctx.should_stop = true;
    pthread_join(server_tid, NULL);
    omni_service_destroy(server_ctx.service);
    omni_runtime_destroy(server_ctx.runtime);
    stopSM(sm_pid);

    printf("\nAll C API tests passed.\n");
    return 0;
}
