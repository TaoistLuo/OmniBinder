#include <gtest/gtest.h>
#include "test_common.h"
#include "topic_manager.h"
#include "core/topic_runtime.h"

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

TEST(TopicManagerTest, QueryPublishedTopicsUsesLivePublisherOwnerAndSorts) {
    TopicManager mgr;
    ServiceInfo a = makeInfo("svc.a", "127.0.0.1", 9001);
    ServiceInfo b = makeInfo("svc.b", "127.0.0.1", 9002);
    EXPECT_TRUE(mgr.registerPublisher("zeta", a, 11));
    EXPECT_TRUE(mgr.registerPublisher("alpha", a, 11));
    EXPECT_TRUE(mgr.registerPublisher("beta", b, 11));
    EXPECT_TRUE(mgr.addSubscriber("subscriber-only", 22));

    std::vector<std::string> topics = mgr.getPublishedTopics("svc.a");
    ASSERT_EQ(topics.size(), 2u);
    EXPECT_EQ(topics[0], "alpha");
    EXPECT_EQ(topics[1], "zeta");
    EXPECT_TRUE(mgr.getPublishedTopics("missing").empty());
}

TEST(TopicManagerTest, ServiceCleanupPreservesSiblingPublishersAndSubscriptions) {
    TopicManager mgr;
    ServiceInfo a = makeInfo("svc.a", "127.0.0.1", 9001);
    ServiceInfo b = makeInfo("svc.b", "127.0.0.1", 9002);
    EXPECT_TRUE(mgr.registerPublisher("topic/a", a, 11));
    EXPECT_TRUE(mgr.setIdlHash("topic/a", 0x1234u));
    EXPECT_TRUE(mgr.addSubscriber("topic/a", 22));
    EXPECT_TRUE(mgr.registerPublisher("topic/b", b, 11));
    EXPECT_TRUE(mgr.addSubscriber("other", 11));

    EXPECT_TRUE(mgr.removePublishersByService("svc.a", 99).empty());
    std::vector<std::string> removed = mgr.removePublishersByService("svc.a", 11);
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0], "topic/a");
    EXPECT_FALSE(mgr.hasPublisher("topic/a"));
    EXPECT_TRUE(mgr.hasPublisher("topic/b"));
    EXPECT_EQ(mgr.getSubscribers("topic/a").size(), 1u);
    EXPECT_EQ(mgr.getSubscribers("other").size(), 1u);
    uint32_t hash = 0;
    EXPECT_FALSE(mgr.getIdlHash("topic/a", hash));
}

TEST(TopicManagerTest, PublisherHashRegistrationIsAtomic) {
    TopicManager mgr;
    ServiceInfo info = makeInfo("svc", "127.0.0.1", 9001);
    ASSERT_TRUE(mgr.registerPublisher("topic/hash", info, 11, 0x12345678u));
    uint32_t hash = 0;
    ASSERT_TRUE(mgr.getIdlHash("topic/hash", hash));
    EXPECT_EQ(hash, 0x12345678u);
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

static ServiceInfo makeInstanceInfo(const std::string& name, const std::string& host,
                                    uint16_t port, const std::string& host_id,
                                    const std::string& instance_id) {
    ServiceInfo info = makeInfo(name, host, port);
    info.host_id = host_id;
    info.instance_id = instance_id;
    return info;
}

static TopicSubscriptionFilter serviceSelector(const std::string& service) {
    return TopicSubscriptionFilter::service(service);
}

static TopicSubscriptionFilter instanceSelector(const std::string& service,
                                                  const std::string& host_id,
                                                  const std::string& instance_id) {
    return TopicSubscriptionFilter::instance(service, host_id, instance_id);
}

TEST(TopicManagerTest, CoexistingPublishersAcrossServicesAndInstances) {
    TopicManager mgr;
    ServiceInfo a1 = makeInstanceInfo("svc.a", "10.0.0.1", 9001, "h1", "i1");
    ServiceInfo a2 = makeInstanceInfo("svc.a", "10.0.0.1", 9002, "h1", "i2");
    ServiceInfo b1 = makeInstanceInfo("svc.b", "10.0.0.2", 9101, "h2", "i1");

    EXPECT_TRUE(mgr.registerPublisher("shared/topic", a1, 11));
    EXPECT_TRUE(mgr.registerPublisher("shared/topic", a2, 12));
    EXPECT_TRUE(mgr.registerPublisher("shared/topic", b1, 21));
    // 相同发布者实例重复注册为幂等更新，不再拒绝
    EXPECT_TRUE(mgr.registerPublisher("shared/topic", a1, 11));

    std::vector<TopicPublisherView> pubs =
        mgr.getMatchingPublishers("shared/topic", serviceSelector("svc.a"));
    ASSERT_EQ(pubs.size(), 2u);

    pubs = mgr.getMatchingPublishers("shared/topic", instanceSelector("svc.a", "h1", "i2"));
    ASSERT_EQ(pubs.size(), 1u);
    EXPECT_EQ(pubs[0].info.port, 9002);

    pubs = mgr.getMatchingPublishers("shared/topic", TopicSubscriptionFilter());
    ASSERT_EQ(pubs.size(), 3u);

    EXPECT_TRUE(mgr.removePublisher("shared/topic", 12));
    EXPECT_TRUE(mgr.isPublisherOwner("shared/topic", 11));
    EXPECT_FALSE(mgr.isPublisherOwner("shared/topic", 12));
    EXPECT_TRUE(mgr.isPublisherOwner("shared/topic", 21));
}

TEST(TopicManagerTest, MatchingSubscribersRespectSelector) {
    TopicManager mgr;
    ServiceInfo a1 = makeInstanceInfo("svc.a", "10.0.0.1", 9001, "h1", "i1");
    ServiceInfo a2 = makeInstanceInfo("svc.a", "10.0.0.1", 9002, "h1", "i2");
    ServiceInfo b1 = makeInstanceInfo("svc.b", "10.0.0.2", 9101, "h2", "i1");
    ASSERT_TRUE(mgr.registerPublisher("t", a1, 11));
    ASSERT_TRUE(mgr.registerPublisher("t", a2, 12));
    ASSERT_TRUE(mgr.registerPublisher("t", b1, 21));

    EXPECT_TRUE(mgr.addSubscriber("t", serviceSelector("svc.a"), 100));
    EXPECT_TRUE(mgr.addSubscriber("t", instanceSelector("svc.a", "h1", "i2"), 101));
    EXPECT_TRUE(mgr.addSubscriber("t", TopicSubscriptionFilter(), 102));
    EXPECT_TRUE(mgr.addSubscriber("t", 103));

    std::vector<int> subs = mgr.getMatchingSubscribers("t", a1);
    std::sort(subs.begin(), subs.end());
    ASSERT_EQ(subs.size(), 3u);
    EXPECT_EQ(subs[0], 100);
    EXPECT_EQ(subs[1], 102);
    EXPECT_EQ(subs[2], 103);

    subs = mgr.getMatchingSubscribers("t", a2);
    std::sort(subs.begin(), subs.end());
    ASSERT_EQ(subs.size(), 4u);
    EXPECT_EQ(subs[0], 100);
    EXPECT_EQ(subs[1], 101);
    EXPECT_EQ(subs[2], 102);
    EXPECT_EQ(subs[3], 103);

    // 服务级 svc.a 订阅不匹配 svc.b 发布者
    subs = mgr.getMatchingSubscribers("t", b1);
    std::sort(subs.begin(), subs.end());
    ASSERT_EQ(subs.size(), 2u);
    EXPECT_EQ(subs[0], 102);
    EXPECT_EQ(subs[1], 103);
}

TEST(TopicManagerTest, RejectsInvalidTopicNamesWithoutGrowingState) {
    TopicManager mgr;
    ServiceInfo info = makeInfo("pub", "host", 1000);
    const std::string oversized(MAX_TOPIC_NAME_LENGTH + 1, 'x');
    EXPECT_FALSE(mgr.registerPublisher("", info, 1));
    EXPECT_FALSE(mgr.registerPublisher(oversized, info, 1));
    EXPECT_FALSE(mgr.addSubscriber("", 2));
    EXPECT_FALSE(mgr.addSubscriber(oversized, 2));
    EXPECT_FALSE(mgr.removePublisher("", 1));
    EXPECT_FALSE(mgr.removeSubscriber(oversized, 2));
    EXPECT_TRUE(mgr.listTopics().empty());
}

TEST(TopicManagerTest, EnforcesPerConnectionPublicationLimit) {
    TopicManager mgr;
    ServiceInfo info = makeInfo("pub", "host", 1000);
    for (size_t i = 0; i < MAX_TOPIC_PUBLICATIONS_PER_CONNECTION; ++i) {
        ASSERT_TRUE(mgr.registerPublisher("pub/" + std::to_string(i), info, 7));
    }
    EXPECT_FALSE(mgr.registerPublisher("pub/overflow", info, 7));
    EXPECT_EQ(mgr.getPublishedTopics("pub").size(),
              MAX_TOPIC_PUBLICATIONS_PER_CONNECTION);
}

TEST(TopicManagerTest, EnforcesPerConnectionSubscriptionLimit) {
    TopicManager mgr;
    for (size_t i = 0; i < MAX_TOPIC_SUBSCRIPTIONS_PER_CONNECTION; ++i) {
        ASSERT_TRUE(mgr.addSubscriber("sub/" + std::to_string(i), 9));
    }
    EXPECT_FALSE(mgr.addSubscriber("sub/overflow", 9));
    EXPECT_EQ(mgr.listTopics().size(), MAX_TOPIC_SUBSCRIPTIONS_PER_CONNECTION);
}

TEST(TopicManagerTest, EnforcesGlobalTopicLimit) {
    TopicManager mgr;
    for (size_t i = 0; i < MAX_TOPICS_GLOBAL; ++i) {
        const int fd = static_cast<int>(i / MAX_TOPIC_SUBSCRIPTIONS_PER_CONNECTION) + 1;
        ASSERT_TRUE(mgr.addSubscriber("global/" + std::to_string(i), fd));
    }
    EXPECT_FALSE(mgr.addSubscriber("global/overflow", 99));
    EXPECT_EQ(mgr.listTopics().size(), MAX_TOPICS_GLOBAL);
}

TEST(TopicManagerTest, EnforcesSubscribersPerTopicLimit) {
    TopicManager mgr;
    for (size_t i = 0; i < MAX_SUBSCRIBERS_PER_TOPIC; ++i) {
        ASSERT_TRUE(mgr.addSubscriber("shared/topic", static_cast<int>(i + 1)));
    }
    EXPECT_FALSE(mgr.addSubscriber(
        "shared/topic", static_cast<int>(MAX_SUBSCRIBERS_PER_TOPIC + 1)));
    EXPECT_EQ(mgr.getSubscribers("shared/topic").size(), MAX_SUBSCRIBERS_PER_TOPIC);
}

TEST(TopicManagerTest, EnforcesGlobalSubscriptionEdgeLimitAndReleasesCapacity) {
    TopicManager mgr;
    for (size_t i = 0; i < MAX_TOPIC_SUBSCRIPTIONS_GLOBAL; ++i) {
        const int fd = static_cast<int>(i / MAX_TOPIC_SUBSCRIPTIONS_PER_CONNECTION) + 1;
        ASSERT_TRUE(mgr.addSubscriber("edge/" + std::to_string(i), fd));
    }
    EXPECT_FALSE(mgr.addSubscriber("edge/overflow", 99));
    EXPECT_TRUE(mgr.removeSubscriber("edge/0", 1));
    EXPECT_TRUE(mgr.addSubscriber("edge/reused", 99));
}

// ============================================================
// TopicRuntime — SHM 订阅关系（由 test_shm_transport 迁入）
// ============================================================

TEST(TopicRuntimeTest, ExactShmRemovalPreservesOtherClientsAndServices) {
    TopicRuntime topics;
    const uint32_t first_topic = 11;
    const uint32_t second_topic = 22;
    topics.addShmSubscriberService(first_topic, "service-a", 1);
    topics.addShmSubscriberService(first_topic, "service-a", 2);
    topics.addShmSubscriberService(first_topic, "service-b", 1);
    topics.addShmSubscriberService(second_topic, "service-a", 1);
    topics.addShmSubscriberService(second_topic, "service-a", 3);

    topics.removeShmSubscriberService("service-a", 1);

    ASSERT_EQ(topics.shmSubscribers(first_topic).size(), 2u);
    EXPECT_EQ(topics.shmSubscribers(first_topic)[0].client_id, 2u);
    EXPECT_EQ(topics.shmSubscribers(first_topic)[1].service_name, "service-b");
    ASSERT_EQ(topics.shmSubscribers(second_topic).size(), 1u);
    EXPECT_EQ(topics.shmSubscribers(second_topic)[0].client_id, 3u);
}
