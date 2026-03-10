// test_integration.cpp - End-to-end integration test
//
// This test starts a ServiceManager process, then uses OmniRuntime to:
// 1. Register a service (in a background thread)
// 2. Discover the service from another client
// 3. Invoke methods on the service
// 4. List services / query interfaces
// 5. Unregister and verify cleanup

#include <omnibinder/omnibinder.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>

using namespace omnibinder;

#define TEST(name) printf("  TEST %s ... ", #name); fflush(stdout);
#define PASS() printf("PASS\n"); fflush(stdout);

// Method IDs (FNV-1a hashes)
static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("TestService");

// ============================================================
// A simple test service
// ============================================================
class TestService : public Service {
public:
    TestService() : Service("TestService"), invoke_count_(0) {
        setShmConfig(ShmConfig(4 * 1024, 4 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = "TestService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }

    const char* serviceName() const override { return "TestService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
    int invokeCount() const { return invoke_count_; }

protected:
    void onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        invoke_count_++;
        if (method_id == METHOD_ADD) {
            Buffer req(request.data(), request.size());
            int32_t a = req.readInt32();
            int32_t b = req.readInt32();
            response.writeInt32(a + b);
        } else if (method_id == METHOD_ECHO) {
            if (request.size() > 0) {
                response.writeRaw(request.data(), request.size());
            }
        } else {
            response.writeInt32(-1);
        }
    }

private:
    InterfaceInfo iface_;
    int invoke_count_;
};

// ============================================================
// Server thread context
// ============================================================
struct ServerContext {
    OmniRuntime runtime;
    TestService service;
    volatile bool registered;
    volatile bool should_stop;
    uint16_t sm_port;

    ServerContext() : registered(false), should_stop(false), sm_port(0) {}
};

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);

    int ret = ctx->runtime.init("127.0.0.1", ctx->sm_port);
    if (ret != 0) {
        fprintf(stderr, "Server: failed to init client\n");
        return NULL;
    }

    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) {
        fprintf(stderr, "Server: failed to register service\n");
        ctx->runtime.stop();
        return NULL;
    }

    ctx->registered = true;

    // Event loop: process incoming requests
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(50);
    }

    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

// ============================================================
// Helper: start ServiceManager process
// ============================================================
static pid_t startServiceManager(uint16_t port) {
    char kill_cmd[128];
    snprintf(kill_cmd, sizeof(kill_cmd), "pkill -f 'service_manager --port %u' >/dev/null 2>&1 || true", port);
    int kill_rc = system(kill_cmd);
    (void)kill_rc;
    usleep(100000);

    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        const char* paths[] = {
            "./build/target/bin/service_manager",
            "./target/bin/service_manager",
            "./service_manager/service_manager",
            "../service_manager/service_manager",
            "service_manager",
            NULL
        };
        for (int i = 0; paths[i] != NULL; i++) {
            execl(paths[i], "service_manager",
                  "--port", port_str, "--log-level", "3", (char*)NULL);
        }
        fprintf(stderr, "Failed to exec service_manager\n");
        _exit(1);
    }
    return pid;
}

static void stopServiceManager(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status;
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

static bool waitForSM(const char* host, uint16_t port, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        OmniRuntime probe;
        int ret = probe.init(host, port);
        if (ret == 0) {
            probe.stop();
            return true;
        }
        usleep(100000);
    }
    return false;
}

// ============================================================
// Main
// ============================================================
static uint16_t SM_PORT = 19901;

int main() {
    printf("=== Integration Tests ===\n\n");

    // Start ServiceManager
    printf("Starting ServiceManager on port %u...\n", SM_PORT);
    pid_t sm_pid = startServiceManager(SM_PORT);
    assert(sm_pid > 0);

    bool ready = waitForSM("127.0.0.1", SM_PORT, 30);
    if (!ready) {
        fprintf(stderr, "ServiceManager failed to start\n");
        stopServiceManager(sm_pid);
        return 1;
    }
    printf("ServiceManager ready (pid=%d)\n\n", sm_pid);

    // ============================================================
    // Test: Connect to ServiceManager
    // ============================================================
    TEST(connect_to_sm) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);
        runtime.stop();
        PASS();
    }

    // ============================================================
    // Start server in background thread
    // ============================================================
    ServerContext server_ctx;
    server_ctx.sm_port = SM_PORT;
    pthread_t server_tid;
    int pret = pthread_create(&server_tid, NULL, serverThread, &server_ctx);
    assert(pret == 0);

    // Wait for service to be registered
    for (int i = 0; i < 50 && !server_ctx.registered; i++) {
        usleep(100000);
    }
    assert(server_ctx.registered);
    printf("  Server thread started, service registered on port %u\n\n",
           server_ctx.service.port());

    // ============================================================
    // Test: List services
    // ============================================================
    TEST(list_services) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        std::vector<ServiceInfo> services;
        ret = runtime.listServices(services);
        assert(ret == 0);
        assert(services.size() >= 1);

        bool found = false;
        for (size_t i = 0; i < services.size(); i++) {
            if (services[i].name == "TestService") {
                found = true;
                break;
            }
        }
        assert(found);
        runtime.stop();
        PASS();
    }

    // ============================================================
    // Test: Lookup service
    // ============================================================
    TEST(lookup_service) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        ServiceInfo info;
        ret = runtime.lookupService("TestService", info);
        assert(ret == 0);
        assert(info.name == "TestService");
        assert(info.port == server_ctx.service.port());
        assert(!info.host_id.empty());
        assert(info.shm_config.req_ring_capacity == 4 * 1024);
        assert(info.shm_config.resp_ring_capacity == 4 * 1024);
        assert(info.interfaces.size() == 1);
        assert(info.interfaces[0].name == "TestService");
        assert(info.interfaces[0].methods.size() == 2);
        runtime.stop();
        PASS();
    }

    // ============================================================
    // Test: Lookup non-existent service
    // ============================================================
    TEST(lookup_nonexistent) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        ServiceInfo info;
        ret = runtime.lookupService("NonExistentService", info);
        assert(ret != 0);
        runtime.stop();
        PASS();
    }

    // ============================================================
    // Test: Query interfaces
    // ============================================================
    TEST(query_interfaces) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        std::vector<InterfaceInfo> ifaces;
        ret = runtime.queryInterfaces("TestService", ifaces);
        assert(ret == 0);
        assert(ifaces.size() == 1);
        assert(ifaces[0].name == "TestService");
        assert(ifaces[0].interface_id == IFACE_ID);

        bool found_add = false, found_echo = false;
        for (size_t i = 0; i < ifaces[0].methods.size(); i++) {
            if (ifaces[0].methods[i].name == "Add") found_add = true;
            if (ifaces[0].methods[i].name == "Echo") found_echo = true;
        }
        assert(found_add);
        assert(found_echo);
        runtime.stop();
        PASS();
    }

    // ============================================================
    // Test: Invoke Add method
    // ============================================================
    TEST(invoke_add) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        Buffer request;
        request.writeInt32(17);
        request.writeInt32(25);

        Buffer response;
        ret = runtime.invoke("TestService", IFACE_ID, METHOD_ADD,
                            request, response, 5000);
        assert(ret == 0);
        assert(response.size() >= 4);

        int32_t result = response.readInt32();
        assert(result == 42);

        runtime.stop();
        PASS();
    }

    // ============================================================
    // Test: Invoke Echo method
    // ============================================================
    TEST(invoke_echo) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        Buffer request;
        request.writeString("Hello OmniBinder!");

        Buffer response;
        ret = runtime.invoke("TestService", IFACE_ID, METHOD_ECHO,
                            request, response, 5000);
        assert(ret == 0);
        assert(response.size() > 0);

        std::string echo = response.readString();
        assert(echo == "Hello OmniBinder!");

        runtime.stop();
        PASS();
    }

    // ============================================================
    // Test: Multiple sequential invocations
    // ============================================================
    TEST(multiple_invocations) {
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        for (int i = 0; i < 10; i++) {
            Buffer request;
            request.writeInt32(i);
            request.writeInt32(100);

            Buffer response;
            ret = runtime.invoke("TestService", IFACE_ID, METHOD_ADD,
                                request, response, 5000);
            assert(ret == 0);

            int32_t result = response.readInt32();
            assert(result == i + 100);
        }

        runtime.stop();
        PASS();
    }

    // ============================================================
    // Cleanup
    // ============================================================
    printf("\nCleaning up...\n");
    server_ctx.should_stop = true;
    pthread_join(server_tid, NULL);
    printf("  Server thread stopped\n");

    // Verify service is gone after unregister
    TEST(service_gone_after_unregister) {
        usleep(200000); // Give SM time to process
        OmniRuntime runtime;
        int ret = runtime.init("127.0.0.1", SM_PORT);
        assert(ret == 0);

        ServiceInfo info;
        ret = runtime.lookupService("TestService", info);
        assert(ret != 0);
        runtime.stop();
        PASS();
    }

    stopServiceManager(sm_pid);

    printf("\nAll integration tests passed!\n");
    return 0;
}
