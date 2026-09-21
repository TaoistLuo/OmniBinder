#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "platform/platform.h"
#include "platform/platform_test.h"

// Soak 稳定性运行器：每轮顺序执行一组系统级测试，任一轮失败即整体失败。
// 子进程拉起/回收统一走 platform.h 抽象（spawnChildProcess / waitChildProcess /
// terminateChildProcess），不再使用 std::system，保证：
//   - 子进程带 parent-death 信号，父进程被 ctest 超时杀掉后不残留孤儿；
//   - 每个测试有独立超时，超时后 SIGTERM→SIGKILL 兜底回收，避免挂起拖死整轮。
// 本文件不出现任何平台宏分支或 POSIX 直调（AGENTS 原则 3/7）。

#define TEST(name) printf("  TEST %-42s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); fflush(stdout); return 1; } while (0)

namespace {

typedef omnibinder::platform::TestProcess TestProcess;

// 单个被测二进制的超时预算（秒）：需覆盖该测试自身 ctest TIMEOUT，
// 同时保证整轮 soak 不会因单个测试挂起而无限阻塞。
const int kPerTestTimeoutSec = 240;

bool runOneTest(const std::string& name, int* out_exit_code) {
    const std::string path = "./target/test/" + name;
    TestProcess proc = omnibinder::platform::spawnChildProcess(path.c_str());
    if (proc <= 0) {
        printf("FAIL: cannot spawn %s\n", path.c_str());
        return false;
    }
    int exit_code = 0;
    if (!omnibinder::platform::waitChildProcess(proc, kPerTestTimeoutSec, &exit_code)) {
        printf("FAIL: %s timed out after %ds, terminating\n", name.c_str(), kPerTestTimeoutSec);
        omnibinder::platform::terminateChildProcess(proc);
        return false;
    }
    if (out_exit_code) *out_exit_code = exit_code;
    return exit_code == 0;
}

} // namespace

int main(int argc, char** argv) {
    int rounds = 5;
    if (argc > 1) {
        rounds = std::atoi(argv[1]);
        if (rounds <= 0) {
            rounds = 5;
        }
    }

    printf("=== Soak Stability Runner ===\n\n");
    printf("Configured rounds: %d\n", rounds);

    const std::vector<std::string> tests = {
        "test_full_integration",
        "test_control_plane_and_fallback",
        "test_threadsafe_client_and_reconnect",
        "test_system_scenarios"
    };

    for (int round = 1; round <= rounds; ++round) {
        char label[64];
        std::snprintf(label, sizeof(label), "soak_round_%d", round);
        TEST(label);

        for (size_t i = 0; i < tests.size(); ++i) {
            int exit_code = 0;
            if (!runOneTest(tests[i], &exit_code)) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "%s failed in round %d (exit=%d)",
                              tests[i].c_str(), round, exit_code);
                FAIL(msg);
            }
        }
        PASS();
    }

    printf("\nAll soak rounds passed.\n");
    return 0;
}
