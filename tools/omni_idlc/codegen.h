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
#include <ostream>
#include <set>

namespace omnic {

/* @brief FNV-1a 32 位哈希
 * @param[in] str 输入字符串
 * @return 哈希值
 * @note 与运行时一致
 */
inline uint32_t fnv1a_hash(const std::string& str) {
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < str.size(); ++i) {
        hash ^= static_cast<uint8_t>(str[i]);
        hash *= 0x01000193u;
    }
    return hash;
}

/* @brief FNV-1a 组合：将 4 字节整数值混入已有哈希
 * @param[in] hash 已有哈希值
 * @param[in] value 待混入的 4 字节整数值
 * @return 组合后的哈希值
 */
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

/* @brief 生成文件头 banner
 * @param[in,out] os 输出流
 * @param[in] generated_name 生成文件名
 * @param[in] source_idl 源 IDL 名
 * @param[in] brief 文件简要说明
 * @param[in] details 文件详细说明
 * @note C / C++ codegen 共用
 */
inline void emitGeneratedFileBanner(std::ostream& os,
                                    const std::string& generated_name,
                                    const std::string& source_idl,
                                    const char* brief,
                                    const char* details) {
    os << "/**************************************************************************************************\n";
    os << " * @file        " << generated_name << "\n";
    os << " * @brief       " << brief << "\n";
    os << " * @details     " << details << "\n";
    os << " *              Source IDL: " << source_idl << ".bidl\n";
    os << " *              This file is auto-generated. DO NOT EDIT MANUALLY.\n";
    os << " *\n";
    os << " * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)\n";
    os << " *\n";
    os << " * MIT License\n";
    os << " *\n";
    os << " * Permission is hereby granted, free of charge, to any person obtaining a copy\n";
    os << " * of this software and associated documentation files (the \"Software\"), to deal\n";
    os << " * in the Software without restriction, including without limitation the rights\n";
    os << " * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n";
    os << " * copies of the Software, and to permit persons to whom the Software is\n";
    os << " * furnished to do so, subject to the following conditions:\n";
    os << " *\n";
    os << " * The above copyright notice and this permission notice shall be included in all\n";
    os << " * copies or substantial portions of the Software.\n";
    os << " *\n";
    os << " * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n";
    os << " * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n";
    os << " * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n";
    os << " * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n";
    os << " * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n";
    os << " * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n";
    os << " * SOFTWARE.\n";
    os << " *************************************************************************************************/\n";
}

/* @brief 类型最小线长
 * @param[in] type 类型引用
 * @return 最小线长（字节）
 * @note C / C++ codegen 共用
 */
inline size_t minimumWireSize(const TypeRef& type) {
    switch (type.primitive) {
    case TYPE_BOOL:
    case TYPE_INT8:
    case TYPE_UINT8: return 1;
    case TYPE_INT16:
    case TYPE_UINT16: return 2;
    case TYPE_INT32:
    case TYPE_UINT32:
    case TYPE_FLOAT32:
    case TYPE_STRING:
    case TYPE_BYTES:
    case TYPE_ARRAY: return 4;
    case TYPE_INT64:
    case TYPE_UINT64:
    case TYPE_FLOAT64: return 8;
    default: return 0;
    }
}

/* @brief 递归计算 TypeRef 的哈希
 * @param[in] t 类型引用
 * @param[in] ast 类型所属的 AST
 * @param[in,out] visited 已访问类型集合（防止递归环）
 * @return 类型哈希
 * @note 只使用类型信息，不使用字段名/参数名
 */
uint32_t hashTypeRef(const TypeRef& t, const AstFile& ast,
                     std::set<std::string>& visited);
/* @brief 计算单个方法的哈希
 * @param[in] m 方法定义
 * @param[in] ast 方法所属的 AST
 * @return 方法哈希
 * @note 由 method_name + param_type + return_type 组成
 */
uint32_t computeMethodHash(const MethodDef& m, const AstFile& ast);

/* @brief 计算话题的哈希
 * @param[in] t 话题定义
 * @param[in] ast 话题所属的 AST
 * @return 话题哈希
 * @note 由 topic_name + 按声明顺序的全部字段类型组成
 */
uint32_t computeTopicHash(const TopicDef& t, const AstFile& ast);

/* @brief 获取 C++ 类型名
 * @param[in] type 类型引用
 * @return C++ 类型名
 */
std::string cppTypeName(const TypeRef& type);

/* @brief 获取 C 类型名
 * @param[in] type 类型引用
 * @param[in] pkg 包名
 * @return C 类型名
 */
std::string cTypeName(const TypeRef& type, const std::string& pkg);

/* @brief 判断类型是否需要引用传递
 * @param[in] type 类型引用
 * @return 需要引用传递返回 true
 */
bool isReferenceType(const TypeRef& type);

/* @brief 获取基础类型的短名称
 * @param[in] p 基础类型
 * @return 短名称
 * @note 用于哈希计算
 */
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

/* @brief AST 类型校验的共享遍历
 * @param[in] ast 待校验的 AST
 * @param[in] validate 校验回调，签名为 bool(const TypeRef&, const std::string&, bool)
 * @return 校验通过返回 true
 * @note C / C++ codegen 严格共用同一份实现；validate 返回 false 即整体失败并停止
 */
template <typename ValidateFn>
inline bool validateAstCommon(const AstFile& ast, ValidateFn validate) {
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        const StructDef& s = ast.structs[i];
        for (size_t j = 0; j < s.fields.size(); ++j) {
            const FieldDef& f = s.fields[j];
            if (!validate(f.type, "struct '" + s.name + "' field '" + f.name + "'", false)) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < ast.topics.size(); ++i) {
        const TopicDef& t = ast.topics[i];
        for (size_t j = 0; j < t.fields.size(); ++j) {
            const FieldDef& f = t.fields[j];
            if (!validate(f.type, "topic '" + t.name + "' field '" + f.name + "'", false)) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < ast.services.size(); ++i) {
        const ServiceDef& svc = ast.services[i];
        for (size_t j = 0; j < svc.methods.size(); ++j) {
            const MethodDef& m = svc.methods[j];
            if (!validate(m.return_type,
                          "service '" + svc.name + "' method '" + m.name + "' return type",
                          true)) {
                return false;
            }
            if (m.has_param
                && !validate(m.param.type,
                             "service '" + svc.name + "' method '" + m.name
                                 + "' parameter '" + m.param.name + "'",
                             false)) {
                return false;
            }
        }
    }

    return true;
}

} // namespace omnic

#endif // BINDERC_CODEGEN_H
