#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/omnibinder_c.h>
#include <pthread.h>

using namespace omnibinder::test;

static const uint16_t SM_PORT = 19941;
static const uint32_t IFACE_ID = 0x11223344u;
static const uint32_t METHOD_ECHO = 0x55667788u;
static const uint32_t METHOD_GET_NAME = 0x55667789u;
static const uint32_t METHOD_ECHO_BYTES = 0x5566778au;

static int cEchoCallback(uint32_t method_id, const omni_buffer_t* request,
                          omni_buffer_t* response, void* user_data) {
    (void)user_data;
    if (method_id == METHOD_ECHO) {
        uint32_t len = 0;
        char* payload = omni_buffer_read_string(const_cast<omni_buffer_t*>(request), &len);
        if (payload == NULL) return -501;
        omni_buffer_write_string(response, payload, len);
        free(payload);
    } else if (method_id == METHOD_GET_NAME) {
        omni_buffer_write_string(response, "c-api-service", 13);
    } else if (method_id == METHOD_ECHO_BYTES) {
        uint32_t bytes_len = 0;
        uint8_t* bytes = omni_buffer_read_bytes(const_cast<omni_buffer_t*>(request), &bytes_len);
        omni_buffer_write_bytes(response, bytes, bytes_len);
        free(bytes);
    }
    return 0;
}

struct CApiServerCtx {
    omni_runtime_t* runtime;
    omni_service_t* service;
    volatile bool registered;
    volatile bool should_stop;
    CApiServerCtx() : runtime(NULL), service(NULL), registered(false), should_stop(false) {}
};

static void* cApiServerThread(void* arg) {
    CApiServerCtx* ctx = static_cast<CApiServerCtx*>(arg);
    int ret = omni_runtime_init(ctx->runtime, "127.0.0.1", SM_PORT);
    if (ret != 0) return NULL;
    ret = omni_runtime_register_service(ctx->runtime, ctx->service);
    if (ret != 0) return NULL;
    ctx->registered = true;
    while (!ctx->should_stop) omni_runtime_poll_once(ctx->runtime, 20);
    omni_runtime_unregister_service(ctx->runtime, ctx->service);
    omni_runtime_stop(ctx->runtime);
    return NULL;
}

class CApiTest : public ::testing::Test {
protected:
    static pid_t sm_pid_;
    static CApiServerCtx* server_ctx_;
    static pthread_t server_tid_;

    static void SetUpTestSuite() {
        sm_pid_ = startProcess("./target/bin/service_manager", "--port", "19941", "--log-level", "3");
        ASSERT_GT(sm_pid_, 0);
        ASSERT_TRUE(waitPortReady(SM_PORT, 30));

        server_ctx_ = new CApiServerCtx();
        server_ctx_->runtime = omni_runtime_create();
        server_ctx_->service = omni_service_create("CService", IFACE_ID, cEchoCallback, NULL);
        ASSERT_NE(server_ctx_->runtime, nullptr);
        ASSERT_NE(server_ctx_->service, nullptr);
        omni_service_add_method_ex(server_ctx_->service, METHOD_ECHO, "Echo", "std::string", "std::string");
        omni_service_add_method_ex(server_ctx_->service, METHOD_GET_NAME, "GetName", "", "std::string");
        omni_service_add_method_ex(server_ctx_->service, METHOD_ECHO_BYTES, "EchoBytes", "std::vector<uint8_t>", "std::vector<uint8_t>");

        ASSERT_EQ(pthread_create(&server_tid_, NULL, cApiServerThread, server_ctx_), 0);
        for (int i = 0; i < 50 && !server_ctx_->registered; ++i) usleep(100000);
        ASSERT_TRUE(server_ctx_->registered);
    }

    static void TearDownTestSuite() {
        server_ctx_->should_stop = true;
        pthread_join(server_tid_, NULL);
        omni_service_destroy(server_ctx_->service);
        omni_runtime_destroy(server_ctx_->runtime);
        delete server_ctx_;
        stopProcess(sm_pid_);
    }
};

pid_t CApiTest::sm_pid_ = 0;
CApiServerCtx* CApiTest::server_ctx_ = nullptr;
pthread_t CApiTest::server_tid_ = 0;

TEST_F(CApiTest, BufferRoundtripAndHash) {
    omni_buffer_t* buf = omni_buffer_create();
    ASSERT_NE(buf, nullptr);
    omni_buffer_write_int32(buf, 1234);
    omni_buffer_write_string(buf, "hello-c-api", 11);
    EXPECT_GT(omni_buffer_size(buf), 0u);
    omni_buffer_reset(buf);
    EXPECT_EQ(omni_buffer_read_int32(buf), 1234);
    uint32_t len = 0;
    char* text = omni_buffer_read_string(buf, &len);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(len, 11u);
    EXPECT_STREQ(text, "hello-c-api");
    free(text);

    omni_buffer_t* empty = omni_buffer_create();
    ASSERT_NE(empty, nullptr);
    omni_buffer_write_string(empty, NULL, 0);
    omni_buffer_reset(empty);
    uint32_t empty_len = 99;
    char* empty_text = omni_buffer_read_string(empty, &empty_len);
    ASSERT_NE(empty_text, nullptr);
    EXPECT_EQ(empty_len, 0u);
    EXPECT_STREQ(empty_text, "");
    free(empty_text);
    omni_buffer_destroy(empty);

    EXPECT_NE(omni_fnv1a_32("Echo"), 0u);
    omni_buffer_destroy(buf);
}

TEST_F(CApiTest, BufferUnderflowReadsFailSafe) {
    omni_buffer_t* b = omni_buffer_create();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(omni_buffer_read_bool(b), 0);
    EXPECT_EQ(omni_buffer_read_ok(b), 0);
    omni_buffer_destroy(b);

    omni_buffer_t* si8 = omni_buffer_create();
    EXPECT_EQ(omni_buffer_read_int8(si8), 0);
    EXPECT_EQ(omni_buffer_read_ok(si8), 0);
    omni_buffer_destroy(si8);

    omni_buffer_t* si16 = omni_buffer_create();
    EXPECT_EQ(omni_buffer_read_int16(si16), 0);
    EXPECT_EQ(omni_buffer_read_ok(si16), 0);
    omni_buffer_destroy(si16);

    omni_buffer_t* si32 = omni_buffer_create();
    omni_buffer_write_uint16(si32, 0x1234u);
    omni_buffer_reset(si32);
    EXPECT_EQ(omni_buffer_read_int32(si32), 0);
    EXPECT_EQ(omni_buffer_read_ok(si32), 0);
    omni_buffer_destroy(si32);

    omni_buffer_t* marked = omni_buffer_create();
    ASSERT_NE(marked, nullptr);
    omni_buffer_mark_error(marked, -501);
    EXPECT_EQ(omni_buffer_error(marked), -501);
    EXPECT_EQ(omni_buffer_read_ok(marked), 0);
    omni_buffer_clear_error(marked);
    EXPECT_EQ(omni_buffer_error(marked), 0);
    EXPECT_EQ(omni_buffer_read_ok(marked), 1);
    omni_buffer_destroy(marked);
}

TEST_F(CApiTest, RegisterInvokeAndStats) {
    omni_runtime_t* runtime = omni_runtime_create();
    ASSERT_NE(runtime, nullptr);
    ASSERT_EQ(omni_runtime_init(runtime, "127.0.0.1", SM_PORT), 0);

    omni_buffer_t* req = omni_buffer_create();
    omni_buffer_t* resp = omni_buffer_create();
    ASSERT_NE(req, nullptr);
    ASSERT_NE(resp, nullptr);
    omni_buffer_write_string(req, "rpc-through-c-api", 17);

    ASSERT_EQ(omni_runtime_connect_service(runtime, "CService"), 0);
    ASSERT_EQ(omni_runtime_invoke(runtime, "CService", IFACE_ID, METHOD_ECHO, req, resp, 3000), 0);

    uint32_t len = 0;
    char* echoed = omni_buffer_read_string(resp, &len);
    ASSERT_NE(echoed, nullptr);
    EXPECT_EQ(len, 17u);
    EXPECT_STREQ(echoed, "rpc-through-c-api");
    free(echoed);

    omni_runtime_stats_t stats;
    ASSERT_EQ(omni_runtime_get_stats(runtime, &stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 1u);
    EXPECT_EQ(stats.total_rpc_success, 1u);
    EXPECT_EQ(stats.total_rpc_failures, 0u);

    omni_runtime_reset_stats(runtime);
    ASSERT_EQ(omni_runtime_get_stats(runtime, &stats), 0);
    EXPECT_EQ(stats.total_rpc_calls, 0u);

    omni_buffer_destroy(req);
    omni_buffer_destroy(resp);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
}

TEST_F(CApiTest, StringAndBytesMethods) {
    omni_runtime_t* runtime = omni_runtime_create();
    ASSERT_NE(runtime, nullptr);
    ASSERT_EQ(omni_runtime_init(runtime, "127.0.0.1", SM_PORT), 0);
    ASSERT_EQ(omni_runtime_connect_service(runtime, "CService"), 0);

    omni_buffer_t* req = omni_buffer_create();
    omni_buffer_t* resp = omni_buffer_create();
    ASSERT_NE(req, nullptr);
    ASSERT_NE(resp, nullptr);

    ASSERT_EQ(omni_runtime_invoke(runtime, "CService", IFACE_ID, METHOD_GET_NAME, req, resp, 3000), 0);
    uint32_t name_len = 0;
    char* name = omni_buffer_read_string(resp, &name_len);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name_len, 13u);
    EXPECT_STREQ(name, "c-api-service");
    free(name);

    omni_buffer_reset(req);
    omni_buffer_reset(resp);
    const uint8_t payload[] = {0x00, 0x11, 0x22, 0x33, 0xfe, 0xff};
    omni_buffer_write_bytes(req, payload, sizeof(payload));
    ASSERT_EQ(omni_runtime_invoke(runtime, "CService", IFACE_ID, METHOD_ECHO_BYTES, req, resp, 3000), 0);
    uint32_t out_len = 0;
    uint8_t* out = omni_buffer_read_bytes(resp, &out_len);
    EXPECT_EQ(out_len, sizeof(payload));
    EXPECT_EQ(std::memcmp(out, payload, sizeof(payload)), 0);
    free(out);

    omni_buffer_destroy(req);
    omni_buffer_destroy(resp);
    omni_runtime_stop(runtime);
    omni_runtime_destroy(runtime);
}
