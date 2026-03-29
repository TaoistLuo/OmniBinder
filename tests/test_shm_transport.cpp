// test_shm_transport.cpp - Tests for multi-client shared memory transport

#include "transport/shm_transport.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>

using namespace omnibinder;

#define TEST(name) printf("  TEST %s ... ", #name);
#define PASS() printf("PASS\n");

int main() {
    printf("=== Shared Memory Transport Tests (Multi-Client) ===\n\n");

    platform::netInit();

    // ============================================================
    // Utility function tests
    // ============================================================
    printf("--- Utility Tests ---\n");

    TEST(calculate_shm_size) {
        // ControlBlock + RequestQueue(header+data) + 4 * ResponseSlot(header+data)
        assert(calculateShmSize(1024, 512, 4)
               == sizeof(ShmControlBlock)
                + sizeof(ShmRingHeader) + 1024
                + (sizeof(ShmRingHeader) + 512) * 4);
        PASS();
    }

    TEST(generate_shm_name) {
        std::string name = generateShmName("myservice");
        assert(name == "/binder_myservice");

        std::string name2 = generateShmName("test");
        assert(name2 == "/binder_test");
        PASS();
    }

    // ============================================================
    // Server creation tests
    // ============================================================
    printf("\n--- Server Creation Tests ---\n");

    TEST(server_create) {
        std::string shm_name = generateShmName("srv_create_test");
        ShmTransport server(shm_name, true);
        assert(server.state() == ConnectionState::CONNECTED);
        assert(server.isServer());
        assert(server.type() == TransportType::SHM);
        assert(server.fd() >= 0);
        assert(server.clientCount() == 0);
        server.close();
        PASS();
    }

    // ============================================================
    // Client connection tests
    // ============================================================
    printf("\n--- Client Connection Tests ---\n");

    TEST(client_connect) {
        std::string shm_name = generateShmName("client_conn_test");
        ShmTransport server(shm_name, true);
        assert(server.state() == ConnectionState::CONNECTED);

        ShmTransport client(shm_name, false);
        assert(client.state() == ConnectionState::DISCONNECTED);

        assert(client.connect("", 0) == 0);
        assert(client.state() == ConnectionState::CONNECTED);
        assert(client.clientId() == 0);  // First client gets slot 0

        assert(server.clientCount() == 1);

        client.close();
        server.close();
        PASS();
    }

    TEST(multiple_clients_connect) {
        std::string shm_name = generateShmName("multi_client_test");
        ShmTransport server(shm_name, true);

        ShmTransport client0(shm_name, false);
        assert(client0.connect("", 0) == 0);
        assert(client0.clientId() == 0);

        ShmTransport client1(shm_name, false);
        assert(client1.connect("", 0) == 0);
        assert(client1.clientId() == 1);

        ShmTransport client2(shm_name, false);
        assert(client2.connect("", 0) == 0);
        assert(client2.clientId() == 2);

        assert(server.clientCount() == 3);
        assert(server.responseSlotsInUse() == 3);

        client0.close();
        assert(server.clientCount() == 2);
        assert(server.responseSlotsInUse() == 2);

        ShmTransport client_reuse(shm_name, false);
        assert(client_reuse.connect("", 0) == 0);
        assert(client_reuse.clientId() == 0);

        client1.close();
        client2.close();
        client_reuse.close();
        server.close();
        PASS();
    }

    TEST(active_response_arena_stats) {
        std::string shm_name = generateShmName("arena_stats_test");
        ShmTransport server(shm_name, true);

        assert(server.totalResponseArenaSize() > 0);
        assert(server.activeResponseArenaSize() == 0);
        assert(server.responseSlotsInUse() == 0);

        ShmTransport client0(shm_name, false);
        assert(client0.connect("", 0) == 0);
        const size_t per_client_block_size = server.activeResponseArenaSize();
        (void)per_client_block_size;
        assert(server.activeResponseArenaSize() > 0);
        assert(server.responseSlotsInUse() == 1);

        ShmTransport client1(shm_name, false);
        assert(client1.connect("", 0) == 0);
        assert(server.responseSlotsInUse() == 2);
        assert(server.activeResponseArenaSize() == per_client_block_size * 2);

        std::vector<uint32_t> ids = server.activeClientIds();
        std::sort(ids.begin(), ids.end());
        assert(ids.size() == 2);
        assert(ids[0] == 0);
        assert(ids[1] == 1);

        client0.close();
        assert(server.responseSlotsInUse() == 1);
        assert(server.activeResponseArenaSize() == per_client_block_size);

        client1.close();
        assert(server.responseSlotsInUse() == 0);
        assert(server.activeResponseArenaSize() == 0);

        server.close();
        PASS();
    }

    // ============================================================
    // Communication tests (multi-client model)
    // ============================================================
    printf("\n--- Communication Tests ---\n");

    TEST(client_send_server_recv) {
        std::string shm_name = generateShmName("c2s_test");
        ShmTransport server(shm_name, true);

        ShmTransport client(shm_name, false);
        assert(client.connect("", 0) == 0);

        // Client sends data (goes to RequestQueue)
        assert(client.send(reinterpret_cast<const uint8_t*>("Hello from client!"), strlen("Hello from client!"))
               == static_cast<int>(strlen("Hello from client!")));

        platform::sleepMs(10);

        // Server receives from RequestQueue
        uint8_t recv_buf[256];
        memset(recv_buf, 0, sizeof(recv_buf));
        uint32_t from_client_id = 0;
        (void)from_client_id;
        assert(server.serverRecv(recv_buf, sizeof(recv_buf), from_client_id)
               == static_cast<int>(strlen("Hello from client!")));
        assert(from_client_id == 0);  // From client slot 0
        assert(memcmp(recv_buf, "Hello from client!", strlen("Hello from client!")) == 0);

        client.close();
        server.close();
        PASS();
    }

    TEST(server_send_client_recv) {
        std::string shm_name = generateShmName("s2c_test");
        ShmTransport server(shm_name, true);

        ShmTransport client(shm_name, false);
        assert(client.connect("", 0) == 0);

        // Server sends to client's ResponseSlot
        assert(server.serverSend(0, reinterpret_cast<const uint8_t*>("Hello from server!"), strlen("Hello from server!"))
               == static_cast<int>(strlen("Hello from server!")));

        platform::sleepMs(10);

        // Client receives from its ResponseSlot
        uint8_t recv_buf[256];
        memset(recv_buf, 0, sizeof(recv_buf));
        assert(client.recv(recv_buf, sizeof(recv_buf)) == static_cast<int>(strlen("Hello from server!")));
        assert(memcmp(recv_buf, "Hello from server!", strlen("Hello from server!")) == 0);

        client.close();
        server.close();
        PASS();
    }

    TEST(request_response_roundtrip) {
        std::string shm_name = generateShmName("roundtrip_test");
        ShmTransport server(shm_name, true);

        ShmTransport client(shm_name, false);
        assert(client.connect("", 0) == 0);

        // Client sends request
        assert(client.send(reinterpret_cast<const uint8_t*>("GetData"), strlen("GetData"))
               == static_cast<int>(strlen("GetData")));

        platform::sleepMs(10);

        // Server receives request
        uint8_t buf[256];
        uint32_t from_id = 0;
        (void)from_id;
        assert(server.serverRecv(buf, sizeof(buf), from_id) == static_cast<int>(strlen("GetData")));
        assert(from_id == 0);

        // Server sends response to the requesting client
        assert(server.serverSend(from_id, reinterpret_cast<const uint8_t*>("DataResult:42"),
                                 strlen("DataResult:42")) == static_cast<int>(strlen("DataResult:42")));

        platform::sleepMs(10);

        // Client receives response
        memset(buf, 0, sizeof(buf));
        assert(client.recv(buf, sizeof(buf)) == static_cast<int>(strlen("DataResult:42")));
        assert(memcmp(buf, "DataResult:42", strlen("DataResult:42")) == 0);

        client.close();
        server.close();
        PASS();
    }

    TEST(multi_client_request_response) {
        std::string shm_name = generateShmName("multi_rr_test");
        ShmTransport server(shm_name, true);

        ShmTransport client0(shm_name, false);
        client0.connect("", 0);
        ShmTransport client1(shm_name, false);
        client1.connect("", 0);

        // Both clients send requests
        const char* req0 = "from_client_0";
        const char* req1 = "from_client_1";
        client0.send(reinterpret_cast<const uint8_t*>(req0), strlen(req0));
        client1.send(reinterpret_cast<const uint8_t*>(req1), strlen(req1));

        platform::sleepMs(10);

        // Server receives both requests
        uint8_t buf[256];
        uint32_t from_id = 0;
        (void)from_id;

        assert(server.serverRecv(buf, sizeof(buf), from_id) == static_cast<int>(strlen(req0)));
        assert(from_id == 0);
        assert(memcmp(buf, req0, strlen(req0)) == 0);

        assert(server.serverRecv(buf, sizeof(buf), from_id) == static_cast<int>(strlen(req1)));
        assert(from_id == 1);
        assert(memcmp(buf, req1, strlen(req1)) == 0);

        // Server sends different responses to each client
        const char* resp0 = "reply_for_0";
        const char* resp1 = "reply_for_1";
        server.serverSend(0, reinterpret_cast<const uint8_t*>(resp0), strlen(resp0));
        server.serverSend(1, reinterpret_cast<const uint8_t*>(resp1), strlen(resp1));

        platform::sleepMs(10);

        // Each client receives its own response
        memset(buf, 0, sizeof(buf));
        assert(client0.recv(buf, sizeof(buf)) == static_cast<int>(strlen(resp0)));
        assert(memcmp(buf, resp0, strlen(resp0)) == 0);

        memset(buf, 0, sizeof(buf));
        assert(client1.recv(buf, sizeof(buf)) == static_cast<int>(strlen(resp1)));
        assert(memcmp(buf, resp1, strlen(resp1)) == 0);

        client0.close();
        client1.close();
        server.close();
        PASS();
    }

    TEST(server_broadcast) {
        std::string shm_name = generateShmName("broadcast_test");
        ShmTransport server(shm_name, true);

        ShmTransport client0(shm_name, false);
        client0.connect("", 0);
        ShmTransport client1(shm_name, false);
        client1.connect("", 0);

        // Server broadcasts to all clients
        assert(server.serverBroadcast(
            reinterpret_cast<const uint8_t*>("broadcast_data"), strlen("broadcast_data")) == 2);

        platform::sleepMs(10);

        // Both clients receive the broadcast
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        assert(client0.recv(buf, sizeof(buf)) == static_cast<int>(strlen("broadcast_data")));
        assert(memcmp(buf, "broadcast_data", strlen("broadcast_data")) == 0);

        memset(buf, 0, sizeof(buf));
        assert(client1.recv(buf, sizeof(buf)) == static_cast<int>(strlen("broadcast_data")));
        assert(memcmp(buf, "broadcast_data", strlen("broadcast_data")) == 0);

        client0.close();
        client1.close();
        server.close();
        PASS();
    }

    TEST(large_data_transfer) {
        std::string shm_name = generateShmName("large_test");
        ShmTransport server(shm_name, true);

        ShmTransport client(shm_name, false);
        client.connect("", 0);

        // Create a 64KB payload
        const size_t data_size = 64 * 1024;
        std::vector<uint8_t> send_data(data_size);
        for (size_t i = 0; i < data_size; i++) {
            send_data[i] = static_cast<uint8_t>(i & 0xFF);
        }

        // Client -> Server
        assert(client.send(send_data.data(), data_size) == static_cast<int>(data_size));

        platform::sleepMs(20);

        std::vector<uint8_t> recv_data(data_size);
        uint32_t from_id = 0;
        (void)from_id;
        assert(server.serverRecv(recv_data.data(), data_size, from_id) == static_cast<int>(data_size));
        assert(from_id == 0);
        assert(memcmp(send_data.data(), recv_data.data(), data_size) == 0);

        // Server -> Client
        for (size_t i = 0; i < data_size; i++) {
            send_data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
        }
        assert(server.serverSend(0, send_data.data(), data_size) == static_cast<int>(data_size));

        platform::sleepMs(20);

        memset(recv_data.data(), 0, data_size);
        assert(client.recv(recv_data.data(), data_size) == static_cast<int>(data_size));
        assert(memcmp(send_data.data(), recv_data.data(), data_size) == 0);

        client.close();
        server.close();
        PASS();
    }

    TEST(recv_returns_zero_when_no_data) {
        std::string shm_name = generateShmName("nodata_test");
        ShmTransport server(shm_name, true);

        ShmTransport client(shm_name, false);
        client.connect("", 0);

        // No data sent, recv should return 0
        uint8_t recv_buf[64];
        uint32_t from_id = 0;
        (void)recv_buf;
        (void)from_id;
        assert(server.serverRecv(recv_buf, sizeof(recv_buf), from_id) == 0);
        assert(client.recv(recv_buf, sizeof(recv_buf)) == 0);

        client.close();
        server.close();
        PASS();
    }

    platform::netCleanup();

    printf("\nAll shared memory transport tests passed!\n");
    return 0;
}
