#ifndef OMNIBINDER_TESTS_RUNTIME_THREAD_H
#define OMNIBINDER_TESTS_RUNTIME_THREAD_H

#include <omnibinder/runtime.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace omnibinder {
namespace test {

/*
 * 测试专用 runtime 驱动：业务线程执行 start()，析构时 stop()+join()。
 * 只存在于 tests/，不安装、不进入 include/，不引入核心线程依赖。
 */
class RuntimeThread {
public:
    explicit RuntimeThread(OmniRuntime& runtime,
                           int poll_timeout_ms = -1,
                           const LoopTickCallback& on_tick = LoopTickCallback())
        : runtime_(runtime), stopped_(false),
          thread_([this, poll_timeout_ms, on_tick]() {
              runtime_.start(poll_timeout_ms, on_tick);
          }) {
        for (int i = 0; i < 200 && !runtime_.isRunning(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~RuntimeThread() { stop(); }

    void stop() {
        if (stopped_.exchange(true)) return;
        runtime_.stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    RuntimeThread(const RuntimeThread&);
    RuntimeThread& operator=(const RuntimeThread&);

    OmniRuntime& runtime_;
    std::atomic<bool> stopped_;
    std::thread thread_;
};

} // namespace test
} // namespace omnibinder

#endif
