#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

#define TEST(name) printf("  TEST %-42s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); fflush(stdout); return 1; } while (0)

static int runCommand(const std::string& command) {
    int rc = std::system(command.c_str());
    if (rc == -1) {
        return -1;
    }
    if (WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
    return rc;
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
            int rc = runCommand(commands[i]);
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
