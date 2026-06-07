#include <gtest/gtest.h>
#include "transport/tcp_transport.h"
#include "transport/transport_factory.h"
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
    TcpTransportServer server;
    int port = server.listen("127.0.0.1", 0);
    ASSERT_GT(port, 0);

    TcpTransport client;
    int ret = client.connect("127.0.0.1", static_cast<uint16_t>(port));
    ASSERT_GE(ret, 0);

    if (ret == 1) {
        for (int i = 0; i < 50; ++i) {
            client.checkConnectComplete();
            if (client.state() == ConnectionState::CONNECTED) break;
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        }
    }
    ASSERT_EQ(client.state(), ConnectionState::CONNECTED);

    ITransport* accepted = server.accept();
    ASSERT_NE(accepted, nullptr);

    const char* msg = "Hello OmniBinder!";
    int sent = client.send(reinterpret_cast<const uint8_t*>(msg), strlen(msg));
    ASSERT_EQ(sent, static_cast<int>(strlen(msg)));

    struct timespec ts = {0, 50000000};
    nanosleep(&ts, NULL);

    char buf[256] = {0};
    int recvd = accepted->recv(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    ASSERT_EQ(recvd, static_cast<int>(strlen(msg)));
    EXPECT_STREQ(buf, msg);

    accepted->close();
    delete accepted;
    client.close();
    server.close();
}

TEST_F(TransportTest, TcpSendReturnsPartialWhenPeerNotDraining) {
    TcpTransportServer server;
    int port = server.listen("127.0.0.1", 0);
    ASSERT_GT(port, 0);

    TcpTransport client;
    int ret = client.connect("127.0.0.1", static_cast<uint16_t>(port));
    ASSERT_GE(ret, 0);
    if (ret == 1) {
        for (int i = 0; i < 50; ++i) {
            client.checkConnectComplete();
            if (client.state() == ConnectionState::CONNECTED) break;
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        }
    }
    ASSERT_EQ(client.state(), ConnectionState::CONNECTED);

    ITransport* accepted = server.accept();
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
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }

    EXPECT_GT(sent, 0);
    EXPECT_LT(sent, static_cast<int>(payload.size()));

    accepted->close();
    delete accepted;
    client.close();
    server.close();
}

TEST_F(TransportTest, TransportPolicySameMachinePrefersShm) {
    TransportType type = TransportFactory::selectPreferredTransport(
        "host-A", "host-A", "/binder_test");
    EXPECT_EQ(type, TransportType::SHM);
}

TEST_F(TransportTest, TransportPolicyCrossMachineUsesTcp) {
    TransportType type = TransportFactory::selectPreferredTransport(
        "host-A", "host-B", "/binder_test");
    EXPECT_EQ(type, TransportType::TCP);
}

TEST_F(TransportTest, TransportPolicyEmptyHostIdsUsesTcp) {
    TransportType type = TransportFactory::selectPreferredTransport(
        "", "", "/binder_test");
    EXPECT_EQ(type, TransportType::TCP);
}
