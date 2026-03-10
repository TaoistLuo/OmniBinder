#include <omnibinder/omnibinder.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace omnibinder;

#define TEST(name) printf("  TEST %-40s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)

static const uint16_t SM_PORT = 19931;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("ThreadSafeService");

class ThreadSafeService : public Service {
public:
    ThreadSafeService() : Service("ThreadSafeService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "ThreadSafeService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }

    const char* serviceName() const override { return "ThreadSafeService"; }
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
    ThreadSafeService service;
    volatile bool registered;
    volatile bool should_stop;
    ServerContext() : registered(false), should_stop(false) {}
};

struct ClientThreadArg {
    OmniRuntime* runtime;
    int thread_index;
    int iterations;
    volatile bool done;
    int failures;
    ClientThreadArg() : runtime(NULL), thread_index(0), iterations(0), done(false), failures(0) {}
};

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", SM_PORT);
    if (ret != 0) {
        fprintf(stderr, "server init failed: %d\n", ret);
        return NULL;
    }
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) {
        fprintf(stderr, "server register failed: %d\n", ret);
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

static void* concurrentInvokeThread(void* arg) {
    ClientThreadArg* ctx = static_cast<ClientThreadArg*>(arg);
    for (int i = 0; i < ctx->iterations; ++i) {
        Buffer req;
        Buffer resp;
        char payload[64];
        snprintf(payload, sizeof(payload), "thread-%d-iter-%d", ctx->thread_index, i);
        req.writeRaw(payload, strlen(payload));

        int ret = ctx->runtime->invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, req, resp, 5000);
        if (ret != 0) {
            ctx->failures++;
            continue;
        }

        std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
        if (result != payload) {
            ctx->failures++;
        }
    }
    ctx->done = true;
    return NULL;
}

int main() {
    printf("=== Thread-safe Client And Reconnect Tests ===\n\n");

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

    TEST(concurrent_invoke_same_client);
    {
        pthread_t tids[6];
        ClientThreadArg args[6];
        for (int i = 0; i < 6; ++i) {
            args[i].runtime = &runtime;
            args[i].thread_index = i;
            args[i].iterations = 40;
            create_ret = pthread_create(&tids[i], NULL, concurrentInvokeThread, &args[i]);
            assert(create_ret == 0);
        }
        for (int i = 0; i < 6; ++i) {
            pthread_join(tids[i], NULL);
            assert(args[i].done);
            assert(args[i].failures == 0);
        }
        PASS();
    }

    TEST(sm_restart_recovery_same_client);
    {
        stopSM(sm_pid);
        usleep(300000);
        sm_pid = startSM(SM_PORT);
        assert(sm_pid > 0);
        sm_ready = waitSMReady(SM_PORT, 30);
        assert(sm_ready);

        bool recovered = false;
        for (int i = 0; i < 80 && !recovered; ++i) {
            Buffer req;
            Buffer resp;
            const char* payload = "after-restart";
            req.writeRaw(payload, strlen(payload));
            int ret = runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, req, resp, 3000);
            if (ret == 0) {
                std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
                if (result == payload) {
                    recovered = true;
                    break;
                }
            }
            usleep(100000);
        }
        assert(recovered);
        PASS();
    }

    TEST(server_restart_recovery_same_client);
    {
        Buffer warm_req;
        Buffer warm_resp;
        const char* warm_payload = "before-server-restart";
        warm_req.writeRaw(warm_payload, strlen(warm_payload));
        assert(runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, warm_req, warm_resp, 3000) == 0);

        server_ctx.should_stop = true;
        pthread_join(server_tid, NULL);

        server_ctx.registered = false;
        server_ctx.should_stop = false;
        assert(pthread_create(&server_tid, NULL, serverThread, &server_ctx) == 0);
        for (int i = 0; i < 50 && !server_ctx.registered; ++i) {
            usleep(100000);
        }
        assert(server_ctx.registered);

        bool server_recovered = false;
        for (int i = 0; i < 80 && !server_recovered; ++i) {
            Buffer req;
            Buffer resp;
            const char* payload = "after-server-restart";
            req.writeRaw(payload, strlen(payload));
            int ret = runtime.invoke("ThreadSafeService", IFACE_ID, METHOD_ECHO, req, resp, 3000);
            if (ret == 0) {
                std::string result(reinterpret_cast<const char*>(resp.data()), resp.size());
                if (result == payload) {
                    server_recovered = true;
                }
            }
            usleep(100000);
        }
        assert(server_recovered);
        PASS();
    }

    runtime.stop();
    server_ctx.should_stop = true;
    pthread_join(server_tid, NULL);
    stopSM(sm_pid);

    printf("\nAll thread-safe client and reconnect tests passed.\n");
    return 0;
}
