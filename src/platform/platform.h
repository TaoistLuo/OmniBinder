/**************************************************************************************************
 * @file        platform.h
 * @brief       平台抽象层
 * @details     封装操作系统相关的底层 API，提供跨平台的统一接口，包括：
 *              TCP Socket 操作（创建/绑定/监听/连接/收发）、eventfd 事件通知、
 *              POSIX 共享内存（创建/映射/解除/删除）、命名信号量、
 *              以及系统信息查询（机器 ID、时间戳、主机名）。
 *              Linux 使用原生 API，Windows 提供对应的桩实现。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
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
#ifndef OMNIBINDER_PLATFORM_H
#define OMNIBINDER_PLATFORM_H

#include <string>
#include <stdint.h>

#ifdef OMNIBINDER_LINUX
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
    #include <poll.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <signal.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <semaphore.h>

    typedef int SocketFd;
    const SocketFd INVALID_SOCKET_FD = -1;
#elif defined(OMNIBINDER_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    // Undefine Windows macros that conflict with C++ symbols
    #ifdef ERROR
        #undef ERROR
    #endif
    #ifdef IGNORE
        #undef IGNORE
    #endif

    typedef SOCKET SocketFd;
    const SocketFd INVALID_SOCKET_FD = INVALID_SOCKET;
#endif

namespace omnibinder {
namespace platform {

// ============================================================
// Socket 操作
// ============================================================

// 初始化网络库（Windows 需要 WSAStartup）
bool netInit();
void netCleanup();

// 创建 TCP socket
SocketFd createTcpSocket();

// 设置非阻塞
bool setNonBlocking(SocketFd fd);

// 设置地址复用
bool setReuseAddr(SocketFd fd);

// 设置 TCP_NODELAY
bool setTcpNoDelay(SocketFd fd);

// 设置 keepalive
bool setKeepAlive(SocketFd fd);

// 绑定地址
bool bindSocket(SocketFd fd, const std::string& host, uint16_t port);

// 监听
bool listenSocket(SocketFd fd, int backlog = 128);

// 接受连接
SocketFd acceptSocket(SocketFd fd, std::string& remote_host, uint16_t& remote_port);

// 连接（非阻塞）
int connectSocket(SocketFd fd, const std::string& host, uint16_t port);

// 发送数据
int socketSend(SocketFd fd, const void* data, size_t length);
bool socketSendAll(SocketFd fd, const void* data, size_t length,
                   uint32_t timeout_ms, uint32_t* elapsed_ms = NULL);

// 接收数据
int socketRecv(SocketFd fd, void* buf, size_t buf_size);

// 关闭 socket
void closeSocket(SocketFd fd);

// 获取 socket 绑定的端口
uint16_t getSocketPort(SocketFd fd);

// 获取 socket 本地绑定地址（IPv4），失败返回空字符串
std::string getSocketAddress(SocketFd fd);

// 获取最后的 socket 错误码
int getSocketError();

// 判断是否是 EAGAIN/EWOULDBLOCK
bool isWouldBlock(int error_code);

// 判断是否是连接中
bool isInProgress(int error_code);

// 判断是否是连接重置（ECONNRESET/EPIPE）
bool isConnectionReset(int error_code);

// 等待 socket 可写
bool waitSocketWritable(SocketFd fd, uint32_t timeout_ms);

// 等待 fd 可读（超时毫秒），返回 true 表示可读
bool waitFdReadable(int fd, int timeout_ms);

// ============================================================
// 事件通知（用于跨线程唤醒 EventLoop）
// ============================================================

// 创建事件通知 fd（单进程内唤醒）
int createEventFd();

// 创建命名事件通知 fd，name 用于跨进程共享
// Linux:  忽略 name，等价 createEventFd()
// Windows: 以 name 创建命名管道，对端通过 openNamedEventFd(name) 打开
int createNamedEventFd(const std::string& name);

// 打开已存在的命名事件通知 fd（跨进程）
// Linux:  未使用（UDS SCM_RIGHTS 传递 fd，不需要按名打开）
// Windows: 通过 name 打开对端进程创建的命名管道
int openNamedEventFd(const std::string& name);

// 写入事件（唤醒）
bool eventFdNotify(int efd);

// 读取事件（消费）
bool eventFdConsume(int efd);

// 关闭事件 fd
void closeEventFd(int efd);

// ============================================================
// 共享内存
// ============================================================

// 创建或打开共享内存
// 返回: 映射后的地址，失败返回 NULL
void* shmCreate(const std::string& name, size_t size, bool create);

// 解除映射
void shmDetach(void* addr, size_t size);

// 删除共享内存
void shmUnlink(const std::string& name);

// ============================================================
// 命名信号量
// ============================================================

typedef void* SemHandle;
const SemHandle INVALID_SEM = NULL;

SemHandle semCreate(const std::string& name, unsigned int initial_value);
SemHandle semOpen(const std::string& name);
bool semWait(SemHandle sem, uint32_t timeout_ms);
bool semPost(SemHandle sem);
void semClose(SemHandle sem);
void semUnlink(const std::string& name);

// ============================================================
// Unix Domain Socket（用于 SHM eventfd 交换）
// ============================================================

// 创建 UDS socket (SOCK_STREAM)
int udsCreate();

// 绑定并监听
int udsBindListen(const std::string& path, int backlog = 8);

// 接受连接（非阻塞）
int udsAccept(int listen_fd);

// 连接到路径（阻塞）
int udsConnect(const std::string& path);

// 通过 SCM_RIGHTS 发送 fd 数组 + 附带数据
bool udsSendFds(int uds_fd, const int* fds, int fd_count,
                const void* data, size_t data_len);

// 通过 SCM_RIGHTS 接收 fd 数组 + 附带数据
bool udsRecvFds(int uds_fd, int* fds, int fd_count,
                void* buf, size_t buf_size, size_t* out_data_len);

// 服务端向新连接的 SHM 客户端发送响应 fd。
// Linux: 发送 [resp_eventfd, master_eventfd]，*out_new_fd = -1
// Windows: 创建 per-client 通知 pipe，发送 [resp_eventfd, new_pipe]，
//          *out_new_fd = new_pipe（调用方需注册到事件循环）
// 返回 true 表示成功
bool udsSendServerResponse(int client_fd, int resp_eventfd, int master_eventfd,
                           int* out_new_fd);

// 关闭 UDS socket
void udsClose(int fd);

// 删除 UDS 路径
void udsUnlink(const std::string& path);

// 发送普通数据（不带 fd）
int udsSend(int fd, const void* data, size_t len);

// 接收普通数据（默认等待所有数据）
int udsRecv(int fd, void* buf, size_t len, bool wait_all = true);

// 等待 fd 可读
bool udsPollReadable(int fd, uint32_t timeout_ms);

// ============================================================
// Socket 辅助
// ============================================================

// 检查 socket 连接是否完成（用于异步 connect）
bool checkSocketConnected(SocketFd fd, int* out_error);

// ============================================================
// 信号处理
// ============================================================

typedef void (*SignalHandler)(int);

// 注册信号处理器（SIGINT, SIGTERM）
void setupSignalHandlers(SignalHandler handler);

// ============================================================
// 平台能力查询 — 业务层通过此 API 查询平台特性，无需 #ifdef
// ============================================================

// 是否支持 Unix Domain Socket（用于 SHM eventfd 交换）
// Linux/macOS 返回 true，Windows 通过 TCP loopback 模拟 UDS 也返回 true
bool isUdsAvailable();

// ============================================================
// 系统信息
// ============================================================

// 获取本机唯一标识
std::string getMachineId();

// 获取当前时间戳（毫秒）
int64_t currentTimeMs();
int64_t currentTimeUs();
std::string getHostName();

// 获取当前本地时间（分解为 tm + 毫秒）
// out_tm: 输出本地时间
// out_ms: 输出毫秒部分 (0-999)
void getLocalTime(struct tm* out_tm, int* out_ms);

// 休眠指定毫秒数
void sleepMs(uint32_t ms);

// 32位 popcount（计算二进制中1的个数）
uint32_t popcount32(uint32_t value);

// ============================================================
// 原子操作（用于共享内存无锁编程）
// ============================================================

// 内存屏障
void memoryBarrier();

// 原子比较并交换（返回是否成功）
bool atomicCompareSwap(volatile uint32_t* ptr, uint32_t expected, uint32_t desired);

// 原子加法，返回旧值
uint32_t atomicFetchAdd(volatile uint32_t* ptr, uint32_t value);

// 原子减法，返回旧值
uint32_t atomicFetchSub(volatile uint32_t* ptr, uint32_t value);

// 原子 AND，返回旧值
uint32_t atomicFetchAnd(volatile uint32_t* ptr, uint32_t value);

// 自旋锁操作
bool spinLockTestAndSet(volatile uint32_t* lock);
void spinLockRelease(volatile uint32_t* lock);

// CPU 自旋等待优化（x86 pause 指令等）
void spinWaitHint();

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_PLATFORM_H
