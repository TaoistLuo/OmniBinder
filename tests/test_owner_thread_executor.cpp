#include <gtest/gtest.h>
#include "core/owner_thread_executor.h"
#include "core/event_loop.h"

#include <atomic>
#include <thread>

using namespace omnibinder;

TEST(OwnerThreadExecutorTest, InlineWhenLoopNotOwned) {
    OwnerThreadExecutor executor;
    std::atomic<bool> called(false);
    int value = executor.invoke([&called]() -> int {
        called.store(true);
        return 42;
    });
    EXPECT_TRUE(called.load());
    EXPECT_EQ(value, 42);
}

TEST(OwnerThreadExecutorTest, CrossThreadExecutesOnOwnerLoop) {
    EventLoop loop;
    OwnerThreadExecutor executor;
    executor.bindLoop(&loop);

    std::atomic<bool> ready(false);
    std::atomic<bool> done(false);
    std::atomic<int> result(0);
    std::thread::id owner_id;
    std::thread::id worker_seen_id;

    std::thread loop_thread([&]() {
        owner_id = std::this_thread::get_id();
        executor.setOwnerThread(owner_id);
        executor.setLoopOwned(true);
        ready.store(true);
        loop.run();
        executor.setLoopOwned(false);
    });

    while (!ready.load()) {
        std::this_thread::yield();
    }

    std::thread worker([&]() {
        result.store(executor.invoke([&]() -> int {
            worker_seen_id = std::this_thread::get_id();
            return 7;
        }));
        done.store(true);
        loop.stop();
    });

    worker.join();
    loop_thread.join();

    EXPECT_TRUE(done.load());
    EXPECT_EQ(result.load(), 7);
    EXPECT_EQ(worker_seen_id, owner_id);
}

TEST(OwnerThreadExecutorTest, VoidInvokePropagatesCompletion) {
    EventLoop loop;
    OwnerThreadExecutor executor;
    executor.bindLoop(&loop);

    std::atomic<bool> ready(false);
    std::atomic<bool> called(false);

    std::thread loop_thread([&]() {
        executor.setOwnerThread(std::this_thread::get_id());
        executor.setLoopOwned(true);
        ready.store(true);
        loop.run();
        executor.setLoopOwned(false);
    });

    while (!ready.load()) {
        std::this_thread::yield();
    }

    std::thread worker([&]() {
        int ret = executor.invoke([&called]() {
            called.store(true);
        });
        EXPECT_EQ(ret, 0);
        loop.stop();
    });

    worker.join();
    loop_thread.join();

    EXPECT_TRUE(called.load());
}

TEST(OwnerThreadExecutorTest, InvokeOnOwnerReturnsFuncResult) {
    OwnerThreadExecutor executor;
    int value = executor.invokeOnOwner([]() -> int {
        return 42;
    });
    EXPECT_EQ(value, 42);
}
