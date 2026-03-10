/**************************************************************************************************
 * @file        parser.h
 * @brief       IDL 语法分析器
 * @details     递归下降解析器，将 Lexer 产生的 Token 流解析为 AstFile 语法树。
 *              支持解析 package 声明、import 导入、struct/topic/service 定义，包括字段类型、
 *              方法签名（返回值 + 最多一个参数）和 publishes 话题列表。
 *              支持跨包类型引用（pkg.Type 语法）和递归解析被导入的文件。
 *
 * @author      taoist.luo
 * @version     1.1.0
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
#ifndef BINDERC_PARSER_H
#define BINDERC_PARSER_H

#include "ast.h"
#include "lexer.h"
#include <string>
#include <map>
#include <set>
#include <vector>

namespace omnic {

/// 跨文件共享的解析上下文
struct ParseContext {
    /// 包名 → AST 映射（所有已加载的文件）
    std::map<std::string, AstFile> loaded_packages;
    
    /// 规范化路径 → 包名（防止重复加载同一文件）
    std::map<std::string, std::string> file_to_package;
    
    /// 正在处理中的文件集合（循环依赖检测）
    std::set<std::string> processing_files;
    
    /// 全局类型注册表：类型名 → 包名
    std::map<std::string, std::string> type_registry;
    
    /// 所有加载的文件路径（按加载顺序，用于 dep-file 和代码生成）
    std::vector<std::string> all_files;
    
    ParseContext() {}
};

class Parser {
public:
    /// 带上下文的构造函数（用于递归解析和外部传入上下文）
    Parser(Lexer& lexer, ParseContext& ctx, const std::string& file_path);
    
    /// 无上下文的构造函数（向后兼容，内部创建默认上下文）
    Parser(Lexer& lexer);
    
    bool parse(AstFile& ast);
    bool hasError() const { return has_error_; }
    const std::string& errorMessage() const { return error_msg_; }
    
    /// 获取共享上下文
    ParseContext& context() { return *ctx_; }

private:
    Token expect(TokenType type);
    bool match(TokenType type);
    void error(const std::string& msg);
    
    bool parsePackage(AstFile& ast);
    bool parseImport(AstFile& ast);
    bool parseStruct(AstFile& ast);
    bool parseTopic(AstFile& ast);
    bool parseService(AstFile& ast);
    bool parseField(FieldDef& field);
    bool parseMethod(MethodDef& method);
    bool parseType(TypeRef& type);
    
    /// 将文件中的所有类型注册到全局类型表
    bool registerTypes(const AstFile& ast);
    
    /// 路径解析：相对路径基于 base_dir_，绝对路径直接使用
    std::string resolvePath(const std::string& import_path);
    
    /// 规范化路径（消除 ../ ./ ，转为绝对路径）
    std::string normalizePath(const std::string& path);
    
    Lexer& lexer_;
    Token current_;
    bool has_error_;
    std::string error_msg_;
    
    ParseContext* ctx_;           // 共享上下文指针
    ParseContext default_ctx_;    // 默认上下文（无参构造时使用）
    std::string file_path_;      // 当前文件路径
    std::string base_dir_;       // 当前文件所在目录
};

} // namespace omnic

#endif // BINDERC_PARSER_H
