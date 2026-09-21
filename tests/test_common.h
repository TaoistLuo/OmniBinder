#ifndef OMNIBINDER_TEST_COMMON_H
#define OMNIBINDER_TEST_COMMON_H

#include <gtest/gtest.h>
#include "omnibinder/buffer.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/message.h"
#include "omnibinder/types.h"
#include "platform/platform.h"
#include "platform/platform_test.h"
#include "tcp_test_server.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#ifdef IGNORE
#undef IGNORE
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace omnibinder {
namespace test {

// ============================================================
// Buffer 辅助函数
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
// 进程管理（跨平台）
//
// 平台差异全部下沉到 platform.h 的测试辅助抽象
// （spawnChildProcess / waitChildProcess / terminateChildProcess），
// 本文件不再直接 fork/kill/waitpid。
// ============================================================
typedef intptr_t TestPid;

inline TestPid startProcess(const char* path,
                             const char* a1 = nullptr, const char* a2 = nullptr,
                             const char* a3 = nullptr, const char* a4 = nullptr) {
    return static_cast<TestPid>(
        platform::spawnChildProcess(path, a1, a2, a3, a4));
}

inline void stopProcess(TestPid pid) {
    platform::terminateChildProcess(static_cast<platform::TestProcess>(pid));
}

// ============================================================
// 线程 RAII guard — 早退时先执行可选清理（如置 stop 标志/关 listener），再 join
// 避免 ASSERT 提前 return 后 joinable thread 析构触发 std::terminate / 挂起
// ============================================================
class ScopedThreadJoiner {
public:
    explicit ScopedThreadJoiner(std::thread& thread,
                                const std::function<void()>& before_join = std::function<void()>())
        : thread_(&thread), before_join_(before_join) {
    }

    ~ScopedThreadJoiner() {
        if (before_join_) {
            before_join_();
        }
        if (thread_ && thread_->joinable()) {
            thread_->join();
        }
    }

private:
    ScopedThreadJoiner(const ScopedThreadJoiner&);
    ScopedThreadJoiner& operator=(const ScopedThreadJoiner&);

    std::thread* thread_;
    std::function<void()> before_join_;
};

// ============================================================
// socket 辅助函数
// ============================================================
inline bool waitPortReady(uint16_t port, int timeout_sec = 10) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        platform::SocketFd fd = omnibinder::platform::createTcpSocket();
        if (fd != platform::INVALID_SOCKET_FD) {
            int result = platform::connectSocket(fd, "127.0.0.1", port);
            omnibinder::platform::closeSocket(fd);
            if (result >= 0) return true;
        }
        platform::sleepMs(100);
    }
    return false;
}

} // namespace test
} // namespace omnibinder

#endif // OMNIBINDER_TEST_COMMON_H
