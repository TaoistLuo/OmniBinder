#include "lexer.h"
#include "parser.h"
#include "codegen_cpp.h"
#include "codegen_c.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <map>

static void printUsage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] <file.bidl>\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --lang=<cpp|c|all>   Target language (default: cpp)\n");
    fprintf(stderr, "  --output=<dir>       Output directory (default: .)\n");
    fprintf(stderr, "  --dep-file=<file>    Generate Makefile dependency file\n");
    fprintf(stderr, "  --help               Show this help\n");
}

// 从路径中提取文件名（不含目录）
static std::string extractBasename(const std::string& path) {
    std::string basename = path;
    size_t slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    return basename;
}

// 为单个 AST 生成代码
static bool generateForAst(const omnic::AstFile& ast, const std::string& lang,
                           const std::string& output_dir) {
    std::string basename = extractBasename(ast.file_path);
    if (basename.empty()) return true;  // 无文件路径，跳过
    
    bool ok = true;
    if (lang == "cpp" || lang == "all") {
        omnic::CppCodeGen gen;
        if (!gen.generate(ast, output_dir, basename)) {
            fprintf(stderr, "Error: Failed to generate C++ code for %s\n", basename.c_str());
            ok = false;
        } else {
            printf("Generated: %s/%s.h\n", output_dir.c_str(), basename.c_str());
            printf("Generated: %s/%s.cpp\n", output_dir.c_str(), basename.c_str());
        }
    }
    
    if (lang == "c" || lang == "all") {
        omnic::CCodeGen gen;
        if (!gen.generate(ast, output_dir, basename)) {
            fprintf(stderr, "Error: Failed to generate C code for %s\n", basename.c_str());
            ok = false;
        } else {
            printf("Generated: %s/%s_c.h\n", output_dir.c_str(), basename.c_str());
            printf("Generated: %s/%s.c\n", output_dir.c_str(), basename.c_str());
        }
    }
    
    return ok;
}

// 生成 Makefile 格式依赖文件
static bool generateDepFile(const std::string& dep_file_path,
                            const std::string& output_dir,
                            const std::string& main_basename,
                            const std::string& lang,
                            const omnic::ParseContext& ctx,
                            const std::string& main_file) {
    std::ofstream dep(dep_file_path.c_str());
    if (!dep.is_open()) {
        fprintf(stderr, "Error: Cannot create dep-file: %s\n", dep_file_path.c_str());
        return false;
    }
    
    // 目标文件列表
    if (lang == "cpp" || lang == "all") {
        dep << output_dir << "/" << main_basename << ".h ";
        dep << output_dir << "/" << main_basename << ".cpp";
    }
    if (lang == "all") dep << " ";
    if (lang == "c" || lang == "all") {
        dep << output_dir << "/" << main_basename << "_c.h ";
        dep << output_dir << "/" << main_basename << ".c";
    }
    
    dep << " :";
    
    // 依赖文件列表：主文件 + 所有被导入的文件
    dep << " " << main_file;
    for (size_t i = 0; i < ctx.all_files.size(); ++i) {
        dep << " " << ctx.all_files[i];
    }
    dep << "\n";
    
    return true;
}

int main(int argc, char* argv[]) {
    std::string lang = "cpp";
    std::string output_dir = ".";
    std::string dep_file;
    std::string input_file;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--lang=") == 0) {
            lang = arg.substr(7);
        } else if (arg.find("--output=") == 0) {
            output_dir = arg.substr(9);
        } else if (arg.find("--dep-file=") == 0) {
            dep_file = arg.substr(11);
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg[0] != '-') {
            input_file = arg;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return 1;
        }
    }
    
    if (input_file.empty()) {
        fprintf(stderr, "Error: No input file specified\n");
        printUsage(argv[0]);
        return 1;
    }
    
    std::ifstream ifs(input_file.c_str());
    if (!ifs.is_open()) {
        fprintf(stderr, "Error: Cannot open file: %s\n", input_file.c_str());
        return 1;
    }
    
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string source = ss.str();
    ifs.close();
    
    // 创建共享上下文并解析（Parser 会递归解析所有 import 的文件）
    omnic::ParseContext ctx;
    omnic::Lexer lexer(source);
    omnic::Parser parser(lexer, ctx, input_file);
    omnic::AstFile ast;
    
    if (!parser.parse(ast)) {
        fprintf(stderr, "Parse error: %s\n", parser.errorMessage().c_str());
        return 1;
    }
    
    bool ok = true;
    
    // 先为所有被导入的依赖文件生成代码
    for (std::map<std::string, omnic::AstFile>::const_iterator it = ctx.loaded_packages.begin();
         it != ctx.loaded_packages.end(); ++it) {
        // 跳过主文件自身（主文件最后生成）
        if (it->second.package_name == ast.package_name) continue;
        if (!generateForAst(it->second, lang, output_dir)) {
            ok = false;
        }
    }
    
    // 再为主文件生成代码
    if (!generateForAst(ast, lang, output_dir)) {
        ok = false;
    }
    
    // 生成依赖文件
    if (!dep_file.empty()) {
        std::string main_basename = extractBasename(input_file);
        if (!generateDepFile(dep_file, output_dir, main_basename, lang, ctx, input_file)) {
            ok = false;
        }
    }
    
    return ok ? 0 : 1;
}
