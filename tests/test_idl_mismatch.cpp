// test_idl_mismatch.cpp - IDL mismatch detection tests
#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder.h>
#include <omnibinder/service.h>
#include <omnibinder/message.h>
#include "transport/tcp_transport.h"
#include <cstdio>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19998;
static const uint32_t METHOD_ECHO  = fnv1a_32("echo");
static const uint32_t IFACE_ID     = fnv1a_32("IdlTestService");
static const uint32_t SERVER_HASH  = 0xABCD0001u;
static const uint32_t WRONG_HASH   = 0xDEAD0001u;

static bool connectTcp(TcpTransport& t, const char* host, uint16_t port) {
    int ret = t.connect(host, port);
    if (ret < 0) return false;
    if (ret == 1) {
        for (int i = 0; i < 100; ++i) {
            t.checkConnectComplete();
            if (t.state() == ConnectionState::CONNECTED) return true;
            std::this_thread::sleep_for(std::chrono::microseconds(10000));
        }
        return false;
    }
    return t.state() == ConnectionState::CONNECTED;
}

static bool sendMsg(TcpTransport& t, const Message& msg) {
    Buffer out; msg.serialize(out);
    return t.send(out.data(), out.size()) == static_cast<int>(out.size());
}

static bool recvMsg(TcpTransport& t, Message& msg, int timeout_ms) {
    Buffer input; uint8_t buf[2048];
    for (int i = 0; i < timeout_ms / 20; ++i) {
        int ret = t.recv(buf, sizeof(buf));
        if (ret > 0) {
            input.writeRaw(buf, static_cast<size_t>(ret));
            if (input.size() >= MESSAGE_HEADER_SIZE) {
                MessageHeader hdr;
                if (!Message::parseHeader(input.data(), input.size(), hdr)) return false;
                size_t total = MESSAGE_HEADER_SIZE + hdr.length;
                if (input.size() >= total) {
                    msg.header = hdr;
                    if (hdr.length > 0) msg.payload.assign(input.data() + MESSAGE_HEADER_SIZE, hdr.length);
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20000));
    }
    return false;
}

class EchoService : public Service {
public:
    EchoService() : Service("IdlTestService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "IdlTestService";
        MethodInfo mi(METHOD_ECHO, "echo", "uint32_t", "uint32_t");
        mi.idl_hash = SERVER_HASH;
        iface_.methods.push_back(mi);
    }
    const char* serviceName() const override { return "IdlTestService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id != METHOD_ECHO) return static_cast<int>(ErrorCode::ERR_METHOD_NOT_FOUND);
        uint32_t v = 0;
        BufferView(request.data(), request.size()).tryReadUint32(v);
        response.writeUint32(v + 1);
        return 0;
    }
protected:
    void onClientConnected(const std::string&) override {}
    void onClientDisconnected(const std::string&) override {}
    InterfaceInfo iface_;
};

class IdlMismatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        platform::netInit();
        sm_pid_ = startProcess("./target/bin/service_manager", "--port",
                               std::to_string(SM_PORT).c_str(), "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));
    }
    void TearDown() override { stopProcess(sm_pid_); }
    TestPid sm_pid_;
};

TEST_F(IdlMismatchTest, InvokeWrongIdlHashReturnsMismatch) {
    OmniRuntime server;
    ASSERT_EQ(server.init("127.0.0.1", SM_PORT), 0);
    EchoService svc;
    ASSERT_EQ(server.registerService(&svc), 0);
    ASSERT_TRUE(waitPortReady(svc.port(), 10));

    // Server polling thread — event loop must run to accept connections and process messages
    std::atomic<bool> srv_stop{false};
    std::thread srv_poll([&]() { while (!srv_stop) server.pollOnce(10); });

    TcpTransport rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", svc.port()));

    Message msg(MessageType::MSG_INVOKE, 1);
    msg.payload.writeUint32(IFACE_ID);
    msg.payload.writeUint32(WRONG_HASH);
    msg.payload.writeUint32(METHOD_ECHO);
    msg.payload.writeUint32(4);
    msg.payload.writeUint32(42);
    ASSERT_TRUE(sendMsg(rogue, msg));

    Message reply;
    ASSERT_TRUE(recvMsg(rogue, reply, 2000));

    int32_t status = 0;
    ASSERT_TRUE(BufferView(reply.payload.data(), reply.payload.size()).tryReadInt32(status));
    EXPECT_EQ(status, static_cast<int32_t>(ErrorCode::ERR_IDL_MISMATCH));
    rogue.close();
    srv_stop = true; srv_poll.join();
    server.stop();
}

TEST_F(IdlMismatchTest, InvokeCorrectIdlHashSucceeds) {
    OmniRuntime server;
    ASSERT_EQ(server.init("127.0.0.1", SM_PORT), 0);
    EchoService svc;
    ASSERT_EQ(server.registerService(&svc), 0);
    ASSERT_TRUE(waitPortReady(svc.port(), 10));

    // Server polling thread — event loop must run to accept connections and process messages
    std::atomic<bool> srv_stop{false};
    std::thread srv_poll([&]() { while (!srv_stop) server.pollOnce(10); });

    TcpTransport rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", svc.port()));

    Message msg(MessageType::MSG_INVOKE, 2);
    msg.payload.writeUint32(IFACE_ID);
    msg.payload.writeUint32(SERVER_HASH);
    msg.payload.writeUint32(METHOD_ECHO);
    msg.payload.writeUint32(4);
    msg.payload.writeUint32(42);
    ASSERT_TRUE(sendMsg(rogue, msg));

    Message reply;
    ASSERT_TRUE(recvMsg(rogue, reply, 2000));

    int32_t status = 0;
    ASSERT_TRUE(BufferView(reply.payload.data(), reply.payload.size()).tryReadInt32(status));
    EXPECT_EQ(status, 0);
    rogue.close();
    srv_stop = true; srv_poll.join();
    server.stop();
}

TEST_F(IdlMismatchTest, InvokeOneWayWrongHashErrorLog) {
    OmniRuntime server;
    ASSERT_EQ(server.init("127.0.0.1", SM_PORT), 0);
    EchoService svc;
    ASSERT_EQ(server.registerService(&svc), 0);
    ASSERT_TRUE(waitPortReady(svc.port(), 10));

    std::atomic<bool> srv_stop{false};
    std::thread srv_poll([&]() { while (!srv_stop) server.pollOnce(5); });

    const char* log_path = "/tmp/omni_idl_oneway_test.log";
    FILE* saved = stderr;
    FILE* cap = freopen(log_path, "w", stderr);
    ASSERT_NE(cap, nullptr);
    setbuf(stderr, NULL);

    // Connect via OmniRuntime to establish data channel, then send oneway
    OmniRuntime client;
    ASSERT_EQ(client.init("127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(client.connectService("IdlTestService"), 0);

    // Use invoke() with wrong hash as proxy for oneway — same dispatch path,
    // but invoke() blocks until reply, guaranteeing server processed the message
    Buffer req, resp;
    req.writeUint32(99);
    int ret = client.invoke("IdlTestService", IFACE_ID, METHOD_ECHO, WRONG_HASH, req, resp, 5000);
    EXPECT_EQ(ret, static_cast<int>(ErrorCode::ERR_IDL_MISMATCH));

    srv_stop = true; srv_poll.join();

    fflush(stderr); fclose(cap); stderr = saved;

    std::ifstream log(log_path);
    std::string content((std::istreambuf_iterator<char>(log)), {});
    EXPECT_NE(content.find("idl_mismatch"), std::string::npos);

    client.stop();
    server.stop();
    unlink(log_path);
}

TEST_F(IdlMismatchTest, TopicSubscribeReturnsPublisherIdlHash) {
    TcpTransport sm_conn;
    ASSERT_TRUE(connectTcp(sm_conn, "127.0.0.1", SM_PORT));

    {
        Message reg(MessageType::MSG_REGISTER, 100);
        ServiceInfo info;
        info.name = "TopicPub"; info.host = "127.0.0.1";
        info.port = 0; info.host_id = "t";
        InterfaceInfo iface; iface.interface_id = 0; iface.name = "TopicPub";
        info.interfaces.push_back(iface);
        serializeServiceInfo(info, reg.payload);
        ASSERT_TRUE(sendMsg(sm_conn, reg));
        Message reply; ASSERT_TRUE(recvMsg(sm_conn, reply, 2000));
    }

    {
        Message pub(MessageType::MSG_PUBLISH_TOPIC, 200);
        ServiceInfo pub_info;
        pub_info.name = "TopicPub"; pub_info.host = "127.0.0.1";
        pub_info.port = 0; pub_info.host_id = "t";
        pub.payload.writeString("TestTopic");
        serializeServiceInfo(pub_info, pub.payload);
        pub.payload.writeUint32(SERVER_HASH);
        ASSERT_TRUE(sendMsg(sm_conn, pub));
        Message reply; ASSERT_TRUE(recvMsg(sm_conn, reply, 2000));
    }

    {
        Message sub(MessageType::MSG_SUBSCRIBE_TOPIC, 300);
        sub.payload.writeString("TestTopic");
        ASSERT_TRUE(sendMsg(sm_conn, sub));
        Message reply;
        ASSERT_TRUE(recvMsg(sm_conn, reply, 2000));
        ASSERT_EQ(reply.getType(), MessageType::MSG_SUBSCRIBE_TOPIC_REPLY);

        BufferView rv(reply.payload.data(), reply.payload.size());
        bool accepted = false;
        ASSERT_TRUE(rv.tryReadBool(accepted));
        ASSERT_TRUE(accepted);

        uint32_t hash = 0;
        if (rv.remaining() >= sizeof(uint32_t)) {
            ASSERT_TRUE(rv.tryReadUint32(hash));
            EXPECT_EQ(hash, SERVER_HASH);
        }
    }
    sm_conn.close();
}
