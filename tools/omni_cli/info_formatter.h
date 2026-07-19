/**************************************************************************************************
 * @file        info_formatter.h
 * @brief       服务信息格式化输出
 * @details     omni-cli info 命令的格式化辅助。将 ServiceInfo 结构体渲染为
 *              可读的终端文本输出，支持字段名转义和方法签名展开。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-03-15
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
#ifndef OMNI_CLI_INFO_FORMATTER_H
#define OMNI_CLI_INFO_FORMATTER_H

#include <string>
#include <vector>

namespace omni_cli {

std::string escapeForTerminal(const std::string& input);

std::string formatPublishedTopicsSection(const std::vector<std::string>& topics,
                                         const std::string& unavailable_reason);

} // namespace omni_cli

#endif // OMNI_CLI_INFO_FORMATTER_H
