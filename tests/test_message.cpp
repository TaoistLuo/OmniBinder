#include <omnibinder/message.h>
#include <cstdio>
#include <cassert>

using namespace omnibinder;

int main() {
    printf("=== Message Tests ===\n");
    
    printf("  TEST message_serialize ... ");
    {
        Message msg(MessageType::MSG_REGISTER, 42);
        msg.payload.writeString("TestService");
        msg.payload.writeUint16(8080);
        
        Buffer output;
        assert(msg.serialize(output));
        assert(output.size() == MESSAGE_HEADER_SIZE + msg.payload.size());
        
        MessageHeader hdr;
        assert(Message::parseHeader(output.data(), output.size(), hdr));
        assert(hdr.magic == OMNI_MAGIC);
        assert(hdr.version == OMNI_VERSION);
        assert(hdr.type == static_cast<uint16_t>(MessageType::MSG_REGISTER));
        assert(hdr.sequence == 42);
        assert(hdr.length == msg.payload.size());
    }
    printf("PASS\n");
    
    printf("  TEST service_info_serialize ... ");
    {
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
        assert(deserializeServiceInfo(buf, info2));
        assert(info2.name == "TestService");
        assert(info2.host == "192.168.1.10");
        assert(info2.port == 8080);
        assert(info2.host_id == "abc123");
        assert(info2.interfaces.size() == 1);
        assert(info2.interfaces[0].interface_id == 0x12345678);
        assert(info2.interfaces[0].methods[0].name == "doSomething");
    }
    printf("PASS\n");
    
    printf("\nAll message tests passed!\n");
    return 0;
}
