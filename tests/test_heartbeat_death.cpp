#include <gtest/gtest.h>
#include "heartbeat_monitor.h"
#include "death_notifier.h"
#include <platform/platform.h>

using namespace omnibinder;

// ============================================================
// HeartbeatMonitor
// ============================================================

TEST(HeartbeatMonitorTest, StartStopTracking) {
    HeartbeatMonitor monitor(1000, 3);
    EXPECT_EQ(monitor.trackedCount(), 0u);

    monitor.startTracking(ServiceIdentity("serviceA"));
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.startTracking(ServiceIdentity("serviceB"));
    EXPECT_EQ(monitor.trackedCount(), 2u);

    monitor.stopTracking(ServiceIdentity("serviceA"));
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.stopTracking(ServiceIdentity("serviceB"));
    EXPECT_EQ(monitor.trackedCount(), 0u);

    monitor.stopTracking(ServiceIdentity("nonexistent"));
    EXPECT_EQ(monitor.trackedCount(), 0u);
}

TEST(HeartbeatMonitorTest, UpdateResetsMissed) {
    // timeout=300ms, max_missed=2：若 updateHeartbeat 未重置时间戳，
    // 第二次 check 的 elapsed 将累计到 ~700ms（missed>=2）而超时，用例即失败；
    // 因而该用例能真正证伪"重置失效"。
    HeartbeatMonitor monitor(300, 2);
    monitor.startTracking(ServiceIdentity("serviceA"));

    platform::sleepMs(350);
    std::vector<ServiceIdentity> timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.updateHeartbeat(ServiceIdentity("serviceA"));
    platform::sleepMs(350);
    timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 1u);
}

TEST(HeartbeatMonitorTest, NoTimeoutWhenFresh) {
    HeartbeatMonitor monitor(200, 3);
    monitor.startTracking(ServiceIdentity("serviceA"));
    monitor.startTracking(ServiceIdentity("serviceB"));

    std::vector<ServiceIdentity> timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 2u);

    platform::sleepMs(50);
    timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 2u);
}

TEST(HeartbeatMonitorTest, TimeoutAfterMissed) {
    HeartbeatMonitor monitor(50, 2);
    monitor.startTracking(ServiceIdentity("serviceA"));
    monitor.startTracking(ServiceIdentity("serviceB"));

    platform::sleepMs(60);
    std::vector<ServiceIdentity> timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 2u);

    monitor.updateHeartbeat(ServiceIdentity("serviceB"));

    platform::sleepMs(60);
    timed_out = monitor.checkTimeouts();
    ASSERT_EQ(timed_out.size(), 1u);
    EXPECT_EQ(timed_out[0].name, "serviceA");
    EXPECT_EQ(monitor.trackedCount(), 1u);
}

TEST(HeartbeatMonitorTest, SameNameInstancesTrackedSeparately) {
    HeartbeatMonitor monitor(50, 2);
    ServiceIdentity inst_a("serviceA", "host-1", "inst-a");
    ServiceIdentity inst_b("serviceA", "host-1", "inst-b");
    monitor.startTracking(inst_a);
    monitor.startTracking(inst_b);
    EXPECT_EQ(monitor.trackedCount(), 2u);

    platform::sleepMs(60);
    monitor.updateHeartbeat(inst_b);

    platform::sleepMs(60);
    std::vector<ServiceIdentity> timed_out = monitor.checkTimeouts();
    ASSERT_EQ(timed_out.size(), 1u);
    EXPECT_EQ(timed_out[0].name, "serviceA");
    EXPECT_EQ(timed_out[0].instance_id, "inst-a");
    EXPECT_EQ(monitor.trackedCount(), 1u);

    // 同名另一实例仍被跟踪，未被误删
    monitor.stopTracking(inst_b);
    EXPECT_EQ(monitor.trackedCount(), 0u);
}

TEST(HeartbeatMonitorTest, TrackedCount) {
    HeartbeatMonitor monitor(1000, 3);
    EXPECT_EQ(monitor.trackedCount(), 0u);

    monitor.startTracking(ServiceIdentity("s1"));
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.startTracking(ServiceIdentity("s2"));
    monitor.startTracking(ServiceIdentity("s3"));
    EXPECT_EQ(monitor.trackedCount(), 3u);

    monitor.updateHeartbeat(ServiceIdentity("s4"));
    EXPECT_EQ(monitor.trackedCount(), 4u);

    monitor.startTracking(ServiceIdentity("s1"));
    EXPECT_EQ(monitor.trackedCount(), 4u);

    monitor.stopTracking(ServiceIdentity("s1"));
    monitor.stopTracking(ServiceIdentity("s2"));
    EXPECT_EQ(monitor.trackedCount(), 2u);
}

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
