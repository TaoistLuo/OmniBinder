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
