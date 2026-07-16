#include <gtest/gtest.h>
#include "info_formatter.h"

TEST(OmniCliInfoFormatterTest, FormatsPublishedTopicsExactly) {
    std::vector<std::string> topics;
    topics.push_back("alpha/topic");
    topics.push_back("zeta/topic");
    EXPECT_EQ(omni_cli::formatPublishedTopicsSection(topics, ""),
              "  Published Topics:\n"
              "    - alpha/topic\n"
              "    - zeta/topic\n"
              "\n");
}

TEST(OmniCliInfoFormatterTest, FormatsEmptyAndUnavailableExactly) {
    EXPECT_EQ(omni_cli::formatPublishedTopicsSection(std::vector<std::string>(), ""),
              "  Published Topics:\n"
              "    (none)\n"
              "\n");
    EXPECT_EQ(omni_cli::formatPublishedTopicsSection(
                  std::vector<std::string>(), "Not supported"),
              "  Published Topics:\n"
              "    (unavailable: Not supported)\n"
              "\n");
}

TEST(OmniCliInfoFormatterTest, EscapesTerminalControlsExactly) {
    std::vector<std::string> topics;
    topics.push_back(std::string("evil\x1B[31m\n\r\t", 12));
    topics.push_back(std::string("nul\0del\x7F", 8));
    EXPECT_EQ(omni_cli::formatPublishedTopicsSection(topics, ""),
              "  Published Topics:\n"
              "    - evil\\x1B[31m\\x0A\\x0D\\x09\n"
              "    - nul\\x00del\\x7F\n"
              "\n");
}

TEST(OmniCliInfoFormatterTest, PreservesPrintableUtf8AndEscapesC1AndMalformedBytes) {
    const std::string printable = u8"温度/状态/😀";
    EXPECT_EQ(omni_cli::escapeForTerminal(printable), printable);
    EXPECT_EQ(omni_cli::escapeForTerminal(std::string("\xC2\x9B", 2)),
              "\\xC2\\x9B");
    EXPECT_EQ(omni_cli::escapeForTerminal(std::string("\x9B\xF0\x9F", 3)),
              "\\x9B\\xF0\\x9F");
    EXPECT_EQ(omni_cli::escapeForTerminal(std::string("\xE2\x80\xAE", 3)),
              "\\xE2\\x80\\xAE");
    EXPECT_EQ(omni_cli::formatPublishedTopicsSection(
                  std::vector<std::string>(), std::string("old\nSM\x1B", 7)),
              "  Published Topics:\n"
              "    (unavailable: old\\x0ASM\\x1B)\n"
              "\n");
}
