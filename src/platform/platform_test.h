/**************************************************************************************************
 * @file        platform_test.h
 * @brief       平台测试辅助接口（仅供测试使用，不属于生产平台契约）
 * @details     声明仅由测试代码引用的平台辅助能力：子进程拉起/回收、临时文件删除、
 *              socket 接收缓冲区设置、fd 可读等待。这些声明从生产平台契约
 *              platform.h 中分离出来，仅供 tests/ 下的测试框架使用。
 *              生产平台契约（网络 / EventFd / SHM / 握手 / 时间 / 信号 / 原子操作）
 *              仍以 platform.h 为准；实现位于 platform_linux.cpp / platform_win.cpp。
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
#ifndef OMNIBINDER_PLATFORM_TEST_H
#define OMNIBINDER_PLATFORM_TEST_H

#include "platform/platform.h"

#include <string>
#include <stddef.h>
#include <stdint.h>

namespace omnibinder {
namespace platform {

// ============================================================
// 测试辅助 — 仅测试代码引用，不属于生产平台契约
// ============================================================

/*
 * @brief  在超时时间内等待 fd 变为可读
 * @param[in]  fd          目标句柄
 * @param[in]  timeout_ms  最长等待毫秒数
 * @return true fd 可读；false 超时或发生错误
 * @note   仅测试代码引用：为测试提供跨平台的就绪等待，避免直调 poll/select
 */
bool waitFdReadable(int fd, int timeout_ms);

/*
 * @brief  删除文件/文件系统路径（Linux unlink；Windows DeleteFile）
 * @param[in]  path 目标路径
 * @return true 删除成功
 * @note   仅测试代码引用：清理临时日志文件与 AF_UNIX 握手端点，避免测试直调 POSIX unlink
 */
bool removeFile(const std::string& path);

/*
 * @brief  设置 socket 接收缓冲区大小
 * @param[in]  fd    socket 句柄
 * @param[in]  bytes 目标缓冲区字节数
 * @return true 设置成功
 * @note   仅测试代码引用：构造"对端未及时读取"的分片接收场景
 */
bool setSocketRecvBufferSize(SocketFd fd, int bytes);

// 子进程句柄（仅测试代码引用；Linux 为 pid，Windows 为进程 HANDLE）
typedef intptr_t TestProcess;

/*
 * @brief  拉起测试子进程（与 startProcess 语义一致，最多 4 个参数）
 * @return 子进程句柄；<=0 表示失败
 * @note   Linux 在子进程中设置 parent-death 信号，父进程异常退出不留孤儿
 */
TestProcess spawnChildProcess(const char* path,
                              const char* a1 = NULL, const char* a2 = NULL,
                              const char* a3 = NULL, const char* a4 = NULL);

/*
 * @brief  等待子进程退出并回收
 * @param[in]   proc          子进程句柄
 * @param[in]   timeout_sec   最长等待秒数
 * @param[out]  out_exit_code 正常退出的退出码；被信号杀死返回 -1（可为 NULL）
 * @return true 已退出并回收；false 超时（子进程仍在运行，调用方需 terminateChildProcess）
 */
bool waitChildProcess(TestProcess proc, int timeout_sec, int* out_exit_code = NULL);

/*
 * @brief  终止子进程：先优雅（SIGTERM/TerminateProcess），超时后强杀（SIGKILL），并回收
 * @note   保证测试不残留僵尸/孤儿子进程；对已退出的句柄为安全空操作
 */
void terminateChildProcess(TestProcess proc);

/*
 * @brief  在子进程中执行进程内函数入口（测试专用，用于制造进程内行为场景）
 * @param[in]  entry  子进程入口函数；入口返回后子进程以退出码 0 结束
 * @return 子进程句柄；<=0 表示失败
 * @note   Linux 通过 fork 实现，子进程内设置 parent-death 信号；Windows 无 fork
 *         等价能力，返回 0（依赖该能力的测试在 CMake 层 if(NOT WIN32) 守卫）
 */
TestProcess spawnChildFunction(void (*entry)());

/*
 * @brief  子进程内立即退出（不执行 atexit/全局析构，Linux _exit）
 * @param[in]  exit_code  退出码
 * @note   仅应在 spawnChildFunction 的子进程入口内调用
 */
void exitChildProcess(int exit_code);

/*
 * @brief  创建匿名管道（测试专用）
 * @param[out]  fds  fds[0] 读端、fds[1] 写端
 * @return true 创建成功
 * @note   Linux 为 pipe；Windows 无 POSIX fd 语义的匿名管道，返回 false
 *         （调用方为 Linux 专属测试）
 */
bool createPipe(int fds[2]);

/*
 * @brief  查询 fd 是否处于非阻塞模式（测试专用）
 * @return true 已设置 O_NONBLOCK
 * @note   Linux 为 fcntl(F_GETFL)；Windows 无对应查询，返回 false
 */
bool fdIsNonblocking(int fd);

// waitFdEvents 的 events 参数与返回值共用的事件位
enum TestFdEvent {
    TEST_FD_READABLE = 1,
    TEST_FD_WRITABLE = 2,
    TEST_FD_ERROR    = 4
};

/*
 * @brief  等待 fd 事件（读/写/错误），屏蔽 poll/select 差异（测试专用）
 * @param[in]  fd         目标句柄
 * @param[in]  events     关心的事件位（TestFdEvent 组合）
 * @param[in]  timeout_ms 最长等待毫秒数
 * @return 实际发生的事件位；0 表示超时或错误
 * @note   Linux 为 poll；Windows 返回 0（调用方为 Linux 专属测试）
 */
uint32_t waitFdEvents(int fd, uint32_t events, int timeout_ms);

/*
 * @brief  原样发送"数据 + 一个 SCM_RIGHTS 句柄"（测试专用）
 * @param[in]  fd         目标裸 fd（如握手通道的 UDS 连接）
 * @param[in]  data       数据（可携带自定义/畸形头部）
 * @param[in]  length     数据长度
 * @param[in]  passed_fd  随消息传递的文件描述符
 * @return true 完整发送
 * @note   Linux 为 sendmsg + SCM_RIGHTS；Windows 无 SCM_RIGHTS，返回 false
 */
bool sendRawMessageWithFd(int fd, const void* data, size_t length, int passed_fd);

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_PLATFORM_TEST_H
