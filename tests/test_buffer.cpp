#include <gtest/gtest.h>
#include "test_common.h"

using namespace omnibinder;
using namespace omnibinder::test;

TEST(BufferTest, BasicTypes) {
    Buffer buf;
    buf.writeBool(true); buf.writeBool(false);
    buf.writeInt8(-42); buf.writeUint8(200);
    buf.writeInt16(-1234); buf.writeUint16(50000);
    buf.writeInt32(-100000); buf.writeUint32(3000000000u);
    buf.writeInt64(-9876543210LL); buf.writeUint64(12345678901234ULL);
    buf.writeFloat32(3.14f); buf.writeFloat64(2.718281828);

    EXPECT_EQ(mustRead<bool>(buf, &Buffer::tryReadBool), true);
    EXPECT_EQ(mustRead<bool>(buf, &Buffer::tryReadBool), false);
    EXPECT_EQ(mustRead<int8_t>(buf, &Buffer::tryReadInt8), -42);
    EXPECT_EQ(mustRead<uint8_t>(buf, &Buffer::tryReadUint8), 200);
    EXPECT_EQ(mustRead<int16_t>(buf, &Buffer::tryReadInt16), -1234);
    EXPECT_EQ(mustRead<uint16_t>(buf, &Buffer::tryReadUint16), 50000);
    EXPECT_EQ(mustRead<int32_t>(buf, &Buffer::tryReadInt32), -100000);
    EXPECT_EQ(mustRead<uint32_t>(buf, &Buffer::tryReadUint32), 3000000000u);
    EXPECT_EQ(mustRead<int64_t>(buf, &Buffer::tryReadInt64), -9876543210LL);
    EXPECT_EQ(mustRead<uint64_t>(buf, &Buffer::tryReadUint64), 12345678901234ULL);
    float f = mustRead<float>(buf, &Buffer::tryReadFloat32);
    double d = mustRead<double>(buf, &Buffer::tryReadFloat64);
    EXPECT_GT(f, 3.13f);
    EXPECT_LT(f, 3.15f);
    EXPECT_GT(d, 2.71);
    EXPECT_LT(d, 2.72);
}

TEST(BufferTest, String) {
    Buffer buf;
    buf.writeString("Hello, World!");
    buf.writeString("");
    buf.writeString("OmniBinder");

    EXPECT_EQ(mustReadString(buf), "Hello, World!");
    EXPECT_EQ(mustReadString(buf), "");
    EXPECT_EQ(mustReadString(buf), "OmniBinder");
}

TEST(BufferTest, Bytes) {
    Buffer buf;
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    buf.writeBytes(data);
    std::vector<uint8_t> out = mustReadBytes(buf);
    EXPECT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[4], 5);
}

TEST(BufferTest, Move) {
    Buffer buf1;
    buf1.writeInt32(42);
    Buffer buf2(static_cast<Buffer&&>(buf1));
    EXPECT_EQ(mustRead<int32_t>(buf2, &Buffer::tryReadInt32), 42);
    EXPECT_EQ(buf1.size(), 0u);
}

TEST(BufferTest, Assign) {
    uint8_t data[] = {0x01, 0x00, 0x00, 0x00};
    Buffer buf;
    buf.assign(data, 4);
    EXPECT_EQ(mustRead<int32_t>(buf, &Buffer::tryReadInt32), 1);
}
