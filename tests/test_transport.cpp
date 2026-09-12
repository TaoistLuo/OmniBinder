#include <gtest/gtest.h>
#include "test_common.h"
#include "transport/tcp_transport.h"
#include "transport/transport_selector.h"
#include "platform/platform.h"

using namespace omnibinder;

class TransportTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        platform::netInit();
    }
    static void TearDownTestSuite() {
        platform::netCleanup();
    }
};

TEST_F(TransportTest, TcpEcho) {
    test::TcpTestServer server;
    ASSERT_TRUE(server.start());
    uint16_t port = server.port();
    ASSERT_GT(port, 0);

    TcpClientTransport client;
    int ret = client.connect("127.0.0.1", port);
    ASSERT_GE(ret, 0);

    if (ret == 1) {
        for (int i = 0; i < 50; ++i) {
            client.checkConnectComplete();
            if (client.state() == ConnectionState::CONNECTED) break;
            platform::sleepMs(10);
        }
    }
    ASSERT_EQ(client.state(), ConnectionState::CONNECTED);

    IClientTransport* accepted = server.waitAccept();
    ASSERT_NE(accepted, nullptr);

    const char* msg = "Hello OmniBinder!";
    int sent = client.send(reinterpret_cast<const uint8_t*>(msg), strlen(msg));
    ASSERT_EQ(sent, static_cast<int>(strlen(msg)));

    platform::sleepMs(50);

    char buf[256] = {0};
    int recvd = accepted->recv(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    ASSERT_EQ(recvd, static_cast<int>(strlen(msg)));
    EXPECT_STREQ(buf, msg);

    client.close();
    server.close();
}

#ifndef _WIN32
// 此测试依赖 POSIX socket API 控制发送缓冲区大小
#include <sys/socket.h>

TEST_F(TransportTest, TcpSendReturnsPartialWhenPeerNotDraining) {
    test::TcpTestServer server;
    ASSERT_TRUE(server.start());
    uint16_t port = server.port();
    ASSERT_GT(port, 0);

    TcpClientTransport client;
    int ret = client.connect("127.0.0.1", port);
    ASSERT_GE(ret, 0);
    if (ret == 1) {
        for (int i = 0; i < 50; ++i) {
            client.checkConnectComplete();
            if (client.state() == ConnectionState::CONNECTED) break;
            platform::sleepMs(10);
        }
    }
    ASSERT_EQ(client.state(), ConnectionState::CONNECTED);

    IClientTransport* accepted = server.waitAccept();
    ASSERT_NE(accepted, nullptr);

    int sndbuf = 4096;
    ASSERT_EQ(setsockopt(client.fd(), SOL_SOCKET, SO_SNDBUF,
                         reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf)), 0);

    std::vector<uint8_t> payload(4 * 1024 * 1024, 0x5A);
    int sent = -1;
    for (int i = 0; i < 20; ++i) {
        sent = client.send(payload.data(), payload.size());
        ASSERT_GE(sent, 0);
        if (sent > 0 && sent < static_cast<int>(payload.size())) {
            break;
        }
        platform::sleepMs(10);
    }

    EXPECT_GT(sent, 0);
    EXPECT_LT(sent, static_cast<int>(payload.size()));

    client.close();
    server.close();
}
#endif

TEST_F(TransportTest, TransportPolicySameMachinePrefersShm) {
    EXPECT_EQ(chooseTransportPolicy("host-A", "host-A"),
              TransportSelectionPolicy::PREFER_SHM);
}

TEST_F(TransportTest, TransportPolicyCrossMachineUsesTcp) {
    EXPECT_EQ(chooseTransportPolicy("host-A", "host-B"),
              TransportSelectionPolicy::USE_TCP);
}

TEST_F(TransportTest, TransportPolicyEmptyHostIdsUsesTcp) {
    EXPECT_EQ(chooseTransportPolicy("", ""),
              TransportSelectionPolicy::USE_TCP);
    EXPECT_EQ(chooseTransportPolicy("host-A", ""),
              TransportSelectionPolicy::USE_TCP);
    EXPECT_EQ(chooseTransportPolicy("", "host-A"),
              TransportSelectionPolicy::USE_TCP);
}
