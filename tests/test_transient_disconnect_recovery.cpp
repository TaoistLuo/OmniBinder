#include <cstdio>
#include <cstdlib>
#include <string>
#include "platform/platform.h"
#include "platform/platform_test.h"

using namespace omnibinder;

#define TEST(name) printf("  TEST %-42s ", (name)); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)
#define FAIL(msg) do { printf("FAIL: %s\n", (msg)); fflush(stdout); return 1; } while (0)

static const int CHILD_TIMEOUT_SEC = 120;

/* @brief 运行单个子测试进程，带超时与 SIGKILL 兜底回收
 * @param[in]  command 子测试可执行文件路径
 * @param[out] out_rc  子进程退出码；超时/被信号杀死为 -1
 * @return true 子进程在超时内正常退出；false 超时或启动失败
 */
static bool runCommand(const std::string& command, int& out_rc) {
    platform::TestProcess proc = platform::spawnChildProcess(command.c_str());
    if (proc <= 0) {
        out_rc = -1;
        return false;
    }
    out_rc = -1;
    if (!platform::waitChildProcess(proc, CHILD_TIMEOUT_SEC, &out_rc)) {
        platform::terminateChildProcess(proc);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    int rounds = 3;
    if (argc > 1) {
        rounds = std::atoi(argv[1]);
        if (rounds <= 0) {
            rounds = 3;
        }
    }

    printf("=== Disconnect Recovery Runner ===\n\n");
    printf("Configured rounds: %d\n", rounds);

    const std::string commands[] = {
        "./target/test/test_error_logging",
        "./target/test/test_threadsafe_client_and_reconnect"
    };

    for (int round = 1; round <= rounds; ++round) {
        char label[64];
        std::snprintf(label, sizeof(label), "disconnect_recovery_round_%d", round);
        TEST(label);

        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
            int rc = -1;
            if (!runCommand(commands[i], rc)) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "%s timed out or failed to start in round %d",
                              commands[i].c_str(), round);
                FAIL(msg);
            }
            if (rc != 0) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "%s failed in round %d with rc=%d",
                              commands[i].c_str(), round, rc);
                FAIL(msg);
            }
        }
        PASS();
    }

    printf("\nAll disconnect recovery rounds passed.\n");
    return 0;
}
