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

#define TEST(name) printf("  TEST %-40s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)

static const uint16_t SM_PORT = 19916;
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("ReconnectService");

class ReconnectService : public Service {
public:
    ReconnectService() : Service("ReconnectService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "ReconnectService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }

    const char* serviceName() const override { return "ReconnectService"; }
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
    ReconnectService service;
    volatile bool registered;
    volatile bool should_stop;
    ServerContext() : registered(false), should_stop(false) {}
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
    printf("=== ServiceManager Reconnect Test ===\n\n");

    pid_t sm_pid = startSM(SM_PORT);
    assert(sm_pid > 0);
    assert(waitSMReady(SM_PORT, 30));

    ServerContext server_ctx;
    pthread_t server_tid = 0;
    assert(pthread_create(&server_tid, NULL, serverThread, &server_ctx) == 0);
    for (int i = 0; i < 50 && !server_ctx.registered; ++i) {
        usleep(100000);
    }
    assert(server_ctx.registered);

    OmniRuntime runtime;
    assert(runtime.init("127.0.0.1", SM_PORT) == 0);

    TEST(initial_lookup_and_invoke);
    {
        assert(waitLookup(client, "ReconnectService", 20));
        Buffer req;
        Buffer resp;
        const char* msg = "before-restart";
        req.writeRaw(msg, strlen(msg));
        assert(runtime.invoke("ReconnectService", IFACE_ID, METHOD_ECHO, req, resp, 3000) == 0);
        assert(resp.size() == strlen(msg));
        PASS();
    }

    TEST(sm_restart_recovery);
    {
        stopSM(sm_pid);
        usleep(300000);
        sm_pid = startSM(SM_PORT);
        assert(sm_pid > 0);
        assert(waitSMReady(SM_PORT, 30));

        bool recovered = false;
        for (int i = 0; i < 80 && !recovered; ++i) {
            runtime.pollOnce(100);
            ServiceInfo info;
            if (runtime.lookupService("ReconnectService", info) == 0) {
                Buffer req;
                Buffer resp;
                const char* msg = "after-restart";
                req.writeRaw(msg, strlen(msg));
                if (runtime.invoke("ReconnectService", IFACE_ID, METHOD_ECHO, req, resp, 3000) == 0 &&
                    resp.size() == strlen(msg)) {
                    recovered = true;
                    break;
                }
            }
            usleep(100000);
        }
        assert(recovered);
        PASS();
    }

    server_ctx.should_stop = true;
    pthread_join(server_tid, NULL);
    runtime.stop();
    stopSM(sm_pid);

    printf("\nAll ServiceManager reconnect tests passed.\n");
    return 0;
}
