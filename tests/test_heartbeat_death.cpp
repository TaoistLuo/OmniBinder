#include <gtest/gtest.h>
#include "heartbeat_monitor.h"
#include "death_notifier.h"
#include <platform/platform.h>
#include <algorithm>

using namespace omnibinder;

// ============================================================
// HeartbeatMonitor
// ============================================================

TEST(HeartbeatMonitorTest, StartStopTracking) {
    HeartbeatMonitor monitor(1000, 3);
    EXPECT_EQ(monitor.trackedCount(), 0u);

    monitor.startTracking("serviceA");
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.startTracking("serviceB");
    EXPECT_EQ(monitor.trackedCount(), 2u);

    monitor.stopTracking("serviceA");
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.stopTracking("serviceB");
    EXPECT_EQ(monitor.trackedCount(), 0u);

    monitor.stopTracking("nonexistent");
    EXPECT_EQ(monitor.trackedCount(), 0u);
}

TEST(HeartbeatMonitorTest, UpdateResetsMissed) {
    HeartbeatMonitor monitor(50, 3);
    monitor.startTracking("serviceA");

    platform::sleepMs(60);
    std::vector<std::string> timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.updateHeartbeat("serviceA");
    platform::sleepMs(60);
    timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 1u);
}

TEST(HeartbeatMonitorTest, NoTimeoutWhenFresh) {
    HeartbeatMonitor monitor(200, 3);
    monitor.startTracking("serviceA");
    monitor.startTracking("serviceB");

    std::vector<std::string> timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 2u);

    platform::sleepMs(50);
    timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 2u);
}

TEST(HeartbeatMonitorTest, TimeoutAfterMissed) {
    HeartbeatMonitor monitor(50, 2);
    monitor.startTracking("serviceA");
    monitor.startTracking("serviceB");

    platform::sleepMs(60);
    std::vector<std::string> timed_out = monitor.checkTimeouts();
    EXPECT_TRUE(timed_out.empty());
    EXPECT_EQ(monitor.trackedCount(), 2u);

    monitor.updateHeartbeat("serviceB");

    platform::sleepMs(60);
    timed_out = monitor.checkTimeouts();
    ASSERT_EQ(timed_out.size(), 1u);
    EXPECT_EQ(timed_out[0], "serviceA");
    EXPECT_EQ(monitor.trackedCount(), 1u);
}

TEST(HeartbeatMonitorTest, TrackedCount) {
    HeartbeatMonitor monitor(1000, 3);
    EXPECT_EQ(monitor.trackedCount(), 0u);

    monitor.startTracking("s1");
    EXPECT_EQ(monitor.trackedCount(), 1u);

    monitor.startTracking("s2");
    monitor.startTracking("s3");
    EXPECT_EQ(monitor.trackedCount(), 3u);

    monitor.updateHeartbeat("s4");
    EXPECT_EQ(monitor.trackedCount(), 4u);

    monitor.startTracking("s1");
    EXPECT_EQ(monitor.trackedCount(), 4u);

    monitor.stopTracking("s1");
    monitor.stopTracking("s2");
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

TEST(DeathNotifierTest, NotifyReturnsCorrectFds) {
    DeathNotifier notifier;
    notifier.subscribe(10, "serviceA");
    notifier.subscribe(20, "serviceA");
    notifier.subscribe(30, "serviceB");

    std::vector<int> fds = notifier.notify("serviceA");
    ASSERT_EQ(fds.size(), 2u);
    std::sort(fds.begin(), fds.end());
    EXPECT_EQ(fds[0], 10);
    EXPECT_EQ(fds[1], 20);

    EXPECT_EQ(notifier.subscriberCount("serviceA"), 0u);
    EXPECT_EQ(notifier.subscriberCount("serviceB"), 1u);

    std::vector<int> empty = notifier.notify("nonexistent");
    EXPECT_TRUE(empty.empty());
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

    std::vector<int> fds = notifier.notify("serviceX");
    ASSERT_EQ(fds.size(), 3u);
    std::sort(fds.begin(), fds.end());
    EXPECT_EQ(fds[0], 10);
    EXPECT_EQ(fds[1], 30);
    EXPECT_EQ(fds[2], 40);

    EXPECT_EQ(notifier.subscriberCount("serviceX"), 0u);
}
