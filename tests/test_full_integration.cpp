// test_full_integration.cpp - Comprehensive integration tests
//
// Tests:
// 1. Service init creates both TCP + SHM simultaneously
// 2. SM lookup returns shm_name, client auto-selects SHM for same machine
// 3. Multiple clients share the same SHM (multi-client)
// 4. Broadcast / subscribe (pub/sub)
// 5. Death notification
// 6. Service unregister cleanup

#include <omnibinder/omnibinder.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <vector>
#include <string>
#include <atomic>

using namespace omnibinder;

#define TEST(name) printf("  TEST %-45s ", #name); fflush(stdout);
#define PASS() printf("PASS\n"); fflush(stdout);
#define FAIL(msg) printf("FAIL: %s\n", msg); fflush(stdout);

#define REQUIRE(cond, msg) do { \
    if (!(cond)) { \
        FAIL(msg); \
        return 1; \
    } \
} while (0)

static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("CalcService");
static const uint16_t SM_PORT = 19902;

// ============================================================
// Test service: CalcService
// ============================================================
class CalcService : public Service {
public:
    CalcService() : Service("CalcService"), invoke_count_(0) {
        setShmConfig(ShmConfig(8 * 1024, 12 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = "CalcService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
    }
    const char* serviceName() const override { return "CalcService"; }
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
        }
    }
private:
    InterfaceInfo iface_;
    std::atomic<int> invoke_count_;
};

// ============================================================
// Server thread
// ============================================================
struct ServerContext {
    OmniRuntime runtime;
    CalcService service;
    volatile bool registered;
    volatile bool should_stop;
    uint16_t sm_port;
    ServerContext() : registered(false), should_stop(false), sm_port(0) {}
};

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", ctx->sm_port);
    if (ret != 0) { fprintf(stderr, "Server: init failed (%d)\n", ret); return NULL; }
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) { fprintf(stderr, "Server: register failed (%d)\n", ret); ctx->runtime.stop(); return NULL; }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(20);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

// ============================================================
// Helper: SM process management
// ============================================================
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
        const char* paths[] = {
            "./build/target/bin/service_manager",
            "./target/bin/service_manager",
            "./service_manager/service_manager",
            "../service_manager/service_manager",
            "service_manager",
            NULL
        };
        for (int i = 0; paths[i]; i++) {
            execl(paths[i], "service_manager", "--port", port_str, "--log-level", "3", (char*)NULL);
        }
        _exit(1);
    }
    return pid;
}

static void stopSM(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int s;
        for (int i = 0; i < 20; ++i) {
            pid_t ret = waitpid(pid, &s, WNOHANG);
            if (ret == pid) {
                return;
            }
            usleep(100000);
        }
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &s, 0);
        }
    }
}

static bool waitSM(uint16_t port, int retries) {
    for (int i = 0; i < retries; i++) {
        OmniRuntime probe;
        if (probe.init("127.0.0.1", port) == 0) { probe.stop(); return true; }
        usleep(100000);
    }
    return false;
}

// ============================================================
// Main
// ============================================================
int main() {
    printf("=== Full Integration Tests ===\n\n");

    // Clean up stale resources
    unlink("/dev/shm/binder_CalcService");

    // Start SM
    printf("Starting ServiceManager on port %u...\n", SM_PORT);
    pid_t sm_pid = startSM(SM_PORT);
    if (!(sm_pid > 0)) {
        FAIL("failed to start ServiceManager process");
        return 1;
    }
    if (!waitSM(SM_PORT, 30)) {
        FAIL("ServiceManager did not become ready");
        stopSM(sm_pid);
        return 1;
    }
    printf("ServiceManager ready (pid=%d)\n\n", sm_pid);

    // Start server thread
    ServerContext srv;
    srv.sm_port = SM_PORT;
    pthread_t srv_tid;
    if (pthread_create(&srv_tid, NULL, serverThread, &srv) != 0) {
        FAIL("failed to create server thread");
        stopSM(sm_pid);
        return 1;
    }
    for (int i = 0; i < 50 && !srv.registered; i++) usleep(100000);
    assert(srv.registered);
    printf("CalcService registered on port %u\n\n", srv.service.port());

    // ============================================================
    // TEST 1: Service init creates both TCP and SHM
    // ============================================================
    printf("--- Test Group 1: Dual-channel initialization ---\n");

    TEST(service_has_tcp_port) {
        assert(srv.service.port() > 0);
        PASS();
    }

    TEST(service_has_shm_in_registry) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        ServiceInfo info;
        ret = c.lookupService("CalcService", info);
        REQUIRE(ret == 0, "lookupService failed");
        assert(!info.shm_name.empty());
        assert(info.shm_name.find("binder_CalcService") != std::string::npos);
        assert(info.shm_config.req_ring_capacity == 8 * 1024);
        assert(info.shm_config.resp_ring_capacity == 12 * 1024);
        c.stop();
        PASS();
    }

    TEST(service_registry_host_is_routable) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        ServiceInfo info;
        ret = c.lookupService("CalcService", info);
        REQUIRE(ret == 0, "lookupService failed");
        assert(!info.host.empty());
        assert(info.host != "0.0.0.0");
        c.stop();
        PASS();
    }

    TEST(sm_returns_host_id) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        ServiceInfo info;
        ret = c.lookupService("CalcService", info);
        REQUIRE(ret == 0, "lookupService failed");
        assert(!info.host_id.empty());
        // Same machine: host_id should match client's
        assert(info.host_id == c.hostId());
        c.stop();
        PASS();
    }

    // ============================================================
    // TEST 2: Same-machine client auto-selects SHM
    // ============================================================
    printf("\n--- Test Group 2: SHM auto-selection (same machine) ---\n");

    TEST(invoke_via_shm_add) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        Buffer req; req.writeInt32(100); req.writeInt32(200);
        Buffer resp;
        ret = c.invoke("CalcService", IFACE_ID, METHOD_ADD, req, resp, 5000);
        REQUIRE(ret == 0, "invoke add failed");
        (void)ret;
        assert(resp.readInt32() == 300);
        c.stop();
        PASS();
    }

    TEST(invoke_via_shm_echo) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        Buffer req; req.writeString("SHM echo test");
        Buffer resp;
        ret = c.invoke("CalcService", IFACE_ID, METHOD_ECHO, req, resp, 5000);
        REQUIRE(ret == 0, "invoke echo failed");
        (void)ret;
        assert(resp.readString() == "SHM echo test");
        c.stop();
        PASS();
    }

    TEST(interface_mismatch_is_rejected) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        int before = srv.service.invokeCount();
        Buffer req; req.writeInt32(1); req.writeInt32(2);
        Buffer resp;
        ret = c.invoke("CalcService", IFACE_ID + 1, METHOD_ADD, req, resp, 5000);
        assert(ret == static_cast<int>(ErrorCode::ERR_INTERFACE_NOT_FOUND));
        (void)ret;
        assert(srv.service.invokeCount() == before);
        (void)before;
        c.stop();
        PASS();
    }

    TEST(interface_mismatch_oneway_is_rejected) {
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "client init failed");
        int before = srv.service.invokeCount();
        Buffer req; req.writeInt32(7); req.writeInt32(8);
        ret = c.invokeOneWay("CalcService", IFACE_ID + 1, METHOD_ADD, req);
        REQUIRE(ret == 0, "invokeOneWay failed");
        (void)ret;
        for (int i = 0; i < 10; ++i) {
            srv.runtime.pollOnce(20);
            usleep(10000);
        }
        assert(srv.service.invokeCount() == before);
        (void)before;
        c.stop();
        PASS();
    }

    // ============================================================
    // TEST 3: Multiple clients share the same SHM
    // ============================================================
    printf("\n--- Test Group 3: Multi-client SHM sharing ---\n");

    TEST(three_clients_concurrent_invoke) {
        // Create 3 independent clients, each invokes the service
        struct ClientResult {
            int ret;
            int32_t result;
            volatile bool done;
        };

        ClientResult results[3];
        pthread_t tids[3];

        struct ThreadArg {
            uint16_t port;
            int32_t a, b;
            ClientResult* result;
        };

        ThreadArg args[3];
        for (int i = 0; i < 3; i++) {
            args[i].port = SM_PORT;
            args[i].a = (i + 1) * 10;
            args[i].b = (i + 1) * 20;
            args[i].result = &results[i];
            results[i].ret = -1;
            results[i].result = 0;
            results[i].done = false;
        }

        auto clientFn = [](void* arg) -> void* {
            ThreadArg* a = static_cast<ThreadArg*>(arg);
            OmniRuntime c;
            if (c.init("127.0.0.1", a->port) != 0) {
                a->result->done = true;
                return NULL;
            }
            Buffer req; req.writeInt32(a->a); req.writeInt32(a->b);
            Buffer resp;
            a->result->ret = c.invoke("CalcService", IFACE_ID, METHOD_ADD, req, resp, 5000);
            if (a->result->ret == 0) {
                a->result->result = resp.readInt32();
            }
            c.stop();
            a->result->done = true;
            return NULL;
        };

        for (int i = 0; i < 3; i++) {
            pthread_create(&tids[i], NULL, clientFn, &args[i]);
        }
        for (int i = 0; i < 3; i++) {
            pthread_join(tids[i], NULL);
        }

        for (int i = 0; i < 3; i++) {
            REQUIRE(results[i].done, "client thread did not finish");
            REQUIRE(results[i].ret == 0, "concurrent invoke failed");
            REQUIRE(results[i].result == args[i].a + args[i].b, "unexpected concurrent invoke result");
        }
        PASS();
    }

    TEST(sequential_multi_client_invoke) {
        // 5 clients, each does 5 invocations sequentially
        for (int c = 0; c < 5; c++) {
            OmniRuntime runtime;
            int ret = runtime.init("127.0.0.1", SM_PORT);
            REQUIRE(ret == 0, "sequential client init failed");
            for (int i = 0; i < 5; i++) {
                Buffer req; req.writeInt32(c * 100 + i); req.writeInt32(1);
                Buffer resp;
                ret = runtime.invoke("CalcService", IFACE_ID, METHOD_ADD, req, resp, 5000);
                REQUIRE(ret == 0, "sequential invoke failed");
                (void)ret;
                assert(resp.readInt32() == c * 100 + i + 1);
            }
            runtime.stop();
        }
        PASS();
    }

    // ============================================================
    // TEST 4: Broadcast / Subscribe
    // ============================================================
    printf("\n--- Test Group 4: Broadcast / Subscribe ---\n");

    TEST(publish_and_subscribe_topic) {
        // Server publishes a topic, subscriber receives broadcast
        static const char* TOPIC_NAME = "calc_result";
        uint32_t topic_id = fnv1a_32(TOPIC_NAME);

        // Server publishes topic
        int ret = srv.runtime.publishTopic(TOPIC_NAME);
        REQUIRE(ret == 0, "publishTopic failed");
        (void)ret;

        // Subscriber
        OmniRuntime sub;
        ret = sub.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "subscriber init failed");

        volatile bool received = false;
        volatile int32_t received_value = 0;

        ret = sub.subscribeTopic(TOPIC_NAME,
            [&received, &received_value](uint32_t tid, const Buffer& data) {
                (void)tid;
                Buffer buf(data.data(), data.size());
                received_value = buf.readInt32();
                received = true;
            });
        REQUIRE(ret == 0, "subscribeTopic failed");

        // Wait for subscription to propagate
        for (int i = 0; i < 20; i++) {
            sub.pollOnce(50);
            srv.runtime.pollOnce(10);
        }

        // Broadcast data
        Buffer bdata;
        bdata.writeInt32(42);
        ret = srv.runtime.broadcast(topic_id, bdata);
        REQUIRE(ret == 0, "broadcast failed");

        // Wait for subscriber to receive
        for (int i = 0; i < 50 && !received; i++) {
            sub.pollOnce(50);
            srv.runtime.pollOnce(10);
        }

        REQUIRE(received, "topic callback not received");
        REQUIRE(received_value == 42, "topic callback value mismatch");

        ret = sub.unsubscribeTopic(TOPIC_NAME);
        REQUIRE(ret == 0, "unsubscribeTopic failed");

        received = false;
        received_value = 0;
        Buffer second_broadcast;
        second_broadcast.writeInt32(84);
        ret = srv.runtime.broadcast(topic_id, second_broadcast);
        REQUIRE(ret == 0, "second broadcast failed");

        for (int i = 0; i < 30 && !received; i++) {
            sub.pollOnce(50);
            srv.runtime.pollOnce(10);
        }

        REQUIRE(!received, "topic callback should not be delivered after unsubscribe");
        sub.stop();
        PASS();
    }

    // ============================================================
    // TEST 5: Death notification
    // ============================================================
    printf("\n--- Test Group 5: Death notification ---\n");

    TEST(death_notification_on_service_crash) {
        // Start a second service in a child process, subscribe to its death,
        // then kill the child and verify notification.

        // Fork a child that registers "EphemeralService"
        pid_t child = fork();
        if (child == 0) {
            // Child process: register a service and run until killed
            class EphService : public Service {
            public:
                EphService() : Service("EphemeralService") {
                    iface_.interface_id = fnv1a_32("EphemeralService");
                    iface_.name = "EphemeralService";
                }
                const char* serviceName() const override { return "EphemeralService"; }
                const InterfaceInfo& interfaceInfo() const override { return iface_; }
            protected:
                void onInvoke(uint32_t, const Buffer&, Buffer&) override {}
            private:
                InterfaceInfo iface_;
            };

            OmniRuntime c;
            if (c.init("127.0.0.1", SM_PORT) != 0) _exit(1);
            EphService svc;
            if (c.registerService(&svc) != 0) { c.stop(); _exit(1); }
            // Signal parent we're ready
            while (true) { c.pollOnce(50); }
            _exit(0);
        }

        // Parent: wait for EphemeralService to appear
        OmniRuntime watcher;
        int ret = watcher.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "watcher init failed");

        bool found = false;
        for (int i = 0; i < 50; i++) {
            ServiceInfo info;
            if (watcher.lookupService("EphemeralService", info) == 0) {
                found = true;
                break;
            }
            usleep(100000);
        }
        REQUIRE(found, "EphemeralService not found");
        (void)found;

        // Subscribe to death
        volatile bool death_received = false;
        ret = watcher.subscribeServiceDeath("EphemeralService",
            [&death_received](const std::string& name) {
                (void)name;
                death_received = true;
            });
        REQUIRE(ret == 0, "subscribeServiceDeath failed");

        // Kill the child
        kill(child, SIGKILL);
        int status;
        waitpid(child, &status, 0);

        // Wait for death notification (SM heartbeat timeout ~10s, but we wait up to 15s)
        for (int i = 0; i < 150 && !death_received; i++) {
            watcher.pollOnce(100);
        }

        REQUIRE(death_received, "death notification not received");
        watcher.stop();
        PASS();
    }

    TEST(unsubscribe_service_death_stops_callback) {
        pid_t child = fork();
        if (child == 0) {
            class EphService2 : public Service {
            public:
                EphService2() : Service("EphemeralService2") {
                    iface_.interface_id = fnv1a_32("EphemeralService2");
                    iface_.name = "EphemeralService2";
                }
                const char* serviceName() const override { return "EphemeralService2"; }
                const InterfaceInfo& interfaceInfo() const override { return iface_; }
            protected:
                void onInvoke(uint32_t, const Buffer&, Buffer&) override {}
            private:
                InterfaceInfo iface_;
            };

            OmniRuntime c;
            if (c.init("127.0.0.1", SM_PORT) != 0) _exit(1);
            EphService2 svc;
            if (c.registerService(&svc) != 0) { c.stop(); _exit(1); }
            while (true) { c.pollOnce(50); }
            _exit(0);
        }

        OmniRuntime watcher;
        int ret = watcher.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "watcher init failed");

        bool found = false;
        for (int i = 0; i < 50; i++) {
            ServiceInfo info;
            if (watcher.lookupService("EphemeralService2", info) == 0) {
                found = true;
                break;
            }
            usleep(100000);
        }
        REQUIRE(found, "EphemeralService2 not found");
        (void)found;

        volatile bool death_received = false;
        ret = watcher.subscribeServiceDeath("EphemeralService2",
            [&death_received](const std::string& name) {
                (void)name;
                death_received = true;
            });
        REQUIRE(ret == 0, "subscribeServiceDeath failed");

        ret = watcher.unsubscribeServiceDeath("EphemeralService2");
        REQUIRE(ret == 0, "unsubscribeServiceDeath failed");

        kill(child, SIGKILL);
        int status;
        waitpid(child, &status, 0);

        for (int i = 0; i < 80 && !death_received; i++) {
            watcher.pollOnce(100);
        }

        REQUIRE(!death_received, "death callback should not be delivered after unsubscribe");
        watcher.stop();
        PASS();
    }

    // ============================================================
    // TEST 6: Service unregister and re-register
    // ============================================================
    printf("\n--- Test Group 6: Lifecycle ---\n");

    TEST(invoke_count_accumulated) {
        // Verify the server actually processed invocations
        assert(srv.service.invokeCount() > 0);
        printf("(count=%d) ", srv.service.invokeCount());
        PASS();
    }

    // ============================================================
    // Cleanup
    // ============================================================
    printf("\nCleaning up...\n");
    srv.should_stop = true;
    pthread_join(srv_tid, NULL);
    printf("  Server thread stopped\n");

    TEST(service_gone_after_unregister) {
        usleep(200000);
        OmniRuntime c;
        int ret = c.init("127.0.0.1", SM_PORT);
        REQUIRE(ret == 0, "cleanup verifier init failed");
        ServiceInfo info;
        ret = c.lookupService("CalcService", info);
        REQUIRE(ret != 0, "CalcService should be gone after unregister");
        (void)ret;
        c.stop();
        PASS();
    }

    stopSM(sm_pid);

    printf("\n=== All full integration tests passed! ===\n");
    return 0;
}
