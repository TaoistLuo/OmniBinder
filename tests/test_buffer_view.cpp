#include <gtest/gtest.h>
#include "omnibinder/buffer.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/string_view.h"
#include "omnibinder/omnibinder_c.h"
#include "omnibinder/omnibinder_c_view.h"

#include <cstring>
#include <string>

using namespace omnibinder;

TEST(BufferViewTest, StringAndBytesAreZeroCopy) {
    Buffer buf;
    ASSERT_TRUE(buf.writeUint32(42));
    ASSERT_TRUE(buf.writeString("hello", 5));
    const uint8_t bytes[] = {0x01, 0x02, 0x03};
    ASSERT_TRUE(buf.writeBytes(bytes, sizeof(bytes)));

    BufferView view(buf.data(), buf.size());
    uint32_t value = 0;
    ASSERT_TRUE(view.tryReadUint32(value));
    EXPECT_EQ(value, 42u);

    const char* text = NULL;
    uint32_t text_len = 0;
    ASSERT_TRUE(view.tryReadStringView(text, text_len));
    EXPECT_EQ(text_len, 5u);
    EXPECT_EQ(std::memcmp(text, "hello", 5), 0);
    EXPECT_GE(text, reinterpret_cast<const char*>(buf.data()));
    EXPECT_LT(text, reinterpret_cast<const char*>(buf.data()) + buf.size());

    const uint8_t* data = NULL;
    uint32_t data_len = 0;
    ASSERT_TRUE(view.tryReadBytesView(data, data_len));
    EXPECT_EQ(data_len, 3u);
    EXPECT_EQ(std::memcmp(data, bytes, 3), 0);
    EXPECT_GE(data, buf.data());
    EXPECT_LT(data, buf.data() + buf.size());
}

TEST(BufferViewTest, StringViewHoldsPointerAndLength) {
    const char raw[] = {'a', 'b', 'c'};
    StringView view(raw, sizeof(raw));
    EXPECT_EQ(view.data(), raw);
    EXPECT_EQ(view.size(), 3u);
    EXPECT_FALSE(view.empty());

    ByteView bytes(reinterpret_cast<const uint8_t*>(raw), sizeof(raw));
    EXPECT_EQ(bytes.size(), 3u);
}

TEST(BufferViewTest, CBufferViewReadsWithoutAllocation) {
    omni_buffer_t* msg = omni_buffer_create();
    ASSERT_NE(msg, nullptr);
    omni_buffer_write_uint32(msg, 7);
    omni_buffer_write_string(msg, "payload", 7);
    const uint8_t bytes[] = {0xaa, 0xbb};
    omni_buffer_write_bytes(msg, bytes, sizeof(bytes));

    omni_buffer_view_t view;
    omni_buffer_view_from_buffer(&view, msg);
    EXPECT_EQ(omni_buffer_view_read_uint32(&view), 7u);

    uint32_t len = 0;
    const char* text = omni_buffer_view_read_string(&view, &len);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(len, 7u);
    EXPECT_EQ(std::memcmp(text, "payload", 7), 0);
    EXPECT_GE(text, reinterpret_cast<const char*>(omni_buffer_data(msg)));

    const uint8_t* data = omni_buffer_view_read_bytes(&view, &len);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(len, 2u);
    EXPECT_EQ(std::memcmp(data, bytes, 2), 0);
    EXPECT_TRUE(omni_buffer_view_ok(&view));

    omni_buffer_destroy(msg);
}

TEST(BufferViewTest, CppStringViewRejectsTruncatedInput) {
    Buffer buf;
    ASSERT_TRUE(buf.writeUint32(5));
    ASSERT_TRUE(buf.writeRaw("a", 1));
    BufferView view(buf.data(), buf.size());
    const char* text = NULL;
    uint32_t len = 0;
    EXPECT_FALSE(view.tryReadStringView(text, len));
    EXPECT_EQ(view.readPosition(), 0u);
}

TEST(BufferViewTest, CBufferViewRejectsTruncatedInput) {
    const uint8_t raw[] = {0x05, 0x00, 0x00, 0x00, 'a'};
    omni_buffer_view_t view;
    omni_buffer_view_init(&view, raw, sizeof(raw));
    uint32_t len = 0;
    EXPECT_EQ(omni_buffer_view_read_string(&view, &len), nullptr);
    EXPECT_FALSE(omni_buffer_view_ok(&view));
    EXPECT_EQ(omni_buffer_view_position(&view), 0u);
}
