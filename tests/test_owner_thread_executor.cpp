#include "core/owner_thread_executor.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace omnibinder;

#define TEST(name) printf("  TEST %-42s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)

int main() {
    printf("=== OwnerThreadExecutor Tests ===\n\n");

    TEST(inline_when_loop_not_owned);
    {
        OwnerThreadExecutor executor;
        std::atomic<bool> called(false);
        int value = executor.invoke([&called]() -> int {
            called.store(true);
            return 42;
        });
        if (!called.load()) return 1;
        if (value != 42) return 1;
        PASS();
    }

    TEST(cross_thread_executes_on_owner_loop);
    {
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

        assert(done.load());
        assert(result.load() == 7);
        assert(worker_seen_id == owner_id);
        PASS();
    }

    TEST(cross_thread_exception_completes_waiter);
    {
        EventLoop loop;
        OwnerThreadExecutor executor;
        executor.bindLoop(&loop);

        std::atomic<bool> ready(false);
        std::atomic<bool> caught(false);
        std::string error_message;

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
            try {
                executor.invoke([]() -> int {
                    throw std::runtime_error("boom");
                });
            } catch (const std::runtime_error& e) {
                caught.store(true);
                error_message = e.what();
            }
            loop.stop();
        });

        worker.join();
        loop_thread.join();

        assert(caught.load());
        assert(error_message == "boom");
        PASS();
    }

    TEST(void_invoke_propagates_completion);
    {
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
            executor.invoke([&called]() {
                called.store(true);
            });
            loop.stop();
        });

        worker.join();
        loop_thread.join();

        assert(called.load());
        PASS();
    }

    printf("\nAll OwnerThreadExecutor tests passed!\n");
    return 0;
}
