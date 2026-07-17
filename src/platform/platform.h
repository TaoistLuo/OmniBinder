/**************************************************************************************************
 * @file        platform.h
 * @brief       平台抽象层
 * @details     封装操作系统相关的底层 API，提供跨平台的统一接口。
 *              Linux 使用原生 API，Windows 提供对应的桩实现。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
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

    typedef int SocketFd;
    const SocketFd INVALID_SOCKET_FD = -1;
#elif defined(OMNIBINDER_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
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
// 网络 / Socket — 生产代码使用
// ============================================================

bool netInit();
void netCleanup();
SocketFd createTcpSocket();
bool setNonBlocking(SocketFd fd);
bool setReuseAddr(SocketFd fd);
bool setTcpNoDelay(SocketFd fd);
bool setKeepAlive(SocketFd fd);
bool bindSocket(SocketFd fd, const std::string& host, uint16_t port);
bool listenSocket(SocketFd fd, int backlog = 128);
SocketFd acceptSocket(SocketFd fd, std::string& remote_host, uint16_t& remote_port);
int  connectSocket(SocketFd fd, const std::string& host, uint16_t port);
int  socketSend(SocketFd fd, const void* data, size_t length);
bool socketSendAll(SocketFd fd, const void* data, size_t length,
                   uint32_t timeout_ms, uint32_t* elapsed_ms = NULL);
int  socketRecv(SocketFd fd, void* buf, size_t buf_size);
void closeSocket(SocketFd fd);
uint16_t getSocketPort(SocketFd fd);
std::string getSocketAddress(SocketFd fd);
int  getSocketError();
bool isWouldBlock(int error_code);
bool isConnectionReset(int error_code);
bool waitSocketWritable(SocketFd fd, uint32_t timeout_ms);
bool checkSocketConnected(SocketFd fd, int* out_error);

// ============================================================
// EventFd — 生产代码使用
// ============================================================

int  createEventFd();
int  createNamedEventFd(const std::string& name);
int  openNamedEventFd(const std::string& name);
bool eventFdNotify(int efd);
bool eventFdConsume(int efd);
void closeEventFd(int efd);

// ============================================================
// 共享内存 — 生产代码使用
// ============================================================

void* shmCreate(const std::string& name, size_t size, bool create);
void  shmDetach(void* addr, size_t size);
void  shmUnlink(const std::string& name);

// ============================================================
// Unix Domain Socket — 生产代码使用
// ============================================================

int  udsBindListen(const std::string& path, int backlog = 8);
int  udsAccept(int listen_fd);
int  udsConnect(const std::string& path);
bool udsSendFds(int uds_fd, const int* fds, int fd_count,
                const void* data, size_t data_len);
bool udsRecvFds(int uds_fd, int* fds, int fd_count,
                void* buf, size_t buf_size, size_t* out_data_len);
bool udsSendServerResponse(int client_fd, int resp_eventfd, int master_eventfd,
                           int* out_new_fd);
void udsClose(int fd);
void udsUnlink(const std::string& path);

// ============================================================
// 时间 / 系统 — 生产代码使用
// ============================================================

std::string getMachineId();
int64_t     currentTimeMs();
int64_t     currentTimeUs();
std::string getHostName();
int         getPid();
std::string getProcessName();
void        getLocalTime(struct tm* out_tm, int* out_ms);
void        sleepMs(uint32_t ms);

// ============================================================
// 信号处理 — 生产代码使用
// ============================================================

typedef void (*SignalHandler)(int);
void setupSignalHandlers(SignalHandler handler);

// ============================================================
// 原子操作 — 生产代码使用
// ============================================================

void memoryBarrier();

// ============================================================
// 平台能力查询
// ============================================================

bool isUdsAvailable();

// ============================================================
// Test helpers — 仅测试代码引用
// ============================================================

bool waitFdReadable(int fd, int timeout_ms);

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_PLATFORM_H
