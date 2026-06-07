#include <gtest/gtest.h>
#include "transport/shm_transport.h"
#include "platform/platform.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#ifdef _WIN32
// SHM transport test requires UDS (Unix Domain Sockets) which
// are not available on Windows. The test is disabled.
int main() { return 0; }
#else
#include <poll.h>

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
                struct pollfd pfd;
                pfd.fd = listen_fd;
                pfd.events = POLLIN;
                int ret = ::poll(&pfd, 1, 10);
                if (ret > 0 && (pfd.revents & POLLIN)) {
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
    EXPECT_EQ(calculateShmSize(1024, 512, 4),
              sizeof(ShmControlBlock)
              + sizeof(ShmRingHeader) + 1024
              + (sizeof(ShmRingHeader) + 512) * 4);
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
    EXPECT_EQ(client.clientId(), 0u);
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
    EXPECT_EQ(client0.clientId(), 0u);

    ShmTransport client1(shm_name, false);
    ASSERT_EQ(client1.connect("", 0), 0);
    EXPECT_EQ(client1.clientId(), 1u);

    ShmTransport client2(shm_name, false);
    ASSERT_EQ(client2.connect("", 0), 0);
    EXPECT_EQ(client2.clientId(), 2u);

    EXPECT_EQ(server.clientCount(), 3);
    EXPECT_EQ(server.responseSlotsInUse(), 3);

    client0.close();
    EXPECT_EQ(server.clientCount(), 2);
    EXPECT_EQ(server.responseSlotsInUse(), 2);

    ShmTransport client_reuse(shm_name, false);
    ASSERT_EQ(client_reuse.connect("", 0), 0);
    EXPECT_EQ(client_reuse.clientId(), 0u);

    client1.close();
    client2.close();
    client_reuse.close();
    server.close();
}

TEST_F(ShmTransportTest, ActiveResponseArenaStats) {
    std::string shm_name = generateShmName("arena_stats_test");
    ShmTransport server(shm_name, true);
    UdsHandlerThread uds_handler(server);

    EXPECT_GT(server.totalResponseArenaSize(), 0u);
    EXPECT_EQ(server.activeResponseArenaSize(), 0u);
    EXPECT_EQ(server.responseSlotsInUse(), 0);

    ShmTransport client0(shm_name, false);
    ASSERT_EQ(client0.connect("", 0), 0);
    const size_t per_client_block_size = server.activeResponseArenaSize();
    EXPECT_GT(server.activeResponseArenaSize(), 0u);
    EXPECT_EQ(server.responseSlotsInUse(), 1);

    ShmTransport client1(shm_name, false);
    ASSERT_EQ(client1.connect("", 0), 0);
    EXPECT_EQ(server.responseSlotsInUse(), 2);
    EXPECT_EQ(server.activeResponseArenaSize(), per_client_block_size * 2);

    std::vector<uint32_t> ids = server.activeClientIds();
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 0u);
    EXPECT_EQ(ids[1], 1u);

    client0.close();
    EXPECT_EQ(server.responseSlotsInUse(), 1);
    EXPECT_EQ(server.activeResponseArenaSize(), per_client_block_size);

    client1.close();
    EXPECT_EQ(server.responseSlotsInUse(), 0);
    EXPECT_EQ(server.activeResponseArenaSize(), 0u);

    server.close();
}

TEST_F(ShmTransportTest, ClientSendServerRecv) {
    std::string shm_name = generateShmName("c2s_test");
    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    UdsHandlerThread uds_handler(server);
    ShmTransport client(shm_name, false);
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
    EXPECT_EQ(from_id, 0u);
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    ASSERT_EQ(server.serverSend(0, send_data.data(), data_size), static_cast<int>(data_size));
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
#endif // _WIN32
