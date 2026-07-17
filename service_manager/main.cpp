#include "service_manager_app.h"
#include "platform/platform.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>


// ============================================================
// Global shutdown eventfd for signal handling
// ============================================================
static volatile sig_atomic_t g_shutdown_fd = -1;

static void signalHandler(int) {
    int fd = g_shutdown_fd;
    if (fd >= 0) {
        platform::eventFdNotify(fd);
    }
}

} // namespace omnibinder

// ============================================================
// Command line argument parsing
// ============================================================
static void printUsage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --host <addr>     Listen address (default: 0.0.0.0)\n");
    fprintf(stderr, "  --port <port>     Listen port (default: 9900)\n");
    fprintf(stderr, "  --log-level <n>   Log level: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR (default: 1)\n");
    fprintf(stderr, "  --help            Show this help\n");
}

int main(int argc, char* argv[]) {
    std::string host = "0.0.0.0";
    uint16_t port = omnibinder::DEFAULT_SM_PORT;
    int log_level = OMNI_LOG_INFO;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            log_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    // Set log level
    if (log_level >= 0 && log_level <= 5) {
        omnibinder::setLogLevel(static_cast<omnibinder::LogLevel>(log_level));
    }

    OMNI_LOG_INFO(TAG, "OmniBinder ServiceManager starting...");
    OMNI_LOG_INFO(TAG, "Host: %s, Port: %u, LogLevel: %d", host.c_str(), port, log_level);

    // Create and initialize the application
    omnibinder::ServiceManagerApp app;

    if (!app.init(host, port)) {
        OMNI_LOG_ERROR(TAG, "Failed to initialize ServiceManager");
        return 1;
    }

    omnibinder::g_shutdown_fd = app.shutdownFd();
    omnibinder::platform::setupSignalHandlers(omnibinder::signalHandler);

    // Run the event loop
    app.run();

    omnibinder::g_shutdown_fd = -1;
    OMNI_LOG_INFO(TAG, "ServiceManager exited cleanly");
    return 0;
}
