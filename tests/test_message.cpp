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

TEST(MessageTest, ServiceInfoWireFormatRemainsLegacyCompatible) {
    ServiceInfo info;
    info.name = "svc";
    info.host = "host";
    info.port = 42;
    info.host_id = "node";
    info.shm_config = ShmConfig(1024, 2048);

    Buffer encoded;
    serializeServiceInfo(info, encoded);

    Buffer legacy;
    legacy.writeString("svc");
    legacy.writeString("host");
    legacy.writeUint16(42);
    legacy.writeString("node");
    legacy.writeUint32(1024);
    legacy.writeUint32(2048);
    legacy.writeUint16(0);
    ASSERT_EQ(encoded.size(), legacy.size());
    EXPECT_EQ(memcmp(encoded.data(), legacy.data(), legacy.size()), 0);
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
