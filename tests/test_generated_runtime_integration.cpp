#include <gtest/gtest.h>
#include "test_common.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen_cpp.h"
#include "codegen_c.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace omnibinder;
using namespace omnibinder::test;
using namespace omnic;

#ifndef OMNI_SOURCE_DIR
#define OMNI_SOURCE_DIR "."
#endif

#ifndef OMNI_BUILD_DIR
#define OMNI_BUILD_DIR "."
#endif

static bool parseFile(const std::string& file_path, AstFile& ast, ParseContext& ctx) {
    std::ifstream in(file_path.c_str());
    if (!in.good()) {
        fprintf(stderr, "parseFile: cannot open '%s'\n", file_path.c_str());
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Lexer lexer(source);
    Parser parser(lexer, ctx, file_path);
    if (!parser.parse(ast) || parser.hasError()) {
        fprintf(stderr, "parseFile: parse failed for '%s'\n", file_path.c_str());
        return false;
    }
    return true;
}

static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path.c_str());
    if (!out.good()) {
        fprintf(stderr, "FAIL: writeFile failed for %s\n", path.c_str());
        abort();
    }
    out << content;
}

static std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'') {
            quoted += "'\\''";
        } else {
            quoted += value[i];
        }
    }
    quoted += "'";
    return quoted;
}

static bool runCommand(const std::string& command) {
    return system(command.c_str()) == 0;
}

static std::string replaceAll(std::string input, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = input.find(from, pos)) != std::string::npos) {
        input.replace(pos, from.size(), to);
        pos += to.size();
    }
    return input;
}

static const std::string kCppHarnessTemplate = R"CPP(
#include "guarded.h"
#include <omnibinder/omnibinder.h>
#include <omnibinder/message.h>
#include "transport/tcp_transport.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace omnibinder;

static const uint16_t SM_PORT_REQ = __CPP_REQ_PORT__;
static const uint16_t SM_PORT_REPLY = __CPP_REPLY_PORT__;
static const uint32_t METHOD_ECHO = fnv1a_32("echoItem");
static const uint32_t IFACE_ID = fnv1a_32("demo.ItemService");
static const uint32_t TOPIC_ID = fnv1a_32("ItemTopic");

static pid_t g_sm_pids[4] = {};
static int g_sm_count = 0;
static void cleanupSM() {
    for (int i = 0; i < g_sm_count; ++i) {
        if (g_sm_pids[i] > 0) {
            kill(g_sm_pids[i], SIGKILL);
            waitpid(g_sm_pids[i], NULL, 0);
        }
    }
}

static bool connectTcp(TcpTransport& transport, const std::string& host, uint16_t port) {
    int ret = transport.connect(host, port);
    if (ret < 0) return false;
    if (ret == 1) {
        for (int i = 0; i < 100; ++i) {
            transport.checkConnectComplete();
            if (transport.state() == ConnectionState::CONNECTED) return true;
            usleep(10000);
        }
        return false;
    }
    return transport.state() == ConnectionState::CONNECTED;
}

static bool sendMessage(TcpTransport& transport, const Message& msg) {
    Buffer out;
    msg.serialize(out);
    return transport.send(out.data(), out.size()) == static_cast<int>(out.size());
}

static bool recvFullMessage(TcpTransport& transport, Message& msg, int timeout_ms) {
    Buffer input;
    uint8_t buf[2048];
    int loops = timeout_ms / 20;
    for (int i = 0; i < loops; ++i) {
        int ret = transport.recv(buf, sizeof(buf));
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
        usleep(20000);
    }
    return false;
}

static bool registerFakeService(TcpTransport& transport, uint32_t seq, const std::string& name, uint16_t port, const std::string& host_id) {
    Message msg(MessageType::MSG_REGISTER, seq);
    ServiceInfo info;
    info.name = name;
    info.host = "127.0.0.1";
    info.port = port;
    info.host_id = host_id;
    InterfaceInfo iface;
    iface.interface_id = IFACE_ID;
    iface.name = name;
    iface.methods.push_back(MethodInfo(METHOD_ECHO, "echoItem"));
    info.interfaces.push_back(iface);
    serializeServiceInfo(info, msg.payload);
    if (!sendMessage(transport, msg)) return false;
    Message reply;
    if (!recvFullMessage(transport, reply, 2000)) return false;
    if (reply.getType() != MessageType::MSG_REGISTER_REPLY) return false;
    Buffer payload(reply.payload.data(), reply.payload.size());
    bool value = false;
    return payload.tryReadBool(value) && value;
}

static pid_t startSM(uint16_t port) {
    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        execl("__SERVICE_MANAGER__", "service_manager", "--port", port_str, "--log-level", "3", (char*)NULL);
        _exit(1);
    }
    if (pid > 0 && g_sm_count < 4) g_sm_pids[g_sm_count++] = pid;
    return pid;
}

static void stopSM(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid) return;
            usleep(100000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

static bool waitSM(uint16_t port) {
    for (int i = 0; i < 30; ++i) {
        OmniRuntime probe;
        if (probe.init("127.0.0.1", port) == 0) {
            probe.stop();
            return true;
        }
        usleep(100000);
    }
    return false;
}

class ItemServiceImpl : public demo::ItemServiceStub {
public:
    ItemServiceImpl() : echo_count_(0) {}
    demo::Item echoItem(const demo::Item& item) override {
        echo_count_++;
        return item;
    }
    int echoCount() const { return echo_count_; }
private:
    std::atomic<int> echo_count_;
};

struct ServerCtx {
    OmniRuntime runtime;
    ItemServiceImpl service;
    volatile bool registered;
    volatile bool should_stop;
    ServerCtx() : registered(false), should_stop(false) {}
};

static void* serverThread(void* arg) {
    ServerCtx* ctx = static_cast<ServerCtx*>(arg);
    if (ctx->runtime.init("127.0.0.1", SM_PORT_REQ) != 0) return NULL;
    if (ctx->runtime.registerService(&ctx->service) != 0) { ctx->runtime.stop(); return NULL; }
    ctx->runtime.publishTopic("ItemTopic");
    ctx->registered = true;
    while (!ctx->should_stop) ctx->runtime.pollOnce(20);
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

enum RawReplyMode {
    RAW_REPLY_EMPTY_SUCCESS = 0,
    RAW_REPLY_TRUNCATED_STATUS,
    RAW_REPLY_TRUNCATED_LENGTH
};

struct RawReplyCtx {
    TcpTransportServer server;
    uint16_t port;
    volatile bool ready;
    volatile bool done;
    RawReplyMode mode;
    RawReplyCtx() : port(0), ready(false), done(false), mode(RAW_REPLY_EMPTY_SUCCESS) {}
};

static void* rawReplyThread(void* arg) {
    RawReplyCtx* ctx = static_cast<RawReplyCtx*>(arg);
    int port = ctx->server.listen("127.0.0.1", 0);
    if (port <= 0) return NULL;
    ctx->port = static_cast<uint16_t>(port);
    ctx->ready = true;
    ITransport* accepted = NULL;
    for (int i = 0; i < 100 && !accepted; ++i) {
        accepted = ctx->server.accept();
        if (!accepted) usleep(50000);
    }
    if (!accepted) { ctx->done = true; return NULL; }
    uint8_t buf[4096];
    for (int i = 0; i < 100; ++i) {
        int ret = accepted->recv(buf, sizeof(buf));
        if (ret > 0) {
            Buffer input(buf, static_cast<size_t>(ret));
            MessageHeader hdr;
            assert(Message::parseHeader(input.data(), input.size(), hdr));
            Message reply(MessageType::MSG_INVOKE_REPLY, hdr.sequence);
            if (ctx->mode == RAW_REPLY_TRUNCATED_STATUS) {
                reply.payload.writeUint16(0);
            } else if (ctx->mode == RAW_REPLY_TRUNCATED_LENGTH) {
                reply.payload.writeInt32(0);
                reply.payload.writeUint16(0);
            } else {
                reply.payload.writeInt32(0);
                reply.payload.writeUint32(0);
            }
            Buffer out;
            reply.serialize(out);
            accepted->send(out.data(), out.size());
            break;
        }
        usleep(20000);
    }
    delete accepted;
    ctx->done = true;
    return NULL;
}

int main() {
    atexit(cleanupSM);
    pid_t sm_req = startSM(SM_PORT_REQ);
    assert(sm_req > 0);
    assert(waitSM(SM_PORT_REQ));

    ServerCtx server;
    pthread_t server_tid = 0;
    assert(pthread_create(&server_tid, NULL, serverThread, &server) == 0);
    for (int i = 0; i < 50 && !server.registered; ++i) usleep(100000);
    assert(server.registered);

    TcpTransport rogue;
    assert(connectTcp(rogue, "127.0.0.1", server.service.port()));
    Message bad_invoke(MessageType::MSG_INVOKE, 9001);
    bad_invoke.payload.writeUint32(IFACE_ID);
    bad_invoke.payload.writeUint32(METHOD_ECHO);
    bad_invoke.payload.writeUint32(0);
    assert(sendMessage(rogue, bad_invoke));
    Message invoke_reply;
    assert(recvFullMessage(rogue, invoke_reply, 2000));
    Buffer invoke_payload(invoke_reply.payload.data(), invoke_reply.payload.size());
    int32_t status = 0;
    assert(invoke_payload.tryReadInt32(status));
    assert(status == static_cast<int32_t>(ErrorCode::ERR_DESERIALIZE));
    assert(server.service.echoCount() == 0);
    rogue.close();

    OmniRuntime sub_runtime;
    assert(sub_runtime.init("127.0.0.1", SM_PORT_REQ) == 0);
    demo::ItemServiceProxy proxy(sub_runtime);
    std::atomic<int> topic_hits(0);
    proxy.SubscribeItemTopic([&topic_hits](const demo::ItemTopic&) { topic_hits++; });
    TcpTransport broadcast_rogue;
    assert(connectTcp(broadcast_rogue, "127.0.0.1", server.service.port()));
    Message bad_broadcast(MessageType::MSG_BROADCAST, 9002);
    bad_broadcast.payload.writeUint32(TOPIC_ID);
    assert(sendMessage(broadcast_rogue, bad_broadcast));
    for (int i = 0; i < 20; ++i) { sub_runtime.pollOnce(20); usleep(10000); }
    assert(topic_hits.load() == 0);
    broadcast_rogue.close();
    sub_runtime.stop();

    server.should_stop = true;
    pthread_join(server_tid, NULL);
    stopSM(sm_req);

    pid_t sm_reply = startSM(SM_PORT_REPLY);
    assert(sm_reply > 0);
    assert(waitSM(SM_PORT_REPLY));

    RawReplyCtx raw_ctx;
    raw_ctx.mode = RAW_REPLY_EMPTY_SUCCESS;
    pthread_t raw_tid = 0;
    assert(pthread_create(&raw_tid, NULL, rawReplyThread, &raw_ctx) == 0);
    for (int i = 0; i < 50 && !raw_ctx.ready; ++i) usleep(100000);
    assert(raw_ctx.ready);

    OmniRuntime reg_runtime;
    assert(reg_runtime.init("127.0.0.1", SM_PORT_REPLY) == 0);
    TcpTransport sm_conn;
    assert(connectTcp(sm_conn, "127.0.0.1", SM_PORT_REPLY));
    assert(registerFakeService(sm_conn, 9101, "ItemService", raw_ctx.port, "remote-raw-node"));

    OmniRuntime client_runtime;
    assert(client_runtime.init("127.0.0.1", SM_PORT_REPLY) == 0);
    demo::ItemServiceProxy reply_proxy(client_runtime);
    demo::Item input;
    input.id = 77;
    demo::Item result;
    int ret = reply_proxy.echoItem(input, &result);
    fprintf(stderr, "DEBUG: echoItem ret=%d\\n", ret);
    assert(ret == static_cast<int>(ErrorCode::ERR_DESERIALIZE));
    client_runtime.stop();
    reg_runtime.stop();
    sm_conn.close();

    pthread_join(raw_tid, NULL);
    stopSM(sm_reply);

    pid_t sm_reply_truncated_status = startSM(SM_PORT_REPLY);
    assert(sm_reply_truncated_status > 0);
    assert(waitSM(SM_PORT_REPLY));

    RawReplyCtx raw_status_ctx;
    raw_status_ctx.mode = RAW_REPLY_TRUNCATED_STATUS;
    pthread_t raw_status_tid = 0;
    assert(pthread_create(&raw_status_tid, NULL, rawReplyThread, &raw_status_ctx) == 0);
    for (int i = 0; i < 50 && !raw_status_ctx.ready; ++i) usleep(100000);
    assert(raw_status_ctx.ready);

    OmniRuntime reg_status_runtime;
    assert(reg_status_runtime.init("127.0.0.1", SM_PORT_REPLY) == 0);
    TcpTransport sm_status_conn;
    assert(connectTcp(sm_status_conn, "127.0.0.1", SM_PORT_REPLY));
    assert(registerFakeService(sm_status_conn, 9102, "ItemService", raw_status_ctx.port, "remote-raw-node"));

    OmniRuntime client_status_runtime;
    assert(client_status_runtime.init("127.0.0.1", SM_PORT_REPLY) == 0);
    demo::ItemServiceProxy status_proxy(client_status_runtime);
    demo::Item status_input;
    status_input.id = 78;
    demo::Item status_result;
    ret = status_proxy.echoItem(status_input, &status_result);
    assert(ret == static_cast<int>(ErrorCode::ERR_DESERIALIZE));
    client_status_runtime.stop();
    reg_status_runtime.stop();
    sm_status_conn.close();

    pthread_join(raw_status_tid, NULL);
    stopSM(sm_reply_truncated_status);

    pid_t sm_reply_truncated_length = startSM(SM_PORT_REPLY);
    assert(sm_reply_truncated_length > 0);
    assert(waitSM(SM_PORT_REPLY));

    RawReplyCtx raw_length_ctx;
    raw_length_ctx.mode = RAW_REPLY_TRUNCATED_LENGTH;
    pthread_t raw_length_tid = 0;
    assert(pthread_create(&raw_length_tid, NULL, rawReplyThread, &raw_length_ctx) == 0);
    for (int i = 0; i < 50 && !raw_length_ctx.ready; ++i) usleep(100000);
    assert(raw_length_ctx.ready);

    OmniRuntime reg_length_runtime;
    assert(reg_length_runtime.init("127.0.0.1", SM_PORT_REPLY) == 0);
    TcpTransport sm_length_conn;
    assert(connectTcp(sm_length_conn, "127.0.0.1", SM_PORT_REPLY));
    assert(registerFakeService(sm_length_conn, 9103, "ItemService", raw_length_ctx.port, "remote-raw-node"));

    OmniRuntime client_length_runtime;
    assert(client_length_runtime.init("127.0.0.1", SM_PORT_REPLY) == 0);
    demo::ItemServiceProxy length_proxy(client_length_runtime);
    demo::Item length_input;
    length_input.id = 79;
    demo::Item length_result;
    ret = length_proxy.echoItem(length_input, &length_result);
    assert(ret == static_cast<int>(ErrorCode::ERR_DESERIALIZE));
    client_length_runtime.stop();
    reg_length_runtime.stop();
    sm_length_conn.close();

    pthread_join(raw_length_tid, NULL);
    stopSM(sm_reply_truncated_length);
    return 0;
}
)CPP";

static const std::string kCHarnessTemplate = R"CPP(
#include "guarded_c.h"
#include <omnibinder/omnibinder.h>
#include <omnibinder/message.h>
#include "transport/tcp_transport.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static const uint16_t SM_PORT_REQ = __C_REQ_PORT__;
static const uint16_t SM_PORT_REPLY = __C_REPLY_PORT__;
static const uint32_t METHOD_ECHO = omni_fnv1a_32("echoItem");
static const uint32_t IFACE_ID = omni_fnv1a_32("demo.ItemService");
static const uint32_t TOPIC_ID = omni_fnv1a_32("ItemTopic");

static pid_t g_sm_pids[4] = {};
static int g_sm_count = 0;
static void cleanupSM() {
    for (int i = 0; i < g_sm_count; ++i) {
        if (g_sm_pids[i] > 0) {
            kill(g_sm_pids[i], SIGKILL);
            waitpid(g_sm_pids[i], NULL, 0);
        }
    }
}

static bool connectTcp(omnibinder::TcpTransport& transport, const std::string& host, uint16_t port) {
    int ret = transport.connect(host, port);
    if (ret < 0) return false;
    if (ret == 1) {
        for (int i = 0; i < 100; ++i) {
            transport.checkConnectComplete();
            if (transport.state() == omnibinder::ConnectionState::CONNECTED) return true;
            usleep(10000);
        }
        return false;
    }
    return transport.state() == omnibinder::ConnectionState::CONNECTED;
}

static bool sendMessage(omnibinder::TcpTransport& transport, const omnibinder::Message& msg) {
    omnibinder::Buffer out;
    msg.serialize(out);
    return transport.send(out.data(), out.size()) == static_cast<int>(out.size());
}

static bool recvFullMessage(omnibinder::TcpTransport& transport, omnibinder::Message& msg, int timeout_ms) {
    omnibinder::Buffer input;
    uint8_t buf[2048];
    int loops = timeout_ms / 20;
    for (int i = 0; i < loops; ++i) {
        int ret = transport.recv(buf, sizeof(buf));
        if (ret > 0) {
            input.writeRaw(buf, static_cast<size_t>(ret));
            if (input.size() >= omnibinder::MESSAGE_HEADER_SIZE) {
                omnibinder::MessageHeader hdr;
                if (!omnibinder::Message::parseHeader(input.data(), input.size(), hdr)) return false;
                size_t total = omnibinder::MESSAGE_HEADER_SIZE + hdr.length;
                if (input.size() >= total) {
                    msg.header = hdr;
                    if (hdr.length > 0) msg.payload.assign(input.data() + omnibinder::MESSAGE_HEADER_SIZE, hdr.length);
                    return true;
                }
            }
        }
        usleep(20000);
    }
    return false;
}

static bool registerFakeService(omnibinder::TcpTransport& transport, uint32_t seq, const std::string& name, uint16_t port, const std::string& host_id) {
    omnibinder::Message msg(omnibinder::MessageType::MSG_REGISTER, seq);
    omnibinder::ServiceInfo info;
    info.name = name;
    info.host = "127.0.0.1";
    info.port = port;
    info.host_id = host_id;
    omnibinder::InterfaceInfo iface;
    iface.interface_id = IFACE_ID;
    iface.name = name;
    iface.methods.push_back(omnibinder::MethodInfo(METHOD_ECHO, "echoItem"));
    info.interfaces.push_back(iface);
    omnibinder::serializeServiceInfo(info, msg.payload);
    if (!sendMessage(transport, msg)) return false;
    omnibinder::Message reply;
    if (!recvFullMessage(transport, reply, 2000)) return false;
    if (reply.getType() != omnibinder::MessageType::MSG_REGISTER_REPLY) return false;
    omnibinder::Buffer payload(reply.payload.data(), reply.payload.size());
    bool value = false;
    return payload.tryReadBool(value) && value;
}

static pid_t startSM(uint16_t port) {
    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        execl("__SERVICE_MANAGER__", "service_manager", "--port", port_str, "--log-level", "3", (char*)NULL);
        _exit(1);
    }
    if (pid > 0 && g_sm_count < 4) g_sm_pids[g_sm_count++] = pid;
    return pid;
}

static void stopSM(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid) return;
            usleep(100000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

static bool waitSM(uint16_t port) {
    for (int i = 0; i < 30; ++i) {
        omni_runtime_t* probe = omni_runtime_create();
        if (omni_runtime_init(probe, "127.0.0.1", port) == 0) {
            omni_runtime_stop(probe);
            omni_runtime_destroy(probe);
            return true;
        }
        omni_runtime_destroy(probe);
        usleep(100000);
    }
    return false;
}

struct ServerCtx {
    omni_runtime_t* runtime;
    omni_service_t* service;
    std::atomic<int> echo_count;
    volatile bool registered;
    volatile bool should_stop;
    ServerCtx() : runtime(NULL), service(NULL), echo_count(0), registered(false), should_stop(false) {}
};

static void cEchoItem(const struct demo_Item* item, struct demo_Item* result, void* user_data) {
    ServerCtx* ctx = static_cast<ServerCtx*>(user_data);
    ctx->echo_count++;
    result->id = item->id;
}

static void* serverThread(void* arg) {
    ServerCtx* ctx = static_cast<ServerCtx*>(arg);
    ctx->runtime = omni_runtime_create();
    assert(omni_runtime_init(ctx->runtime, "127.0.0.1", SM_PORT_REQ) == 0);
    demo_ItemService_callbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.echoItem = cEchoItem;
    cbs.user_data = ctx;
    ctx->service = demo_ItemService_stub_create_from_callbacks(&cbs);
    assert(ctx->service != NULL);
    assert(omni_runtime_register_service(ctx->runtime, ctx->service) == 0);
    assert(omni_runtime_publish_topic(ctx->runtime, "ItemTopic") == 0);
    ctx->registered = true;
    while (!ctx->should_stop) omni_runtime_poll_once(ctx->runtime, 20);
    omni_runtime_unregister_service(ctx->runtime, ctx->service);
    demo_ItemService_stub_destroy(ctx->service);
    omni_runtime_stop(ctx->runtime);
    omni_runtime_destroy(ctx->runtime);
    return NULL;
}

enum RawReplyMode {
    RAW_REPLY_EMPTY_SUCCESS = 0,
    RAW_REPLY_TRUNCATED_STATUS,
    RAW_REPLY_TRUNCATED_LENGTH
};

struct RawReplyCtx {
    omnibinder::TcpTransportServer server;
    uint16_t port;
    volatile bool ready;
    volatile bool done;
    RawReplyMode mode;
    RawReplyCtx() : port(0), ready(false), done(false), mode(RAW_REPLY_EMPTY_SUCCESS) {}
};

static void* rawReplyThread(void* arg) {
    RawReplyCtx* ctx = static_cast<RawReplyCtx*>(arg);
    int port = ctx->server.listen("127.0.0.1", 0);
    if (port <= 0) return NULL;
    ctx->port = static_cast<uint16_t>(port);
    ctx->ready = true;
    omnibinder::ITransport* accepted = NULL;
    for (int i = 0; i < 100 && !accepted; ++i) {
        accepted = ctx->server.accept();
        if (!accepted) usleep(50000);
    }
    if (!accepted) { ctx->done = true; return NULL; }
    uint8_t buf[4096];
    for (int i = 0; i < 100; ++i) {
        int ret = accepted->recv(buf, sizeof(buf));
        if (ret > 0) {
            omnibinder::Buffer input(buf, static_cast<size_t>(ret));
            omnibinder::MessageHeader hdr;
            assert(omnibinder::Message::parseHeader(input.data(), input.size(), hdr));
            omnibinder::Message reply(omnibinder::MessageType::MSG_INVOKE_REPLY, hdr.sequence);
            if (ctx->mode == RAW_REPLY_TRUNCATED_STATUS) {
                reply.payload.writeUint16(0);
            } else if (ctx->mode == RAW_REPLY_TRUNCATED_LENGTH) {
                reply.payload.writeInt32(0);
                reply.payload.writeUint16(0);
            } else {
                reply.payload.writeInt32(0);
                reply.payload.writeUint32(0);
            }
            omnibinder::Buffer out;
            reply.serialize(out);
            accepted->send(out.data(), out.size());
            break;
        }
        usleep(20000);
    }
    delete accepted;
    ctx->done = true;
    return NULL;
}

static void topicCallback(const demo_ItemTopic* msg, void* user_data) {
    (void)msg;
    std::atomic<int>* hits = static_cast<std::atomic<int>*>(user_data);
    (*hits)++;
}

int main() {
    atexit(cleanupSM);
    pid_t sm_req = startSM(SM_PORT_REQ);
    assert(sm_req > 0);
    assert(waitSM(SM_PORT_REQ));

    ServerCtx server;
    pthread_t server_tid = 0;
    assert(pthread_create(&server_tid, NULL, serverThread, &server) == 0);
    for (int i = 0; i < 50 && !server.registered; ++i) usleep(100000);
    assert(server.registered);

    omnibinder::TcpTransport rogue;
    assert(connectTcp(rogue, "127.0.0.1", omni_service_port(server.service)));
    omnibinder::Message bad_invoke(omnibinder::MessageType::MSG_INVOKE, 9201);
    bad_invoke.payload.writeUint32(IFACE_ID);
    bad_invoke.payload.writeUint32(METHOD_ECHO);
    bad_invoke.payload.writeUint32(0);
    assert(sendMessage(rogue, bad_invoke));
    omnibinder::Message invoke_reply;
    assert(recvFullMessage(rogue, invoke_reply, 2000));
    omnibinder::Buffer invoke_payload(invoke_reply.payload.data(), invoke_reply.payload.size());
    int32_t status = 0;
    assert(invoke_payload.tryReadInt32(status));
    assert(status == static_cast<int32_t>(omnibinder::ErrorCode::ERR_DESERIALIZE));
    assert(server.echo_count.load() == 0);
    rogue.close();

    omni_runtime_t* sub_runtime = omni_runtime_create();
    assert(omni_runtime_init(sub_runtime, "127.0.0.1", SM_PORT_REQ) == 0);
    demo_ItemService_proxy proxy;
    demo_ItemService_proxy_init(&proxy, sub_runtime);
    std::atomic<int> topic_hits(0);
    demo_ItemService_proxy_subscribe_item_topic(&proxy, topicCallback, &topic_hits);
    omnibinder::TcpTransport broadcast_rogue;
    assert(connectTcp(broadcast_rogue, "127.0.0.1", omni_service_port(server.service)));
    omnibinder::Message bad_broadcast(omnibinder::MessageType::MSG_BROADCAST, 9202);
    bad_broadcast.payload.writeUint32(TOPIC_ID);
    assert(sendMessage(broadcast_rogue, bad_broadcast));
    for (int i = 0; i < 20; ++i) { omni_runtime_poll_once(sub_runtime, 20); usleep(10000); }
    assert(topic_hits.load() == 0);
    broadcast_rogue.close();
    omni_runtime_stop(sub_runtime);
    omni_runtime_destroy(sub_runtime);

    server.should_stop = true;
    pthread_join(server_tid, NULL);
    stopSM(sm_req);

    pid_t sm_reply = startSM(SM_PORT_REPLY);
    assert(sm_reply > 0);
    assert(waitSM(SM_PORT_REPLY));

    RawReplyCtx raw_ctx;
    raw_ctx.mode = RAW_REPLY_EMPTY_SUCCESS;
    pthread_t raw_tid = 0;
    assert(pthread_create(&raw_tid, NULL, rawReplyThread, &raw_ctx) == 0);
    for (int i = 0; i < 50 && !raw_ctx.ready; ++i) usleep(100000);
    assert(raw_ctx.ready);

    omni_runtime_t* reg_runtime = omni_runtime_create();
    assert(omni_runtime_init(reg_runtime, "127.0.0.1", SM_PORT_REPLY) == 0);
    omnibinder::TcpTransport sm_conn;
    assert(connectTcp(sm_conn, "127.0.0.1", SM_PORT_REPLY));
    assert(registerFakeService(sm_conn, 9301, "ItemService", raw_ctx.port, std::string()));

    omni_runtime_t* client_runtime = omni_runtime_create();
    assert(omni_runtime_init(client_runtime, "127.0.0.1", SM_PORT_REPLY) == 0);
    demo_ItemService_proxy reply_proxy;
    demo_ItemService_proxy_init(&reply_proxy, client_runtime);
    struct demo_Item input;
    demo_Item_init(&input);
    input.id = 88;
    struct demo_Item result;
    demo_Item_init(&result);
    int ret = demo_ItemService_proxy_echo_item(&reply_proxy, &input, &result);
    assert(ret == -501);
    demo_Item_destroy(&input);
    demo_Item_destroy(&result);
    omni_runtime_stop(client_runtime);
    omni_runtime_destroy(client_runtime);
    omni_runtime_stop(reg_runtime);
    omni_runtime_destroy(reg_runtime);
    sm_conn.close();

    pthread_join(raw_tid, NULL);
    stopSM(sm_reply);

    pid_t sm_reply_truncated_status = startSM(SM_PORT_REPLY);
    assert(sm_reply_truncated_status > 0);
    assert(waitSM(SM_PORT_REPLY));

    RawReplyCtx raw_status_ctx;
    raw_status_ctx.mode = RAW_REPLY_TRUNCATED_STATUS;
    pthread_t raw_status_tid = 0;
    assert(pthread_create(&raw_status_tid, NULL, rawReplyThread, &raw_status_ctx) == 0);
    for (int i = 0; i < 50 && !raw_status_ctx.ready; ++i) usleep(100000);
    assert(raw_status_ctx.ready);

    omni_runtime_t* reg_status_runtime = omni_runtime_create();
    assert(omni_runtime_init(reg_status_runtime, "127.0.0.1", SM_PORT_REPLY) == 0);
    omnibinder::TcpTransport sm_status_conn;
    assert(connectTcp(sm_status_conn, "127.0.0.1", SM_PORT_REPLY));
    assert(registerFakeService(sm_status_conn, 9302, "ItemService", raw_status_ctx.port, std::string()));

    omni_runtime_t* client_status_runtime = omni_runtime_create();
    assert(omni_runtime_init(client_status_runtime, "127.0.0.1", SM_PORT_REPLY) == 0);
    demo_ItemService_proxy status_proxy;
    demo_ItemService_proxy_init(&status_proxy, client_status_runtime);
    struct demo_Item status_input;
    demo_Item_init(&status_input);
    status_input.id = 89;
    struct demo_Item status_result;
    demo_Item_init(&status_result);
    ret = demo_ItemService_proxy_echo_item(&status_proxy, &status_input, &status_result);
    assert(ret == -501);
    demo_Item_destroy(&status_input);
    demo_Item_destroy(&status_result);
    omni_runtime_stop(client_status_runtime);
    omni_runtime_destroy(client_status_runtime);
    omni_runtime_stop(reg_status_runtime);
    omni_runtime_destroy(reg_status_runtime);
    sm_status_conn.close();

    pthread_join(raw_status_tid, NULL);
    stopSM(sm_reply_truncated_status);

    pid_t sm_reply_truncated_length = startSM(SM_PORT_REPLY);
    assert(sm_reply_truncated_length > 0);
    assert(waitSM(SM_PORT_REPLY));

    RawReplyCtx raw_length_ctx;
    raw_length_ctx.mode = RAW_REPLY_TRUNCATED_LENGTH;
    pthread_t raw_length_tid = 0;
    assert(pthread_create(&raw_length_tid, NULL, rawReplyThread, &raw_length_ctx) == 0);
    for (int i = 0; i < 50 && !raw_length_ctx.ready; ++i) usleep(100000);
    assert(raw_length_ctx.ready);

    omni_runtime_t* reg_length_runtime = omni_runtime_create();
    assert(omni_runtime_init(reg_length_runtime, "127.0.0.1", SM_PORT_REPLY) == 0);
    omnibinder::TcpTransport sm_length_conn;
    assert(connectTcp(sm_length_conn, "127.0.0.1", SM_PORT_REPLY));
    assert(registerFakeService(sm_length_conn, 9303, "ItemService", raw_length_ctx.port, std::string()));

    omni_runtime_t* client_length_runtime = omni_runtime_create();
    assert(omni_runtime_init(client_length_runtime, "127.0.0.1", SM_PORT_REPLY) == 0);
    demo_ItemService_proxy length_proxy;
    demo_ItemService_proxy_init(&length_proxy, client_length_runtime);
    struct demo_Item length_input;
    demo_Item_init(&length_input);
    length_input.id = 90;
    struct demo_Item length_result;
    demo_Item_init(&length_result);
    ret = demo_ItemService_proxy_echo_item(&length_proxy, &length_input, &length_result);
    assert(ret == -501);
    demo_Item_destroy(&length_input);
    demo_Item_destroy(&length_result);
    omni_runtime_stop(client_length_runtime);
    omni_runtime_destroy(client_length_runtime);
    omni_runtime_stop(reg_length_runtime);
    omni_runtime_destroy(reg_length_runtime);
    sm_length_conn.close();

    pthread_join(raw_length_tid, NULL);
    stopSM(sm_reply_truncated_length);
    return 0;
}
)CPP";

class GeneratedRuntimeTest : public ::testing::Test {
protected:
    static std::string dir_;
    static std::string idl_path_;
    static std::string cpp_harness_path_;
    static std::string c_harness_path_;

    static std::string getIncludeFlags() {
        return std::string("-DOMNIBINDER_LINUX") +
               " -I" + shellQuote(std::string(OMNI_SOURCE_DIR) + "/include") +
               " -I" + shellQuote(std::string(OMNI_SOURCE_DIR) + "/src") +
               " -I" + shellQuote(dir_);
    }

    static std::string getLibPath() {
        return std::string(OMNI_BUILD_DIR) + "/target/lib/libomnibinder.a";
    }

    static void SetUpTestSuite() {
        system("pkill -f 'service_manager --port 1996' 2>/dev/null || true");
        usleep(200000);

        char dir_template[] = "/tmp/omnibinder_generated_runtime_XXXXXX";
        char* dir_path = mkdtemp(dir_template);
        ASSERT_TRUE(dir_path != NULL);
        dir_ = dir_path;

        const std::string idl =
            "package demo;\n"
            "struct Item {\n"
            "    int32 id;\n"
            "}\n"
            "topic ItemTopic {\n"
            "    Item item;\n"
            "}\n"
            "service ItemService {\n"
            "    Item echoItem(Item item);\n"
            "    publishes ItemTopic;\n"
            "}";

        idl_path_ = dir_ + "/guarded.bidl";
        writeFile(idl_path_, idl);

        AstFile ast;
        ParseContext ctx;
        ASSERT_TRUE(parseFile(idl_path_, ast, ctx));
        ASSERT_EQ(ctx.loaded_packages.count("demo"), 1u);

        CppCodeGen cpp_gen;
        CCodeGen c_gen;
        ASSERT_TRUE(cpp_gen.generate(ast, dir_, "guarded"));
        ASSERT_TRUE(c_gen.generate(ast, dir_, "guarded"));

        std::string cpp_harness = kCppHarnessTemplate;
        cpp_harness = replaceAll(cpp_harness, "__CPP_REQ_PORT__", "19961");
        cpp_harness = replaceAll(cpp_harness, "__CPP_REPLY_PORT__", "19962");
        cpp_harness = replaceAll(cpp_harness, "__SERVICE_MANAGER__", std::string(OMNI_BUILD_DIR) + "/target/bin/service_manager");

        std::string c_harness = kCHarnessTemplate;
        c_harness = replaceAll(c_harness, "__C_REQ_PORT__", "19963");
        c_harness = replaceAll(c_harness, "__C_REPLY_PORT__", "19964");
        c_harness = replaceAll(c_harness, "__SERVICE_MANAGER__", std::string(OMNI_BUILD_DIR) + "/target/bin/service_manager");

        cpp_harness_path_ = dir_ + "/generated_cpp_harness.cpp";
        c_harness_path_ = dir_ + "/generated_c_harness.cpp";
        writeFile(cpp_harness_path_, cpp_harness);
        writeFile(c_harness_path_, c_harness);
    }

    static void TearDownTestSuite() {
        unlink(idl_path_.c_str());
    }
};

std::string GeneratedRuntimeTest::dir_;
std::string GeneratedRuntimeTest::idl_path_;
std::string GeneratedRuntimeTest::cpp_harness_path_;
std::string GeneratedRuntimeTest::c_harness_path_;

TEST_F(GeneratedRuntimeTest, CompileGeneratedCppRuntimeHarness) {
    std::string cmd = std::string("g++ -std=c++11 ") + getIncludeFlags() +
        " " + shellQuote(cpp_harness_path_) +
        " " + shellQuote(dir_ + "/guarded.cpp") +
        " " + shellQuote(getLibPath()) +
        " -lpthread -lrt -o " + shellQuote(dir_ + "/generated_cpp_harness");
    ASSERT_TRUE(runCommand(cmd));
}

TEST_F(GeneratedRuntimeTest, RunGeneratedCppRuntimeHarness) {
    ASSERT_TRUE(runCommand(shellQuote(dir_ + "/generated_cpp_harness")));
}

TEST_F(GeneratedRuntimeTest, CompileGeneratedCRuntimeHarness) {
    std::string cmd = std::string("g++ -std=c++11 ") + getIncludeFlags() +
        " " + shellQuote(c_harness_path_) +
        " " + shellQuote(dir_ + "/guarded.c") +
        " " + shellQuote(getLibPath()) +
        " -lpthread -lrt -o " + shellQuote(dir_ + "/generated_c_harness");
    ASSERT_TRUE(runCommand(cmd));
}

TEST_F(GeneratedRuntimeTest, RunGeneratedCRuntimeHarness) {
    ASSERT_TRUE(runCommand(shellQuote(dir_ + "/generated_c_harness")));
}
