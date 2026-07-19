#include <gtest/gtest.h>
#include <omnibinder/error.h>

#include <cstddef>
#include <cstdint>
#include <string>

using omnibinder::ErrorCode;
using omnibinder::errorCodeToString;

namespace {

struct ErrorExpectation {
    ErrorCode code;
    int32_t value;
    const char* text;
};

const ErrorExpectation kPublicErrors[] = {
    {ErrorCode::OK,                       0, "OK"},
    {ErrorCode::ERR_UNKNOWN,             -1, "Unknown error"},
    {ErrorCode::ERR_INVALID_PARAM,       -2, "Invalid parameter"},
    {ErrorCode::ERR_OUT_OF_MEMORY,       -3, "Out of memory"},
    {ErrorCode::ERR_TIMEOUT,             -4, "Timeout"},
    {ErrorCode::ERR_NOT_INITIALIZED,     -5, "Not initialized"},
    {ErrorCode::ERR_ALREADY_INITIALIZED, -6, "Already initialized"},
    {ErrorCode::ERR_NOT_SUPPORTED,       -7, "Not supported"},
    {ErrorCode::ERR_INTERNAL,            -8, "Internal error"},
    {ErrorCode::ERR_CONNECT_FAILED,      -100, "Connection failed"},
    {ErrorCode::ERR_CONNECTION_CLOSED,   -101, "Connection closed"},
    {ErrorCode::ERR_SEND_FAILED,         -102, "Send failed"},
    {ErrorCode::ERR_RECV_FAILED,         -103, "Receive failed"},
    {ErrorCode::ERR_PROTOCOL_ERROR,      -104, "Protocol error"},
    {ErrorCode::ERR_SM_UNREACHABLE,      -105, "ServiceManager unreachable"},
    {ErrorCode::ERR_BIND_FAILED,         -106, "Bind failed"},
    {ErrorCode::ERR_LISTEN_FAILED,       -107, "Listen failed"},
    {ErrorCode::ERR_ACCEPT_FAILED,       -108, "Accept failed"},
    {ErrorCode::ERR_SERVICE_NOT_FOUND,   -200, "Service not found"},
    {ErrorCode::ERR_SERVICE_EXISTS,      -201, "Service already exists"},
    {ErrorCode::ERR_SERVICE_OFFLINE,     -202, "Service offline"},
    {ErrorCode::ERR_INTERFACE_NOT_FOUND, -203, "Interface not found"},
    {ErrorCode::ERR_METHOD_NOT_FOUND,    -204, "Method not found"},
    {ErrorCode::ERR_INVOKE_FAILED,       -205, "Invoke failed"},
    {ErrorCode::ERR_REGISTER_FAILED,     -206, "Register failed"},
    {ErrorCode::ERR_UNREGISTER_FAILED,   -207, "Unregister failed"},
    {ErrorCode::ERR_IDL_MISMATCH,        -208, "IDL mismatch"},
    {ErrorCode::ERR_TOPIC_NOT_FOUND,     -300, "Topic not found"},
    {ErrorCode::ERR_TOPIC_EXISTS,        -301, "Topic already exists"},
    {ErrorCode::ERR_NOT_SUBSCRIBED,      -302, "Not subscribed"},
    {ErrorCode::ERR_NOT_PUBLISHER,       -303, "Not a publisher"},
    {ErrorCode::ERR_TRANSPORT_INIT,      -400, "Transport init failed"},
    {ErrorCode::ERR_SHM_CREATE,          -401, "Shared memory create failed"},
    {ErrorCode::ERR_SHM_ATTACH,          -402, "Shared memory attach failed"},
    {ErrorCode::ERR_SHM_FULL,            -403, "Shared memory full"},
    {ErrorCode::ERR_SHM_TIMEOUT,         -404, "Shared memory timeout"},
    {ErrorCode::ERR_SERIALIZE,           -500, "Serialize error"},
    {ErrorCode::ERR_DESERIALIZE,         -501, "Deserialize error"},
    {ErrorCode::ERR_BUFFER_OVERFLOW,     -502, "Buffer overflow"},
    {ErrorCode::ERR_BUFFER_UNDERFLOW,    -503, "Buffer underflow"},
};

TEST(ErrorCodeCompatibilityTest, PublicValuesAndStringsRemainStable) {
    for (size_t i = 0; i < sizeof(kPublicErrors) / sizeof(kPublicErrors[0]); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(static_cast<int32_t>(kPublicErrors[i].code), kPublicErrors[i].value);
        EXPECT_EQ(std::string(errorCodeToString(kPublicErrors[i].code)),
                  kPublicErrors[i].text);
    }
}

TEST(ErrorCodeCompatibilityTest, UnknownValueUsesStableFallbackText) {
    EXPECT_STREQ(errorCodeToString(static_cast<ErrorCode>(1234567)),
                 "Unknown error code");
}

} // namespace
