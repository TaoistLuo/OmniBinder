#include <gtest/gtest.h>
#include "service_watch_notifier.h"

using namespace omnibinder;

TEST(ServiceWatchNotifierTest, MatchesInstanceFiltersIgnoringHost) {
    ServiceWatchNotifier notifier;
    std::vector<ServiceWatchFilter> filters;
    filters.push_back(ServiceWatchFilter("Svc", "host-A", ""));
    filters.push_back(ServiceWatchFilter("Other", "", "instance-1"));
    ASSERT_TRUE(notifier.subscribe(7, 100, filters));

    std::vector<ServiceWatchGroup> hit =
        notifier.matching(ServiceIdentity("Svc", "host-B", "instance-9"));
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0].watch_id, 100u);

    EXPECT_TRUE(notifier.matching(ServiceIdentity("Other", "host-B", "instance-1")).size() == 1u);
    EXPECT_TRUE(notifier.matching(ServiceIdentity("Other", "host-B", "instance-2")).empty());
}

TEST(ServiceWatchNotifierTest, OverlapDetectionIsLogical) {
    EXPECT_TRUE(ServiceWatchNotifier::overlaps(
        ServiceWatchFilter("Svc", "", ""), ServiceWatchFilter("Svc", "host-A", "id")));
    EXPECT_TRUE(ServiceWatchNotifier::overlaps(
        ServiceWatchFilter("Svc", "host-A", ""), ServiceWatchFilter("Svc", "", "id")));
    EXPECT_TRUE(ServiceWatchNotifier::overlaps(
        ServiceWatchFilter("Svc", "host-A", ""), ServiceWatchFilter("Svc", "host-B", "")));
    EXPECT_FALSE(ServiceWatchNotifier::overlaps(
        ServiceWatchFilter("Svc", "", "id-1"), ServiceWatchFilter("Svc", "", "id-2")));
}

TEST(ServiceWatchNotifierTest, RemoveSubscriberDropsGroup) {
    ServiceWatchNotifier notifier;
    std::vector<ServiceWatchFilter> filters;
    filters.push_back(ServiceWatchFilter("Svc", "", ""));
    ASSERT_TRUE(notifier.subscribe(3, 10, filters));
    notifier.removeSubscriber(3);
    EXPECT_TRUE(notifier.matching(ServiceIdentity("Svc", "h", "i")).empty());
    EXPECT_FALSE(notifier.unsubscribe(3, 10));
}
