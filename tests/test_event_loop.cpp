#include <gtest/gtest.h>
#include "core/event_loop.h"
#include "platform/platform.h"

using namespace omnibinder;

TEST(EventLoopTest, Timer) {
    EventLoop loop;
    int count = 0;
    loop.addTimer(50, [&count, &loop]() {
        count++;
        if (count >= 3) loop.stop();
    }, true);
    loop.run();
    EXPECT_GE(count, 3);
}

TEST(EventLoopTest, Post) {
    EventLoop loop;
    bool called = false;
    loop.addTimer(10, [&loop, &called]() {
        loop.post([&called, &loop]() {
            called = true;
            loop.stop();
        });
    }, false);
    loop.run();
    EXPECT_TRUE(called);
}

TEST(EventLoopTest, CallbackCanRemoveItsOwnFd) {
    EventLoop loop;
    int event_fd = platform::createEventFd();
    ASSERT_GE(event_fd, 0);
    bool continued_after_remove = false;
    loop.addFd(event_fd, EventLoop::EVENT_READ,
        [&loop, &continued_after_remove](int fd, uint32_t) {
            loop.removeFd(fd);
            continued_after_remove = true;
        });
    ASSERT_TRUE(platform::eventFdNotify(event_fd));
    loop.pollOnce(100);
    EXPECT_TRUE(continued_after_remove);
    platform::closeEventFd(event_fd);
}
