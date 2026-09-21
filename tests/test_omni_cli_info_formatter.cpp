#include <gtest/gtest.h>
#include "info_formatter.h"

#include <algorithm>

namespace {

omni_cli::ServiceInstanceView makeInstance(const std::string& name,
                                           const std::string& host_id,
                                           const std::string& instance_id,
                                           const std::string& host,
                                           uint16_t port) {
    omni_cli::ServiceInstanceView view;
    view.name = name;
    view.host_id = host_id;
    view.instance_id = instance_id;
    view.host = host;
    view.port = port;
    return view;
}

omni_cli::InstanceSelector makeSelector(bool host_id_set, const std::string& host_id,
                                        bool instance_id_set, const std::string& instance_id) {
    omni_cli::InstanceSelector selector;
    selector.host_id_set = host_id_set;
    selector.host_id = host_id;
    selector.instance_id_set = instance_id_set;
    selector.instance_id = instance_id;
    return selector;
}

} // namespace

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

// ============================================================
// 多实例服务身份：list / info / call 的展示与选择
// ============================================================

TEST(OmniCliInstanceModelTest, DisplayInstanceIdMarksDefault) {
    EXPECT_EQ(omni_cli::displayInstanceId("ttyUSB0"), "ttyUSB0");
    EXPECT_EQ(omni_cli::displayInstanceId(""), "(default)");
    EXPECT_EQ(omni_cli::displayInstanceId(std::string("\x1B", 1)), "\\x1B");
}

TEST(OmniCliInstanceModelTest, ListsMultipleInstancesInOneTable) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("SensorService", "board-a", "ttyUSB0", "10.0.0.1", 35451));
    instances.push_back(makeInstance("SensorService", "board-b", "ttyUSB1", "10.0.0.2", 35452));

    const std::string expected =
        "NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS\n"
        "------------------------ -------------------- -------------------------------- --------------------- ------\n"
        "SensorService            ttyUSB0              board-a                          10.0.0.1:35451        ONLINE\n"
        "SensorService            ttyUSB1              board-b                          10.0.0.2:35452        ONLINE\n";
    EXPECT_EQ(omni_cli::formatInstanceTable(instances), expected);
}

TEST(OmniCliInstanceModelTest, SingleInstanceIsOneRowOfSamePresentation) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("SensorService", "board-a", "ttyUSB0", "10.0.0.1", 35451));

    const std::string table = omni_cli::formatInstanceTable(instances);
    EXPECT_EQ(static_cast<int>(std::count(table.begin(), table.end(), '\n')), 3)
        << "single instance = header + separator + exactly one data row";
    EXPECT_NE(table.find("SensorService            ttyUSB0              board-a"),
              std::string::npos);
}

TEST(OmniCliInstanceModelTest, RendersDefaultInstanceAsDefaultRow) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("SensorService", "board-a", "", "127.0.0.1", 35451));

    EXPECT_EQ(omni_cli::formatInstanceListRow(instances[0]),
              "SensorService            (default)            board-a                          "
              "127.0.0.1:35451       ONLINE\n");
}

TEST(OmniCliInstanceModelTest, EscapesInstanceFieldsInRows) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance(std::string("Svc\x1B", 4), "host", std::string("i\x0A", 2),
                                     "127.0.0.1", 1));

    const std::string row = omni_cli::formatInstanceListRow(instances[0]);
    EXPECT_NE(row.find("Svc\\x1B"), std::string::npos);
    EXPECT_NE(row.find("i\\x0A"), std::string::npos);
    EXPECT_EQ(row.find('\x1B'), std::string::npos);
    EXPECT_EQ(row.find('\x0A'), row.size() - 1) << "only the trailing newline may be a raw LF";
}

TEST(OmniCliInstanceModelTest, SelectsInstanceByIdAndHost) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("Svc", "host-a", "inst-a", "10.0.0.1", 1));
    instances.push_back(makeInstance("Svc", "host-b", "inst-b", "10.0.0.2", 2));

    omni_cli::ServiceInstanceView selected;
    std::string error;

    EXPECT_EQ(omni_cli::selectInstance(
                  instances, makeSelector(false, "", true, "inst-b"), selected, error), 0);
    EXPECT_EQ(selected.instance_id, "inst-b");
    EXPECT_TRUE(error.empty());

    EXPECT_EQ(omni_cli::selectInstance(
                  instances, makeSelector(true, "host-a", true, "inst-a"), selected, error), 0);
    EXPECT_EQ(selected.host_id, "host-a");
    EXPECT_EQ(selected.instance_id, "inst-a");
}

TEST(OmniCliInstanceModelTest, SelectsInstanceByHostOnlyWhenUnique) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("Svc", "host-a", "inst-a", "10.0.0.1", 1));
    instances.push_back(makeInstance("Svc", "host-b", "inst-b", "10.0.0.2", 2));

    omni_cli::ServiceInstanceView selected;
    std::string error;
    EXPECT_EQ(omni_cli::selectInstance(
                  instances, makeSelector(true, "host-b", false, ""), selected, error), 0);
    EXPECT_EQ(selected.instance_id, "inst-b");
}

TEST(OmniCliInstanceModelTest, RejectsAmbiguousInstanceSelection) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("Svc", "host-a", "dev-a", "10.0.0.1", 1));
    instances.push_back(makeInstance("Svc", "host-b", "dev-a", "10.0.0.2", 2));

    omni_cli::ServiceInstanceView selected;
    std::string error;
    EXPECT_EQ(omni_cli::selectInstance(
                  instances, makeSelector(false, "", true, "dev-a"), selected, error), 2);
    EXPECT_NE(error.find("multiple instances match"), std::string::npos);
    EXPECT_NE(error.find("--host-id"), std::string::npos);

    std::vector<omni_cli::ServiceInstanceView> same_host;
    same_host.push_back(makeInstance("Svc", "host-a", "inst-a", "10.0.0.1", 1));
    same_host.push_back(makeInstance("Svc", "host-a", "inst-b", "10.0.0.2", 2));
    EXPECT_EQ(omni_cli::selectInstance(
                  same_host, makeSelector(true, "host-a", false, ""), selected, error), 2);
}

TEST(OmniCliInstanceModelTest, RejectsUnknownInstanceSelection) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("Svc", "host-a", "inst-a", "10.0.0.1", 1));

    omni_cli::ServiceInstanceView selected;
    std::string error;
    EXPECT_EQ(omni_cli::selectInstance(
                  instances, makeSelector(false, "", true, "missing"), selected, error), 1);
    EXPECT_NE(error.find("no instance matches"), std::string::npos);
    EXPECT_NE(error.find("missing"), std::string::npos);
}

TEST(OmniCliInstanceModelTest, DefaultsToDeterministicFirstInstance) {
    std::vector<omni_cli::ServiceInstanceView> instances;
    instances.push_back(makeInstance("Svc", "host-a", "inst-a", "10.0.0.1", 1));
    instances.push_back(makeInstance("Svc", "host-b", "inst-b", "10.0.0.2", 2));

    omni_cli::ServiceInstanceView selected;
    std::string error;
    EXPECT_EQ(omni_cli::selectInstance(
                  instances, makeSelector(false, "", false, ""), selected, error), 0);
    EXPECT_EQ(selected.instance_id, "inst-a");
    EXPECT_EQ(selected.host_id, "host-a");
}

TEST(OmniCliInstanceModelTest, FormatsInstanceIdentity) {
    EXPECT_EQ(omni_cli::formatInstanceIdentity(
                  makeInstance("Svc", "board-a", "ttyUSB0", "10.0.0.1", 35451)),
              "Instance: instance_id=\"ttyUSB0\" host_id=\"board-a\" "
              "endpoint=10.0.0.1:35451");
    EXPECT_EQ(omni_cli::formatInstanceIdentity(
                  makeInstance("Svc", "board-a", "", "127.0.0.1", 35451)),
              "Instance: instance_id=\"\" (default) host_id=\"board-a\" "
              "endpoint=127.0.0.1:35451");
}

TEST(OmniCliInstanceModelTest, FormatsCallTargetNotice) {
    const omni_cli::ServiceInstanceView target =
        makeInstance("Svc", "board-b", "ttyUSB1", "10.0.0.2", 35452);

    EXPECT_EQ(omni_cli::formatCallTargetNotice(target, true),
              "Target instance: instance_id=\"ttyUSB1\" host_id=\"board-b\" "
              "endpoint=10.0.0.2:35452");

    EXPECT_EQ(omni_cli::formatCallTargetNotice(target, false),
              "No instance selector given; using deterministic first instance: "
              "instance_id=\"ttyUSB1\" host_id=\"board-b\" endpoint=10.0.0.2:35452"
              "\n  Hint: use --instance-id <id> [--host-id <id>] to target another instance.");
}

TEST(OmniCliInstanceModelTest, FormatsSelectionHintForMultiInstanceInfo) {
    EXPECT_EQ(omni_cli::formatInstanceSelectionHint(),
              "Multiple instances found; not selecting one automatically. "
              "Use --instance-id <id> [--host-id <id>] to inspect a specific instance.\n");
}
