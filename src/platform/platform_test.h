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

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_PLATFORM_TEST_H
