#include <gtest/gtest.h>
#include "transport/shm_transport.h"
#include "platform/platform.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include <memory>

using namespace omnibinder;

// RAII helper: runs a background thread that accepts UDS connections
// on the server's listen fd and calls onUdsClientConnect() for each.
// This is needed because client connect() triggers a UDS handshake
// that requires the server side to accept and send eventfds.
class UdsHandlerThread {
public:
    explicit UdsHandlerThread(ShmTransport& server)
        : stop_(false)
    {
        thread_ = std::thread([this, &server]() {
            int listen_fd = server.udsListenFd();
            while (!stop_.load()) {
                if (platform::waitFdReadable(listen_fd, 10)) {
                    server.onUdsClientConnect();
                }
            }
        });
    }

    ~UdsHandlerThread() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::atomic<bool> stop_;
    std::thread thread_;
};

class ShmTransportTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { platform::netInit(); }
    static void TearDownTestSuite() { platform::netCleanup(); }
};

TEST_F(ShmTransportTest, CalculateShmSize) {
    EXPECT_GE(calculateShmSize(1024, 512),
              sizeof(ShmControlBlock)
              + sizeof(ShmRingHeader) + 1024
              + sizeof(ShmRingHeader) + 512);
    EXPECT_EQ(sizeof(ShmControlBlock) % alignof(ShmRingHeader), 0u);
    EXPECT_EQ(sizeof(ShmRingHeader) % alignof(ShmRingHeader), 0u);
}

TEST_F(ShmTransportTest, GenerateShmName) {
    EXPECT_EQ(generateShmName("myservice"), "/binder_myservice");
    EXPECT_EQ(generateShmName("test"), "/binder_test");
}

TEST_F(ShmTransportTest, ServerCreate) {
    std::string shm_name = generateShmName("srv_create_test");
    ShmTransport server(shm_name, true);
    EXPECT_EQ(server.state(), ConnectionState::CONNECTED);
    EXPECT_TRUE(server.isServer());
    EXPECT_EQ(server.type(), TransportType::SHM);
    EXPECT_GE(server.fd(), 0);
    EXPECT_EQ(server.clientCount(), 0);
    server.close();
}

TEST_F(ShmTransportTest, ClientConnect) {
    std::string shm_name = generateShmName("client_conn_test");
    ShmTransport server(shm_name, true);
    ASSERT_EQ(server.state(), ConnectionState::CONNECTED);
    UdsHandlerThread uds_handler(server);

    ShmTransport client(shm_name, false);
    EXPECT_EQ(client.state(), ConnectionState::DISCONNECTED);

    ASSERT_EQ(client.connect("", 0), 0);
    EXPECT_EQ(client.state(), ConnectionState::CONNECTED);

    // Wait for UdsHandlerThread to process the handshake on server side
    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) {
        platform::sleepMs(1);
    }
    EXPECT_EQ(server.clientCount(), 1);

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, MultipleClientsConnect) {
    std::string shm_name = generateShmName("multi_client_test");
    ShmTransport server(shm_name, true);
    UdsHandlerThread uds_handler(server);

    ShmTransport client0(shm_name, false);
    ASSERT_EQ(client0.connect("", 0), 0);

    ShmTransport client1(shm_name, false);
    ASSERT_EQ(client1.connect("", 0), 0);

    ShmTransport client2(shm_name, false);
    ASSERT_EQ(client2.connect("", 0), 0);

    // Wait for UdsHandlerThread to process all handshakes
    for (int i = 0; i < 50 && server.clientCount() < 3u; ++i) {
        platform::sleepMs(1);
    }
    EXPECT_EQ(server.clientCount(), 3u);

    client0.close();
    // Server detects disconnection asynchronously

    client1.close();
    client2.close();
    server.close();
}

TEST_F(ShmTransportTest, ActiveResponseArenaStats) {
    std::string shm_name = generateShmName("arena_stats_test");
    ShmTransport server(shm_name, true);
    UdsHandlerThread uds_handler(server);

    EXPECT_GE(server.clientCount(), 0u);

    ShmTransport client0(shm_name, false);
    ASSERT_EQ(client0.connect("", 0), 0);
    // Wait for UDS handshake on server side
    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) platform::sleepMs(1);
    EXPECT_EQ(server.clientCount(), 1u);

    ShmTransport client1(shm_name, false);
    ASSERT_EQ(client1.connect("", 0), 0);
    for (int i = 0; i < 50 && server.clientCount() < 2u; ++i) platform::sleepMs(1);
    EXPECT_EQ(server.clientCount(), 2u);

    client0.close();
    client1.close();
    server.close();
}

TEST_F(ShmTransportTest, ClientSendServerRecv) {
    std::string shm_name = generateShmName("c2s_test");
    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    UdsHandlerThread uds_handler(server);
    ShmTransport client(shm_name, false, 128 * 1024, 128 * 1024);
    client.connect("", 0);

    const size_t data_size = 64 * 1024;
    std::vector<uint8_t> send_data(data_size);
    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    ASSERT_EQ(client.send(send_data.data(), data_size), static_cast<int>(data_size));
    platform::sleepMs(20);

    std::vector<uint8_t> recv_data(data_size);
    uint32_t from_id = 0;
    ASSERT_EQ(server.serverRecv(recv_data.data(), data_size, from_id), static_cast<int>(data_size));
    EXPECT_GT(from_id, 0u);
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    ASSERT_EQ(server.serverSend(from_id, send_data.data(), data_size), static_cast<int>(data_size));
    platform::sleepMs(20);

    memset(recv_data.data(), 0, data_size);
    ASSERT_EQ(client.recv(recv_data.data(), data_size), static_cast<int>(data_size));
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, RecvReturnsZeroWhenNoData) {
    std::string shm_name = generateShmName("nodata_test");
    ShmTransport server(shm_name, true);
    UdsHandlerThread uds_handler(server);
    ShmTransport client(shm_name, false);
    client.connect("", 0);

    uint8_t recv_buf[64];
    uint32_t from_id = 0;
    EXPECT_EQ(server.serverRecv(recv_buf, sizeof(recv_buf), from_id), 0);
    EXPECT_EQ(client.recv(recv_buf, sizeof(recv_buf)), 0);

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, ConcurrentMultiClientSendRecv) {
    const int NUM_CLIENTS = 4;
    const size_t data_size = 4096;
    std::string shm_name = generateShmName("concurrent_test");

    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    UdsHandlerThread uds_handler(server);

    // Create and connect all clients
    std::vector<std::unique_ptr<ShmTransport>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto c = std::unique_ptr<ShmTransport>(new ShmTransport(shm_name, false, 128 * 1024, 128 * 1024));
        ASSERT_EQ(c->connect("", 0), 0);
        clients.push_back(std::move(c));
    }
    // Wait for UdsHandlerThread to process all handshakes
    for (int i = 0; i < 100 && server.clientCount() < static_cast<uint32_t>(NUM_CLIENTS); ++i) {
        platform::sleepMs(1);
    }
    EXPECT_EQ(server.clientCount(), static_cast<uint32_t>(NUM_CLIENTS));

    // All clients send simultaneously
    std::vector<std::vector<uint8_t>> send_data(NUM_CLIENTS);
    std::vector<std::thread> threads;
    std::atomic<int> send_ok{0};

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        send_data[i].resize(data_size);
        for (size_t j = 0; j < data_size; ++j) {
            send_data[i][j] = static_cast<uint8_t>((i * 256 + j) & 0xFF);
        }
        threads.emplace_back([&, i]() {
            ASSERT_EQ(clients[i]->send(send_data[i].data(), data_size),
                      static_cast<int>(data_size));
            send_ok++;
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(send_ok.load(), NUM_CLIENTS);

    // Server receives all
    platform::sleepMs(50);
    int received = 0;
    for (int i = 0; i < NUM_CLIENTS * 10 && received < NUM_CLIENTS; ++i) {
        std::vector<uint8_t> buf(data_size);
        uint32_t from_id = 0;
        int ret = server.serverRecv(buf.data(), data_size, from_id);
        if (ret > 0) {
            EXPECT_GE(from_id, 1u);
            received++;
        }
        platform::sleepMs(5);
    }
    EXPECT_EQ(received, NUM_CLIENTS);

    for (auto& c : clients) c->close();
    server.close();
}

TEST_F(ShmTransportTest, ConcurrentDataIntegrity) {
    const int NUM_CLIENTS = 3;
    const int ROUNDS = 50;
    std::string shm_name = generateShmName("integrity_test");

    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    UdsHandlerThread uds_handler(server);

    // Create clients
    std::vector<std::unique_ptr<ShmTransport>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto c = std::unique_ptr<ShmTransport>(new ShmTransport(shm_name, false, 128 * 1024, 128 * 1024));
        ASSERT_EQ(c->connect("", 0), 0);
        clients.push_back(std::move(c));
    }

    // Multi-round: each client sends, server echoes back
    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            uint32_t marker = static_cast<uint32_t>((round << 16) | (i << 8) | 0xAA);
            std::vector<uint8_t> send_buf(sizeof(marker));
            memcpy(send_buf.data(), &marker, sizeof(marker));

            ASSERT_EQ(clients[i]->send(send_buf.data(), sizeof(marker)),
                      static_cast<int>(sizeof(marker)));
        }

        platform::sleepMs(10);

        // Server recv all and echo back
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            std::vector<uint8_t> recv_buf(64);
            uint32_t from_id = 0;
            int ret = server.serverRecv(recv_buf.data(), recv_buf.size(), from_id);
            ASSERT_GT(ret, 0);
            // Echo back
            ASSERT_EQ(server.serverSend(from_id, recv_buf.data(), static_cast<size_t>(ret)),
                      ret);
        }

        // Clients verify echo
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            std::vector<uint8_t> resp(64);
            int ret = clients[i]->recv(resp.data(), resp.size());
            ASSERT_GT(ret, 0);
            uint32_t echoed = 0;
            memcpy(&echoed, resp.data(), sizeof(echoed));
            uint32_t expected = static_cast<uint32_t>((round << 16) | (i << 8) | 0xAA);
            EXPECT_EQ(echoed, expected) << "round=" << round << " client=" << i;
        }
    }

    for (auto& c : clients) c->close();
    server.close();
}
