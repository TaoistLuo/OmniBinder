#include <gtest/gtest.h>
#include "test_common.h"

using namespace omnibinder;
using namespace omnibinder::test;

TEST(MessageTest, SerializeDeserialize) {
    Message msg(MessageType::MSG_REGISTER, 42);
    msg.payload.writeString("TestService");
    msg.payload.writeUint16(8080);

    Buffer output;
    ASSERT_TRUE(msg.serialize(output));
    EXPECT_EQ(output.size(), MESSAGE_HEADER_SIZE + msg.payload.size());

    MessageHeader hdr;
    ASSERT_TRUE(Message::parseHeader(output.data(), output.size(), hdr));
    EXPECT_EQ(hdr.magic, OMNI_MAGIC);
    EXPECT_EQ(hdr.version, OMNI_VERSION);
    EXPECT_EQ(hdr.type, static_cast<uint16_t>(MessageType::MSG_REGISTER));
    EXPECT_EQ(hdr.sequence, 42u);
    EXPECT_EQ(hdr.length, msg.payload.size());
}

TEST(MessageTest, ServiceInfoSerializeDeserialize) {
    ServiceInfo info;
    info.name = "TestService";
    info.host = "192.168.1.10";
    info.port = 8080;
    info.host_id = "abc123";
    InterfaceInfo iface;
    iface.interface_id = 0x12345678;
    iface.name = "ITest";
    iface.methods.push_back(MethodInfo(0xAABBCCDD, "doSomething"));
    info.interfaces.push_back(iface);

    Buffer buf;
    serializeServiceInfo(info, buf);

    ServiceInfo info2;
    ASSERT_TRUE(deserializeServiceInfo(buf, info2));
    EXPECT_EQ(info2.name, "TestService");
    EXPECT_EQ(info2.host, "192.168.1.10");
    EXPECT_EQ(info2.port, 8080);
    EXPECT_EQ(info2.host_id, "abc123");
    EXPECT_EQ(info2.interfaces.size(), 1u);
    EXPECT_EQ(info2.interfaces[0].interface_id, 0x12345678u);
    EXPECT_EQ(info2.interfaces[0].methods[0].name, "doSomething");
}
