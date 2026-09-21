/**************************************************************************************************
 * @file        watch_monitor.h
 * @brief       omni-cli watch 监控：诊断事件解码与格式化输出
 * @details     从 main.cpp 拆出的 watch 子系统。负责按 IDL 方法表把诊断事件
 *              （REQUEST/RESPONSE/ONE_WAY/SUBSCRIBE/BROADCAST）解码为可读文本，
 *              并驱动 owner event-loop 轮询直到收到终止信号。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-09-23
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
#ifndef OMNI_CLI_WATCH_MONITOR_H
#define OMNI_CLI_WATCH_MONITOR_H

#include <stdint.h>
#include <string>

namespace omnibinder { class OmniRuntime; }
namespace omnic { struct ParseContext; }

namespace omni_cli {

/* @brief 启动 watch 监控并阻塞轮询直到 SIGINT/SIGTERM
 * @param[in,out] runtime   OmniRuntime 实例
 * @param[in] pid           被监控的运行时 PID
 * @param[in] filter        方法/话题过滤（NULL 或空串表示不过滤）
 * @param[in] parse_ctx     已解析的 IDL 上下文
 * @param[in] package       主 IDL 包名
 * @return 成功返回 0，启动失败返回 1
 * @note 退出前会调用 unwatchPid 摘除监控
 */
int runWatchMonitor(omnibinder::OmniRuntime& runtime, uint32_t pid, const char* filter,
                    omnic::ParseContext& parse_ctx, const std::string& package);

} // namespace omni_cli

#endif // OMNI_CLI_WATCH_MONITOR_H
