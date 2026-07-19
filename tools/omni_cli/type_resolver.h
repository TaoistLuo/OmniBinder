/**************************************************************************************************
 * @file        type_resolver.h
 * @brief       类型解析器
 * @details     基于 IDL AST 将 JSON 中的类型名称（含嵌套结构体）解析为对应的
 *              AST 类型节点，为 JSON ↔ Buffer 编解码提供类型信息。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-03-05
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
#ifndef OMNI_CLI_TYPE_RESOLVER_H
#define OMNI_CLI_TYPE_RESOLVER_H

#include "simple_json.h"
#include "parser.h"

namespace omni_cli {

const char* primitiveTypeName(omnic::PrimitiveType primitive);
bool fillPrimitiveTypeRef(const std::string& type_name, omnic::TypeRef& type_ref);
bool findTypeRef(const omnic::ParseContext* parse_ctx, const std::string& type_name,
                 const std::string& package, omnic::TypeRef& type_ref);
bool isScalarCliType(const omnic::TypeRef& type_ref);
bool parseScalarCliValue(const char* text, const omnic::TypeRef& type_ref, simple_json::Value& value);

} // namespace omni_cli

#endif // OMNI_CLI_TYPE_RESOLVER_H
