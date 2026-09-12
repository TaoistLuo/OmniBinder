/**************************************************************************************************
 * @file        semantic_validator.h
 * @brief       IDL 语义验证器
 * @details     对 IDL AST 进行语义检查：声明去重、类型引用解析、循环依赖检测、
 *              结构体拓扑排序。确保生成的代码在语义层面正确。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-02-20
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
#ifndef OMNIC_SEMANTIC_VALIDATOR_H
#define OMNIC_SEMANTIC_VALIDATOR_H

#include "parser.h"

#include <string>

namespace omnic {

/* @brief 验证解析上下文中所有可达包的语义
 * @param[in,out] root 根 AST（本地 struct 会按依赖顺序重排）
 * @param[in] context 解析上下文
 * @param[out] error 失败时的错误信息
 * @return 验证通过返回 true
 * @note 对本地 struct 稳定排序，确保按值依赖先于其使用者声明
 */
bool validateSemantics(AstFile& root, ParseContext& context, std::string& error);

} // namespace omnic

#endif
