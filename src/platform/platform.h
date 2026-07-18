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
#include <ctime>

namespace omnibinder {
namespace platform {

typedef intptr_t  SocketFd;
const  SocketFd   INVALID_SOCKET_FD = (SocketFd)(-1);

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
// SHM 握手通道 — 跨进程交换通知句柄
//
// 适配指南：
//   实现一个命名的双向通道（handshake channel），用于在 client/server
//   进程之间完成一次性握手：交换数据（SHM 名称）和文件描述符（eventfd）。
//   通道在握手完成后关闭，不保持长连接。
//
//   流程：
//     Server: Listen → Accept → Recv(SHM name) → Send(eventfds) → Close
//     Client: Connect → Send(SHM name) → Recv(eventfds) → Close
//
//   平台参考：
//     Linux:   AF_UNIX + SCM_RIGHTS，name = 文件系统路径
//     Windows: TCP loopback + Named Pipe 名称序列化，name 映射为本地端口
//
//   注意：
//     listener owns its named endpoint; handshakeCloseListener closes it and
//     removes the endpoint. Accepted/connected channels are independently owned.
//     Each send transfers one bounded payload and 0..2 notification handles.
//     Recv publishes payload/handles only after the complete frame is valid.
//     handshakeGetFd() returns the channel readiness descriptor.
// ============================================================

struct handshake_listener;
struct handshake_channel;

const size_t HANDSHAKE_MAX_PAYLOAD = 64 * 1024;

handshake_listener* handshakeListen(const std::string& name);
handshake_channel*  handshakeAccept(handshake_listener* listener);
handshake_channel*  handshakeConnect(const std::string& name);
void                handshakeCloseListener(handshake_listener* listener);

bool handshakeSend(handshake_channel* ch, const void* data, size_t len,
                   const int* fds, int fd_count);
bool handshakeRecv(handshake_channel* ch, void* buf, size_t bufsz, size_t* out_len,
                   int* fds, int max_fds, int* out_fd_count);
// Returns an optional server-owned notification fd created while sending.
// Ownership transfers to the caller; Linux always returns -1.
int  handshakeTakeLocalNotifyFd(handshake_channel* ch);
void handshakeClose(handshake_channel* ch);

int  handshakeGetFd(handshake_channel* ch);
int  handshakeGetListenerFd(handshake_listener* listener);

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

bool isShmHandshakeAvailable();

// ============================================================
// Test helpers — 仅测试代码引用
// ============================================================

bool waitFdReadable(int fd, int timeout_ms);

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_PLATFORM_H
