#include "topic_manager.h"
#include <omnibinder/types.h>
#include <cstdio>
#include <cassert>
#include <algorithm>

using namespace omnibinder;

#define TEST(name) printf("  TEST %s ... ", #name);
#define PASS() printf("PASS\n");

static ServiceInfo makeInfo(const std::string& name, const std::string& host, uint16_t port) {
    ServiceInfo info;
    info.name = name;
    info.host = host;
    info.port = port;
    return info;
}

// ============================================================
// TopicManager Tests
// ============================================================

void test_register_remove_publisher() {
    TEST(register_remove_publisher) {
        TopicManager mgr;

        ServiceInfo info = makeInfo("pub1", "127.0.0.1", 9000);
        assert(mgr.registerPublisher("topic/sensor", info, 10) == true);
        assert(mgr.hasPublisher("topic/sensor") == true);

        // Verify publisher info can be retrieved
        ServiceInfo out;
        assert(mgr.getPublisher("topic/sensor", out) == true);
        assert(out.host == "127.0.0.1");
        assert(out.port == 9000);

        // Remove publisher
        assert(mgr.removePublisher("topic/sensor") == true);
        assert(mgr.hasPublisher("topic/sensor") == false);

        // Remove again should fail
        assert(mgr.removePublisher("topic/sensor") == false);

        // Remove non-existent topic
        assert(mgr.removePublisher("nonexistent") == false);

        PASS();
    }
}

void test_add_remove_subscriber() {
    TEST(add_remove_subscriber) {
        TopicManager mgr;

        // Subscribe to a topic that has no publisher yet
        assert(mgr.addSubscriber("topic/data", 20) == true);
        assert(mgr.addSubscriber("topic/data", 30) == true);

        // Duplicate subscriber should return false
        assert(mgr.addSubscriber("topic/data", 20) == false);

        std::vector<int> subs = mgr.getSubscribers("topic/data");
        assert(subs.size() == 2);

        // Remove one subscriber
        assert(mgr.removeSubscriber("topic/data", 20) == true);
        subs = mgr.getSubscribers("topic/data");
        assert(subs.size() == 1);
        assert(subs[0] == 30);

        // Remove non-existent subscriber
        assert(mgr.removeSubscriber("topic/data", 999) == false);

        // Remove from non-existent topic
        assert(mgr.removeSubscriber("nonexistent", 30) == false);

        PASS();
    }
}

void test_get_subscribers_get_publisher() {
    TEST(get_subscribers_get_publisher) {
        TopicManager mgr;

        ServiceInfo info = makeInfo("cam", "10.0.0.1", 8080);
        mgr.registerPublisher("topic/camera", info, 5);
        mgr.addSubscriber("topic/camera", 10);
        mgr.addSubscriber("topic/camera", 20);
        mgr.addSubscriber("topic/camera", 30);

        // getSubscribers
        std::vector<int> subs = mgr.getSubscribers("topic/camera");
        assert(subs.size() == 3);
        std::sort(subs.begin(), subs.end());
        assert(subs[0] == 10);
        assert(subs[1] == 20);
        assert(subs[2] == 30);

        // getPublisher
        ServiceInfo out;
        assert(mgr.getPublisher("topic/camera", out) == true);
        assert(out.host == "10.0.0.1");
        assert(out.port == 8080);

        // getPublisher for topic with no publisher
        assert(mgr.getPublisher("nonexistent", out) == false);

        // getSubscribers for non-existent topic
        std::vector<int> empty = mgr.getSubscribers("nonexistent");
        assert(empty.empty());

        PASS();
    }
}

void test_has_publisher() {
    TEST(has_publisher) {
        TopicManager mgr;

        assert(mgr.hasPublisher("topic/x") == false);

        ServiceInfo info = makeInfo("pub", "localhost", 5000);
        mgr.registerPublisher("topic/x", info, 7);
        assert(mgr.hasPublisher("topic/x") == true);

        mgr.removePublisher("topic/x");
        assert(mgr.hasPublisher("topic/x") == false);

        // Topic with only subscribers should not have a publisher
        mgr.addSubscriber("topic/y", 10);
        assert(mgr.hasPublisher("topic/y") == false);

        PASS();
    }
}

void test_remove_publisher_owner_check() {
    TEST(remove_publisher_owner_check) {
        TopicManager mgr;

        ServiceInfo info = makeInfo("pub", "127.0.0.1", 9000);
        assert(mgr.registerPublisher("topic/owned", info, 7) == true);
        assert(mgr.isPublisherOwner("topic/owned", 7) == true);
        assert(mgr.isPublisherOwner("topic/owned", 8) == false);
        assert(mgr.removePublisher("topic/owned", 8) == false);
        assert(mgr.hasPublisher("topic/owned") == true);
        assert(mgr.removePublisher("topic/owned", 7) == true);
        assert(mgr.hasPublisher("topic/owned") == false);

        PASS();
    }
}

void test_remove_by_owner() {
    TEST(remove_by_owner_service_fd_scope) {
        TopicManager mgr;

        ServiceInfo a1 = makeInfo("svc.a", "127.0.0.1", 9001);
        ServiceInfo a2 = makeInfo("svc.a", "127.0.0.1", 9002);
        ServiceInfo b1 = makeInfo("svc.b", "127.0.0.1", 9010);

        assert(mgr.registerPublisher("topic/a1", a1, 11) == true);
        assert(mgr.registerPublisher("topic/a2", a2, 11) == true);
        assert(mgr.registerPublisher("topic/b1", b1, 22) == true);

        mgr.removeByFd(11);

        assert(mgr.hasPublisher("topic/a1") == false);
        assert(mgr.hasPublisher("topic/a2") == false);
        assert(mgr.hasPublisher("topic/b1") == true);

        PASS();
    }
}

void test_remove_by_fd() {
    TEST(remove_by_fd) {
        TopicManager mgr;

        ServiceInfo info1 = makeInfo("pub1", "host1", 1000);
        ServiceInfo info2 = makeInfo("pub2", "host2", 2000);

        // fd=5 publishes two topics
        mgr.registerPublisher("topic/a", info1, 5);
        mgr.registerPublisher("topic/b", info2, 5);

        // fd=5 also subscribes to topic/c
        mgr.addSubscriber("topic/c", 5);

        // fd=10 subscribes to topic/a and topic/b
        mgr.addSubscriber("topic/a", 10);
        mgr.addSubscriber("topic/b", 10);

        // Another publisher for topic/c
        ServiceInfo info3 = makeInfo("pub3", "host3", 3000);
        mgr.registerPublisher("topic/c", info3, 15);

        // Remove fd=5 entirely
        std::vector<std::string> removed_pubs = mgr.removeByFd(5);

        // Should have removed publications for topic/a and topic/b
        assert(removed_pubs.size() == 2);
        std::sort(removed_pubs.begin(), removed_pubs.end());
        assert(removed_pubs[0] == "topic/a");
        assert(removed_pubs[1] == "topic/b");

        // topic/a and topic/b should no longer have publishers
        assert(mgr.hasPublisher("topic/a") == false);
        assert(mgr.hasPublisher("topic/b") == false);

        // But fd=10 should still be subscribed to topic/a and topic/b
        std::vector<int> subs_a = mgr.getSubscribers("topic/a");
        assert(subs_a.size() == 1);
        assert(subs_a[0] == 10);

        // topic/c should still have its publisher (fd=15) but fd=5 subscriber removed
        assert(mgr.hasPublisher("topic/c") == true);
        std::vector<int> subs_c = mgr.getSubscribers("topic/c");
        assert(subs_c.empty());

        PASS();
    }
}

void test_list_topics() {
    TEST(list_topics) {
        TopicManager mgr;

        std::vector<std::string> topics = mgr.listTopics();
        assert(topics.empty());

        ServiceInfo info = makeInfo("pub", "host", 1000);
        mgr.registerPublisher("topic/alpha", info, 1);
        mgr.addSubscriber("topic/beta", 2);
        mgr.registerPublisher("topic/gamma", info, 3);

        topics = mgr.listTopics();
        assert(topics.size() == 3);

        std::sort(topics.begin(), topics.end());
        assert(topics[0] == "topic/alpha");
        assert(topics[1] == "topic/beta");
        assert(topics[2] == "topic/gamma");

        // Remove a topic's publisher and subscribers to clean it up
        mgr.removePublisher("topic/alpha");
        topics = mgr.listTopics();
        assert(topics.size() == 2);

        PASS();
    }
}

void test_duplicate_publisher_rejection() {
    TEST(duplicate_publisher_rejection) {
        TopicManager mgr;

        ServiceInfo info1 = makeInfo("pub1", "host1", 1000);
        ServiceInfo info2 = makeInfo("pub2", "host2", 2000);

        assert(mgr.registerPublisher("topic/exclusive", info1, 10) == true);

        // Second publisher for the same topic should be rejected
        assert(mgr.registerPublisher("topic/exclusive", info2, 20) == false);

        // Original publisher should still be intact
        ServiceInfo out;
        assert(mgr.getPublisher("topic/exclusive", out) == true);
        assert(out.host == "host1");
        assert(out.port == 1000);

        // After removing the first publisher, a new one can register
        mgr.removePublisher("topic/exclusive");
        assert(mgr.registerPublisher("topic/exclusive", info2, 20) == true);

        assert(mgr.getPublisher("topic/exclusive", out) == true);
        assert(out.host == "host2");
        assert(out.port == 2000);

        // Empty topic name should be rejected
        assert(mgr.registerPublisher("", info1, 30) == false);

        PASS();
    }
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== TopicManager Tests ===\n");
    test_register_remove_publisher();
    test_add_remove_subscriber();
    test_get_subscribers_get_publisher();
    test_has_publisher();
    test_remove_publisher_owner_check();
    test_remove_by_owner();
    test_remove_by_fd();
    test_list_topics();
    test_duplicate_publisher_rejection();

    printf("\nAll topic manager tests passed!\n");
    return 0;
}
