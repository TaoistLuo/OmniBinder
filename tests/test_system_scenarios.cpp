/**************************************************************************************************
 * @file        test_system_scenarios.cpp
 * @brief       系统级场景测试 — SM 重启、纯客户端重连、进程崩溃恢复
 * @details     覆盖单元测试无法验证的生产级场景：SM 断线重连、无服务客户端存活、
 *              服务端崩溃恢复等跨进程生命周期场景。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-07-19
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/

#include <gtest/gtest.h>
#include "test_common.h"
#include <omnibinder/runtime.h>
#include <omnibinder/service.h>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace omnibinder;
using namespace omnibinder::test;

static const uint16_t SCENARIO_SM_PORT = 19920;

// ─── 场景 1: 纯客户端（无注册服务）SM 重启后自动重连 ───
//
// 这是之前缺失的测试：客户端未注册任何服务时，sendHeartbeat 的
// for(local_services_) 循环为空，不会调用 sendToSM，因此重连永远不触发。
// 修复后 sendHeartbeat 开头无条件检查 reconnectServiceManagerIfNeeded。

TEST(SmRestartPureClient, ClientReconnectsAfterSmRestart) {
    // 启动 SM
    TestPid sm_pid = startProcess("./target/bin/service_manager",
        "--port", std::to_string(SCENARIO_SM_PORT).c_str(),
        "--log-level", "3");
    ASSERT_GT(sm_pid, 0);
    ASSERT_TRUE(waitPortReady(SCENARIO_SM_PORT, 30));

    // 创建纯客户端（不注册任何服务，不发布 topic）
    OmniRuntime runtime;
    ASSERT_EQ(runtime.init("127.0.0.1", SCENARIO_SM_PORT), 0);

    // 启动 event-loop（在后台线程驱动，模拟真实运行）
    std::atomic<bool> client_running{true};
    std::thread client_thread([&]() {
        while (client_running.load()) {
            runtime.pollOnce(100);
        }
    });

    // 验证客户端可以查询自己的运行时信息（证明 SM 连接正常）
    std::vector<RuntimeInfo> runtimes;
    int ret = runtime.listRuntimes(runtimes);
    EXPECT_EQ(ret, 0);
    EXPECT_GE(runtimes.size(), 1u) << "Client should see itself in runtime list";

    // 杀死 SM
    stopProcess(sm_pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 重启 SM
    sm_pid = startProcess("./target/bin/service_manager",
        "--port", std::to_string(SCENARIO_SM_PORT).c_str(),
        "--log-level", "3");
    ASSERT_GT(sm_pid, 0);
    ASSERT_TRUE(waitPortReady(SCENARIO_SM_PORT, 30));

    // 等待客户端自动重连（最多等 10 秒，heartbeat_interval 默认 5 秒）
    bool reconnected = false;
    for (int i = 0; i < 100 && !reconnected; ++i) {
        std::vector<RuntimeInfo> after;
        if (runtime.listRuntimes(after) == 0 && after.size() >= 1u) {
            reconnected = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(reconnected) << "Pure client should reconnect to SM after restart";

    // 清理
    client_running = false;
    runtime.stop();
    client_thread.join();
    stopProcess(sm_pid);
}

// ============================================================
// 注：死亡通知测试（场景 2）需要独立进程 + SIGKILL 才能
// 真正触发 SM 心跳超时检测。当前作为集成测试独立维护。
// ============================================================
