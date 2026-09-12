/**************************************************************************************************
 * @file        codegen_c_internal.h
 * @brief       C 代码生成器内部共享辅助声明
 * @details     供 codegen_c_common.cpp / codegen_c_header.cpp / codegen_c_source.cpp 跨编译单元
 *              复用的类型命名与代码片段生成辅助函数声明。这些符号不是对外 API，仅限
 *              omni-idlc 内部使用。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-09-12
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
#ifndef BINDERC_CODEGEN_C_INTERNAL_H
#define BINDERC_CODEGEN_C_INTERNAL_H

#include "codegen.h"
#include <ostream>
#include <string>
#include <vector>

namespace omnic {

/* @brief 生成数组类型名
 * @param[in] type 数组类型引用
 * @param[in] pkg 包名
 * @return 数组类型名（pkg + 元素类型 token + "_array"）
 */
std::string cArrayTypeName(const TypeRef& type, const std::string& pkg);

/* @brief CamelCase 转 snake_case
 * @param[in] name 原始名称
 * @return snake_case 名称
 * @note 生成函数名/宏名用
 */
std::string cToSnakeCase(const std::string& name);

/* @brief 生成声明处类型名
 * @param[in] type 类型引用
 * @param[in] pkg 包名
 * @return 声明处类型名
 * @note 本包自定义类型带 "struct " 前置声明
 */
std::string cDeclaredTypeName(const TypeRef& type, const std::string& pkg);

/* @brief 递归收集 AST 中所有数组类型
 * @param[in] ast 待遍历的 AST
 * @param[in] pkg 包名
 * @return 去重后的数组类型列表
 * @note 嵌套数组先内后外
 */
std::vector<TypeRef> collectArrayTypesFromAst(const AstFile& ast, const std::string& pkg);

/* @brief 数组元素是否携带独立长度字段
 * @param[in] type 数组元素类型
 * @return 需要独立长度字段返回 true
 * @note string / bytes 返回 true
 */
bool arrayElementNeedsLengths(const TypeRef& type);

/* @brief 是否以指针形式传递
 * @param[in] type 类型引用
 * @return 以指针形式传递返回 true
 * @note 自定义结构体 / 数组返回 true
 */
bool isCompositePointerType(const TypeRef& type);

/* @brief 是否为 string / bytes
 * @param[in] type 类型引用
 * @return 是 string / bytes 返回 true
 * @note 以 指针 + 长度 形式传递
 */
bool isCStringLike(const TypeRef& type);

/* @brief 获取基础类型对应的 omni_buffer 读函数名
 * @param[in] p 基础类型
 * @return 读函数名；非基础类型返回 NULL
 */
const char* primitiveReadFunc(PrimitiveType p);
/* @brief 获取基础类型对应的 omni_buffer 写函数名
 * @param[in] p 基础类型
 * @return 写函数名；非基础类型返回 NULL
 */
const char* primitiveWriteFunc(PrimitiveType p);

/* @brief 生成 proxy 方法参数列表
 * @param[in,out] os 输出流
 * @param[in] m 方法定义
 * @param[in] pkg 包名
 * @param[in] declared_names true 时使用声明处类型名
 */
void emitProxyMethodParams(std::ostream& os, const MethodDef& m, const std::string& pkg,
                           bool declared_names);

} // namespace omnic

#endif // BINDERC_CODEGEN_C_INTERNAL_H
