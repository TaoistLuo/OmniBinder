#include <gtest/gtest.h>
#include "test_common.h"
#include "service_registry.h"

using namespace omnibinder;
using namespace omnibinder::test;

static ServiceInfo makeInfo(const std::string& name,
                            const std::string& host = "127.0.0.1",
                            uint16_t port = 8080) {
    ServiceInfo info;
    info.name = name;
    info.host = host;
    info.port = port;
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

TEST(ServiceRegistryTest, AddDuplicateNameRejected) {
    ServiceRegistry reg;
    EXPECT_NE(reg.addService(makeInfo("svc.dup"), 10), INVALID_HANDLE);
    EXPECT_EQ(reg.addService(makeInfo("svc.dup"), 11), INVALID_HANDLE);
    EXPECT_EQ(reg.count(), 1u);
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

TEST(ServiceRegistryTest, RemoveByHandle) {
    ServiceRegistry reg;
    EXPECT_TRUE(reg.removeServiceByHandle(reg.addService(makeInfo("svc.byhandle"), 10)));
    EXPECT_EQ(reg.count(), 0u);
    EXPECT_FALSE(reg.exists("svc.byhandle"));
}

TEST(ServiceRegistryTest, RemoveByHandleNonexistent) {
    ServiceRegistry reg;
    EXPECT_FALSE(reg.removeServiceByHandle(999));
}

// ============================================================
// removeByFd
// ============================================================

TEST(ServiceRegistryTest, RemoveByFdSingle) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.fd1"), 20);
    std::vector<std::string> removed = reg.removeByFd(20);
    EXPECT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], "svc.fd1");
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, RemoveByFdMultiple) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.x"), 30);
    reg.addService(makeInfo("svc.y"), 30);
    reg.addService(makeInfo("svc.z"), 31);

    std::vector<std::string> removed = reg.removeByFd(30);
    EXPECT_EQ(removed.size(), 2u);
    EXPECT_EQ(reg.count(), 1u);
    EXPECT_FALSE(reg.exists("svc.x"));
    EXPECT_FALSE(reg.exists("svc.y"));
    EXPECT_TRUE(reg.exists("svc.z"));
}

TEST(ServiceRegistryTest, RemoveByFdNonexistent) {
    ServiceRegistry reg;
    std::vector<std::string> removed = reg.removeByFd(999);
    EXPECT_TRUE(removed.empty());
}

TEST(ServiceRegistryTest, RemoveByFdClearsHandleLookup) {
    ServiceRegistry reg;
    const ServiceHandle handle = reg.addService(makeInfo("svc.fdh"), 40);
    reg.removeByFd(40);

    ServiceEntry entry;
    EXPECT_FALSE(reg.findServiceByHandle(handle, entry));
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

TEST(ServiceRegistryTest, FindByHandle) {
    ServiceRegistry reg;
    const ServiceHandle handle = reg.addService(makeInfo("svc.hfind"), 60);

    ServiceEntry entry;
    ASSERT_TRUE(reg.findServiceByHandle(handle, entry));
    EXPECT_EQ(entry.info.name, "svc.hfind");
    EXPECT_EQ(entry.handle, handle);
}

TEST(ServiceRegistryTest, FindByHandleNonexistent) {
    ServiceRegistry reg;
    ServiceEntry entry;
    EXPECT_FALSE(reg.findServiceByHandle(12345, entry));
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

// ============================================================
// Handle generation
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
// Re-registration after removal
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

// ============================================================
// Edge cases
// ============================================================

TEST(ServiceRegistryTest, RemoveByNameCleansFdMap) {
    ServiceRegistry reg;
    reg.addService(makeInfo("svc.fa"), 50);
    reg.addService(makeInfo("svc.fb"), 50);
    reg.removeService("svc.fa");

    std::vector<std::string> removed = reg.removeByFd(50);
    EXPECT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], "svc.fb");
    EXPECT_EQ(reg.count(), 0u);
}

TEST(ServiceRegistryTest, RemoveByHandleCleansFdMap) {
    ServiceRegistry reg;
    ServiceHandle h1 = reg.addService(makeInfo("svc.ha"), 60);
    reg.addService(makeInfo("svc.hb"), 60);
    reg.removeServiceByHandle(h1);

    std::vector<std::string> removed = reg.removeByFd(60);
    EXPECT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], "svc.hb");
    EXPECT_EQ(reg.count(), 0u);
}
