#include "parser.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef _WIN32
  #include <climits>
  #include <unistd.h>
#endif

namespace omnic {

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------

static std::string getDirectory(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

static bool isAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    // X:\ or X:/
    if (path.size() >= 3 && isalpha(path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\'))
        return true;
    // UNC path
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\')
        return true;
#endif
    return path[0] == '/';
}

std::string Parser::resolvePath(const std::string& import_path) {
    if (isAbsolutePath(import_path)) {
        return import_path;
    }
    return base_dir_ + "/" + import_path;
}

std::string Parser::normalizePath(const std::string& path) {
#ifdef _WIN32
    char resolved[260];  // MAX_PATH
    if (_fullpath(resolved, path.c_str(), 260)) {
        return std::string(resolved);
    }
#else
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) {
        return std::string(resolved);
    }
#endif
    // fallback: 返回原始路径
    return path;
}

// ---------------------------------------------------------------------------
// 构造函数
// ---------------------------------------------------------------------------

Parser::Parser(Lexer& lexer, ParseContext& ctx, const std::string& file_path)
    : lexer_(lexer)
    , has_error_(false)
    , ctx_(&ctx)
    , file_path_(file_path)
    , base_dir_(getDirectory(file_path))
{
    current_ = lexer_.nextToken();
}

Parser::Parser(Lexer& lexer)
    : lexer_(lexer)
    , has_error_(false)
    , ctx_(&default_ctx_)
    , file_path_("")
    , base_dir_(".")
{
    current_ = lexer_.nextToken();
}

// ---------------------------------------------------------------------------
// 基础解析工具
// ---------------------------------------------------------------------------

Token Parser::expect(TokenType type) {
    if (current_.type != type) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Expected '%s' but got '%s' ('%s') at line %d:%d",
                 tokenTypeToString(type), tokenTypeToString(current_.type),
                 current_.value.c_str(), current_.line, current_.column);
        error(buf);
        return current_;
    }
    Token tok = current_;
    current_ = lexer_.nextToken();
    return tok;
}

bool Parser::match(TokenType type) {
    if (current_.type == type) {
        current_ = lexer_.nextToken();
        return true;
    }
    return false;
}

void Parser::error(const std::string& msg) {
    has_error_ = true;
    if (!file_path_.empty()) {
        error_msg_ = file_path_ + ": " + msg;
    } else {
        error_msg_ = msg;
    }
}

// ---------------------------------------------------------------------------
// 类型注册
// ---------------------------------------------------------------------------

bool Parser::registerTypes(const AstFile& ast) {
    const std::string& pkg = ast.package_name;
    
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        const std::string& name = ast.structs[i].name;
        std::map<std::string, std::string>::iterator it = ctx_->type_registry.find(name);
        if (it != ctx_->type_registry.end() && it->second != pkg) {
            error("Type '" + name + "' already defined in package '" + it->second +
                  "', cannot redefine in package '" + pkg + "'");
            return false;
        }
        ctx_->type_registry[name] = pkg;
    }
    
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        const std::string& name = ast.topics[i].name;
        std::map<std::string, std::string>::iterator it = ctx_->type_registry.find(name);
        if (it != ctx_->type_registry.end() && it->second != pkg) {
            error("Type '" + name + "' already defined in package '" + it->second +
                  "', cannot redefine in package '" + pkg + "'");
            return false;
        }
        ctx_->type_registry[name] = pkg;
    }
    
    for (size_t i = 0; i < ast.services.size(); ++i) {
        const std::string& name = ast.services[i].name;
        std::map<std::string, std::string>::iterator it = ctx_->type_registry.find(name);
        if (it != ctx_->type_registry.end() && it->second != pkg) {
            error("Type '" + name + "' already defined in package '" + it->second +
                  "', cannot redefine in package '" + pkg + "'");
            return false;
        }
        ctx_->type_registry[name] = pkg;
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// 主解析入口
// ---------------------------------------------------------------------------

bool Parser::parse(AstFile& ast) {
    ast.file_path = file_path_;
    
    // 将当前文件加入 processing_files（用于循环依赖检测）
    std::string normalized_self;
    if (!file_path_.empty()) {
        normalized_self = normalizePath(file_path_);
        ctx_->processing_files.insert(normalized_self);
    }
    
    while (current_.type != TOK_EOF && !has_error_) {
        switch (current_.type) {
        case TOK_PACKAGE:  parsePackage(ast); break;
        case TOK_IMPORT:   parseImport(ast); break;
        case TOK_STRUCT:   parseStruct(ast); break;
        case TOK_TOPIC:    parseTopic(ast); break;
        case TOK_SERVICE:  parseService(ast); break;
        default:
            error("Unexpected token: " + current_.value);
            return false;
        }
    }
    
    if (!has_error_) {
        // 注册当前文件的类型到全局类型表
        if (!registerTypes(ast)) {
            if (!normalized_self.empty()) ctx_->processing_files.erase(normalized_self);
            return false;
        }
        
        // 将当前文件的 AST 存入上下文
        if (!ast.package_name.empty()) {
            // 检查包名是否已被其他文件使用
            std::map<std::string, AstFile>::iterator it =
                ctx_->loaded_packages.find(ast.package_name);
            if (it != ctx_->loaded_packages.end() && it->second.file_path != file_path_) {
                error("Package '" + ast.package_name + "' already defined in '" +
                      it->second.file_path + "'");
                if (!normalized_self.empty()) ctx_->processing_files.erase(normalized_self);
                return false;
            }
            ctx_->loaded_packages[ast.package_name] = ast;
        }
    }
    
    // 解析完成，从 processing_files 中移除
    if (!normalized_self.empty()) ctx_->processing_files.erase(normalized_self);
    
    return !has_error_;
}

// ---------------------------------------------------------------------------
// package / import
// ---------------------------------------------------------------------------

bool Parser::parsePackage(AstFile& ast) {
    expect(TOK_PACKAGE);
    Token name = expect(TOK_IDENTIFIER);
    expect(TOK_SEMICOLON);
    ast.package_name = name.value;
    return !has_error_;
}

bool Parser::parseImport(AstFile& ast) {
    expect(TOK_IMPORT);
    Token path_tok = expect(TOK_STRING);
    expect(TOK_SEMICOLON);
    if (has_error_) return false;
    
    std::string import_path = path_tok.value;
    ast.imports.push_back(import_path);
    
    // 解析路径
    std::string resolved = resolvePath(import_path);
    std::string normalized = normalizePath(resolved);
    
    // 已加载？跳过
    if (ctx_->file_to_package.find(normalized) != ctx_->file_to_package.end()) {
        return true;
    }
    
    // 循环依赖检测
    if (ctx_->processing_files.find(normalized) != ctx_->processing_files.end()) {
        error("Circular import detected: " + import_path);
        return false;
    }
    
    // 读取文件
    std::ifstream ifs(resolved.c_str());
    if (!ifs.is_open()) {
        error("Cannot open imported file: " + resolved);
        return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string source = ss.str();
    ifs.close();
    
    // 记录文件路径（用于 dep-file）
    ctx_->all_files.push_back(normalized);
    
    // 递归解析（共享同一个 ParseContext）
    // parse() 内部会自动处理 processing_files 的插入和移除
    Lexer dep_lexer(source);
    Parser dep_parser(dep_lexer, *ctx_, resolved);
    AstFile dep_ast;
    
    if (!dep_parser.parse(dep_ast)) {
        has_error_ = true;
        error_msg_ = dep_parser.errorMessage();
        return false;
    }
    
    // 记录文件到包名的映射
    ctx_->file_to_package[normalized] = dep_ast.package_name;
    
    return true;
}

// ---------------------------------------------------------------------------
// struct / topic / service
// ---------------------------------------------------------------------------

bool Parser::parseStruct(AstFile& ast) {
    expect(TOK_STRUCT);
    Token name = expect(TOK_IDENTIFIER);
    expect(TOK_LBRACE);
    
    StructDef s;
    s.name = name.value;
    
    while (current_.type != TOK_RBRACE && current_.type != TOK_EOF && !has_error_) {
        FieldDef field;
        if (parseField(field)) {
            s.fields.push_back(field);
        }
    }
    expect(TOK_RBRACE);
    ast.structs.push_back(s);
    return !has_error_;
}

bool Parser::parseTopic(AstFile& ast) {
    expect(TOK_TOPIC);
    Token name = expect(TOK_IDENTIFIER);
    expect(TOK_LBRACE);
    
    TopicDef t;
    t.name = name.value;
    
    while (current_.type != TOK_RBRACE && current_.type != TOK_EOF && !has_error_) {
        FieldDef field;
        if (parseField(field)) {
            t.fields.push_back(field);
        }
    }
    expect(TOK_RBRACE);
    ast.topics.push_back(t);
    return !has_error_;
}

bool Parser::parseService(AstFile& ast) {
    expect(TOK_SERVICE);
    Token name = expect(TOK_IDENTIFIER);
    expect(TOK_LBRACE);
    
    ServiceDef svc;
    svc.name = name.value;
    
    while (current_.type != TOK_RBRACE && current_.type != TOK_EOF && !has_error_) {
        if (current_.type == TOK_PUBLISHES) {
            current_ = lexer_.nextToken();
            Token topic = expect(TOK_IDENTIFIER);
            expect(TOK_SEMICOLON);
            svc.publishes.push_back(topic.value);
        } else {
            MethodDef method;
            if (parseMethod(method)) {
                svc.methods.push_back(method);
            }
        }
    }
    expect(TOK_RBRACE);
    ast.services.push_back(svc);
    return !has_error_;
}

// ---------------------------------------------------------------------------
// field / method / type
// ---------------------------------------------------------------------------

bool Parser::parseField(FieldDef& field) {
    if (!parseType(field.type)) return false;
    Token name = expect(TOK_IDENTIFIER);
    field.name = name.value;
    expect(TOK_SEMICOLON);
    return !has_error_;
}

bool Parser::parseMethod(MethodDef& method) {
    if (!parseType(method.return_type)) return false;
    Token name = expect(TOK_IDENTIFIER);
    method.name = name.value;
    expect(TOK_LPAREN);
    
    method.has_param = false;
    if (current_.type != TOK_RPAREN) {
        method.has_param = true;
        if (!parseType(method.param.type)) return false;
        Token pname = expect(TOK_IDENTIFIER);
        method.param.name = pname.value;
    }
    expect(TOK_RPAREN);
    expect(TOK_SEMICOLON);
    return !has_error_;
}

bool Parser::parseType(TypeRef& type) {
    switch (current_.type) {
    case TOK_BOOL:    type.primitive = TYPE_BOOL; break;
    case TOK_INT8:    type.primitive = TYPE_INT8; break;
    case TOK_UINT8:   type.primitive = TYPE_UINT8; break;
    case TOK_INT16:   type.primitive = TYPE_INT16; break;
    case TOK_UINT16:  type.primitive = TYPE_UINT16; break;
    case TOK_INT32:   type.primitive = TYPE_INT32; break;
    case TOK_UINT32:  type.primitive = TYPE_UINT32; break;
    case TOK_INT64:   type.primitive = TYPE_INT64; break;
    case TOK_UINT64:  type.primitive = TYPE_UINT64; break;
    case TOK_FLOAT32: type.primitive = TYPE_FLOAT32; break;
    case TOK_FLOAT64: type.primitive = TYPE_FLOAT64; break;
    case TOK_STRING_TYPE: type.primitive = TYPE_STRING; break;
    case TOK_BYTES:   type.primitive = TYPE_BYTES; break;
    case TOK_VOID:    type.primitive = TYPE_VOID; break;
    case TOK_ARRAY:
        type.primitive = TYPE_ARRAY;
        current_ = lexer_.nextToken();
        expect(TOK_LANGLE);
        type.element_type = new TypeRef();
        if (!parseType(*type.element_type)) return false;
        expect(TOK_RANGLE);
        return !has_error_;
    case TOK_IDENTIFIER: {
        type.primitive = TYPE_CUSTOM;
        std::string first_name = current_.value;
        current_ = lexer_.nextToken();
        
        // 检查是否是 pkg.Type 跨包引用
        if (current_.type == TOK_DOT) {
            current_ = lexer_.nextToken();  // 消费 '.'
            if (current_.type != TOK_IDENTIFIER) {
                error("Expected type name after '" + first_name + ".'");
                return false;
            }
            type.package_name = first_name;
            type.custom_name = current_.value;
            current_ = lexer_.nextToken();  // 消费类型名
        } else {
            type.custom_name = first_name;
        }
        return !has_error_;
    }
    default:
        error("Expected type, got: " + current_.value);
        return false;
    }
    current_ = lexer_.nextToken();
    return !has_error_;
}

} // namespace omnic
