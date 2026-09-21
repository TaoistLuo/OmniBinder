#include <gtest/gtest.h>
#include "test_common.h"
#include "service_registry.h"

using namespace omnibinder;
using namespace omnibinder::test;

static ServiceInfo makeInfo(const std::string& name,
                            const std::string& host = "127.0.0.1",
                            uint16_t port = 8080,
                            const std::string& host_id = "",
                            const std::string& instance_id = "") {
    ServiceInfo info;
    info.name = name;
    info.host = host;
    info.port = port;
    info.host_id = host_id;
    info.instance_id = instance_id;
    return info;
}

// ============================================================
// addService
// ============================================================

TEST(ServiceRegistryTest, AddBasic) {
    ServiceRegistry reg;
    EXPECT_NE(reg.addService(makeInfo("svc.audio"), 10), INVALID_HANDLE);
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.exists("svc.audio"));
}

TEST(ServiceRegistryTest, AddDuplicateNameTakesOver) {
    ServiceRegistry reg;
    const ServiceHandle h1 = reg.addService(makeInfo("svc.dup"), 10);
    ASSERT_NE(h1, INVALID_HANDLE);

    int displaced_fd = -1;
    const ServiceHandle h2 = reg.addService(makeInfo("svc.dup"), 11, &displaced_fd);
    EXPECT_EQ(h2, h1);            // 名称身份保持，handle 不变
    EXPECT_EQ(displaced_fd, 10);  // 旧控制连接被顶替
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_EQ(reg.getControlFd("svc.dup"), 11);
    EXPECT_TRUE(reg.ownsService(11, "svc.dup"));
    EXPECT_FALSE(reg.ownsService(10, "svc.dup"));
}

TEST(ServiceRegistryTest, AddDuplicateSameFdHasNoDisplacedFd) {
    ServiceRegistry reg;
    const ServiceHandle h1 = reg.addService(makeInfo("svc.dupsame"), 10);
    ASSERT_NE(h1, INVALID_HANDLE);

    int displaced_fd = -1;
    const ServiceHandle h2 = reg.addService(makeInfo("svc.dupsame"), 10, &displaced_fd);
    EXPECT_EQ(h2, h1);
    EXPECT_EQ(displaced_fd, -1);
    EXPECT_EQ(reg.count(), 1u);
}

TEST(ServiceRegistryTest, AddDuplicateDifferentHostCoexists) {
    ServiceRegistry reg;
    ServiceInfo a = makeInfo("svc.cross", "127.0.0.1", 8080, "host-a");
    ASSERT_NE(reg.addService(a, 10), INVALID_HANDLE);

    ServiceInfo b = makeInfo("svc.cross", "127.0.0.1", 8081, "host-b");
    int displaced_fd = -1;
    EXPECT_NE(reg.addService(b, 11, &displaced_fd), INVALID_HANDLE);
    EXPECT_EQ(displaced_fd, -1);   // 异身份新增，不顶替
    EXPECT_EQ(reg.count(), 2u);    // 两板同名实例并存

    ServiceEntry ea;
    ServiceEntry eb;
    EXPECT_TRUE(reg.findServiceByIdentity(ServiceIdentity("svc.cross", "host-a", ""), ea));
    EXPECT_TRUE(reg.findServiceByIdentity(ServiceIdentity("svc.cross", "host-b", ""), eb));
    EXPECT_EQ(ea.control_fd, 10);
    EXPECT_EQ(eb.control_fd, 11);
}

TEST(ServiceRegistryTest, AddSameNameDifferentInstanceCoexistsOnSameHost) {
    ServiceRegistry reg;
    ServiceInfo a = makeInfo("svc.multi", "127.0.0.1", 8080, "host-1", "inst-a");
    ServiceInfo b = makeInfo("svc.multi", "127.0.0.1", 8081, "host-1", "inst-b");

    ASSERT_NE(reg.addService(a, 10), INVALID_HANDLE);
    int displaced_fd = -1;
    EXPECT_NE(reg.addService(b, 11, &displaced_fd), INVALID_HANDLE);
    EXPECT_EQ(displaced_fd, -1);
    EXPECT_EQ(reg.count(), 2u);
    EXPECT_TRUE(reg.exists("svc.multi"));

    ServiceEntry ea;
    ServiceEntry eb;
    ASSERT_TRUE(reg.findServiceByIdentity(ServiceIdentity("svc.multi", "host-1", "inst-a"), ea));
    ASSERT_TRUE(reg.findServiceByIdentity(ServiceIdentity("svc.multi", "host-1", "inst-b"), eb));
    EXPECT_EQ(ea.control_fd, 10);
    EXPECT_EQ(eb.control_fd, 11);
    EXPECT_EQ(ea.info.instance_id, "inst-a");
    EXPECT_EQ(eb.info.instance_id, "inst-b");

    EXPECT_TRUE(reg.ownsService(10, "svc.multi"));
    EXPECT_TRUE(reg.ownsService(11, "svc.multi"));
    EXPECT_EQ(reg.listServices().size(), 2u);
}

TEST(ServiceRegistryTest, AddSameFullIdentityTakesOverKeepsHandle) {
    ServiceRegistry reg;
    ServiceInfo first = makeInfo("svc.take", "127.0.0.1", 8080, "host-1", "inst-a");
    const ServiceHandle h1 = reg.addService(first, 10);
    ASSERT_NE(h1, INVALID_HANDLE);

    ServiceInfo second = makeInfo("svc.take", "127.0.0.1", 8081, "host-1", "inst-a");
    int displaced_fd = -1;
    const ServiceHandle h2 = reg.addService(second, 11, &displaced_fd);
    EXPECT_EQ(h2, h1);             // 同身份顶替，handle 保持
    EXPECT_EQ(displaced_fd, 10);   // 旧控制连接被顶替
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.ownsService(11, "svc.take"));
    EXPECT_FALSE(reg.ownsService(10, "svc.take"));

    ServiceEntry entry;
    ASSERT_TRUE(reg.findServiceByIdentity(ServiceIdentity("svc.take", "host-1", "inst-a"), entry));
    EXPECT_EQ(entry.control_fd, 11);
    EXPECT_EQ(entry.info.port, 8081);
}

TEST(ServiceRegistryTest, RemoveServiceByIdentityRemovesOnlyThatInstance) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8080, "host-1", "inst-a"), 10);
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8081, "host-1", "inst-b"), 11);
    ASSERT_EQ(reg.count(), 2u);

    EXPECT_TRUE(reg.removeServiceByIdentity(ServiceIdentity("svc.multi", "host-1", "inst-a")));
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.exists("svc.multi"));

    ServiceEntry entry;
    EXPECT_FALSE(reg.findServiceByIdentity(ServiceIdentity("svc.multi", "host-1", "inst-a"), entry));
    EXPECT_TRUE(reg.findServiceByIdentity(ServiceIdentity("svc.multi", "host-1", "inst-b"), entry));
    EXPECT_EQ(entry.control_fd, 11);
}

TEST(ServiceRegistryTest, RemoveServiceRemovesDeterministicFirstInstance) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8080, "host-1", "inst-a"), 10);
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8081, "host-1", "inst-b"), 11);
    ASSERT_EQ(reg.count(), 2u);

    // 文档承诺：removeService(name) 移除与 findService 相同的"确定性第一实例"
    ServiceEntry first;
    ASSERT_TRUE(reg.findService("svc.multi", first));
    const std::string first_instance = first.info.instance_id;

    EXPECT_TRUE(reg.removeService("svc.multi"));
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.exists("svc.multi"));

    ServiceEntry remaining;
    ASSERT_TRUE(reg.findService("svc.multi", remaining));
    EXPECT_NE(remaining.info.instance_id, first_instance);

    ServiceEntry gone;
    EXPECT_FALSE(reg.findServiceByIdentity(
        ServiceIdentity("svc.multi", "host-1", first_instance), gone));
}

TEST(ServiceRegistryTest, FindServiceByFdAndNameResolvesInstance) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8080, "host-1", "inst-a"), 10);
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8081, "host-1", "inst-b"), 11);

    ServiceEntry entry;
    ASSERT_TRUE(reg.findServiceByFdAndName(11, "svc.multi", entry));
    EXPECT_EQ(entry.info.instance_id, "inst-b");
    EXPECT_FALSE(reg.findServiceByFdAndName(12, "svc.multi", entry));
    EXPECT_FALSE(reg.findServiceByFdAndName(10, "svc.none", entry));
}

TEST(ServiceRegistryTest, RemoveByFdRemovesAllInstancesOfSameName) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8080, "host-1", "inst-a"), 10);
    reg.addService(makeInfo("svc.multi", "127.0.0.1", 8081, "host-1", "inst-b"), 10);
    ASSERT_EQ(reg.count(), 2u);

    std::vector<ServiceIdentity> removed = reg.removeByFd(10);
    EXPECT_EQ(removed.size(), 2u);
    EXPECT_EQ(reg.count(), 0u);
    EXPECT_TRUE(reg.listServiceNamesByFd(10).empty());
}

TEST(ServiceRegistryTest, AddEmptyNameRejected) {
    ServiceRegistry reg;
    EXPECT_EQ(reg.addService(makeInfo(""), 10), INVALID_HANDLE);
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, AddNameTooLongRejected) {
    ServiceRegistry reg;
    std::string long_name(MAX_SERVICE_NAME_LENGTH + 1, 'x');
    EXPECT_EQ(reg.addService(makeInfo(long_name), 10), INVALID_HANDLE);
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, AddNameAtMaxLengthAccepted) {
    ServiceRegistry reg;
    std::string max_name(MAX_SERVICE_NAME_LENGTH, 'a');
    EXPECT_NE(reg.addService(makeInfo(max_name), 10), INVALID_HANDLE);
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.exists(max_name));
}

TEST(ServiceRegistryTest, AddPreservesServiceInfo) {
    ServiceRegistry reg;
    ServiceInfo info;
    info.name = "svc.camera";
    info.host = "192.168.1.5";
    info.port = 9999;
    info.host_id = "node-42";

    EXPECT_NE(reg.addService(info, 7), INVALID_HANDLE);

    ServiceEntry entry;
    ASSERT_TRUE(reg.findService("svc.camera", entry));
    EXPECT_EQ(entry.info.name, "svc.camera");
    EXPECT_EQ(entry.info.host, "192.168.1.5");
    EXPECT_EQ(entry.info.port, 9999);
    EXPECT_EQ(entry.info.host_id, "node-42");
    EXPECT_NE(entry.handle, INVALID_HANDLE);
    EXPECT_EQ(entry.control_fd, 7);
}

TEST(ServiceRegistryTest, AddMultipleServicesSameFd) {
    ServiceRegistry reg;
    EXPECT_NE(reg.addService(makeInfo("svc.a"), 10), INVALID_HANDLE);
    EXPECT_NE(reg.addService(makeInfo("svc.b"), 10), INVALID_HANDLE);
    EXPECT_NE(reg.addService(makeInfo("svc.c"), 10), INVALID_HANDLE);
    EXPECT_EQ(reg.count(), 3u);
}

// ============================================================
// removeService
// ============================================================

TEST(ServiceRegistryTest, RemoveExisting) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.rm"), 10);
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.removeService("svc.rm"));
    EXPECT_EQ(reg.count(), 0u);
    EXPECT_FALSE(reg.exists("svc.rm"));
}

TEST(ServiceRegistryTest, RemoveNonexistent) {
    ServiceRegistry reg;
    EXPECT_FALSE(reg.removeService("no.such.service"));
}

TEST(ServiceRegistryTest, RemoveDoesNotAffectOthers) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.keep"), 10);
    reg.addService(makeInfo("svc.drop"), 11);
    EXPECT_EQ(reg.count(), 2u);
    reg.removeService("svc.drop");
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_TRUE(reg.exists("svc.keep"));
    EXPECT_FALSE(reg.exists("svc.drop"));
}

// ============================================================
// removeByFd
// ============================================================

TEST(ServiceRegistryTest, RemoveByFdSingle) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.fd1"), 20);
    std::vector<ServiceIdentity> removed = reg.removeByFd(20);
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0].name, "svc.fd1");
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, RemoveByFdMultiple) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.x"), 30);
    reg.addService(makeInfo("svc.y"), 30);
    reg.addService(makeInfo("svc.z"), 31);

    std::vector<ServiceIdentity> removed = reg.removeByFd(30);
    EXPECT_EQ(removed.size(), 2u);
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_FALSE(reg.exists("svc.x"));
    EXPECT_FALSE(reg.exists("svc.y"));
    EXPECT_TRUE(reg.exists("svc.z"));
}

TEST(ServiceRegistryTest, RemoveByFdNonexistent) {
    ServiceRegistry reg;
    std::vector<ServiceIdentity> removed = reg.removeByFd(999);
    EXPECT_TRUE(removed.empty());
}

TEST(ServiceRegistryTest, RemoveByFdClearsLookup) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.fdh"), 40);
    reg.removeByFd(40);

    ServiceEntry entry;
    EXPECT_FALSE(reg.findServiceByIdentity(ServiceIdentity("svc.fdh"), entry));
}

// ============================================================
// findService
// ============================================================

TEST(ServiceRegistryTest, FindExisting) {
    ServiceRegistry reg;
    EXPECT_NE(reg.addService(makeInfo("svc.find", "10.0.0.1", 5555), 50), INVALID_HANDLE);

    ServiceEntry entry;
    ASSERT_TRUE(reg.findService("svc.find", entry));
    EXPECT_EQ(entry.info.name, "svc.find");
    EXPECT_EQ(entry.info.host, "10.0.0.1");
    EXPECT_EQ(entry.info.port, 5555);
    EXPECT_NE(entry.handle, INVALID_HANDLE);
    EXPECT_EQ(entry.control_fd, 50);
}

TEST(ServiceRegistryTest, FindNonexistent) {
    ServiceRegistry reg;
    ServiceEntry entry;
    EXPECT_FALSE(reg.findService("ghost", entry));
}

// ============================================================
// listServices
// ============================================================

TEST(ServiceRegistryTest, ListEmpty) {
    ServiceRegistry reg;
    std::vector<ServiceInfo> list = reg.listServices();
    EXPECT_TRUE(list.empty());
}

TEST(ServiceRegistryTest, ListMultiple) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.alpha"), 10);
    reg.addService(makeInfo("svc.beta"), 11);
    reg.addService(makeInfo("svc.gamma"), 12);

    std::vector<ServiceInfo> list = reg.listServices();
    EXPECT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].name, "svc.alpha");
    EXPECT_EQ(list[1].name, "svc.beta");
    EXPECT_EQ(list[2].name, "svc.gamma");
}

TEST(ServiceRegistryTest, ListAfterRemoval) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.one"), 10);
    reg.addService(makeInfo("svc.two"), 11);
    reg.removeService("svc.one");

    std::vector<ServiceInfo> list = reg.listServices();
    EXPECT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "svc.two");
}

// ============================================================
// listServiceInstances
// ============================================================

TEST(ServiceRegistryTest, ListServiceInstancesByName) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.list", "127.0.0.1", 8080, "host-1", "inst-a"), 10);
    reg.addService(makeInfo("svc.list", "127.0.0.1", 8081, "host-1", "inst-b"), 11);
    reg.addService(makeInfo("svc.other", "127.0.0.1", 8082, "host-1", ""), 12);

    std::vector<ServiceInfo> list = reg.listServiceInstances("svc.list");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "svc.list");
    EXPECT_EQ(list[0].instance_id, "inst-a");
    EXPECT_EQ(list[0].port, 8080);
    EXPECT_EQ(list[1].instance_id, "inst-b");
    EXPECT_EQ(list[1].port, 8081);

    std::vector<ServiceInfo> other = reg.listServiceInstances("svc.other");
    ASSERT_EQ(other.size(), 1u);
    EXPECT_TRUE(other[0].instance_id.empty());
    EXPECT_TRUE(reg.listServiceInstances("svc.none").empty());
}

TEST(ServiceRegistryTest, ListServiceInstancesOrdersAcrossHosts) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.cross", "127.0.0.1", 9080, "host-b", ""), 10);
    reg.addService(makeInfo("svc.cross", "127.0.0.1", 9081, "host-a", "inst-b"), 11);
    reg.addService(makeInfo("svc.cross", "127.0.0.1", 9082, "host-a", "inst-a"), 12);

    std::vector<ServiceInfo> list = reg.listServiceInstances("svc.cross");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].host_id, "host-a");
    EXPECT_EQ(list[0].instance_id, "inst-a");
    EXPECT_EQ(list[1].host_id, "host-a");
    EXPECT_EQ(list[1].instance_id, "inst-b");
    EXPECT_EQ(list[2].host_id, "host-b");
}

// ============================================================
// exists / count
// ============================================================

TEST(ServiceRegistryTest, ExistsTrue) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.e"), 10);
    EXPECT_TRUE(reg.exists("svc.e"));
}

TEST(ServiceRegistryTest, ExistsFalse) {
    ServiceRegistry reg;
    EXPECT_FALSE(reg.exists("nope"));
}

TEST(ServiceRegistryTest, CountEmpty) {
    ServiceRegistry reg;
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, CountAfterAddRemove) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.c1"), 10);
    reg.addService(makeInfo("svc.c2"), 11);
    EXPECT_EQ(reg.count(), 2u);
    reg.removeService("svc.c1");
    EXPECT_EQ(reg.count(), 1u);
    reg.removeService("svc.c2");
    EXPECT_EQ(reg.count(), 0u);
}

// ============================================================
// getControlFd / ownsService
// ============================================================

TEST(ServiceRegistryTest, GetControlFdFound) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.fd"), 77);
    EXPECT_EQ(reg.getControlFd("svc.fd"), 77);
}

TEST(ServiceRegistryTest, GetControlFdNotFound) {
    ServiceRegistry reg;
    EXPECT_EQ(reg.getControlFd("missing"), -1);
}

TEST(ServiceRegistryTest, OwnsServiceTrueForOwnerFd) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.owner"), 88);
    EXPECT_TRUE(reg.ownsService(88, "svc.owner"));
    EXPECT_FALSE(reg.ownsService(99, "svc.owner"));
}

TEST(ServiceRegistryTest, OwnsServiceFalseForMissingService) {
    ServiceRegistry reg;
    EXPECT_FALSE(reg.ownsService(88, "missing"));
}

TEST(ServiceRegistryTest, ListServiceNamesByFd) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.l1"), 70);
    reg.addService(makeInfo("svc.l2"), 70);
    reg.addService(makeInfo("svc.l3"), 71);
    reg.removeService("svc.l2");

    std::vector<std::string> names = reg.listServiceNamesByFd(70);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "svc.l1");
    names = reg.listServiceNamesByFd(71);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "svc.l3");
    EXPECT_TRUE(reg.listServiceNamesByFd(999).empty());
}

// ============================================================
// handle 生成
// ============================================================

TEST(ServiceRegistryTest, HandlesAreUnique) {
    ServiceRegistry reg;
    const ServiceHandle h1 = reg.addService(makeInfo("svc.h1"), 10);
    const ServiceHandle h2 = reg.addService(makeInfo("svc.h2"), 11);
    const ServiceHandle h3 = reg.addService(makeInfo("svc.h3"), 12);
    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);
    EXPECT_NE(h3, INVALID_HANDLE);
    EXPECT_NE(h1, h2);
    EXPECT_NE(h2, h3);
    EXPECT_NE(h1, h3);
}

TEST(ServiceRegistryTest, HandlesAreSequential) {
    ServiceRegistry reg;
    const ServiceHandle h1 = reg.addService(makeInfo("svc.s1"), 10);
    const ServiceHandle h2 = reg.addService(makeInfo("svc.s2"), 11);
    EXPECT_EQ(h1, ServiceHandle(1));
    EXPECT_EQ(h2, ServiceHandle(2));
}

TEST(ServiceRegistryTest, HandleSurvivesRemovalAndReuse) {
    ServiceRegistry reg;
    const ServiceHandle h1 = reg.addService(makeInfo("svc.r1"), 10);
    reg.removeService("svc.r1");
    const ServiceHandle h2 = reg.addService(makeInfo("svc.r2"), 11);
    EXPECT_NE(h1, h2);
    EXPECT_GT(h2, h1);
}

// ============================================================
// 移除后重新注册
// ============================================================

TEST(ServiceRegistryTest, ReRegisterAfterRemove) {
    ServiceRegistry reg;
    const ServiceHandle h1 = reg.addService(makeInfo("svc.rereg"), 10);
    EXPECT_NE(h1, INVALID_HANDLE);
    reg.removeService("svc.rereg");
    EXPECT_FALSE(reg.exists("svc.rereg"));

    const ServiceHandle h2 = reg.addService(makeInfo("svc.rereg"), 20);
    EXPECT_NE(h2, INVALID_HANDLE);
    EXPECT_NE(h2, h1);
    EXPECT_TRUE(reg.exists("svc.rereg"));
    EXPECT_EQ(reg.getControlFd("svc.rereg"), 20);
}

TEST(ServiceRegistryTest, ReRegisterAfterRemoveByFd) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.rfr"), 10);
    reg.removeByFd(10);
    EXPECT_FALSE(reg.exists("svc.rfr"));

    EXPECT_NE(reg.addService(makeInfo("svc.rfr"), 20), INVALID_HANDLE);
    EXPECT_TRUE(reg.exists("svc.rfr"));
}

TEST(ServiceRegistryTest, IdempotentReRegisterMovesFdOwnership) {
    ServiceRegistry reg;
    ServiceInfo first = makeInfo("svc.move");
    first.host_id = "runtime-1";
    ASSERT_NE(reg.addService(first, 80), INVALID_HANDLE);

    ServiceInfo second = makeInfo("svc.move");
    second.host_id = "runtime-1";
    EXPECT_NE(reg.addService(second, 81), INVALID_HANDLE);

    EXPECT_TRUE(reg.ownsService(81, "svc.move"));
    EXPECT_FALSE(reg.ownsService(80, "svc.move"));
    EXPECT_TRUE(reg.listServiceNamesByFd(80).empty());
    ASSERT_EQ(reg.listServiceNamesByFd(81).size(), 1u);
    EXPECT_EQ(reg.listServiceNamesByFd(81)[0], "svc.move");

    EXPECT_TRUE(reg.removeByFd(80).empty());
    EXPECT_TRUE(reg.exists("svc.move"));
}

// ============================================================
// 边界用例
// ============================================================

TEST(ServiceRegistryTest, RemoveByNameCleansFdMap) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.fa"), 50);
    reg.addService(makeInfo("svc.fb"), 50);
    reg.removeService("svc.fa");

    std::vector<ServiceIdentity> removed = reg.removeByFd(50);
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0].name, "svc.fb");
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, RemoveByIdentityCleansFdMap) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.ha"), 60);
    reg.addService(makeInfo("svc.hb"), 60);
    reg.removeServiceByIdentity(ServiceIdentity("svc.ha"));

    std::vector<ServiceIdentity> removed = reg.removeByFd(60);
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0].name, "svc.hb");
    EXPECT_EQ(reg.count(), 0u);
}
