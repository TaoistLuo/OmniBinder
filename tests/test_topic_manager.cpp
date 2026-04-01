#include <gtest/gtest.h>
#include "test_common.h"
#include "topic_manager.h"

using namespace omnibinder;
using namespace omnibinder::test;

static ServiceInfo makeInfo(const std::string& name, const std::string& host, uint16_t port) {
    ServiceInfo info;
    info.name = name;
    info.host = host;
    info.port = port;
    return info;
}

TEST(TopicManagerTest, RegisterRemovePublisher) {
    TopicManager mgr;
    ServiceInfo info = makeInfo("pub1", "127.0.0.1", 9000);
    EXPECT_TRUE(mgr.registerPublisher("topic/sensor", info, 10));
    EXPECT_TRUE(mgr.hasPublisher("topic/sensor"));

    ServiceInfo out;
    ASSERT_TRUE(mgr.getPublisher("topic/sensor", out));
    EXPECT_EQ(out.host, "127.0.0.1");
    EXPECT_EQ(out.port, 9000);

    EXPECT_TRUE(mgr.removePublisher("topic/sensor"));
    EXPECT_FALSE(mgr.hasPublisher("topic/sensor"));
    EXPECT_FALSE(mgr.removePublisher("topic/sensor"));
    EXPECT_FALSE(mgr.removePublisher("nonexistent"));
}

TEST(TopicManagerTest, AddRemoveSubscriber) {
    TopicManager mgr;
    EXPECT_TRUE(mgr.addSubscriber("topic/data", 20));
    EXPECT_TRUE(mgr.addSubscriber("topic/data", 30));
    EXPECT_FALSE(mgr.addSubscriber("topic/data", 20));

    std::vector<int> subs = mgr.getSubscribers("topic/data");
    EXPECT_EQ(subs.size(), 2u);

    EXPECT_TRUE(mgr.removeSubscriber("topic/data", 20));
    subs = mgr.getSubscribers("topic/data");
    ASSERT_EQ(subs.size(), 1u);
    EXPECT_EQ(subs[0], 30);

    EXPECT_FALSE(mgr.removeSubscriber("topic/data", 999));
    EXPECT_FALSE(mgr.removeSubscriber("nonexistent", 30));
}

TEST(TopicManagerTest, GetSubscribersGetPublisher) {
    TopicManager mgr;
    ServiceInfo info = makeInfo("cam", "10.0.0.1", 8080);
    mgr.registerPublisher("topic/camera", info, 5);
    mgr.addSubscriber("topic/camera", 10);
    mgr.addSubscriber("topic/camera", 20);
    mgr.addSubscriber("topic/camera", 30);

    std::vector<int> subs = mgr.getSubscribers("topic/camera");
    ASSERT_EQ(subs.size(), 3u);
    std::sort(subs.begin(), subs.end());
    EXPECT_EQ(subs[0], 10);
    EXPECT_EQ(subs[1], 20);
    EXPECT_EQ(subs[2], 30);

    ServiceInfo out;
    ASSERT_TRUE(mgr.getPublisher("topic/camera", out));
    EXPECT_EQ(out.host, "10.0.0.1");
    EXPECT_EQ(out.port, 8080);

    EXPECT_FALSE(mgr.getPublisher("nonexistent", out));
    EXPECT_TRUE(mgr.getSubscribers("nonexistent").empty());
}

TEST(TopicManagerTest, HasPublisher) {
    TopicManager mgr;
    EXPECT_FALSE(mgr.hasPublisher("topic/x"));

    ServiceInfo info = makeInfo("pub", "localhost", 5000);
    mgr.registerPublisher("topic/x", info, 7);
    EXPECT_TRUE(mgr.hasPublisher("topic/x"));

    mgr.removePublisher("topic/x");
    EXPECT_FALSE(mgr.hasPublisher("topic/x"));

    mgr.addSubscriber("topic/y", 10);
    EXPECT_FALSE(mgr.hasPublisher("topic/y"));
}

TEST(TopicManagerTest, RemovePublisherOwnerCheck) {
    TopicManager mgr;
    ServiceInfo info = makeInfo("pub", "127.0.0.1", 9000);
    EXPECT_TRUE(mgr.registerPublisher("topic/owned", info, 7));
    EXPECT_TRUE(mgr.isPublisherOwner("topic/owned", 7));
    EXPECT_FALSE(mgr.isPublisherOwner("topic/owned", 8));
    EXPECT_FALSE(mgr.removePublisher("topic/owned", 8));
    EXPECT_TRUE(mgr.hasPublisher("topic/owned"));
    EXPECT_TRUE(mgr.removePublisher("topic/owned", 7));
    EXPECT_FALSE(mgr.hasPublisher("topic/owned"));
}

TEST(TopicManagerTest, RemoveByOwnerServiceFdScope) {
    TopicManager mgr;
    ServiceInfo a1 = makeInfo("svc.a", "127.0.0.1", 9001);
    ServiceInfo a2 = makeInfo("svc.a", "127.0.0.1", 9002);
    ServiceInfo b1 = makeInfo("svc.b", "127.0.0.1", 9010);

    EXPECT_TRUE(mgr.registerPublisher("topic/a1", a1, 11));
    EXPECT_TRUE(mgr.registerPublisher("topic/a2", a2, 11));
    EXPECT_TRUE(mgr.registerPublisher("topic/b1", b1, 22));

    mgr.removeByFd(11);

    EXPECT_FALSE(mgr.hasPublisher("topic/a1"));
    EXPECT_FALSE(mgr.hasPublisher("topic/a2"));
    EXPECT_TRUE(mgr.hasPublisher("topic/b1"));
}

TEST(TopicManagerTest, RemoveByFd) {
    TopicManager mgr;
    ServiceInfo info1 = makeInfo("pub1", "host1", 1000);
    ServiceInfo info2 = makeInfo("pub2", "host2", 2000);
    ServiceInfo info3 = makeInfo("pub3", "host3", 3000);

    mgr.registerPublisher("topic/a", info1, 5);
    mgr.registerPublisher("topic/b", info2, 5);
    mgr.addSubscriber("topic/c", 5);
    mgr.addSubscriber("topic/a", 10);
    mgr.addSubscriber("topic/b", 10);
    mgr.registerPublisher("topic/c", info3, 15);

    std::vector<std::string> removed_pubs = mgr.removeByFd(5);
    ASSERT_EQ(removed_pubs.size(), 2u);
    std::sort(removed_pubs.begin(), removed_pubs.end());
    EXPECT_EQ(removed_pubs[0], "topic/a");
    EXPECT_EQ(removed_pubs[1], "topic/b");

    EXPECT_FALSE(mgr.hasPublisher("topic/a"));
    EXPECT_FALSE(mgr.hasPublisher("topic/b"));

    std::vector<int> subs_a = mgr.getSubscribers("topic/a");
    ASSERT_EQ(subs_a.size(), 1u);
    EXPECT_EQ(subs_a[0], 10);

    EXPECT_TRUE(mgr.hasPublisher("topic/c"));
    EXPECT_TRUE(mgr.getSubscribers("topic/c").empty());
}

TEST(TopicManagerTest, ListTopics) {
    TopicManager mgr;
    EXPECT_TRUE(mgr.listTopics().empty());

    ServiceInfo info = makeInfo("pub", "host", 1000);
    mgr.registerPublisher("topic/alpha", info, 1);
    mgr.addSubscriber("topic/beta", 2);
    mgr.registerPublisher("topic/gamma", info, 3);

    std::vector<std::string> topics = mgr.listTopics();
    ASSERT_EQ(topics.size(), 3u);
    std::sort(topics.begin(), topics.end());
    EXPECT_EQ(topics[0], "topic/alpha");
    EXPECT_EQ(topics[1], "topic/beta");
    EXPECT_EQ(topics[2], "topic/gamma");

    mgr.removePublisher("topic/alpha");
    EXPECT_EQ(mgr.listTopics().size(), 2u);
}

TEST(TopicManagerTest, DuplicatePublisherRejection) {
    TopicManager mgr;
    ServiceInfo info1 = makeInfo("pub1", "host1", 1000);
    ServiceInfo info2 = makeInfo("pub2", "host2", 2000);

    EXPECT_TRUE(mgr.registerPublisher("topic/exclusive", info1, 10));
    EXPECT_FALSE(mgr.registerPublisher("topic/exclusive", info2, 20));

    ServiceInfo out;
    ASSERT_TRUE(mgr.getPublisher("topic/exclusive", out));
    EXPECT_EQ(out.host, "host1");
    EXPECT_EQ(out.port, 1000);

    mgr.removePublisher("topic/exclusive");
    EXPECT_TRUE(mgr.registerPublisher("topic/exclusive", info2, 20));

    ASSERT_TRUE(mgr.getPublisher("topic/exclusive", out));
    EXPECT_EQ(out.host, "host2");
    EXPECT_EQ(out.port, 2000);

    EXPECT_FALSE(mgr.registerPublisher("", info1, 30));
}
