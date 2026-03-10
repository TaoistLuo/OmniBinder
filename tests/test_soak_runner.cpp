#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

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
        "test_performance"
    };

    for (int round = 1; round <= rounds; ++round) {
        char label[64];
        std::snprintf(label, sizeof(label), "soak_round_%d", round);
        TEST(label);

        for (size_t i = 0; i < tests.size(); ++i) {
            std::string command = std::string("./target/test/") + tests[i];
            int rc = runCommand(command);
            if (rc != 0) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "%s failed in round %d with rc=%d",
                              tests[i].c_str(), round, rc);
                FAIL(msg);
            }
        }
        PASS();
    }

    printf("\nAll soak rounds passed.\n");
    return 0;
}
