#ifndef OMNIBINDER_TEST_COMMON_H
#define OMNIBINDER_TEST_COMMON_H

#include <gtest/gtest.h>
#include "omnibinder/buffer.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/message.h"
#include "omnibinder/types.h"
#include "platform/platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace omnibinder {
namespace test {

// ============================================================
// Buffer helpers
// ============================================================
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

// ============================================================
// Process management (cross-platform)
// ============================================================
typedef intptr_t TestPid;

inline TestPid startProcess(const char* path,
                             const char* a1 = nullptr, const char* a2 = nullptr,
                             const char* a3 = nullptr, const char* a4 = nullptr) {
#ifdef _WIN32
    std::string cmd = std::string(path);
    if (a1) { cmd += " "; cmd += a1; }
    if (a2) { cmd += " "; cmd += a2; }
    if (a3) { cmd += " "; cmd += a3; }
    if (a4) { cmd += " "; cmd += a4; }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    return reinterpret_cast<TestPid>(pi.hProcess);
#else
    pid_t pid = fork();
    if (pid == 0) {
        if (a1 && a2 && a3 && a4)
            execl(path, path, a1, a2, a3, a4, (char*)NULL);
        else if (a1 && a2 && a3)
            execl(path, path, a1, a2, a3, (char*)NULL);
        else if (a1 && a2)
            execl(path, path, a1, a2, (char*)NULL);
        else if (a1)
            execl(path, path, a1, (char*)NULL);
        else
            execl(path, path, (char*)NULL);
        _exit(1);
    }
    return static_cast<TestPid>(pid);
#endif
}

inline void stopProcess(TestPid pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    TerminateProcess(reinterpret_cast<HANDLE>(pid), 0);
    WaitForSingleObject(reinterpret_cast<HANDLE>(pid), 5000);
    CloseHandle(reinterpret_cast<HANDLE>(pid));
#else
    kill(static_cast<pid_t>(pid), SIGTERM);
    int status = 0;
    waitpid(static_cast<pid_t>(pid), &status, 0);
#endif
}

inline bool waitForProcess(TestPid pid, int timeout_sec = 10) {
    if (pid <= 0) return false;
#ifdef _WIN32
    DWORD ret = WaitForSingleObject(reinterpret_cast<HANDLE>(pid),
                                     static_cast<DWORD>(timeout_sec) * 1000);
    return ret == WAIT_OBJECT_0;
#else
    for (int i = 0; i < timeout_sec * 10; ++i) {
        int status = 0;
        if (waitpid(static_cast<pid_t>(pid), &status, WNOHANG) > 0)
            return true;
        platform::sleepMs(100);
    }
    return false;
#endif
}

// ============================================================
// Socket helpers
// ============================================================
inline bool waitPortReady(uint16_t port, int timeout_sec = 10) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        SocketFd fd = omnibinder::platform::createTcpSocket();
        if (fd != INVALID_SOCKET_FD) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int result = ::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
            omnibinder::platform::closeSocket(fd);
            if (result == 0) return true;
        }
        platform::sleepMs(100);
    }
    return false;
}

} // namespace test
} // namespace omnibinder

#endif // OMNIBINDER_TEST_COMMON_H
