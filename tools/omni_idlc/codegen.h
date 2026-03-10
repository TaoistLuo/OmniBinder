/**************************************************************************************************
 * @file        codegen.h
 * @brief       代码生成公共工具
 * @details     omnic 编译器 C++ 和 C 代码生成器的公共基础设施，包括 FNV-1a 32 位
 *              哈希函数（与运行时 omnibinder::fnv1a_32 一致）、AST 类型到 C++/C
 *              类型名的映射函数，以及引用传递类型的判断辅助。
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
#ifndef BINDERC_CODEGEN_H
#define BINDERC_CODEGEN_H

#include "ast.h"
#include <string>
#include <sstream>

namespace omnic {

// FNV-1a 32位哈希（与运行时一致）
inline uint32_t fnv1a_hash(const std::string& str) {
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < str.size(); ++i) {
        hash ^= static_cast<uint8_t>(str[i]);
        hash *= 0x01000193u;
    }
    return hash;
}

// 获取 C++ 类型名
std::string cppTypeName(const TypeRef& type);

// 获取 C 类型名
std::string cTypeName(const TypeRef& type, const std::string& pkg);

// 判断类型是否需要引用传递
bool isReferenceType(const TypeRef& type);

} // namespace omnic

#endif // BINDERC_CODEGEN_H
