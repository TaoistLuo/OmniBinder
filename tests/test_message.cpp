#include <gtest/gtest.h>
#include <omnibinder/message.h>

using namespace omnibinder;

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

TEST(MessageTest, DeclaredPayloadMustFitAvailableFrame) {
    Message msg(MessageType::MSG_INVOKE, 7);
    ASSERT_TRUE(msg.payload.writeUint32(0x12345678));
    Buffer serialized;
    ASSERT_TRUE(msg.serialize(serialized));

    // 保留 message 分帧边界上的畸形 SHM 帧场景：
    // header 声明 8 字节 payload，但实际只有 4 字节可用。
    serialized.mutableData()[12] = 0x08;
    serialized.mutableData()[13] = 0x00;
    serialized.mutableData()[14] = 0x00;
    serialized.mutableData()[15] = 0x00;

    MessageHeader header;
    ASSERT_TRUE(Message::parseHeader(serialized.data(), serialized.size(), header));
    ASSERT_TRUE(Message::validateHeader(header));
    EXPECT_GT(MESSAGE_HEADER_SIZE + static_cast<size_t>(header.length),
              serialized.size());

    // 生产分帧路径必须把"声明长度超出可用字节"判为 NeedMore（等待更多数据）
    MessageHeader extracted;
    size_t frame_size = 0;
    EXPECT_EQ(tryExtractFrame(serialized.data(), serialized.size(), extracted, frame_size),
              FrameStatus::NeedMore);

    // 头部长度字段超过协议上限时，生产校验必须判为 Corrupt
    Buffer corrupt;
    Message oversized(MessageType::MSG_INVOKE, 9);
    ASSERT_TRUE(oversized.serialize(corrupt));
    corrupt.mutableData()[12] = 0xFF;
    corrupt.mutableData()[13] = 0xFF;
    corrupt.mutableData()[14] = 0xFF;
    corrupt.mutableData()[15] = 0xFF;
    MessageHeader corrupt_header;
    EXPECT_EQ(tryExtractFrame(corrupt.data(), corrupt.size(), corrupt_header, frame_size),
              FrameStatus::Corrupt);
}

TEST(MessageTest, ServiceInfoSerializeDeserialize) {
    ServiceInfo info;
    info.name = "TestService";
    info.host = "192.168.1.10";
    info.port = 8080;
    info.host_id = "abc123";
    info.instance_id = "inst-7";
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
    EXPECT_EQ(info2.instance_id, "inst-7");
    EXPECT_EQ(info2.interfaces.size(), 1u);
    EXPECT_EQ(info2.interfaces[0].interface_id, 0x12345678u);
    EXPECT_EQ(info2.interfaces[0].methods[0].name, "doSomething");
}

TEST(MessageTest, ServiceInfoWireFormatExact) {
    ServiceInfo info;
    info.name = "svc";
    info.host = "host";
    info.port = 42;
    info.host_id = "node";
    info.instance_id = "inst";
    info.shm_config = ShmConfig(1024, 2048);

    Buffer encoded;
    serializeServiceInfo(info, encoded);

    Buffer expected;
    expected.writeString("svc");
    expected.writeString("host");
    expected.writeUint16(42);
    expected.writeString("node");
    expected.writeString("inst");
    expected.writeUint32(1024);
    expected.writeUint32(2048);
    expected.writeUint16(0);
    ASSERT_EQ(encoded.size(), expected.size());
    EXPECT_EQ(memcmp(encoded.data(), expected.data(), expected.size()), 0);
}

TEST(MessageTest, ServiceIdentityPayloadRoundTrip) {
    Buffer encoded;
    ASSERT_TRUE(writeServiceIdentityPayload(encoded, "SvcA", "host-1", "inst-a"));

    std::string name;
    std::string host_id;
    std::string instance_id;
    ASSERT_TRUE(readServiceIdentityPayload(encoded, name, host_id, instance_id));
    EXPECT_EQ(name, "SvcA");
    EXPECT_EQ(host_id, "host-1");
    EXPECT_EQ(instance_id, "inst-a");

    // 空 host_id / instance_id（默认单实例）是合法身份
    Buffer default_instance;
    ASSERT_TRUE(writeServiceIdentityPayload(default_instance, "SvcA", "host-1", ""));
    ASSERT_TRUE(readServiceIdentityPayload(default_instance, name, host_id, instance_id));
    EXPECT_EQ(name, "SvcA");
    EXPECT_EQ(host_id, "host-1");
    EXPECT_TRUE(instance_id.empty());
}

TEST(MessageTest, ServiceIdentityPayloadRejectsMalformedAndTruncated) {
    const std::string keep_name = "keep";
    const std::string keep_host = "keep-host";
    const std::string keep_instance = "keep-inst";
    std::string name = keep_name;
    std::string host_id = keep_host;
    std::string instance_id = keep_instance;

    Buffer empty;
    EXPECT_FALSE(readServiceIdentityPayload(empty, name, host_id, instance_id));

    Buffer only_name;
    only_name.writeString("SvcA");
    EXPECT_FALSE(readServiceIdentityPayload(only_name, name, host_id, instance_id));

    Buffer missing_instance;
    missing_instance.writeString("SvcA");
    missing_instance.writeString("host-1");
    EXPECT_FALSE(readServiceIdentityPayload(missing_instance, name, host_id, instance_id));

    // 恶意长度前缀：声明 1GB 但无数据，不得越界读
    Buffer huge_length;
    huge_length.writeUint32(0x40000000u);
    EXPECT_FALSE(readServiceIdentityPayload(huge_length, name, host_id, instance_id));

    Buffer empty_name;
    empty_name.writeString("");
    empty_name.writeString("host-1");
    empty_name.writeString("inst-a");
    EXPECT_FALSE(readServiceIdentityPayload(empty_name, name, host_id, instance_id));

    Buffer oversized;
    oversized.writeString(std::string(MAX_SERVICE_NAME_LENGTH + 1, 'x'));
    oversized.writeString("host-1");
    oversized.writeString("inst-a");
    EXPECT_FALSE(readServiceIdentityPayload(oversized, name, host_id, instance_id));

    Buffer trailing;
    ASSERT_TRUE(writeServiceIdentityPayload(trailing, "SvcA", "host-1", "inst-a"));
    trailing.writeUint8(1);
    EXPECT_FALSE(readServiceIdentityPayload(trailing, name, host_id, instance_id));

    // 失败时不改动输出参数
    EXPECT_EQ(name, keep_name);
    EXPECT_EQ(host_id, keep_host);
    EXPECT_EQ(instance_id, keep_instance);
}

TEST(MessageTest, InstanceDeathMessageTypeStrings) {
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_DEATH_NOTIFY_INSTANCE),
                 "DEATH_NOTIFY_INSTANCE");
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_SUBSCRIBE_SERVICE_INSTANCE),
                 "SUBSCRIBE_SERVICE_INSTANCE");
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_UNSUBSCRIBE_SERVICE_INSTANCE),
                 "UNSUBSCRIBE_SERVICE_INSTANCE");
}

TEST(MessageTest, PublishedTopicsReplyRoundTrip) {
    std::vector<std::string> expected;
    expected.push_back("alpha/topic");
    expected.push_back("zeta/topic");
    Buffer encoded;
    ASSERT_TRUE(serializePublishedTopicsReply(true, expected, encoded));

    bool found = false;
    std::vector<std::string> actual;
    ASSERT_TRUE(deserializePublishedTopicsReply(encoded, found, actual));
    EXPECT_TRUE(found);
    EXPECT_EQ(actual, expected);
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_QUERY_PUBLISHED_TOPICS),
                 "QUERY_PUBLISHED_TOPICS");
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY),
                 "QUERY_PUBLISHED_TOPICS_REPLY");
}

TEST(MessageTest, ListInstancesMessageTypeStrings) {
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_LIST_INSTANCES), "LIST_INSTANCES");
    EXPECT_STREQ(messageTypeToString(MessageType::MSG_LIST_INSTANCES_REPLY),
                 "LIST_INSTANCES_REPLY");
}

TEST(MessageTest, PublishedTopicsReplyDistinguishesMissingAndEmpty) {
    Buffer missing;
    ASSERT_TRUE(serializePublishedTopicsReply(false, std::vector<std::string>(), missing));
    bool found = true;
    std::vector<std::string> topics(1, "stale");
    ASSERT_TRUE(deserializePublishedTopicsReply(missing, found, topics));
    EXPECT_FALSE(found);
    EXPECT_TRUE(topics.empty());

    Buffer empty;
    ASSERT_TRUE(serializePublishedTopicsReply(true, std::vector<std::string>(), empty));
    ASSERT_TRUE(deserializePublishedTopicsReply(empty, found, topics));
    EXPECT_TRUE(found);
    EXPECT_TRUE(topics.empty());
}

TEST(MessageTest, PublishedTopicsReplyRejectsMalformedAndOversizedLists) {
    bool found = false;
    std::vector<std::string> topics(1, "unchanged");

    Buffer impossible_count;
    impossible_count.writeBool(true);
    impossible_count.writeUint32(2);
    EXPECT_FALSE(deserializePublishedTopicsReply(impossible_count, found, topics));
    EXPECT_EQ(topics[0], "unchanged");

    Buffer oversized_name;
    oversized_name.writeBool(true);
    oversized_name.writeUint32(1);
    oversized_name.writeString(std::string(MAX_TOPIC_NAME_LENGTH + 1, 'x'));
    EXPECT_FALSE(deserializePublishedTopicsReply(oversized_name, found, topics));

    Buffer truncated;
    truncated.writeBool(true);
    truncated.writeUint32(1);
    truncated.writeUint32(8);
    truncated.writeRaw("short", 5);
    EXPECT_FALSE(deserializePublishedTopicsReply(truncated, found, topics));

    Buffer trailing;
    ASSERT_TRUE(serializePublishedTopicsReply(true, std::vector<std::string>(), trailing));
    trailing.writeUint8(1);
    EXPECT_FALSE(deserializePublishedTopicsReply(trailing, found, topics));
}

TEST(MessageTest, PublishedTopicsReplyAcceptsCountAndAggregateBoundaries) {
    std::vector<std::string> count_boundary(MAX_PUBLISHED_TOPICS, "x");
    Buffer count_encoded;
    ASSERT_TRUE(serializePublishedTopicsReply(true, count_boundary, count_encoded));
    bool found = false;
    std::vector<std::string> count_decoded;
    ASSERT_TRUE(deserializePublishedTopicsReply(count_encoded, found, count_decoded));
    EXPECT_EQ(count_decoded.size(), MAX_PUBLISHED_TOPICS);

    std::vector<std::string> topics(
        MAX_PUBLISHED_TOPICS_BYTES / MAX_TOPIC_NAME_LENGTH,
        std::string(MAX_TOPIC_NAME_LENGTH, 'x'));
    ASSERT_EQ(topics.size() * topics[0].size(), MAX_PUBLISHED_TOPICS_BYTES);

    Buffer encoded;
    ASSERT_TRUE(serializePublishedTopicsReply(true, topics, encoded));
    std::vector<std::string> decoded;
    ASSERT_TRUE(deserializePublishedTopicsReply(encoded, found, decoded));
    EXPECT_TRUE(found);
    EXPECT_EQ(decoded.size(), topics.size());
    EXPECT_EQ(decoded.front().size(), MAX_TOPIC_NAME_LENGTH);
}

TEST(MessageTest, PublishedTopicsReplyRejectsCountAndAggregateOverflowTransactionally) {
    Buffer destination;
    destination.writeString("prefix");
    const size_t original_size = destination.size();

    std::vector<std::string> too_many(MAX_PUBLISHED_TOPICS + 1, "x");
    EXPECT_FALSE(serializePublishedTopicsReply(true, too_many, destination));
    EXPECT_EQ(destination.size(), original_size);

    std::vector<std::string> empty_name(1, std::string());
    EXPECT_FALSE(serializePublishedTopicsReply(true, empty_name, destination));
    EXPECT_EQ(destination.size(), original_size);

    std::vector<std::string> aggregate_overflow(
        MAX_PUBLISHED_TOPICS_BYTES / MAX_TOPIC_NAME_LENGTH + 1,
        std::string(MAX_TOPIC_NAME_LENGTH, 'x'));
    EXPECT_FALSE(serializePublishedTopicsReply(true, aggregate_overflow, destination));
    EXPECT_EQ(destination.size(), original_size);

    Buffer aggregate_payload;
    aggregate_payload.writeBool(true);
    aggregate_payload.writeUint32(static_cast<uint32_t>(aggregate_overflow.size()));
    for (size_t i = 0; i < aggregate_overflow.size(); ++i) {
        aggregate_payload.writeString(aggregate_overflow[i]);
    }
    bool aggregate_found = false;
    std::vector<std::string> aggregate_decoded(1, "unchanged");
    EXPECT_FALSE(deserializePublishedTopicsReply(
        aggregate_payload, aggregate_found, aggregate_decoded));
    EXPECT_EQ(aggregate_decoded[0], "unchanged");

    Buffer excessive_count;
    excessive_count.writeBool(true);
    excessive_count.writeUint32(static_cast<uint32_t>(MAX_PUBLISHED_TOPICS + 1));
    for (size_t i = 0; i < MAX_PUBLISHED_TOPICS + 1; ++i) {
        excessive_count.writeString("x");
    }
    bool found = false;
    std::vector<std::string> decoded(1, "unchanged");
    EXPECT_FALSE(deserializePublishedTopicsReply(excessive_count, found, decoded));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0], "unchanged");
}
