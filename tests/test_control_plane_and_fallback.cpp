#include <omnibinder/omnibinder.h>
#include "transport/tcp_transport.h"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>

using namespace omnibinder;

#define TEST(name) printf("  TEST %-45s ", #name); fflush(stdout);
#define PASS() printf("PASS\n"); fflush(stdout);

static const uint16_t SM_PORT = 19912;
static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t IFACE_ID = fnv1a_32("OwnedService");

static bool recvMessage(TcpTransport& transport, Message& msg, int timeout_ms);
static bool sendMessage(TcpTransport& transport, const Message& msg);

class OwnedService : public Service {
public:
    OwnedService() : Service("OwnedService"), invoke_count_(0) {
        iface_.interface_id = IFACE_ID;
        iface_.name = "OwnedService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
    }
    const char* serviceName() const override { return "OwnedService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
    int invokeCount() const { return invoke_count_; }
protected:
    void onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ADD) {
            Buffer req(request.data(), request.size());
            int32_t a = req.readInt32();
            int32_t b = req.readInt32();
            invoke_count_++;
            response.writeInt32(a + b);
        }
    }
private:
    InterfaceInfo iface_;
    int invoke_count_;
};

struct OwnedServerCtx {
    OmniRuntime runtime;
    OwnedService service;
    volatile bool registered;
    volatile bool should_stop;
    OwnedServerCtx() : registered(false), should_stop(false) {}
};

[[maybe_unused]] static void* ownedServerThread(void* arg) {
    OwnedServerCtx* ctx = static_cast<OwnedServerCtx*>(arg);
    if (ctx->runtime.init("127.0.0.1", SM_PORT) != 0) {
        return NULL;
    }
    if (ctx->runtime.registerService(&ctx->service) != 0) {
        ctx->runtime.stop();
        return NULL;
    }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(20);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

struct RawTcpServiceCtx {
    TcpTransportServer server;
    uint16_t port;
    volatile bool ready;
    volatile bool done;
    RawTcpServiceCtx() : port(0), ready(false), done(false) {}
};

[[maybe_unused]] static void* rawTcpServiceThread(void* arg) {
    RawTcpServiceCtx* ctx = static_cast<RawTcpServiceCtx*>(arg);
    int port = ctx->server.listen("127.0.0.1", 0);
    if (port <= 0) {
        return NULL;
    }
    ctx->port = static_cast<uint16_t>(port);
    ctx->ready = true;

    ITransport* accepted = NULL;
    for (int i = 0; i < 100 && !accepted; ++i) {
        accepted = ctx->server.accept();
        if (!accepted) {
            usleep(50000);
        }
    }
    if (!accepted) {
        ctx->done = true;
        return NULL;
    }

    uint8_t buf[4096];
    for (int i = 0; i < 100; ++i) {
        int ret = accepted->recv(buf, sizeof(buf));
        if (ret > 0) {
            Buffer input(buf, static_cast<size_t>(ret));
            MessageHeader hdr;
            assert(Message::parseHeader(input.data(), input.size(), hdr));
            Message msg;
            msg.header = hdr;
            if (hdr.length > 0) {
                msg.payload.assign(input.data() + MESSAGE_HEADER_SIZE, hdr.length);
            }
            Buffer req(msg.payload.data(), msg.payload.size());
            uint32_t interface_id = req.readUint32();
            uint32_t method_id = req.readUint32();
            uint32_t payload_len = req.readUint32();
            assert(interface_id == IFACE_ID);
            assert(method_id == METHOD_ADD);
            (void)interface_id;
            (void)method_id;
            Buffer payload;
            if (payload_len > 0) {
                payload.writeRaw(req.data() + req.readPosition(), payload_len);
            }
            Buffer p(payload.data(), payload.size());
            int32_t result = p.readInt32() + p.readInt32();

            Message reply(MessageType::MSG_INVOKE_REPLY, msg.getSequence());
            reply.payload.writeInt32(0);
            reply.payload.writeUint32(sizeof(int32_t));
            reply.payload.writeInt32(result);

            Buffer out;
            reply.serialize(out);
            assert(accepted->send(out.data(), out.size()) == static_cast<int>(out.size()));
            break;
        }
        usleep(20000);
    }

    accepted->close();
    delete accepted;
    ctx->server.close();
    ctx->done = true;
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
        const char* paths[] = {
            "./target/bin/service_manager",
            "./build/target/bin/service_manager",
            "./service_manager/service_manager",
            NULL
        };
        for (int i = 0; paths[i]; ++i) {
            execl(paths[i], "service_manager", "--port", port_str, "--log-level", "3", (char*)NULL);
        }
        _exit(1);
    }
    return pid;
}

static void stopSM(pid_t pid) {
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

[[maybe_unused]] static bool waitSM(uint16_t port, int retries) {
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

[[maybe_unused]] static bool connectTcp(TcpTransport& transport, const std::string& host, uint16_t port) {
    int ret = transport.connect(host, port);
    if (ret < 0) {
        return false;
    }
    if (ret == 1) {
        for (int i = 0; i < 100; ++i) {
            transport.checkConnectComplete();
            if (transport.state() == ConnectionState::CONNECTED) {
                return true;
            }
            usleep(10000);
        }
        return false;
    }
    return transport.state() == ConnectionState::CONNECTED;
}

[[maybe_unused]] static bool recvMessage(TcpTransport& transport, Message& msg, int timeout_ms) {
    uint8_t buf[4096];
    int loops = timeout_ms / 20;
    for (int i = 0; i < loops; ++i) {
        int ret = transport.recv(buf, sizeof(buf));
        if (ret > 0) {
            Buffer input(buf, static_cast<size_t>(ret));
            MessageHeader hdr;
            if (!Message::parseHeader(input.data(), input.size(), hdr)) {
                return false;
            }
            msg.header = hdr;
            if (hdr.length > 0) {
                msg.payload.assign(input.data() + MESSAGE_HEADER_SIZE, hdr.length);
            }
            return true;
        }
        usleep(20000);
    }
    return false;
}

[[maybe_unused]] static bool recvFullMessage(TcpTransport& transport, Message& msg, int timeout_ms) {
    Buffer input;
    uint8_t buf[2048];
    int loops = timeout_ms / 20;
    for (int i = 0; i < loops; ++i) {
        int ret = transport.recv(buf, sizeof(buf));
        if (ret > 0) {
            input.writeRaw(buf, static_cast<size_t>(ret));
            if (input.size() >= MESSAGE_HEADER_SIZE) {
                MessageHeader hdr;
                if (!Message::parseHeader(input.data(), input.size(), hdr)) {
                    return false;
                }
                size_t total = MESSAGE_HEADER_SIZE + hdr.length;
                if (input.size() >= total) {
                    msg.header = hdr;
                    if (hdr.length > 0) {
                        msg.payload.assign(input.data() + MESSAGE_HEADER_SIZE, hdr.length);
                    }
                    return true;
                }
            }
        }
        usleep(20000);
    }
    return false;
}

[[maybe_unused]] static bool registerFakeService(TcpTransport& transport, uint32_t seq,
                                                 const std::string& name, uint16_t port,
                                                 const std::string& host_id) {
    Message msg(MessageType::MSG_REGISTER, seq);
    ServiceInfo info;
    info.name = name;
    info.host = "127.0.0.1";
    info.port = port;
    info.host_id = host_id;
    InterfaceInfo iface;
    iface.interface_id = fnv1a_32(name);
    iface.name = name;
    iface.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
    info.interfaces.push_back(iface);
    serializeServiceInfo(info, msg.payload);

    if (!sendMessage(transport, msg)) {
        return false;
    }

    Message reply;
    if (!recvMessage(transport, reply, 2000)) {
        return false;
    }
    if (reply.getType() != MessageType::MSG_REGISTER_REPLY) {
        return false;
    }
    Buffer payload(reply.payload.data(), reply.payload.size());
    return payload.readBool();
}

[[maybe_unused]] static bool sendMessage(TcpTransport& transport, const Message& msg) {
    Buffer out;
    msg.serialize(out);
    return transport.send(out.data(), out.size()) == static_cast<int>(out.size());
}

[[maybe_unused]] static bool invokeOwnedService(OmniRuntime& runtime, int32_t a, int32_t b, int32_t& sum) {
    Buffer req;
    req.writeInt32(a);
    req.writeInt32(b);
    Buffer resp;
    int ret = runtime.invoke("OwnedService", IFACE_ID, METHOD_ADD, req, resp, 5000);
    if (ret != 0 || resp.size() < sizeof(int32_t)) {
        return false;
    }
    sum = resp.readInt32();
    return true;
}

int main() {
    printf("=== Control Plane And Fallback Tests ===\n\n");

    pid_t sm_pid = startSM(SM_PORT);
    assert(sm_pid > 0);
    bool sm_ready = waitSM(SM_PORT, 30);
    assert(sm_ready);
    (void)sm_ready;

    OwnedServerCtx owned_ctx;
    pthread_t owned_tid = 0;
    int pthread_ret = pthread_create(&owned_tid, NULL, ownedServerThread, &owned_ctx);
    assert(pthread_ret == 0);
    (void)pthread_ret;
    for (int i = 0; i < 50 && !owned_ctx.registered; ++i) usleep(100000);
    assert(owned_ctx.registered);

    TEST(illegal_unregister_rejected) {
        TcpTransport rogue;
        assert(connectTcp(rogue, "127.0.0.1", SM_PORT));
        Message msg(MessageType::MSG_UNREGISTER, 1001);
        msg.payload.writeString("OwnedService");
        bool send_ok = sendMessage(rogue, msg);
        assert(send_ok);
        (void)send_ok;
        Message reply;
        bool recv_ok = recvMessage(rogue, reply, 2000);
        assert(recv_ok);
        (void)recv_ok;
        assert(reply.getType() == MessageType::MSG_UNREGISTER_REPLY);
        Buffer payload(reply.payload.data(), reply.payload.size());
        assert(payload.readBool() == false);

        OmniRuntime checker;
        assert(checker.init("127.0.0.1", SM_PORT) == 0);
        ServiceInfo info;
        assert(checker.lookupService("OwnedService", info) == 0);
        checker.stop();
        rogue.close();
        PASS();
    }

    TEST(malformed_lookup_and_subscribe_topic_do_not_crash_sm) {
        TcpTransport rogue;
        assert(connectTcp(rogue, "127.0.0.1", SM_PORT));

        Message bad_lookup(MessageType::MSG_LOOKUP, 1004);
        bad_lookup.payload.writeUint32(0x12345678u);
        bool send_ok = sendMessage(rogue, bad_lookup);
        assert(send_ok);
        Message lookup_reply;
        bool recv_ok = recvMessage(rogue, lookup_reply, 2000);
        assert(recv_ok);
        assert(lookup_reply.getType() == MessageType::MSG_LOOKUP_REPLY);
        Buffer lookup_payload(lookup_reply.payload.data(), lookup_reply.payload.size());
        assert(lookup_payload.readBool() == false);

        Message bad_subscribe(MessageType::MSG_SUBSCRIBE_TOPIC, 1005);
        bad_subscribe.payload.writeUint16(7);
        send_ok = sendMessage(rogue, bad_subscribe);
        assert(send_ok);
        Message subscribe_reply;
        recv_ok = recvMessage(rogue, subscribe_reply, 2000);
        assert(recv_ok);
        assert(subscribe_reply.getType() == MessageType::MSG_SUBSCRIBE_TOPIC_REPLY);
        Buffer subscribe_payload(subscribe_reply.payload.data(), subscribe_reply.payload.size());
        assert(subscribe_payload.readBool() == false);

        OmniRuntime checker;
        assert(checker.init("127.0.0.1", SM_PORT) == 0);
        ServiceInfo info;
        assert(checker.lookupService("OwnedService", info) == 0);
        checker.stop();

        rogue.close();
        PASS();
    }

    TEST(illegal_publish_topic_rejected) {
        TcpTransport rogue;
        assert(connectTcp(rogue, "127.0.0.1", SM_PORT));
        Message msg(MessageType::MSG_PUBLISH_TOPIC, 1002);
        msg.payload.writeString("owned/topic");
        ServiceInfo pub_info;
        pub_info.name = "OwnedService";
        pub_info.host = "127.0.0.1";
        pub_info.port = owned_ctx.service.port();
        pub_info.host_id = owned_ctx.runtime.hostId();
        serializeServiceInfo(pub_info, msg.payload);
        bool send_ok = sendMessage(rogue, msg);
        assert(send_ok);
        (void)send_ok;
        Message reply;
        bool recv_ok = recvMessage(rogue, reply, 2000);
        assert(recv_ok);
        (void)recv_ok;
        assert(reply.getType() == MessageType::MSG_PUBLISH_TOPIC_REPLY);
        Buffer payload(reply.payload.data(), reply.payload.size());
        assert(payload.readBool() == false);
        rogue.close();
        PASS();
    }

    TEST(shm_failure_falls_back_to_tcp) {
        RawTcpServiceCtx raw_ctx;
        pthread_t raw_tid = 0;
        int raw_thread_ret = pthread_create(&raw_tid, NULL, rawTcpServiceThread, &raw_ctx);
        assert(raw_thread_ret == 0);
        (void)raw_thread_ret;
        for (int i = 0; i < 50 && !raw_ctx.ready; ++i) usleep(100000);
        assert(raw_ctx.ready);

        TcpTransport control;
        assert(connectTcp(control, "127.0.0.1", SM_PORT));
        OmniRuntime probe;
        assert(probe.init("127.0.0.1", SM_PORT) == 0);

        Message msg(MessageType::MSG_REGISTER, 1003);
        ServiceInfo info;
        info.name = "FallbackService";
        info.host = "127.0.0.1";
        info.port = raw_ctx.port;
        info.host_id = probe.hostId();
        info.shm_name = "/binder_missing_fallback_service";
        info.shm_config = ShmConfig(4 * 1024, 4 * 1024);
        InterfaceInfo iface;
        iface.interface_id = IFACE_ID;
        iface.name = "FallbackService";
        iface.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        info.interfaces.push_back(iface);
        serializeServiceInfo(info, msg.payload);
        bool send_ok = sendMessage(control, msg);
        assert(send_ok);
        (void)send_ok;
        Message reply;
        bool recv_ok = recvMessage(control, reply, 2000);
        assert(recv_ok);
        (void)recv_ok;
        assert(reply.getType() == MessageType::MSG_REGISTER_REPLY);

        Buffer req;
        req.writeInt32(5);
        req.writeInt32(9);
        Buffer resp;
        int ret = probe.invoke("FallbackService", IFACE_ID, METHOD_ADD, req, resp, 5000);
        assert(ret == 0);
        (void)ret;
        assert(resp.readInt32() == 14);

        control.close();
        probe.stop();
        pthread_join(raw_tid, NULL);
        PASS();
    }

    TEST(malformed_invoke_payload_returns_deserialize_without_crash) {
        TcpTransport rogue;
        assert(connectTcp(rogue, "127.0.0.1", owned_ctx.service.port()));

        int before_count = owned_ctx.service.invokeCount();

        Message short_header(MessageType::MSG_INVOKE, 2001);
        short_header.payload.writeUint32(IFACE_ID);
        short_header.payload.writeUint32(METHOD_ADD);
        bool send_ok = sendMessage(rogue, short_header);
        assert(send_ok);
        (void)send_ok;

        Message reply;
        bool recv_ok = recvFullMessage(rogue, reply, 2000);
        assert(recv_ok);
        (void)recv_ok;
        assert(reply.getType() == MessageType::MSG_INVOKE_REPLY);
        Buffer payload(reply.payload.data(), reply.payload.size());
        assert(payload.readInt32() == static_cast<int32_t>(ErrorCode::ERR_DESERIALIZE));
        assert(payload.readUint32() == 0);
        assert(owned_ctx.service.invokeCount() == before_count);

        Message short_body(MessageType::MSG_INVOKE, 2002);
        short_body.payload.writeUint32(IFACE_ID);
        short_body.payload.writeUint32(METHOD_ADD);
        short_body.payload.writeUint32(sizeof(int32_t));
        short_body.payload.writeInt32(7);
        send_ok = sendMessage(rogue, short_body);
        assert(send_ok);
        Message short_body_reply;
        recv_ok = recvFullMessage(rogue, short_body_reply, 2000);
        assert(recv_ok);
        assert(short_body_reply.getType() == MessageType::MSG_INVOKE_REPLY);
        Buffer short_body_payload(short_body_reply.payload.data(), short_body_reply.payload.size());
        assert(short_body_payload.readInt32() == static_cast<int32_t>(ErrorCode::ERR_DESERIALIZE));
        assert(short_body_payload.readUint32() == 0);
        assert(owned_ctx.service.invokeCount() == before_count);

        OmniRuntime checker;
        assert(checker.init("127.0.0.1", SM_PORT) == 0);
        int32_t sum = 0;
        assert(invokeOwnedService(checker, 20, 22, sum));
        assert(sum == 42);
        checker.stop();

        rogue.close();
        PASS();
    }

    TEST(malformed_subscribe_broadcast_does_not_crash_service) {
        TcpTransport rogue;
        assert(connectTcp(rogue, "127.0.0.1", owned_ctx.service.port()));

        Message bad_subscribe(MessageType::MSG_SUBSCRIBE_BROADCAST, 2003);
        bad_subscribe.payload.writeUint32(0x99887766u);
        bool send_ok = sendMessage(rogue, bad_subscribe);
        assert(send_ok);

        Message bad_broadcast(MessageType::MSG_BROADCAST, 2004);
        bad_broadcast.payload.writeUint32(0x99887766u);
        send_ok = sendMessage(rogue, bad_broadcast);
        assert(send_ok);

        usleep(100000);

        OmniRuntime checker;
        assert(checker.init("127.0.0.1", SM_PORT) == 0);
        int32_t sum = 0;
        assert(invokeOwnedService(checker, 1, 2, sum));
        assert(sum == 3);
        checker.stop();

        rogue.close();
        PASS();
    }

    TEST(list_services_large_reply_handles_partial_send) {
        TcpTransport registrant;
        assert(connectTcp(registrant, "127.0.0.1", SM_PORT));

        OmniRuntime host_probe;
        assert(host_probe.init("127.0.0.1", SM_PORT) == 0);
        std::string host_id = host_probe.hostId();

        for (int i = 0; i < 48; ++i) {
            char name[64];
            snprintf(name, sizeof(name), "BulkService_%02d_partial_send_case", i);
            assert(registerFakeService(registrant, 2000 + static_cast<uint32_t>(i),
                                       name, static_cast<uint16_t>(41000 + i), host_id));
        }

        std::vector<ServiceInfo> services;
        assert(host_probe.listServices(services) == 0);
        assert(services.size() >= 49);

        registrant.close();
        host_probe.stop();
        PASS();
    }

    owned_ctx.should_stop = true;
    pthread_join(owned_tid, NULL);
    stopSM(sm_pid);

    printf("\nAll control plane and fallback tests passed!\n");
    return 0;
}
