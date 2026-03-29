#include <omnibinder/buffer.h>
#include <cstdio>
#include <cstring>
#include <cassert>

using namespace omnibinder;

template<typename T>
static T mustRead(Buffer& buf, bool (Buffer::*fn)(T&)) {
    T value = T();
    if (!(buf.*fn)(value)) {
        fprintf(stderr, "mustRead failed\n");
        abort();
    }
    return value;
}

static std::string mustReadString(Buffer& buf) {
    std::string value;
    if (!buf.tryReadString(value)) {
        fprintf(stderr, "mustReadString failed\n");
        abort();
    }
    return value;
}

static std::vector<uint8_t> mustReadBytes(Buffer& buf) {
    std::vector<uint8_t> value;
    if (!buf.tryReadBytes(value)) {
        fprintf(stderr, "mustReadBytes failed\n");
        abort();
    }
    return value;
}

#define TEST(name) printf("  TEST %s ... ", #name);
#define PASS() printf("PASS\n");

int main() {
    printf("=== Buffer Tests ===\n");
    
    TEST(basic_types) {
        Buffer buf;
        buf.writeBool(true); buf.writeBool(false);
        buf.writeInt8(-42); buf.writeUint8(200);
        buf.writeInt16(-1234); buf.writeUint16(50000);
        buf.writeInt32(-100000); buf.writeUint32(3000000000u);
        buf.writeInt64(-9876543210LL); buf.writeUint64(12345678901234ULL);
        buf.writeFloat32(3.14f); buf.writeFloat64(2.718281828);
        
        assert(mustRead<bool>(buf, &Buffer::tryReadBool) == true);
        assert(mustRead<bool>(buf, &Buffer::tryReadBool) == false);
        assert(mustRead<int8_t>(buf, &Buffer::tryReadInt8) == -42);
        assert(mustRead<uint8_t>(buf, &Buffer::tryReadUint8) == 200);
        assert(mustRead<int16_t>(buf, &Buffer::tryReadInt16) == -1234);
        assert(mustRead<uint16_t>(buf, &Buffer::tryReadUint16) == 50000);
        assert(mustRead<int32_t>(buf, &Buffer::tryReadInt32) == -100000);
        assert(mustRead<uint32_t>(buf, &Buffer::tryReadUint32) == 3000000000u);
        assert(mustRead<int64_t>(buf, &Buffer::tryReadInt64) == -9876543210LL);
        assert(mustRead<uint64_t>(buf, &Buffer::tryReadUint64) == 12345678901234ULL);
        float f = mustRead<float>(buf, &Buffer::tryReadFloat32);
        double d = mustRead<double>(buf, &Buffer::tryReadFloat64);
        if (!(f > 3.13f && f < 3.15f)) abort();
        if (!(d > 2.71 && d < 2.72)) abort();
        PASS();
    }
    
    TEST(string) {
        Buffer buf;
        buf.writeString("Hello, World!");
        buf.writeString("");
        buf.writeString("OmniBinder");
        
        if (mustReadString(buf) != "Hello, World!") abort();
        if (mustReadString(buf) != "") abort();
        if (mustReadString(buf) != "OmniBinder") abort();
        PASS();
    }
    
    TEST(bytes) {
        Buffer buf;
        std::vector<uint8_t> data = {1, 2, 3, 4, 5};
        buf.writeBytes(data);
        std::vector<uint8_t> out = mustReadBytes(buf);
        assert(out.size() == 5);
        assert(out[0] == 1 && out[4] == 5);
        PASS();
    }
    
    TEST(move) {
        Buffer buf1;
        buf1.writeInt32(42);
        Buffer buf2(static_cast<Buffer&&>(buf1));
        assert(mustRead<int32_t>(buf2, &Buffer::tryReadInt32) == 42);
        assert(buf1.size() == 0);
        PASS();
    }
    
    TEST(assign) {
        uint8_t data[] = {0x01, 0x00, 0x00, 0x00};
        Buffer buf;
        buf.assign(data, 4);
        assert(mustRead<int32_t>(buf, &Buffer::tryReadInt32) == 1);
        PASS();
    }
    
    printf("\nAll buffer tests passed!\n");
    return 0;
}
