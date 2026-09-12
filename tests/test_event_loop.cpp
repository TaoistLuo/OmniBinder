#include <gtest/gtest.h>
#include "core/event_loop.h"
#include "platform/platform.h"

#include <atomic>
#include <thread>

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

TEST(EventLoopTest, StopBeforeRunIsNotSwallowed) {
    EventLoop loop;
    loop.stop();
    // 修复前：run() 无条件 running_=true，stop 被吞掉，run 永不退出
    loop.run();
    EXPECT_TRUE(loop.stopRequested());
}

TEST(EventLoopTest, StopDuringRunExits) {
    EventLoop loop;
    std::atomic<bool> returned(false);
    std::thread t([&loop, &returned]() {
        loop.run();
        returned.store(true);
    });
    loop.stop();
    t.join();
    EXPECT_TRUE(returned.load());
}

TEST(EventLoopTest, PostAfterStopIsRejected) {
    EventLoop loop;
    loop.stop();
    bool called = false;
    EXPECT_FALSE(loop.post([&called]() { called = true; }));
    loop.pollOnce(0);
    EXPECT_FALSE(called);
}

TEST(EventLoopTest, PostBeforeStopIsDrainedByRun) {
    EventLoop loop;
    std::atomic<bool> called(false);
    ASSERT_TRUE(loop.post([&called]() { called.store(true); }));
    loop.stop();
    // 关闭前成功入队的 functor 必须在 run() 返回前被排空，否则投递线程永久挂起
    loop.run();
    EXPECT_TRUE(called.load());
}
