#include "transport/tcp_transport.h"
#include "transport/transport_factory.h"
#include "platform/platform.h"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>

using namespace omnibinder;

int main() {
    printf("=== Transport Tests ===\n");
    platform::netInit();
    
    printf("  TEST tcp_echo ... ");
    {
        TcpTransportServer server;
        int port = server.listen("127.0.0.1", 0);
        assert(port > 0);
        
        TcpTransport client;
        int ret = client.connect("127.0.0.1", static_cast<uint16_t>(port));
        assert(ret >= 0);
        
        if (ret == 1) {
            for (int i = 0; i < 50; ++i) {
                client.checkConnectComplete();
                if (client.state() == ConnectionState::CONNECTED) break;
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
            }
        }
        assert(client.state() == ConnectionState::CONNECTED);
        
        ITransport* accepted = server.accept();
        assert(accepted != NULL);
        
        const char* msg = "Hello OmniBinder!";
        int sent = client.send(reinterpret_cast<const uint8_t*>(msg), strlen(msg));
        assert(sent == (int)strlen(msg));
        (void)sent;
        
        // 等待数据到达
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
        
        char buf[256] = {0};
        int recvd = accepted->recv(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        assert(recvd == (int)strlen(msg));
        (void)recvd;
        assert(strcmp(buf, msg) == 0);
        
        accepted->close();
        delete accepted;
        client.close();
        server.close();
    }
    printf("PASS\n");

    printf("  TEST tcp_send_returns_partial_when_peer_not_draining ... ");
    {
        TcpTransportServer server;
        int port = server.listen("127.0.0.1", 0);
        assert(port > 0);

        TcpTransport client;
        int ret = client.connect("127.0.0.1", static_cast<uint16_t>(port));
        assert(ret >= 0);
        if (ret == 1) {
            for (int i = 0; i < 50; ++i) {
                client.checkConnectComplete();
                if (client.state() == ConnectionState::CONNECTED) break;
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
            }
        }
        assert(client.state() == ConnectionState::CONNECTED);

        ITransport* accepted = server.accept();
        assert(accepted != NULL);

        int sndbuf = 4096;
        assert(setsockopt(client.fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
        (void)sndbuf;

        std::vector<uint8_t> payload(4 * 1024 * 1024, 0x5A);
        int sent = -1;
        for (int i = 0; i < 20; ++i) {
            sent = client.send(payload.data(), payload.size());
            assert(sent >= 0);
            if (sent > 0 && sent < static_cast<int>(payload.size())) {
                break;
            }
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        }

        assert(sent > 0);
        assert(sent < static_cast<int>(payload.size()));

        accepted->close();
        delete accepted;
        client.close();
        server.close();
    }
    printf("PASS\n");

    printf("  TEST transport_policy_same_machine_prefers_shm ... ");
    {
        TransportType type = TransportFactory::selectPreferredTransport(
            "host-A", "host-A", "/binder_test");
        assert(type == TransportType::SHM);
        (void)type;
    }
    printf("PASS\n");

    printf("  TEST transport_policy_cross_machine_uses_tcp ... ");
    {
        TransportType type = TransportFactory::selectPreferredTransport(
            "host-A", "host-B", "/binder_test");
        assert(type == TransportType::TCP);
        (void)type;
    }
    printf("PASS\n");

    printf("  TEST transport_policy_empty_host_ids_uses_tcp ... ");
    {
        TransportType type = TransportFactory::selectPreferredTransport(
            "", "", "/binder_test");
        assert(type == TransportType::TCP);
        (void)type;
    }
    printf("PASS\n");
     
    printf("\nAll transport tests passed!\n");
    platform::netCleanup();
    return 0;
}
