#include "service_registry.h"
#include <omnibinder/types.h>
#include <cstdio>
#include <cassert>
#include <cstring>

using namespace omnibinder;

#define TEST(name) printf("  TEST %s ... ", #name);
#define PASS() printf("PASS\n");

static ServiceInfo makeInfo(const std::string& name,
                            const std::string& host = "127.0.0.1",
                            uint16_t port = 8080)
{
    ServiceInfo info;
    info.name = name;
    info.host = host;
    info.port = port;
    return info;
}

int main() {
    printf("=== ServiceRegistry Tests ===\n");

    // ----------------------------------------------------------
    // addService
    // ----------------------------------------------------------

    TEST(add_basic) {
        ServiceRegistry reg;
        ServiceHandle h = reg.addService(makeInfo("svc.audio"), 10);
        assert(h != INVALID_HANDLE);
        assert(reg.count() == 1);
        assert(reg.exists("svc.audio"));
        PASS();
    }

    TEST(add_duplicate_name_rejected) {
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.dup"), 10);
        ServiceHandle h2 = reg.addService(makeInfo("svc.dup"), 11);
        assert(h1 != INVALID_HANDLE);
        assert(h2 == INVALID_HANDLE);
        assert(reg.count() == 1);
        PASS();
    }

    TEST(add_empty_name_rejected) {
        ServiceRegistry reg;
        ServiceHandle h = reg.addService(makeInfo(""), 10);
        assert(h == INVALID_HANDLE);
        assert(reg.count() == 0);
        PASS();
    }

    TEST(add_name_too_long_rejected) {
        ServiceRegistry reg;
        std::string long_name(MAX_SERVICE_NAME_LENGTH + 1, 'x');
        ServiceHandle h = reg.addService(makeInfo(long_name), 10);
        assert(h == INVALID_HANDLE);
        assert(reg.count() == 0);
        PASS();
    }

    TEST(add_name_at_max_length_accepted) {
        ServiceRegistry reg;
        std::string max_name(MAX_SERVICE_NAME_LENGTH, 'a');
        ServiceHandle h = reg.addService(makeInfo(max_name), 10);
        assert(h != INVALID_HANDLE);
        assert(reg.count() == 1);
        assert(reg.exists(max_name));
        PASS();
    }

    TEST(add_preserves_service_info) {
        ServiceRegistry reg;
        ServiceInfo info;
        info.name = "svc.camera";
        info.host = "192.168.1.5";
        info.port = 9999;
        info.host_id = "node-42";

        ServiceHandle h = reg.addService(info, 7);
        assert(h != INVALID_HANDLE);

        ServiceEntry entry;
        bool found = reg.findService("svc.camera", entry);
        assert(found);
        assert(entry.info.name == "svc.camera");
        assert(entry.info.host == "192.168.1.5");
        assert(entry.info.port == 9999);
        assert(entry.info.host_id == "node-42");
        assert(entry.handle == h);
        assert(entry.control_fd == 7);
        PASS();
    }

    TEST(add_multiple_services_same_fd) {
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.a"), 10);
        ServiceHandle h2 = reg.addService(makeInfo("svc.b"), 10);
        ServiceHandle h3 = reg.addService(makeInfo("svc.c"), 10);
        assert(h1 != INVALID_HANDLE);
        assert(h2 != INVALID_HANDLE);
        assert(h3 != INVALID_HANDLE);
        assert(reg.count() == 3);
        PASS();
    }

    // ----------------------------------------------------------
    // removeService
    // ----------------------------------------------------------

    TEST(remove_existing) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.rm"), 10);
        assert(reg.count() == 1);
        bool ok = reg.removeService("svc.rm");
        assert(ok);
        assert(reg.count() == 0);
        assert(!reg.exists("svc.rm"));
        PASS();
    }

    TEST(remove_nonexistent) {
        ServiceRegistry reg;
        bool ok = reg.removeService("no.such.service");
        assert(!ok);
        PASS();
    }

    TEST(remove_does_not_affect_others) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.keep"), 10);
        reg.addService(makeInfo("svc.drop"), 11);
        assert(reg.count() == 2);
        reg.removeService("svc.drop");
        assert(reg.count() == 1);
        assert(reg.exists("svc.keep"));
        assert(!reg.exists("svc.drop"));
        PASS();
    }

    TEST(remove_by_handle) {
        ServiceRegistry reg;
        ServiceHandle h = reg.addService(makeInfo("svc.byhandle"), 10);
        assert(h != INVALID_HANDLE);
        bool ok = reg.removeServiceByHandle(h);
        assert(ok);
        assert(reg.count() == 0);
        assert(!reg.exists("svc.byhandle"));
        PASS();
    }

    TEST(remove_by_handle_nonexistent) {
        ServiceRegistry reg;
        bool ok = reg.removeServiceByHandle(999);
        assert(!ok);
        PASS();
    }

    // ----------------------------------------------------------
    // removeByFd
    // ----------------------------------------------------------

    TEST(remove_by_fd_single) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.fd1"), 20);
        std::vector<std::string> removed = reg.removeByFd(20);
        assert(removed.size() == 1);
        assert(removed[0] == "svc.fd1");
        assert(reg.count() == 0);
        PASS();
    }

    TEST(remove_by_fd_multiple) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.x"), 30);
        reg.addService(makeInfo("svc.y"), 30);
        reg.addService(makeInfo("svc.z"), 31);

        std::vector<std::string> removed = reg.removeByFd(30);
        assert(removed.size() == 2);
        assert(reg.count() == 1);
        assert(!reg.exists("svc.x"));
        assert(!reg.exists("svc.y"));
        assert(reg.exists("svc.z"));
        PASS();
    }

    TEST(remove_by_fd_nonexistent) {
        ServiceRegistry reg;
        std::vector<std::string> removed = reg.removeByFd(999);
        assert(removed.empty());
        PASS();
    }

    TEST(remove_by_fd_clears_handle_lookup) {
        ServiceRegistry reg;
        ServiceHandle h = reg.addService(makeInfo("svc.fdh"), 40);
        reg.removeByFd(40);

        ServiceEntry entry;
        bool found = reg.findServiceByHandle(h, entry);
        assert(!found);
        PASS();
    }

    // ----------------------------------------------------------
    // findService
    // ----------------------------------------------------------

    TEST(find_existing) {
        ServiceRegistry reg;
        ServiceHandle h = reg.addService(makeInfo("svc.find", "10.0.0.1", 5555), 50);

        ServiceEntry entry;
        bool found = reg.findService("svc.find", entry);
        assert(found);
        assert(entry.info.name == "svc.find");
        assert(entry.info.host == "10.0.0.1");
        assert(entry.info.port == 5555);
        assert(entry.handle == h);
        assert(entry.control_fd == 50);
        PASS();
    }

    TEST(find_nonexistent) {
        ServiceRegistry reg;
        ServiceEntry entry;
        bool found = reg.findService("ghost", entry);
        assert(!found);
        PASS();
    }

    TEST(find_by_handle) {
        ServiceRegistry reg;
        ServiceHandle h = reg.addService(makeInfo("svc.hfind"), 60);

        ServiceEntry entry;
        bool found = reg.findServiceByHandle(h, entry);
        assert(found);
        assert(entry.info.name == "svc.hfind");
        assert(entry.handle == h);
        PASS();
    }

    TEST(find_by_handle_nonexistent) {
        ServiceRegistry reg;
        ServiceEntry entry;
        bool found = reg.findServiceByHandle(12345, entry);
        assert(!found);
        PASS();
    }

    // ----------------------------------------------------------
    // listServices
    // ----------------------------------------------------------

    TEST(list_empty) {
        ServiceRegistry reg;
        std::vector<ServiceInfo> list = reg.listServices();
        assert(list.empty());
        PASS();
    }

    TEST(list_multiple) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.alpha"), 10);
        reg.addService(makeInfo("svc.beta"), 11);
        reg.addService(makeInfo("svc.gamma"), 12);

        std::vector<ServiceInfo> list = reg.listServices();
        assert(list.size() == 3);

        // std::map is ordered, so names should be sorted
        assert(list[0].name == "svc.alpha");
        assert(list[1].name == "svc.beta");
        assert(list[2].name == "svc.gamma");
        PASS();
    }

    TEST(list_after_removal) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.one"), 10);
        reg.addService(makeInfo("svc.two"), 11);
        reg.removeService("svc.one");

        std::vector<ServiceInfo> list = reg.listServices();
        assert(list.size() == 1);
        assert(list[0].name == "svc.two");
        PASS();
    }

    // ----------------------------------------------------------
    // exists and count
    // ----------------------------------------------------------

    TEST(exists_true) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.e"), 10);
        assert(reg.exists("svc.e"));
        PASS();
    }

    TEST(exists_false) {
        ServiceRegistry reg;
        assert(!reg.exists("nope"));
        PASS();
    }

    TEST(count_empty) {
        ServiceRegistry reg;
        assert(reg.count() == 0);
        PASS();
    }

    TEST(count_after_add_remove) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.c1"), 10);
        reg.addService(makeInfo("svc.c2"), 11);
        assert(reg.count() == 2);
        reg.removeService("svc.c1");
        assert(reg.count() == 1);
        reg.removeService("svc.c2");
        assert(reg.count() == 0);
        PASS();
    }

    // ----------------------------------------------------------
    // getControlFd
    // ----------------------------------------------------------

    TEST(get_control_fd_found) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.fd"), 77);
        int fd = reg.getControlFd("svc.fd");
        assert(fd == 77);
        PASS();
    }

    TEST(get_control_fd_not_found) {
        ServiceRegistry reg;
        int fd = reg.getControlFd("missing");
        assert(fd == -1);
        PASS();
    }

    TEST(owns_service_true_for_owner_fd) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.owner"), 88);
        assert(reg.ownsService(88, "svc.owner") == true);
        assert(reg.ownsService(99, "svc.owner") == false);
        PASS();
    }

    TEST(owns_service_false_for_missing_service) {
        ServiceRegistry reg;
        assert(reg.ownsService(88, "missing") == false);
        PASS();
    }

    // ----------------------------------------------------------
    // Handle generation (unique handles)
    // ----------------------------------------------------------

    TEST(handles_are_unique) {
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.h1"), 10);
        ServiceHandle h2 = reg.addService(makeInfo("svc.h2"), 11);
        ServiceHandle h3 = reg.addService(makeInfo("svc.h3"), 12);
        assert(h1 != INVALID_HANDLE);
        assert(h2 != INVALID_HANDLE);
        assert(h3 != INVALID_HANDLE);
        assert(h1 != h2);
        assert(h2 != h3);
        assert(h1 != h3);
        PASS();
    }

    TEST(handles_are_sequential) {
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.s1"), 10);
        ServiceHandle h2 = reg.addService(makeInfo("svc.s2"), 11);
        assert(h1 == 1);
        assert(h2 == 2);
        PASS();
    }

    TEST(handle_survives_removal_and_reuse) {
        // After removing a service, the next handle should still increment,
        // not reuse the old handle.
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.r1"), 10);
        reg.removeService("svc.r1");
        ServiceHandle h2 = reg.addService(makeInfo("svc.r2"), 11);
        assert(h1 != h2);
        assert(h2 > h1);
        PASS();
    }

    // ----------------------------------------------------------
    // Re-registration after removal
    // ----------------------------------------------------------

    TEST(re_register_after_remove) {
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.rereg"), 10);
        assert(h1 != INVALID_HANDLE);
        reg.removeService("svc.rereg");
        assert(!reg.exists("svc.rereg"));

        ServiceHandle h2 = reg.addService(makeInfo("svc.rereg"), 20);
        assert(h2 != INVALID_HANDLE);
        assert(h2 != h1);
        assert(reg.exists("svc.rereg"));

        // Verify the new entry has the updated fd
        assert(reg.getControlFd("svc.rereg") == 20);
        PASS();
    }

    TEST(re_register_after_remove_by_fd) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.rfr"), 10);
        reg.removeByFd(10);
        assert(!reg.exists("svc.rfr"));

        ServiceHandle h = reg.addService(makeInfo("svc.rfr"), 20);
        assert(h != INVALID_HANDLE);
        assert(reg.exists("svc.rfr"));
        PASS();
    }

    // ----------------------------------------------------------
    // Edge: remove by name cleans up fd map correctly
    // ----------------------------------------------------------

    TEST(remove_by_name_cleans_fd_map) {
        ServiceRegistry reg;
        reg.addService(makeInfo("svc.fa"), 50);
        reg.addService(makeInfo("svc.fb"), 50);
        reg.removeService("svc.fa");

        // Removing by fd should only return the remaining service
        std::vector<std::string> removed = reg.removeByFd(50);
        assert(removed.size() == 1);
        assert(removed[0] == "svc.fb");
        assert(reg.count() == 0);
        PASS();
    }

    TEST(remove_by_handle_cleans_fd_map) {
        ServiceRegistry reg;
        ServiceHandle h1 = reg.addService(makeInfo("svc.ha"), 60);
        reg.addService(makeInfo("svc.hb"), 60);
        reg.removeServiceByHandle(h1);

        std::vector<std::string> removed = reg.removeByFd(60);
        assert(removed.size() == 1);
        assert(removed[0] == "svc.hb");
        assert(reg.count() == 0);
        PASS();
    }

    printf("\nAll service registry tests passed!\n");
    return 0;
}
