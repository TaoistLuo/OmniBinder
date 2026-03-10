/**************************************************************************************************
 * @file        ast.h
 * @brief       IDL 抽象语法树定义
 * @details     定义 omnic 编译器的 AST 节点类型，包括基础类型枚举（PrimitiveType）、
 *              类型引用（TypeRef，支持深拷贝的递归结构）、字段/参数/方法/结构体/话题/
 *              服务的定义节点，以及整个 IDL 文件的顶层 AST 容器（AstFile）。
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
#ifndef BINDERC_AST_H
#define BINDERC_AST_H

#include <string>
#include <vector>
#include <stdint.h>

namespace omnic {

// 基础类型枚举
enum PrimitiveType {
    TYPE_BOOL, TYPE_INT8, TYPE_UINT8, TYPE_INT16, TYPE_UINT16,
    TYPE_INT32, TYPE_UINT32, TYPE_INT64, TYPE_UINT64,
    TYPE_FLOAT32, TYPE_FLOAT64, TYPE_STRING, TYPE_BYTES,
    TYPE_VOID, TYPE_CUSTOM, TYPE_ARRAY
};

// 类型引用
struct TypeRef {
    PrimitiveType primitive;
    std::string   custom_name;   // TYPE_CUSTOM 时使用
    std::string   package_name;  // 跨包引用时使用（如 common.Point 中的 "common"）
    TypeRef*      element_type;  // TYPE_ARRAY 时使用
    
    TypeRef() : primitive(TYPE_VOID), element_type(NULL) {}
    ~TypeRef() { delete element_type; }
    
    // 拷贝构造函数（深拷贝 element_type）
    TypeRef(const TypeRef& other)
        : primitive(other.primitive)
        , custom_name(other.custom_name)
        , package_name(other.package_name)
        , element_type(other.element_type ? new TypeRef(*other.element_type) : NULL)
    {}
    
    // 赋值运算符（深拷贝 element_type）
    TypeRef& operator=(const TypeRef& other) {
        if (this != &other) {
            primitive = other.primitive;
            custom_name = other.custom_name;
            package_name = other.package_name;
            delete element_type;
            element_type = other.element_type ? new TypeRef(*other.element_type) : NULL;
        }
        return *this;
    }
    
    bool isVoid() const { return primitive == TYPE_VOID; }
    bool isCustom() const { return primitive == TYPE_CUSTOM; }
    bool isArray() const { return primitive == TYPE_ARRAY; }
};

// 字段定义
struct FieldDef {
    TypeRef     type;
    std::string name;
};

// 结构体定义
struct StructDef {
    std::string name;
    std::vector<FieldDef> fields;
};

// 话题定义
struct TopicDef {
    std::string name;
    std::vector<FieldDef> fields;
};

// 方法参数
struct ParamDef {
    TypeRef     type;
    std::string name;
};

// 方法定义
struct MethodDef {
    std::string name;
    TypeRef     return_type;
    ParamDef    param;       // 最多一个参数
    bool        has_param;
    
    MethodDef() : has_param(false) {}
};

// 服务定义
struct ServiceDef {
    std::string name;
    std::vector<MethodDef> methods;
    std::vector<std::string> publishes;  // 发布的话题名
};

// 整个 IDL 文件的 AST
struct AstFile {
    std::string package_name;
    std::string file_path;                  // 源文件路径
    std::vector<std::string> imports;       // import 路径列表
    std::vector<StructDef>  structs;
    std::vector<TopicDef>   topics;
    std::vector<ServiceDef> services;
};

} // namespace omnic

#endif // BINDERC_AST_H
