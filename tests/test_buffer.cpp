#include <omnibinder/buffer.h>
#include <cstdio>
#include <cstring>
#include <cassert>

using namespace omnibinder;

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
        
        assert(buf.readBool() == true);
        assert(buf.readBool() == false);
        assert(buf.readInt8() == -42);
        assert(buf.readUint8() == 200);
        assert(buf.readInt16() == -1234);
        assert(buf.readUint16() == 50000);
        assert(buf.readInt32() == -100000);
        assert(buf.readUint32() == 3000000000u);
        assert(buf.readInt64() == -9876543210LL);
        assert(buf.readUint64() == 12345678901234ULL);
        float f = buf.readFloat32(); assert(f > 3.13f && f < 3.15f);
        double d = buf.readFloat64(); assert(d > 2.71 && d < 2.72);
        PASS();
    }
    
    TEST(string) {
        Buffer buf;
        buf.writeString("Hello, World!");
        buf.writeString("");
        buf.writeString("OmniBinder");
        
        assert(buf.readString() == "Hello, World!");
        assert(buf.readString() == "");
        assert(buf.readString() == "OmniBinder");
        PASS();
    }
    
    TEST(bytes) {
        Buffer buf;
        std::vector<uint8_t> data = {1, 2, 3, 4, 5};
        buf.writeBytes(data);
        std::vector<uint8_t> out = buf.readBytes();
        assert(out.size() == 5);
        assert(out[0] == 1 && out[4] == 5);
        PASS();
    }
    
    TEST(move) {
        Buffer buf1;
        buf1.writeInt32(42);
        Buffer buf2(static_cast<Buffer&&>(buf1));
        assert(buf2.readInt32() == 42);
        assert(buf1.size() == 0);
        PASS();
    }
    
    TEST(assign) {
        uint8_t data[] = {0x01, 0x00, 0x00, 0x00};
        Buffer buf;
        buf.assign(data, 4);
        assert(buf.readInt32() == 1);
        PASS();
    }
    
    printf("\nAll buffer tests passed!\n");
    return 0;
}
