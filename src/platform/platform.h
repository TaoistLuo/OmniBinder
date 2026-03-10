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

// ============================================================
// 事件通知（用于跨线程唤醒 EventLoop）
// ============================================================

// 创建事件通知 fd
int createEventFd();

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

// 关闭 UDS socket
void udsClose(int fd);

// 删除 UDS 路径
void udsUnlink(const std::string& path);

// ============================================================
// 系统信息
// ============================================================

// 获取本机唯一标识
std::string getMachineId();

// 获取当前时间戳（毫秒）
int64_t currentTimeMs();

// 获取主机名
std::string getHostName();

// 休眠指定毫秒数
void sleepMs(uint32_t ms);

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_PLATFORM_H
