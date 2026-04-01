#ifndef OMNIBINDER_TEST_COMMON_H
#define OMNIBINDER_TEST_COMMON_H

#include <gtest/gtest.h>
#include "omnibinder/buffer.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/message.h"
#include "omnibinder/types.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace omnibinder {
namespace test {

template<typename T>
inline T mustRead(Buffer& buf, bool (Buffer::*fn)(T&)) {
    T value = T();
    EXPECT_TRUE((buf.*fn)(value)) << "mustRead failed";
    return value;
}

inline std::string mustReadString(Buffer& buf) {
    std::string value;
    EXPECT_TRUE(buf.tryReadString(value)) << "mustReadString failed";
    return value;
}

inline std::vector<uint8_t> mustReadBytes(Buffer& buf) {
    std::vector<uint8_t> value;
    EXPECT_TRUE(buf.tryReadBytes(value)) << "mustReadBytes failed";
    return value;
}

template<typename T>
inline T mustReadFrom(const Buffer& source, bool (BufferView::*fn)(T&)) {
    BufferView buf(source.data(), source.size());
    T value = T();
    EXPECT_TRUE((buf.*fn)(value)) << "mustReadFrom failed";
    return value;
}

inline std::string mustReadStringFrom(const Buffer& source) {
    BufferView buf(source.data(), source.size());
    std::string value;
    EXPECT_TRUE(buf.tryReadString(value)) << "mustReadStringFrom failed";
    return value;
}

inline pid_t startProcess(const char* path, const char* arg1 = NULL,
                           const char* arg2 = NULL, const char* arg3 = NULL,
                           const char* arg4 = NULL) {
    pid_t pid = fork();
    if (pid == 0) {
        if (arg1 && arg2 && arg3 && arg4) {
            execl(path, path, arg1, arg2, arg3, arg4, (char*)NULL);
        } else if (arg1 && arg2 && arg3) {
            execl(path, path, arg1, arg2, arg3, (char*)NULL);
        } else if (arg1 && arg2) {
            execl(path, path, arg1, arg2, (char*)NULL);
        } else if (arg1) {
            execl(path, path, arg1, (char*)NULL);
        } else {
            execl(path, path, (char*)NULL);
        }
        _exit(1);
    }
    return pid;
}

inline void stopProcess(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

inline bool waitPortReady(uint16_t port, int timeout_sec = 10) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(fd);
                return true;
            }
            close(fd);
        }
        usleep(100000);
    }
    return false;
}

inline bool waitForProcess(pid_t pid, int timeout_sec = 10) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        int status = 0;
        int ret = waitpid(pid, &status, WNOHANG);
        if (ret > 0) return true;
        if (ret < 0) return true;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return false;
}

} // namespace test
} // namespace omnibinder

#endif // OMNIBINDER_TEST_COMMON_H
