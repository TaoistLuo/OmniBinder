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
#include <set>

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

// FNV-1a 组合：将 4 字节整数值混入已有哈希
inline uint32_t fnv1a_combine(uint32_t hash, uint32_t value) {
    hash ^= (value & 0xFFu);
    hash *= 0x01000193u;
    hash ^= ((value >> 8) & 0xFFu);
    hash *= 0x01000193u;
    hash ^= ((value >> 16) & 0xFFu);
    hash *= 0x01000193u;
    hash ^= ((value >> 24) & 0xFFu);
    hash *= 0x01000193u;
    return hash;
}

// Hash a TypeRef recursively - uses only type information, NOT field/parameter names
uint32_t hashTypeRef(const TypeRef& t, const AstFile& ast,
                     std::set<std::string>& visited);

// Hash a single method: method_name + param_type + return_type
uint32_t computeMethodHash(const MethodDef& m, const AstFile& ast);

// Hash a topic: topic_name + all field types in declaration order
uint32_t computeTopicHash(const TopicDef& t, const AstFile& ast);

// 获取 C++ 类型名
std::string cppTypeName(const TypeRef& type);

// 获取 C 类型名
std::string cTypeName(const TypeRef& type, const std::string& pkg);

// 判断类型是否需要引用传递
bool isReferenceType(const TypeRef& type);

// 获取基础类型的短名称（用于哈希计算）
inline const char* primitiveHashName(PrimitiveType p) {
    switch (p) {
    case TYPE_BOOL:    return "bool";
    case TYPE_INT8:    return "int8";
    case TYPE_UINT8:   return "uint8";
    case TYPE_INT16:   return "int16";
    case TYPE_UINT16:  return "uint16";
    case TYPE_INT32:   return "int32";
    case TYPE_UINT32:  return "uint32";
    case TYPE_INT64:   return "int64";
    case TYPE_UINT64:  return "uint64";
    case TYPE_FLOAT32: return "float";
    case TYPE_FLOAT64: return "double";
    case TYPE_STRING:  return "string";
    case TYPE_BYTES:   return "bytes";
    case TYPE_VOID:    return "void";
    default:           return "void";
    }
}

inline uint32_t hashTypeRef(const TypeRef& t, const AstFile& ast,
                            std::set<std::string>& visited) {
    switch (t.primitive) {
    case TYPE_ARRAY:
        if (t.element_type) {
            return fnv1a_combine(fnv1a_hash("[]"),
                                 hashTypeRef(*t.element_type, ast, visited));
        }
        return fnv1a_hash("[]");
    case TYPE_CUSTOM: {
        std::string fqn;
        if (!t.package_name.empty()) {
            fqn = t.package_name + "." + t.custom_name;
        } else {
            fqn = ast.package_name + "." + t.custom_name;
        }
        uint32_t hash = fnv1a_hash(fqn);
        if (visited.find(fqn) == visited.end()) {
            visited.insert(fqn);
            for (size_t i = 0; i < ast.structs.size(); ++i) {
                if (ast.structs[i].name == t.custom_name) {
                    for (size_t j = 0; j < ast.structs[i].fields.size(); ++j) {
                        hash = fnv1a_combine(hash,
                            hashTypeRef(ast.structs[i].fields[j].type, ast, visited));
                    }
                    break;
                }
            }
        }
        return hash;
    }
    default:
        return fnv1a_hash(primitiveHashName(t.primitive));
    }
}

inline uint32_t computeMethodHash(const MethodDef& m, const AstFile& ast) {
    std::set<std::string> visited;
    uint32_t hash = fnv1a_hash(m.name);
    if (m.has_param) {
        hash = fnv1a_combine(hash, hashTypeRef(m.param.type, ast, visited));
    }
    hash = fnv1a_combine(hash, hashTypeRef(m.return_type, ast, visited));
    return hash;
}

inline uint32_t computeTopicHash(const TopicDef& t, const AstFile& ast) {
    std::set<std::string> visited;
    uint32_t hash = fnv1a_hash(t.name);
    for (size_t i = 0; i < t.fields.size(); ++i) {
        hash = fnv1a_combine(hash, hashTypeRef(t.fields[i].type, ast, visited));
    }
    return hash;
}

} // namespace omnic

#endif // BINDERC_CODEGEN_H
