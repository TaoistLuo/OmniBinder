#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder.h>
#include "transport/shm_ring.h"
#include "transport/tcp_connection.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <signal.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#endif

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SM_PORT = 19912;
static const uint32_t METHOD_ADD = fnv1a_32("Add");
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t IFACE_ID = fnv1a_32("OwnedService");

static bool tryDecodeInvokeReplyForTest(const Message& msg, int32_t& status, Buffer& response) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    uint32_t payload_len = 0;
    if (!payload.tryReadInt32(status)) {
        return false;
    }
    if (status != 0) {
        response.clear();
        return true;
    }
    if (!payload.tryReadUint32(payload_len)) {
        return false;
    }
    if (payload.remaining() < payload_len) {
        return false;
    }
    response.clear();
    if (payload_len > 0) {
        response.writeRaw(payload.data() + payload.readPosition(), payload_len);
    }
    return true;
}

static TestPid startSM(uint16_t port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    return test::startProcess("target/bin/service_manager",
                               "--port", port_str, "--log-level", "3");
}

static void stopSM(TestPid pid) {
    test::stopProcess(pid);
}

static bool waitSM(uint16_t port, int retries) {
    for (int i = 0; i < retries; ++i) {
        OmniRuntime probe;
        if (probe.init("127.0.0.1", port) == 0) {
            probe.stop();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
    }
    return false;
}

static bool connectTcp(TcpConnection& transport, const std::string& host, uint16_t port) {
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
            std::this_thread::sleep_for(std::chrono::microseconds(10000));
        }
        return false;
    }
    return transport.state() == ConnectionState::CONNECTED;
}

static bool sendMessage(TcpConnection& transport, const Message& msg) {
    Buffer out;
    msg.serialize(out);
    return transport.send(out.data(), out.size()) == static_cast<int>(out.size());
}

static bool recvMessage(TcpConnection& transport, Message& msg, int timeout_ms) {
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
        std::this_thread::sleep_for(std::chrono::microseconds(20000));
    }
    return false;
}

static bool recvFullMessage(IMessageConnection& transport, Message& msg, int timeout_ms) {
    Buffer input;
    std::vector<uint8_t> buf(65536);
    int loops = timeout_ms / 5;
    for (int i = 0; i < loops; ++i) {
        int ret = transport.recv(buf.data(), buf.size());
        if (ret > 0) {
            input.writeRaw(buf.data(), static_cast<size_t>(ret));
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
        std::this_thread::sleep_for(std::chrono::microseconds(5000));
    }
    return false;
}

static bool sendFullMessage(IMessageConnection& transport, const Message& msg, int timeout_ms) {
    Buffer out;
    msg.serialize(out);
    size_t sent = 0;
    int loops = timeout_ms / 5;
    for (int i = 0; i < loops && sent < out.size(); ++i) {
        int ret = transport.send(out.data() + sent, out.size() - sent);
        if (ret < 0) {
            return false;
        }
        if (ret == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(5000));
            continue;
        }
        sent += static_cast<size_t>(ret);
    }
    return sent == out.size();
}

static bool decodeInvokeReplyForTest(const Message& msg, int32_t& status, Buffer& response) {
    return tryDecodeInvokeReplyForTest(msg, status, response);
}

static bool registerFakeService(TcpConnection& transport, uint32_t seq,
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
    return mustRead<bool>(payload, &Buffer::tryReadBool);
}

static bool publishFakeTopic(TcpConnection& transport, uint32_t seq,
                             const std::string& service_name,
                             const std::string& topic) {
    Message msg(MessageType::MSG_PUBLISH_TOPIC, seq);
    msg.payload.writeString(topic);
    ServiceInfo info;
    info.name = service_name;
    info.host = "127.0.0.1";
    info.port = 12345;
    info.host_id = "raw-owner";
    serializeServiceInfo(info, msg.payload);
    if (!sendMessage(transport, msg)) {
        return false;
    }
    Message reply;
    if (!recvMessage(transport, reply, 2000)
        || reply.getType() != MessageType::MSG_PUBLISH_TOPIC_REPLY) {
        return false;
    }
    Buffer payload(reply.payload.data(), reply.payload.size());
    return mustRead<bool>(payload, &Buffer::tryReadBool);
}

static bool queryRawPublishedTopics(TcpConnection& transport, uint32_t seq,
                                    const std::string& service_name, bool& found,
                                    std::vector<std::string>& topics) {
    Message msg(MessageType::MSG_QUERY_PUBLISHED_TOPICS, seq);
    msg.payload.writeString(service_name);
    if (!sendMessage(transport, msg)) {
        return false;
    }
    Message reply;
    if (!recvMessage(transport, reply, 2000)
        || reply.getType() != MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY) {
        return false;
    }
    BufferView payload(reply.payload.data(), reply.payload.size());
    return deserializePublishedTopicsReply(payload, found, topics);
}

static bool sendHeartbeat(TcpConnection& transport, uint32_t seq,
                          const std::string& service_name) {
    Message heartbeat(MessageType::MSG_HEARTBEAT, seq);
    heartbeat.payload.writeString(service_name);
    if (!sendMessage(transport, heartbeat)) return false;
    Message reply;
    return recvMessage(transport, reply, 2000)
        && reply.getType() == MessageType::MSG_HEARTBEAT_ACK;
}

static bool lookupRaw(TcpConnection& transport, uint32_t seq,
                      const std::string& service_name, bool& found) {
    Message msg(MessageType::MSG_LOOKUP, seq);
    msg.payload.writeString(service_name);
    if (!sendMessage(transport, msg)) return false;
    Message reply;
    if (!recvMessage(transport, reply, 2000)
        || reply.getType() != MessageType::MSG_LOOKUP_REPLY) {
        return false;
    }
    Buffer payload(reply.payload.data(), reply.payload.size());
    found = mustRead<bool>(payload, &Buffer::tryReadBool);
    return true;
}

static bool sendRuntimeHello(TcpConnection& transport, uint32_t seq,
                             uint32_t pid, const std::string& process_name) {
    Message msg(MessageType::MSG_RUNTIME_HELLO, seq);
    RuntimeInfo info;
    info.pid = pid;
    info.process_name = process_name;
    serializeRuntimeInfo(info, msg.payload);
    if (!sendMessage(transport, msg)) return false;
    Message reply;
    if (!recvMessage(transport, reply, 2000)
        || reply.getType() != MessageType::MSG_RUNTIME_HELLO_REPLY) {
        return false;
    }
    Buffer payload(reply.payload.data(), reply.payload.size());
    return mustRead<bool>(payload, &Buffer::tryReadBool);
}

static bool sendWatchStart(TcpConnection& transport, uint32_t seq, uint32_t pid) {
    Message msg(MessageType::MSG_DIAG_WATCH_START, seq);
    msg.payload.writeUint32(pid);
    if (!sendMessage(transport, msg)) return false;
    Message reply;
    if (!recvMessage(transport, reply, 2000)
        || reply.getType() != MessageType::MSG_DIAG_WATCH_START_REPLY) {
        return false;
    }
    Buffer payload(reply.payload.data(), reply.payload.size());
    return mustRead<bool>(payload, &Buffer::tryReadBool);
}

static bool sendWatchStop(TcpConnection& transport, uint32_t seq, uint32_t pid) {
    Message msg(MessageType::MSG_DIAG_WATCH_STOP, seq);
    msg.payload.writeUint32(pid);
    if (!sendMessage(transport, msg)) return false;
    Message reply;
    return recvMessage(transport, reply, 2000)
        && reply.getType() == MessageType::MSG_DIAG_WATCH_STOP_REPLY;
}

static bool invokeOwnedService(OmniRuntime& runtime, int32_t a, int32_t b, int32_t& sum) {
    Buffer req;
    req.writeInt32(a);
    req.writeInt32(b);
    Buffer resp;
    int ret = runtime.invoke("OwnedService", IFACE_ID, METHOD_ADD, 0, req, resp, 5000);
    if (ret != 0 || resp.size() < sizeof(int32_t)) {
        return false;
    }
    sum = mustRead<int32_t>(resp, &Buffer::tryReadInt32);
    return true;
}

class OwnedService : public Service {
public:
    OwnedService() : Service("OwnedService"), invoke_count_(0) {
        iface_.interface_id = IFACE_ID;
        iface_.name = "OwnedService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
        iface_.methods.back().idl_hash = 0x5ADD0001u;
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
        iface_.methods.back().idl_hash = 0x5EC00001u;
    }
    const char* serviceName() const override { return "OwnedService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
    int invokeCount() const { return invoke_count_; }
protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ADD) {
            Buffer req(request.data(), request.size());
            int32_t a = 0, b = 0;
            if (!req.tryReadInt32(a) || !req.tryReadInt32(b)) {
                return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
            }
            invoke_count_++;
            if (!response.writeInt32(a + b)) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        } else if (method_id == METHOD_ECHO) {
            invoke_count_++;
            if (request.size() > 0) {
                if (!response.writeRaw(request.data(), request.size())) return static_cast<int>(ErrorCode::ERR_SERIALIZE);
            }
        }
        return 0;
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

static void ownedServerThread(void* arg) {
    OwnedServerCtx* ctx = static_cast<OwnedServerCtx*>(arg);
    if (ctx->runtime.init("127.0.0.1", SM_PORT) != 0) {
        return;
    }
    if (ctx->runtime.registerService(&ctx->service) != 0) {
        ctx->runtime.stop();
        return;
    }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(20);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

class FallbackService : public Service {
public:
    FallbackService() : Service("FallbackService") {
        iface_.interface_id = IFACE_ID;
        iface_.name = "FallbackService";
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
    }
    const char* serviceName() const override { return "FallbackService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id != METHOD_ADD) {
            return static_cast<int>(ErrorCode::ERR_METHOD_NOT_FOUND);
        }
        Buffer req(request.data(), request.size());
        int32_t a = 0;
        int32_t b = 0;
        if (!req.tryReadInt32(a) || !req.tryReadInt32(b)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        if (!response.writeInt32(a + b)) {
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
        return 0;
    }
private:
    InterfaceInfo iface_;
};

struct FallbackServerCtx {
    OmniRuntime runtime;
    FallbackService service;
    volatile bool registered;
    volatile bool should_stop;
    FallbackServerCtx() : registered(false), should_stop(false) {}
};

static void fallbackServerThread(void* arg) {
    FallbackServerCtx* ctx = static_cast<FallbackServerCtx*>(arg);
    if (ctx->runtime.init("127.0.0.1", SM_PORT) != 0) {
        return;
    }
    if (ctx->runtime.registerService(&ctx->service) != 0) {
        ctx->runtime.stop();
        return;
    }
    ctx->registered = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(20);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
}

struct DelayedReadServiceCtx {
    TcpTestServer server;
    uint16_t port;
    volatile bool ready;
    volatile bool done;
    DelayedReadServiceCtx() : port(0), ready(false), done(false) {}
};

static void delayedReadServiceThread(void* arg) {
    DelayedReadServiceCtx* ctx = static_cast<DelayedReadServiceCtx*>(arg);
    if (!ctx->server.start()) {
        return;
    }
    ctx->port = ctx->server.port();
    ctx->ready = true;

    IMessageConnection* accepted = ctx->server.waitAccept();
    if (!accepted) {
        ctx->done = true;
        return;
    }

    int rcvbuf = 4096;
#ifndef _WIN32
    setsockopt(accepted->fd(), SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(300000));

    Buffer input;
    std::vector<uint8_t> buf(65536);
    bool replied = false;
    for (int i = 0; i < 400 && !replied; ++i) {
        int ret = accepted->recv(buf.data(), buf.size());
        if (ret > 0) {
            input.writeRaw(buf.data(), static_cast<size_t>(ret));
            if (input.size() >= MESSAGE_HEADER_SIZE) {
                MessageHeader hdr;
                if (Message::parseHeader(input.data(), input.size(), hdr)) {
                    size_t total = MESSAGE_HEADER_SIZE + hdr.length;
                    if (input.size() >= total) {
                        Message reply(MessageType::MSG_INVOKE_REPLY, hdr.sequence);
                        reply.payload.writeInt32(0);
                        reply.payload.writeUint32(0);
                        Buffer out;
                        reply.serialize(out);
                        size_t sent = 0;
                        while (sent < out.size()) {
                            int n = accepted->send(out.data() + sent, out.size() - sent);
                            if (n < 0) {
                                ctx->server.close();
                                ctx->done = true;
                                return;
                            }
                            if (n == 0) {
                                std::this_thread::sleep_for(std::chrono::microseconds(10000));
                                continue;
                            }
                            sent += static_cast<size_t>(n);
                        }
                        replied = true;
                    }
                }
            }
        }
        if (!replied) {
            std::this_thread::sleep_for(std::chrono::microseconds(10000));
        }
    }

    ctx->server.close();
    ctx->done = true;
}

class ControlPlaneTest : public ::testing::Test {
protected:
    static TestPid sm_pid_;
    static OwnedServerCtx owned_ctx_;
    static std::thread owned_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startSM(SM_PORT);
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitSM(SM_PORT, 30));

        owned_tid_ = std::thread(ownedServerThread, &owned_ctx_);
        for (int i = 0; i < 50 && !owned_ctx_.registered; ++i) std::this_thread::sleep_for(std::chrono::microseconds(100000));
        ASSERT_TRUE(owned_ctx_.registered);
    }

    static void TearDownTestSuite() {
        owned_ctx_.should_stop = true;
        owned_tid_.join();
        stopSM(sm_pid_);
    }
};

TestPid ControlPlaneTest::sm_pid_ = 0;
OwnedServerCtx ControlPlaneTest::owned_ctx_;
std::thread ControlPlaneTest::owned_tid_;

TEST_F(ControlPlaneTest, IllegalUnregisterRejected) {
    TcpConnection rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", SM_PORT));
    Message msg(MessageType::MSG_UNREGISTER, 1001);
    msg.payload.writeString("OwnedService");
    ASSERT_TRUE(sendMessage(rogue, msg));
    Message reply;
    ASSERT_TRUE(recvMessage(rogue, reply, 2000));
    ASSERT_EQ(reply.getType(), MessageType::MSG_UNREGISTER_REPLY);
    Buffer payload(reply.payload.data(), reply.payload.size());
    EXPECT_FALSE(mustRead<bool>(payload, &Buffer::tryReadBool));

    OmniRuntime checker;
    ASSERT_EQ(checker.init("127.0.0.1", SM_PORT), 0);
    ServiceInfo info;
    ASSERT_EQ(checker.lookupService("OwnedService", info), 0);
    checker.stop();
    rogue.close();
}

TEST_F(ControlPlaneTest, MalformedLookupAndSubscribeTopicDoNotCrashSM) {
    TcpConnection rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", SM_PORT));

    Message bad_lookup(MessageType::MSG_LOOKUP, 1004);
    bad_lookup.payload.writeUint32(0x12345678u);
    ASSERT_TRUE(sendMessage(rogue, bad_lookup));
    Message lookup_reply;
    ASSERT_TRUE(recvMessage(rogue, lookup_reply, 2000));
    ASSERT_EQ(lookup_reply.getType(), MessageType::MSG_LOOKUP_REPLY);
    Buffer lookup_payload(lookup_reply.payload.data(), lookup_reply.payload.size());
    EXPECT_FALSE(mustRead<bool>(lookup_payload, &Buffer::tryReadBool));

    Message bad_subscribe(MessageType::MSG_SUBSCRIBE_TOPIC, 1005);
    bad_subscribe.payload.writeUint16(7);
    ASSERT_TRUE(sendMessage(rogue, bad_subscribe));
    Message subscribe_reply;
    ASSERT_TRUE(recvMessage(rogue, subscribe_reply, 2000));
    ASSERT_EQ(subscribe_reply.getType(), MessageType::MSG_SUBSCRIBE_TOPIC_REPLY);
    Buffer subscribe_payload(subscribe_reply.payload.data(), subscribe_reply.payload.size());
    EXPECT_FALSE(mustRead<bool>(subscribe_payload, &Buffer::tryReadBool));

    OmniRuntime checker;
    ASSERT_EQ(checker.init("127.0.0.1", SM_PORT), 0);
    ServiceInfo info;
    ASSERT_EQ(checker.lookupService("OwnedService", info), 0);
    checker.stop();

    rogue.close();
}

TEST_F(ControlPlaneTest, IllegalPublishTopicRejected) {
    TcpConnection rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", SM_PORT));
    Message msg(MessageType::MSG_PUBLISH_TOPIC, 1002);
    msg.payload.writeString("owned/topic");
    ServiceInfo pub_info;
    pub_info.name = "OwnedService";
    pub_info.host = "127.0.0.1";
    pub_info.port = owned_ctx_.service.port();
    pub_info.host_id = owned_ctx_.runtime.hostId();
    serializeServiceInfo(pub_info, msg.payload);
    ASSERT_TRUE(sendMessage(rogue, msg));
    Message reply;
    ASSERT_TRUE(recvMessage(rogue, reply, 2000));
    ASSERT_EQ(reply.getType(), MessageType::MSG_PUBLISH_TOPIC_REPLY);
    Buffer payload(reply.payload.data(), reply.payload.size());
    EXPECT_FALSE(mustRead<bool>(payload, &Buffer::tryReadBool));
    rogue.close();
}

TEST_F(ControlPlaneTest, InvalidTopicControlRequestsDoNotMutateState) {
    TcpConnection owner;
    TcpConnection query;
    ASSERT_TRUE(connectTcp(owner, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(query, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(registerFakeService(owner, 1050, "RawValidatedService", 12340,
                                    "raw-validated"));
    ASSERT_TRUE(publishFakeTopic(owner, 1051, "RawValidatedService", "raw/valid"));

    const std::vector<std::string> invalid_topics = {
        std::string(), std::string(MAX_TOPIC_NAME_LENGTH + 1, 'x')
    };
    for (size_t i = 0; i < invalid_topics.size(); ++i) {
        Message subscribe(MessageType::MSG_SUBSCRIBE_TOPIC, 1052 + i);
        subscribe.payload.writeString(invalid_topics[i]);
        ASSERT_TRUE(sendMessage(query, subscribe));
        Message reply;
        ASSERT_TRUE(recvMessage(query, reply, 2000));
        Buffer payload(reply.payload.data(), reply.payload.size());
        EXPECT_FALSE(mustRead<bool>(payload, &Buffer::tryReadBool));
    }

    Message trailing_subscribe(MessageType::MSG_SUBSCRIBE_TOPIC, 1054);
    trailing_subscribe.payload.writeString("raw/new");
    trailing_subscribe.payload.writeUint8(1);
    ASSERT_TRUE(sendMessage(query, trailing_subscribe));
    Message trailing_subscribe_reply;
    ASSERT_TRUE(recvMessage(query, trailing_subscribe_reply, 2000));
    Buffer trailing_subscribe_payload(trailing_subscribe_reply.payload.data(),
                                      trailing_subscribe_reply.payload.size());
    EXPECT_FALSE(mustRead<bool>(trailing_subscribe_payload, &Buffer::tryReadBool));

    Message trailing_unpublish(MessageType::MSG_UNPUBLISH_TOPIC, 1055);
    trailing_unpublish.payload.writeString("raw/valid");
    trailing_unpublish.payload.writeUint8(1);
    ASSERT_TRUE(sendMessage(owner, trailing_unpublish));

    Message malformed_publish(MessageType::MSG_PUBLISH_TOPIC, 1056);
    malformed_publish.payload.writeString("raw/new");
    ServiceInfo pub_info;
    pub_info.name = "RawValidatedService";
    pub_info.host = "127.0.0.1";
    pub_info.port = 12340;
    serializeServiceInfo(pub_info, malformed_publish.payload);
    malformed_publish.payload.writeUint16(7);
    ASSERT_TRUE(sendMessage(owner, malformed_publish));
    Message malformed_publish_reply;
    ASSERT_TRUE(recvMessage(owner, malformed_publish_reply, 2000));
    Buffer malformed_publish_payload(malformed_publish_reply.payload.data(),
                                     malformed_publish_reply.payload.size());
    EXPECT_FALSE(mustRead<bool>(malformed_publish_payload, &Buffer::tryReadBool));

    bool found = false;
    std::vector<std::string> topics;
    ASSERT_TRUE(queryRawPublishedTopics(query, 1057, "RawValidatedService", found, topics));
    ASSERT_TRUE(found);
    ASSERT_EQ(topics.size(), 1u);
    EXPECT_EQ(topics[0], "raw/valid");
    EXPECT_FALSE(std::find(topics.begin(), topics.end(), "raw/new") != topics.end());

    query.close();
    owner.close();
}

TEST_F(ControlPlaneTest, PublishedTopicsQueryAndServiceScopedUnregister) {
    TcpConnection registrant;
    TcpConnection query;
    ASSERT_TRUE(connectTcp(registrant, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(query, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(registerFakeService(registrant, 1100, "RawServiceA", 12345, "raw-owner"));
    ASSERT_TRUE(registerFakeService(registrant, 1101, "RawServiceB", 12346, "raw-owner"));
    ASSERT_TRUE(publishFakeTopic(registrant, 1102, "RawServiceA", "raw/a"));
    ASSERT_TRUE(publishFakeTopic(registrant, 1103, "RawServiceB", "raw/b"));

    bool found = false;
    std::vector<std::string> topics;
    ASSERT_TRUE(queryRawPublishedTopics(query, 1104, "RawServiceA", found, topics));
    ASSERT_TRUE(found);
    ASSERT_EQ(topics.size(), 1u);
    EXPECT_EQ(topics[0], "raw/a");

    Message unregister(MessageType::MSG_UNREGISTER, 1105);
    unregister.payload.writeString("RawServiceA");
    ASSERT_TRUE(sendMessage(registrant, unregister));
    Message unregister_reply;
    ASSERT_TRUE(recvMessage(registrant, unregister_reply, 2000));
    ASSERT_EQ(unregister_reply.getType(), MessageType::MSG_UNREGISTER_REPLY);

    ASSERT_TRUE(queryRawPublishedTopics(query, 1106, "RawServiceA", found, topics));
    EXPECT_FALSE(found);
    EXPECT_TRUE(topics.empty());

    const std::vector<std::string> malformed_names = {
        std::string(), std::string(MAX_SERVICE_NAME_LENGTH + 1, 'x')
    };
    for (size_t i = 0; i < malformed_names.size(); ++i) {
        Message invalid(MessageType::MSG_QUERY_PUBLISHED_TOPICS, 1113 + i);
        invalid.payload.writeString(malformed_names[i]);
        ASSERT_TRUE(sendMessage(query, invalid));
        Message invalid_reply;
        ASSERT_TRUE(recvMessage(query, invalid_reply, 2000));
        BufferView invalid_payload(invalid_reply.payload.data(), invalid_reply.payload.size());
        ASSERT_TRUE(deserializePublishedTopicsReply(invalid_payload, found, topics));
        EXPECT_FALSE(found);
    }
    Message trailing(MessageType::MSG_QUERY_PUBLISHED_TOPICS, 1115);
    trailing.payload.writeString("RawEmptyService");
    trailing.payload.writeUint8(1);
    ASSERT_TRUE(sendMessage(query, trailing));
    Message trailing_reply;
    ASSERT_TRUE(recvMessage(query, trailing_reply, 2000));
    BufferView trailing_payload(trailing_reply.payload.data(), trailing_reply.payload.size());
    ASSERT_TRUE(deserializePublishedTopicsReply(trailing_payload, found, topics));
    EXPECT_FALSE(found);
    ASSERT_TRUE(queryRawPublishedTopics(query, 1107, "RawServiceB", found, topics));
    ASSERT_TRUE(found);
    ASSERT_EQ(topics.size(), 1u);
    EXPECT_EQ(topics[0], "raw/b");

    Message unpublish(MessageType::MSG_UNPUBLISH_TOPIC, 1108);
    unpublish.payload.writeString("raw/b");
    ASSERT_TRUE(sendMessage(registrant, unpublish));
    ASSERT_TRUE(queryRawPublishedTopics(query, 1109, "RawServiceB", found, topics));
    EXPECT_TRUE(found);
    EXPECT_TRUE(topics.empty());

    ASSERT_TRUE(registerFakeService(registrant, 1110, "RawEmptyService", 12347, "raw-owner"));
    ASSERT_TRUE(queryRawPublishedTopics(query, 1111, "RawEmptyService", found, topics));
    EXPECT_TRUE(found);
    EXPECT_TRUE(topics.empty());

    Message malformed(MessageType::MSG_QUERY_PUBLISHED_TOPICS, 1112);
    malformed.payload.writeUint16(9);
    ASSERT_TRUE(sendMessage(query, malformed));
    Message malformed_reply;
    ASSERT_TRUE(recvMessage(query, malformed_reply, 2000));
    BufferView malformed_payload(malformed_reply.payload.data(), malformed_reply.payload.size());
    ASSERT_TRUE(deserializePublishedTopicsReply(malformed_payload, found, topics));
    EXPECT_FALSE(found);
    EXPECT_TRUE(topics.empty());

    query.close();
    registrant.close();
}

TEST_F(ControlPlaneTest, HeartbeatTimeoutIsServiceScopedOnSharedControlFd) {
    TcpConnection registrant;
    TcpConnection observer;
    TcpConnection query;
    ASSERT_TRUE(connectTcp(registrant, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(observer, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(query, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(registerFakeService(registrant, 1200, "RawTimeoutA", 12400, "raw-heartbeat"));
    ASSERT_TRUE(registerFakeService(registrant, 1201, "RawHealthyB", 12401, "raw-heartbeat"));
    ASSERT_TRUE(publishFakeTopic(registrant, 1202, "RawTimeoutA", "raw/timeout-a"));
    ASSERT_TRUE(publishFakeTopic(registrant, 1203, "RawHealthyB", "raw/healthy-b"));

    Message subscribe(MessageType::MSG_SUBSCRIBE_TOPIC, 1204);
    subscribe.payload.writeString("raw/future");
    ASSERT_TRUE(sendMessage(registrant, subscribe));
    Message subscribe_reply;
    ASSERT_TRUE(recvMessage(registrant, subscribe_reply, 2000));
    ASSERT_EQ(subscribe_reply.getType(), MessageType::MSG_SUBSCRIBE_TOPIC_REPLY);

    Message post_timeout_subscribe(MessageType::MSG_SUBSCRIBE_TOPIC, 1206);
    post_timeout_subscribe.payload.writeString("raw/post-timeout");
    ASSERT_TRUE(sendMessage(registrant, post_timeout_subscribe));
    Message post_timeout_subscribe_reply;
    ASSERT_TRUE(recvMessage(registrant, post_timeout_subscribe_reply, 2000));
    ASSERT_EQ(post_timeout_subscribe_reply.getType(), MessageType::MSG_SUBSCRIBE_TOPIC_REPLY);

    Message watch(MessageType::MSG_SUBSCRIBE_SERVICE, 1205);
    watch.payload.writeString("RawTimeoutA");
    ASSERT_TRUE(sendMessage(observer, watch));
    Message watch_reply;
    ASSERT_TRUE(recvMessage(observer, watch_reply, 2000));

    bool death_seen = false;
    for (int i = 0; i < 40 && !death_seen; ++i) {
        ASSERT_TRUE(sendHeartbeat(registrant, 1210 + i, "RawHealthyB"));
        Message death;
        if (recvMessage(observer, death, 200)) {
            ASSERT_EQ(death.getType(), MessageType::MSG_DEATH_NOTIFY);
            Buffer payload(death.payload.data(), death.payload.size());
            EXPECT_EQ(mustReadString(payload), "RawTimeoutA");
            death_seen = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    ASSERT_TRUE(death_seen);

    bool found = false;
    std::vector<std::string> topics;
    ASSERT_TRUE(queryRawPublishedTopics(query, 1260, "RawTimeoutA", found, topics));
    EXPECT_FALSE(found);
    ASSERT_TRUE(queryRawPublishedTopics(query, 1261, "RawHealthyB", found, topics));
    ASSERT_TRUE(found);
    ASSERT_EQ(topics.size(), 1u);
    EXPECT_EQ(topics[0], "raw/healthy-b");
    EXPECT_TRUE(sendHeartbeat(registrant, 1262, "RawHealthyB"));

    Message stale_heartbeat(MessageType::MSG_HEARTBEAT, 1263);
    stale_heartbeat.payload.writeString("RawTimeoutA");
    ASSERT_TRUE(sendMessage(registrant, stale_heartbeat));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(sendHeartbeat(registrant, 1264, "RawHealthyB"));

    TcpConnection future_publisher;
    ASSERT_TRUE(connectTcp(future_publisher, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(registerFakeService(future_publisher, 1265, "RawFuturePublisher",
                                    12402, "raw-heartbeat"));
    ASSERT_TRUE(publishFakeTopic(future_publisher, 1266, "RawFuturePublisher", "raw/future"));
    Message notify;
    ASSERT_TRUE(recvMessage(registrant, notify, 2000));
    ASSERT_EQ(notify.getType(), MessageType::MSG_TOPIC_PUBLISHER_NOTIFY);
    Buffer notify_payload(notify.payload.data(), notify.payload.size());
    EXPECT_EQ(mustReadString(notify_payload), "raw/future");

    // C2：RawHealthyB 是 registrant 上最后一个存活注册服务。它超时后该控制
    // 连接已无任何存活注册，SM 必须按连接失联关闭它；真实 runtime 会通过
    // reconnect + restoreControlPlaneState 重建订阅，裸客户端在此只能观察到 EOF。
    bool healthy_removed = false;
    for (int i = 0; i < 45 && !healthy_removed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        if (queryRawPublishedTopics(query, 1270 + i, "RawHealthyB", found, topics)) {
            healthy_removed = !found;
        }
    }
    ASSERT_TRUE(healthy_removed);

    bool connection_closed = false;
    uint8_t drain_buf[4096];
    for (int i = 0; i < 100 && !connection_closed; ++i) {
        int ret = registrant.recv(drain_buf, sizeof(drain_buf));
        if (ret < 0) {
            connection_closed = (registrant.state() != ConnectionState::CONNECTED);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(connection_closed)
        << "Connection must be closed once no live service remains";

    future_publisher.close();
    query.close();
    observer.close();
    registrant.close();
}

TEST(ControlPlaneCompatibilityTest, IgnoredPublishedTopicsQueryUsesExplicitTimeout) {
    const uint16_t port = 19919;
    TcpTestServer server;
    ASSERT_TRUE(server.start("127.0.0.1", port));
    std::thread fake_sm([&server]() {
        IMessageConnection* client = server.waitAccept();
        if (!client) return;
        Message hello;
        if (recvFullMessage(*client, hello, 2000)) {
            Message reply(MessageType::MSG_RUNTIME_HELLO_REPLY, hello.getSequence());
            reply.payload.writeBool(true);
            sendFullMessage(*client, reply, 2000);
        }
        Message ignored;
        recvFullMessage(*client, ignored, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        server.releaseAccepted();
    });

    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", port), 0);
    std::vector<std::string> topics;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    EXPECT_EQ(runtime.queryPublishedTopics("AnyService", topics, 250),
              static_cast<int>(ErrorCode::ERR_TIMEOUT));
    const long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(elapsed_ms, 150);
    EXPECT_LE(elapsed_ms, 900);
    runtime.stop();
    fake_sm.join();
    server.close();
}

TEST_F(ControlPlaneTest, ShmFailureFallsBackToTcp) {
#ifdef _WIN32
    GTEST_SKIP() << "Deterministic named SHM endpoint invalidation is POSIX-only";
#else
    FallbackServerCtx fallback_ctx;
    std::thread fallback_tid;
    fallback_tid = std::thread(fallbackServerThread, &fallback_ctx);
    for (int i = 0; i < 50 && !fallback_ctx.registered; ++i) std::this_thread::sleep_for(std::chrono::microseconds(100000));
    ASSERT_TRUE(fallback_ctx.registered);

    OmniRuntime probe;
    ASSERT_EQ(probe.init("127.0.0.1", SM_PORT), 0);
    ServiceInfo info;
    ASSERT_EQ(probe.lookupService("FallbackService", info), 0);
    ASSERT_EQ(info.port, fallback_ctx.service.port());
    ASSERT_EQ(info.host_id, probe.hostId());

    const std::string handshake_path = shmHandshakePath(
        generateShmName("FallbackService"));
    ASSERT_EQ(unlink(handshake_path.c_str()), 0);

    probe.clearServiceCache();
    probe.closeAllConnections();

    Buffer req;
    req.writeInt32(5);
    req.writeInt32(9);
    Buffer resp;
    ASSERT_EQ(probe.connectService("FallbackService"), 0);
    ASSERT_EQ(probe.invoke("FallbackService", IFACE_ID, METHOD_ADD, 0, req, resp, 5000), 0);
    EXPECT_EQ(mustRead<int32_t>(resp, &Buffer::tryReadInt32), 14);

    RuntimeStats stats;
    ASSERT_EQ(probe.getStats(stats), 0);
    EXPECT_EQ(stats.tcp_connections, 1u);
    EXPECT_EQ(stats.shm_connections, 0u);

    probe.stop();
    fallback_ctx.should_stop = true;
    fallback_tid.join();
#endif
}

TEST_F(ControlPlaneTest, MalformedInvokePayloadReturnsDeserializeWithoutCrash) {
    TcpConnection rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", owned_ctx_.service.port()));

    const int before_count = owned_ctx_.service.invokeCount();

    Message short_header(MessageType::MSG_INVOKE, 2001);
    short_header.payload.writeUint32(IFACE_ID);
    short_header.payload.writeUint32(METHOD_ADD);
    ASSERT_TRUE(sendMessage(rogue, short_header));

    Message reply;
    ASSERT_TRUE(recvFullMessage(rogue, reply, 2000));
    ASSERT_EQ(reply.getType(), MessageType::MSG_INVOKE_REPLY);
    Buffer payload(reply.payload.data(), reply.payload.size());
    ASSERT_EQ(mustRead<int32_t>(payload, &Buffer::tryReadInt32), static_cast<int32_t>(ErrorCode::ERR_DESERIALIZE));
    ASSERT_EQ(mustRead<uint32_t>(payload, &Buffer::tryReadUint32), 0u);
    EXPECT_EQ(owned_ctx_.service.invokeCount(), before_count);

    Message short_body(MessageType::MSG_INVOKE, 2002);
    short_body.payload.writeUint32(IFACE_ID);
    short_body.payload.writeUint32(METHOD_ADD);
    short_body.payload.writeUint32(sizeof(int32_t));
    short_body.payload.writeInt32(7);
    ASSERT_TRUE(sendMessage(rogue, short_body));
    Message short_body_reply;
    ASSERT_TRUE(recvFullMessage(rogue, short_body_reply, 2000));
    ASSERT_EQ(short_body_reply.getType(), MessageType::MSG_INVOKE_REPLY);
    Buffer short_body_payload(short_body_reply.payload.data(), short_body_reply.payload.size());
    ASSERT_EQ(mustRead<int32_t>(short_body_payload, &Buffer::tryReadInt32), static_cast<int32_t>(ErrorCode::ERR_DESERIALIZE));
    ASSERT_EQ(mustRead<uint32_t>(short_body_payload, &Buffer::tryReadUint32), 0u);
    EXPECT_EQ(owned_ctx_.service.invokeCount(), before_count);

    OmniRuntime checker;
    ASSERT_EQ(checker.init("127.0.0.1", SM_PORT), 0);
    int32_t sum = 0;
    ASSERT_EQ(checker.connectService("OwnedService"), 0);
    ASSERT_TRUE(invokeOwnedService(checker, 20, 22, sum));
    EXPECT_EQ(sum, 42);
    checker.stop();

    rogue.close();
}

TEST_F(ControlPlaneTest, MalformedSubscribeBroadcastDoesNotCrashService) {
    TcpConnection rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", owned_ctx_.service.port()));

    Message bad_subscribe(MessageType::MSG_SUBSCRIBE_BROADCAST, 2003);
    bad_subscribe.payload.writeUint32(0x99887766u);
    ASSERT_TRUE(sendMessage(rogue, bad_subscribe));

    Message bad_broadcast(MessageType::MSG_BROADCAST, 2004);
    bad_broadcast.payload.writeUint32(0x99887766u);
    ASSERT_TRUE(sendMessage(rogue, bad_broadcast));

    std::this_thread::sleep_for(std::chrono::microseconds(100000));

    OmniRuntime checker;
    ASSERT_EQ(checker.init("127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(checker.connectService("OwnedService"), 0);
    int32_t sum = 0;
    ASSERT_TRUE(invokeOwnedService(checker, 1, 2, sum));
    EXPECT_EQ(sum, 3);
    checker.stop();

    rogue.close();
}

TEST_F(ControlPlaneTest, ListServicesLargeReplyHandlesPartialSend) {
    TcpConnection registrant;
    ASSERT_TRUE(connectTcp(registrant, "127.0.0.1", SM_PORT));

    OmniRuntime host_probe;
    ASSERT_EQ(host_probe.init("127.0.0.1", SM_PORT), 0);
    std::string host_id = host_probe.hostId();

    for (int i = 0; i < 48; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "BulkService_%02d_partial_send_case", i);
        ASSERT_TRUE(registerFakeService(registrant, 2000 + static_cast<uint32_t>(i),
                                       name, static_cast<uint16_t>(41000 + i), host_id));
    }

    std::vector<ServiceInfo> services;
    ASSERT_EQ(host_probe.listServices(services), 0);
    EXPECT_GE(services.size(), 49u);

    registrant.close();
    host_probe.stop();
}

TEST_F(ControlPlaneTest, RuntimeTcpLargeInvokeWaitsForFullRequestSend) {
    DelayedReadServiceCtx delayed_ctx;
    std::thread delayed_tid;
    delayed_tid = std::thread(delayedReadServiceThread, &delayed_ctx);
    for (int i = 0; i < 50 && !delayed_ctx.ready; ++i) std::this_thread::sleep_for(std::chrono::microseconds(100000));
    ASSERT_TRUE(delayed_ctx.ready);

    TcpConnection registrant;
    ASSERT_TRUE(connectTcp(registrant, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(registerFakeService(registrant, 3001, "SlowReadService", delayed_ctx.port, "remote-delayed-node"));

    OmniRuntime probe;
    ASSERT_EQ(probe.init("127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(probe.connectService("SlowReadService"), 0);
    Buffer req;
    std::vector<uint8_t> payload(16 * 1024, 0x5A);
    req.writeRaw(payload.data(), payload.size());
    Buffer resp;
    ASSERT_EQ(probe.invoke("SlowReadService", IFACE_ID, METHOD_ADD, 0, req, resp, 30000), 0);
    EXPECT_EQ(resp.size(), 0u);

    probe.stop();
    registrant.close();
    delayed_tid.join();
}

TEST_F(ControlPlaneTest, ServiceTcpLargeReplyHandlesPartialSend) {
    TcpConnection rogue;
    ASSERT_TRUE(connectTcp(rogue, "127.0.0.1", owned_ctx_.service.port()));

    int rcvbuf = 4096;
#ifndef _WIN32
    setsockopt(rogue.fd(), SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
#endif

    std::vector<uint8_t> payload(512 * 1024, 0x6B);
    Message invoke(MessageType::MSG_INVOKE, 3002);
    invoke.payload.writeUint32(IFACE_ID);
    invoke.payload.writeUint32(0x5EC00001u);  // 与 OwnedService::METHOD_ECHO 匹配的 idl_hash
    invoke.payload.writeUint32(METHOD_ECHO);
    invoke.payload.writeUint32(static_cast<uint32_t>(payload.size()));
    invoke.payload.writeRaw(payload.data(), payload.size());
    ASSERT_TRUE(sendFullMessage(rogue, invoke, 10000));

    std::this_thread::sleep_for(std::chrono::microseconds(300000));

    Message reply;
    ASSERT_TRUE(recvFullMessage(rogue, reply, 15000));
    ASSERT_EQ(reply.getType(), MessageType::MSG_INVOKE_REPLY);
    int32_t status = -1;
    Buffer response;
    ASSERT_TRUE(decodeInvokeReplyForTest(reply, status, response));
    ASSERT_EQ(status, 0);
    ASSERT_EQ(response.size(), payload.size());
    EXPECT_EQ(memcmp(response.data(), payload.data(), payload.size()), 0);

    rogue.close();
}

/*
 * @brief C1 回归：旧控制 fd 关闭不得删除已幂等迁移到新控制 fd 的服务
 * @details 旧 fd 上最后注册的服务特意选为重新注册过的那个，
 *          因为已移除的无保护 closeClient 路径删除的正是该名称。
 */
TEST_F(ControlPlaneTest, StaleFdCloseKeepsReRegisteredService) {
    TcpConnection old_conn;
    TcpConnection new_conn;
    TcpConnection query;
    ASSERT_TRUE(connectTcp(old_conn, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(new_conn, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(query, "127.0.0.1", SM_PORT));

    ASSERT_TRUE(registerFakeService(old_conn, 4001, "RawReconnectGone", 12451,
                                    "raw-reconnect"));
    ASSERT_TRUE(registerFakeService(old_conn, 4002, "RawReconnectKeep", 12450,
                                    "raw-reconnect"));
    ASSERT_TRUE(registerFakeService(new_conn, 4003, "RawReconnectKeep", 12450,
                                    "raw-reconnect"));

    old_conn.close();

    // 等待 SM 处理完 closeClient(old_conn)：仅由 old_conn 持有的服务消失即为证据
    bool found = true;
    bool gone = false;
    for (int i = 0; i < 40 && !gone; ++i) {
        ASSERT_TRUE(lookupRaw(query, 4004 + static_cast<uint32_t>(i),
                              "RawReconnectGone", found));
        if (!found) {
            gone = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ASSERT_TRUE(gone);

    ASSERT_TRUE(lookupRaw(query, 4100, "RawReconnectKeep", found));
    EXPECT_TRUE(found);
    EXPECT_TRUE(sendHeartbeat(new_conn, 4101, "RawReconnectKeep"));

    query.close();
    new_conn.close();
}

/*
 * @brief C5 回归：一个 watcher 连接监控两个 pid 时必须独立跟踪每个 (fd, pid) 对
 * @details 停止 pid A 只停止目标 A；关闭 watcher 才停止剩余的目标 B。
 */
TEST_F(ControlPlaneTest, DiagWatchTracksEachPidPairIndependently) {
    const uint32_t PID_A = 0x7F000001u;
    const uint32_t PID_B = 0x7F000002u;

    TcpConnection target_a;
    TcpConnection target_b;
    TcpConnection watcher;
    ASSERT_TRUE(connectTcp(target_a, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(target_b, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(connectTcp(watcher, "127.0.0.1", SM_PORT));
    ASSERT_TRUE(sendRuntimeHello(target_a, 4200, PID_A, "raw-watch-a"));
    ASSERT_TRUE(sendRuntimeHello(target_b, 4201, PID_B, "raw-watch-b"));

    ASSERT_TRUE(sendWatchStart(watcher, 4202, PID_A));
    ASSERT_TRUE(sendWatchStart(watcher, 4203, PID_B));

    Message start_a;
    ASSERT_TRUE(recvMessage(target_a, start_a, 2000));
    ASSERT_EQ(start_a.getType(), MessageType::MSG_DIAG_WATCH_START);
    Message start_b;
    ASSERT_TRUE(recvMessage(target_b, start_b, 2000));
    ASSERT_EQ(start_b.getType(), MessageType::MSG_DIAG_WATCH_START);

    ASSERT_TRUE(sendWatchStop(watcher, 4204, PID_A));

    Message stop_a;
    ASSERT_TRUE(recvMessage(target_a, stop_a, 2000));
    ASSERT_EQ(stop_a.getType(), MessageType::MSG_DIAG_WATCH_STOP);

    Message unexpected_b;
    EXPECT_FALSE(recvMessage(target_b, unexpected_b, 300));

    watcher.close();

    Message stop_b;
    ASSERT_TRUE(recvMessage(target_b, stop_b, 3000));
    ASSERT_EQ(stop_b.getType(), MessageType::MSG_DIAG_WATCH_STOP);

    target_a.close();
    target_b.close();
}
