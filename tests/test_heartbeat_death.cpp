#include "heartbeat_monitor.h"
#include "death_notifier.h"
#include <platform/platform.h>
#include <cstdio>
#include <cassert>
#include <algorithm>

using namespace omnibinder;

#define TEST(name) printf("  TEST %s ... ", #name);
#define PASS() printf("PASS\n");

// ============================================================
// HeartbeatMonitor Tests
// ============================================================

void test_heartbeat_start_stop_tracking() {
    TEST(heartbeat_start_stop_tracking) {
        HeartbeatMonitor monitor(1000, 3);

        assert(monitor.trackedCount() == 0);

        monitor.startTracking("serviceA");
        assert(monitor.trackedCount() == 1);

        monitor.startTracking("serviceB");
        assert(monitor.trackedCount() == 2);

        monitor.stopTracking("serviceA");
        assert(monitor.trackedCount() == 1);

        monitor.stopTracking("serviceB");
        assert(monitor.trackedCount() == 0);

        // Stopping a non-existent service should be harmless
        monitor.stopTracking("nonexistent");
        assert(monitor.trackedCount() == 0);

        PASS();
    }
}

void test_heartbeat_update_resets_missed() {
    TEST(heartbeat_update_resets_missed) {
        // Use a very short timeout so we can trigger missed heartbeats quickly
        HeartbeatMonitor monitor(50, 3);

        monitor.startTracking("serviceA");

        // Wait long enough for one heartbeat period to elapse
        platform::sleepMs(60);

        // checkTimeouts should increment missed_count but not yet timeout (need 3)
        std::vector<std::string> timed_out = monitor.checkTimeouts();
        assert(timed_out.empty());
        assert(monitor.trackedCount() == 1);

        // Now update the heartbeat - this should reset missed_count to 0
        monitor.updateHeartbeat("serviceA");

        // Wait another period
        platform::sleepMs(60);

        // Should only have 1 missed again (not 2), because update reset the count
        timed_out = monitor.checkTimeouts();
        assert(timed_out.empty());
        assert(monitor.trackedCount() == 1);

        PASS();
    }
}

void test_heartbeat_no_timeout_when_fresh() {
    TEST(heartbeat_no_timeout_when_fresh) {
        HeartbeatMonitor monitor(200, 3);

        monitor.startTracking("serviceA");
        monitor.startTracking("serviceB");

        // Immediately check - no time has passed, should not timeout
        std::vector<std::string> timed_out = monitor.checkTimeouts();
        assert(timed_out.empty());
        assert(monitor.trackedCount() == 2);

        // Sleep a short time but less than timeout
        platform::sleepMs(50);
        timed_out = monitor.checkTimeouts();
        assert(timed_out.empty());
        assert(monitor.trackedCount() == 2);

        PASS();
    }
}

void test_heartbeat_timeout_after_missed() {
    TEST(heartbeat_timeout_after_missed) {
        // 50ms timeout, max 2 missed before declaring timeout
        HeartbeatMonitor monitor(50, 2);

        monitor.startTracking("serviceA");
        monitor.startTracking("serviceB");

        // Miss #1 for both
        platform::sleepMs(60);
        std::vector<std::string> timed_out = monitor.checkTimeouts();
        assert(timed_out.empty());
        assert(monitor.trackedCount() == 2);

        // Keep serviceB alive
        monitor.updateHeartbeat("serviceB");

        // Miss #2 for serviceA (should timeout), miss #1 for serviceB (reset)
        platform::sleepMs(60);
        timed_out = monitor.checkTimeouts();
        assert(timed_out.size() == 1);
        assert(timed_out[0] == "serviceA");

        // serviceA should be removed, serviceB still tracked
        assert(monitor.trackedCount() == 1);

        PASS();
    }
}

void test_heartbeat_tracked_count() {
    TEST(heartbeat_tracked_count) {
        HeartbeatMonitor monitor(1000, 3);

        assert(monitor.trackedCount() == 0);

        monitor.startTracking("s1");
        assert(monitor.trackedCount() == 1);

        monitor.startTracking("s2");
        monitor.startTracking("s3");
        assert(monitor.trackedCount() == 3);

        // updateHeartbeat on a new service also adds it
        monitor.updateHeartbeat("s4");
        assert(monitor.trackedCount() == 4);

        // Re-tracking an existing service should overwrite, not duplicate
        monitor.startTracking("s1");
        assert(monitor.trackedCount() == 4);

        monitor.stopTracking("s1");
        monitor.stopTracking("s2");
        assert(monitor.trackedCount() == 2);

        PASS();
    }
}

// ============================================================
// DeathNotifier Tests
// ============================================================

void test_death_subscribe_unsubscribe() {
    TEST(death_subscribe_unsubscribe) {
        DeathNotifier notifier;

        // Subscribe fd=10 to "serviceA"
        assert(notifier.subscribe(10, "serviceA") == true);

        // Duplicate subscription should return false
        assert(notifier.subscribe(10, "serviceA") == false);

        assert(notifier.subscriberCount("serviceA") == 1);

        // Unsubscribe
        assert(notifier.unsubscribe(10, "serviceA") == true);
        assert(notifier.subscriberCount("serviceA") == 0);

        // Unsubscribe again should return false
        assert(notifier.unsubscribe(10, "serviceA") == false);

        // Unsubscribe from non-existent service
        assert(notifier.unsubscribe(10, "nonexistent") == false);

        PASS();
    }
}

void test_death_notify_returns_correct_fds() {
    TEST(death_notify_returns_correct_fds) {
        DeathNotifier notifier;

        notifier.subscribe(10, "serviceA");
        notifier.subscribe(20, "serviceA");
        notifier.subscribe(30, "serviceB");

        // Notify death of serviceA
        std::vector<int> fds = notifier.notify("serviceA");
        assert(fds.size() == 2);

        // Check both fds are present (order may vary since it comes from a set)
        std::sort(fds.begin(), fds.end());
        assert(fds[0] == 10);
        assert(fds[1] == 20);

        // After notify, serviceA subscriptions should be cleaned up
        assert(notifier.subscriberCount("serviceA") == 0);

        // serviceB should be unaffected
        assert(notifier.subscriberCount("serviceB") == 1);

        // Notify for a service with no subscribers
        std::vector<int> empty = notifier.notify("nonexistent");
        assert(empty.empty());

        PASS();
    }
}

void test_death_remove_subscriber() {
    TEST(death_remove_subscriber) {
        DeathNotifier notifier;

        notifier.subscribe(10, "serviceA");
        notifier.subscribe(10, "serviceB");
        notifier.subscribe(10, "serviceC");
        notifier.subscribe(20, "serviceA");

        // Verify fd=10 watches 3 services
        std::vector<std::string> watched = notifier.getWatchedServices(10);
        assert(watched.size() == 3);

        // Remove all subscriptions for fd=10
        notifier.removeSubscriber(10);

        // fd=10 should have no watched services
        watched = notifier.getWatchedServices(10);
        assert(watched.empty());

        // serviceA should still have fd=20
        assert(notifier.subscriberCount("serviceA") == 1);

        // serviceB and serviceC should have no subscribers
        assert(notifier.subscriberCount("serviceB") == 0);
        assert(notifier.subscriberCount("serviceC") == 0);

        // Removing a non-existent subscriber should be harmless
        notifier.removeSubscriber(999);

        PASS();
    }
}

void test_death_multiple_subscribers_same_service() {
    TEST(death_multiple_subscribers_same_service) {
        DeathNotifier notifier;

        assert(notifier.subscribe(10, "serviceX") == true);
        assert(notifier.subscribe(20, "serviceX") == true);
        assert(notifier.subscribe(30, "serviceX") == true);
        assert(notifier.subscribe(40, "serviceX") == true);

        assert(notifier.subscriberCount("serviceX") == 4);

        // Unsubscribe one
        notifier.unsubscribe(20, "serviceX");
        assert(notifier.subscriberCount("serviceX") == 3);

        // Notify death - should return remaining 3
        std::vector<int> fds = notifier.notify("serviceX");
        assert(fds.size() == 3);

        std::sort(fds.begin(), fds.end());
        assert(fds[0] == 10);
        assert(fds[1] == 30);
        assert(fds[2] == 40);

        // All cleaned up after notify
        assert(notifier.subscriberCount("serviceX") == 0);

        PASS();
    }
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== HeartbeatMonitor Tests ===\n");
    test_heartbeat_start_stop_tracking();
    test_heartbeat_update_resets_missed();
    test_heartbeat_no_timeout_when_fresh();
    test_heartbeat_timeout_after_missed();
    test_heartbeat_tracked_count();

    printf("\n=== DeathNotifier Tests ===\n");
    test_death_subscribe_unsubscribe();
    test_death_notify_returns_correct_fds();
    test_death_remove_subscriber();
    test_death_multiple_subscribers_same_service();

    printf("\nAll heartbeat & death notifier tests passed!\n");
    return 0;
}
