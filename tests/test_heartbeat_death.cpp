#include <gtest/gtest.h>
#include "death_notifier.h"
#include <platform/platform.h>

using namespace omnibinder;

// ============================================================
// DeathNotifier
// ============================================================

TEST(DeathNotifierTest, SubscribeUnsubscribe) {
    DeathNotifier notifier;
    EXPECT_TRUE(notifier.subscribe(10, "serviceA"));
    EXPECT_FALSE(notifier.subscribe(10, "serviceA"));
    EXPECT_EQ(notifier.subscriberCount("serviceA"), 1u);

    EXPECT_TRUE(notifier.unsubscribe(10, "serviceA"));
    EXPECT_EQ(notifier.subscriberCount("serviceA"), 0u);

    EXPECT_FALSE(notifier.unsubscribe(10, "serviceA"));
    EXPECT_FALSE(notifier.unsubscribe(10, "nonexistent"));
}

TEST(DeathNotifierTest, RemoveSubscriber) {
    DeathNotifier notifier;
    notifier.subscribe(10, "serviceA");
    notifier.subscribe(10, "serviceB");
    notifier.subscribe(10, "serviceC");
    notifier.subscribe(20, "serviceA");

    std::vector<std::string> watched = notifier.getWatchedServices(10);
    EXPECT_EQ(watched.size(), 3u);

    notifier.removeSubscriber(10);
    watched = notifier.getWatchedServices(10);
    EXPECT_TRUE(watched.empty());

    EXPECT_EQ(notifier.subscriberCount("serviceA"), 1u);
    EXPECT_EQ(notifier.subscriberCount("serviceB"), 0u);
    EXPECT_EQ(notifier.subscriberCount("serviceC"), 0u);

    notifier.removeSubscriber(999);
}

TEST(DeathNotifierTest, MultipleSubscribersSameService) {
    DeathNotifier notifier;
    EXPECT_TRUE(notifier.subscribe(10, "serviceX"));
    EXPECT_TRUE(notifier.subscribe(20, "serviceX"));
    EXPECT_TRUE(notifier.subscribe(30, "serviceX"));
    EXPECT_TRUE(notifier.subscribe(40, "serviceX"));

    EXPECT_EQ(notifier.subscriberCount("serviceX"), 4u);

    notifier.unsubscribe(20, "serviceX");
    EXPECT_EQ(notifier.subscriberCount("serviceX"), 3u);
}

TEST(DeathNotifierTest, InstanceSubscriptionsAreIsolated) {
    DeathNotifier notifier;
    ServiceIdentity inst_a("Svc", "host-1", "inst-a");
    ServiceIdentity inst_b("Svc", "host-1", "inst-b");

    EXPECT_TRUE(notifier.subscribeInstance(10, inst_a));
    EXPECT_FALSE(notifier.subscribeInstance(10, inst_a));
    EXPECT_TRUE(notifier.subscribeInstance(20, inst_b));

    std::vector<int> fds = notifier.notifyInstanceDeath(inst_a, false);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_EQ(fds[0], 10);

    // inst-a 的订阅在通知后移除，inst-b 的订阅不受影响
    fds = notifier.notifyInstanceDeath(inst_a, false);
    EXPECT_TRUE(fds.empty());

    fds = notifier.notifyInstanceDeath(inst_b, false);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_EQ(fds[0], 20);
}

TEST(DeathNotifierTest, NameSubscribersOnlyNotifiedWhenNameGone) {
    DeathNotifier notifier;
    ServiceIdentity inst_a("Svc", "host-1", "inst-a");
    ServiceIdentity inst_b("Svc", "host-1", "inst-b");

    EXPECT_TRUE(notifier.subscribe(30, "Svc"));
    EXPECT_TRUE(notifier.subscribeInstance(30, inst_a));

    // a 死亡但名字未消失：仅实例级返回，名字级订阅保留
    std::vector<int> fds = notifier.notifyInstanceDeath(inst_a, false);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_EQ(fds[0], 30);
    EXPECT_EQ(notifier.subscriberCount("Svc"), 1u);

    // b 死亡且名字消失：同一 fd 的名字级订阅也被通知并移除
    fds = notifier.notifyInstanceDeath(inst_b, true);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_EQ(fds[0], 30);
    EXPECT_EQ(notifier.subscriberCount("Svc"), 0u);
}

TEST(DeathNotifierTest, RemoveSubscriberClearsInstanceSubscriptions) {
    DeathNotifier notifier;
    ServiceIdentity inst_a("Svc", "host-1", "inst-a");
    notifier.subscribeInstance(10, inst_a);
    notifier.removeSubscriber(10);

    std::vector<int> fds = notifier.notifyInstanceDeath(inst_a, false);
    EXPECT_TRUE(fds.empty());
}

TEST(DeathNotifierTest, InstanceUnsubscribeRemovesOnlyTargetInstance) {
    DeathNotifier notifier;
    ServiceIdentity inst_a("Svc", "host-1", "inst-a");
    ServiceIdentity inst_b("Svc", "host-1", "inst-b");
    notifier.subscribeInstance(10, inst_a);
    notifier.subscribeInstance(20, inst_b);

    EXPECT_TRUE(notifier.unsubscribeInstance(10, inst_a));
    EXPECT_FALSE(notifier.unsubscribeInstance(10, inst_a));

    std::vector<int> fds = notifier.notifyInstanceDeath(inst_a, false);
    EXPECT_TRUE(fds.empty());
    fds = notifier.notifyInstanceDeath(inst_b, false);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_EQ(fds[0], 20);
}
