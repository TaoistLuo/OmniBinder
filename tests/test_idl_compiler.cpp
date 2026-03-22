// test_idl_compiler.cpp - Tests for the IDL compiler lexer and parser

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen_cpp.h"
#include "codegen_c.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>

using namespace omnic;

#define TEST(name) printf("  TEST %s ... ", #name);
#define PASS() printf("PASS\n");

// ============================================================
// Helper: collect all tokens from a source string
// ============================================================
static std::vector<Token> tokenize(const std::string& source) {
    Lexer lexer(source);
    std::vector<Token> tokens;
    while (true) {
        Token tok = lexer.nextToken();
        tokens.push_back(tok);
        if (tok.type == TOK_EOF || tok.type == TOK_ERROR) break;
    }
    return tokens;
}

static std::string readFile(const std::string& path) {
    std::ifstream ifs(path.c_str());
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

static ParseContext parseFile(const std::string& file_path, AstFile& ast) {
    std::ifstream in(file_path.c_str());
    assert(in.good());
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Lexer lexer(source);
    ParseContext ctx;
    Parser parser(lexer, ctx, file_path);
    bool ok = parser.parse(ast);
    assert(ok);
    assert(!parser.hasError());
    return ctx;
}

int main() {
    printf("=== IDL Compiler Tests ===\n\n");

    // ============================================================
    // Lexer tests
    // ============================================================
    printf("--- Lexer Tests ---\n");

    TEST(lexer_simple_struct) {
        std::string src = "struct Point { int32 x; int32 y; }";
        std::vector<Token> toks = tokenize(src);

        // Expected: struct, Point, {, int32, x, ;, int32, y, ;, }, EOF
        assert(toks.size() == 11);
        assert(toks[0].type == TOK_STRUCT);
        assert(toks[0].value == "struct");
        assert(toks[1].type == TOK_IDENTIFIER);
        assert(toks[1].value == "Point");
        assert(toks[2].type == TOK_LBRACE);
        assert(toks[3].type == TOK_INT32);
        assert(toks[3].value == "int32");
        assert(toks[4].type == TOK_IDENTIFIER);
        assert(toks[4].value == "x");
        assert(toks[5].type == TOK_SEMICOLON);
        assert(toks[6].type == TOK_INT32);
        assert(toks[7].type == TOK_IDENTIFIER);
        assert(toks[7].value == "y");
        assert(toks[8].type == TOK_SEMICOLON);
        assert(toks[9].type == TOK_RBRACE);
        assert(toks[10].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_keywords) {
        std::string src = "package struct service topic publishes array";
        std::vector<Token> toks = tokenize(src);

        assert(toks.size() == 7); // 6 keywords + EOF
        assert(toks[0].type == TOK_PACKAGE);
        assert(toks[1].type == TOK_STRUCT);
        assert(toks[2].type == TOK_SERVICE);
        assert(toks[3].type == TOK_TOPIC);
        assert(toks[4].type == TOK_PUBLISHES);
        assert(toks[5].type == TOK_ARRAY);
        assert(toks[6].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_primitive_types) {
        std::string src =
            "bool int8 uint8 int16 uint16 int32 uint32 "
            "int64 uint64 float32 float64 string bytes void";
        std::vector<Token> toks = tokenize(src);

        assert(toks.size() == 15); // 14 types + EOF
        assert(toks[0].type  == TOK_BOOL);
        assert(toks[1].type  == TOK_INT8);
        assert(toks[2].type  == TOK_UINT8);
        assert(toks[3].type  == TOK_INT16);
        assert(toks[4].type  == TOK_UINT16);
        assert(toks[5].type  == TOK_INT32);
        assert(toks[6].type  == TOK_UINT32);
        assert(toks[7].type  == TOK_INT64);
        assert(toks[8].type  == TOK_UINT64);
        assert(toks[9].type  == TOK_FLOAT32);
        assert(toks[10].type == TOK_FLOAT64);
        assert(toks[11].type == TOK_STRING_TYPE);
        assert(toks[12].type == TOK_BYTES);
        assert(toks[13].type == TOK_VOID);
        assert(toks[14].type == TOK_EOF);

        // Verify values match the source text
        assert(toks[0].value  == "bool");
        assert(toks[1].value  == "int8");
        assert(toks[2].value  == "uint8");
        assert(toks[3].value  == "int16");
        assert(toks[4].value  == "uint16");
        assert(toks[5].value  == "int32");
        assert(toks[6].value  == "uint32");
        assert(toks[7].value  == "int64");
        assert(toks[8].value  == "uint64");
        assert(toks[9].value  == "float32");
        assert(toks[10].value == "float64");
        assert(toks[11].value == "string");
        assert(toks[12].value == "bytes");
        assert(toks[13].value == "void");
        PASS();
    }

    TEST(lexer_line_comment) {
        std::string src =
            "// this is a comment\n"
            "int32 x;";
        std::vector<Token> toks = tokenize(src);

        // The comment should be skipped entirely
        assert(toks.size() == 4); // int32, x, ;, EOF
        assert(toks[0].type == TOK_INT32);
        assert(toks[1].type == TOK_IDENTIFIER);
        assert(toks[1].value == "x");
        assert(toks[2].type == TOK_SEMICOLON);
        assert(toks[3].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_block_comment) {
        std::string src =
            "/* multi\n"
            "   line\n"
            "   comment */\n"
            "uint64 counter;";
        std::vector<Token> toks = tokenize(src);

        // The block comment should be skipped entirely
        assert(toks.size() == 4); // uint64, counter, ;, EOF
        assert(toks[0].type == TOK_UINT64);
        assert(toks[1].type == TOK_IDENTIFIER);
        assert(toks[1].value == "counter");
        assert(toks[2].type == TOK_SEMICOLON);
        assert(toks[3].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_mixed_comments) {
        std::string src =
            "// line comment\n"
            "struct /* inline block */ Foo {\n"
            "  // field comment\n"
            "  bool active; /* trailing */\n"
            "}";
        std::vector<Token> toks = tokenize(src);

        // struct, Foo, {, bool, active, ;, }, EOF
        assert(toks.size() == 8);
        assert(toks[0].type == TOK_STRUCT);
        assert(toks[1].type == TOK_IDENTIFIER);
        assert(toks[1].value == "Foo");
        assert(toks[2].type == TOK_LBRACE);
        assert(toks[3].type == TOK_BOOL);
        assert(toks[4].type == TOK_IDENTIFIER);
        assert(toks[4].value == "active");
        assert(toks[5].type == TOK_SEMICOLON);
        assert(toks[6].type == TOK_RBRACE);
        assert(toks[7].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_punctuation) {
        std::string src = "{ } ( ) < > ; ,";
        std::vector<Token> toks = tokenize(src);

        assert(toks.size() == 9); // 8 punctuation + EOF
        assert(toks[0].type == TOK_LBRACE);
        assert(toks[1].type == TOK_RBRACE);
        assert(toks[2].type == TOK_LPAREN);
        assert(toks[3].type == TOK_RPAREN);
        assert(toks[4].type == TOK_LANGLE);
        assert(toks[5].type == TOK_RANGLE);
        assert(toks[6].type == TOK_SEMICOLON);
        assert(toks[7].type == TOK_COMMA);
        assert(toks[8].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_numbers_and_strings) {
        std::string src = "42 \"hello world\"";
        std::vector<Token> toks = tokenize(src);

        assert(toks.size() == 3); // number, string, EOF
        assert(toks[0].type == TOK_NUMBER);
        assert(toks[0].value == "42");
        assert(toks[1].type == TOK_STRING);
        assert(toks[1].value == "hello world");
        assert(toks[2].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_line_tracking) {
        std::string src =
            "package\n"
            "mypackage\n"
            ";";
        Lexer lexer(src);
        Token t1 = lexer.nextToken();
        Token t2 = lexer.nextToken();
        Token t3 = lexer.nextToken();

        assert(t1.line == 1);
        assert(t2.line == 2);
        assert(t3.line == 3);
        PASS();
    }

    TEST(lexer_no_error_on_valid_input) {
        std::string src = "package test; struct Foo { int32 x; }";
        Lexer lexer(src);
        while (true) {
            Token tok = lexer.nextToken();
            if (tok.type == TOK_EOF) break;
            assert(tok.type != TOK_ERROR);
        }
        assert(!lexer.hasError());
        PASS();
    }

    TEST(lexer_error_on_invalid_char) {
        std::string src = "struct Foo { @invalid; }";
        Lexer lexer(src);
        while (true) {
            Token tok = lexer.nextToken();
            if (tok.type == TOK_ERROR) { break; }
            if (tok.type == TOK_EOF) break;
        }
        assert(lexer.hasError());
        PASS();
    }

    // ============================================================
    // Parser tests
    // ============================================================
    printf("\n--- Parser Tests ---\n");

    TEST(parser_simple_struct) {
        std::string src =
            "package myapp;\n"
            "struct Point {\n"
            "    int32 x;\n"
            "    int32 y;\n"
            "    float64 z;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());
        assert(ast.package_name == "myapp");
        assert(ast.structs.size() == 1);
        assert(ast.structs[0].name == "Point");
        assert(ast.structs[0].fields.size() == 3);

        assert(ast.structs[0].fields[0].name == "x");
        assert(ast.structs[0].fields[0].type.primitive == TYPE_INT32);

        assert(ast.structs[0].fields[1].name == "y");
        assert(ast.structs[0].fields[1].type.primitive == TYPE_INT32);

        assert(ast.structs[0].fields[2].name == "z");
        assert(ast.structs[0].fields[2].type.primitive == TYPE_FLOAT64);
        PASS();
    }

    TEST(parser_struct_all_primitives) {
        std::string src =
            "struct AllTypes {\n"
            "    bool    f_bool;\n"
            "    int8    f_i8;\n"
            "    uint8   f_u8;\n"
            "    int16   f_i16;\n"
            "    uint16  f_u16;\n"
            "    int32   f_i32;\n"
            "    uint32  f_u32;\n"
            "    int64   f_i64;\n"
            "    uint64  f_u64;\n"
            "    float32 f_f32;\n"
            "    float64 f_f64;\n"
            "    string  f_str;\n"
            "    bytes   f_bytes;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.structs.size() == 1);
        assert(ast.structs[0].fields.size() == 13);

        static const PrimitiveType expected[] = {
            TYPE_BOOL, TYPE_INT8, TYPE_UINT8, TYPE_INT16, TYPE_UINT16,
            TYPE_INT32, TYPE_UINT32, TYPE_INT64, TYPE_UINT64,
            TYPE_FLOAT32, TYPE_FLOAT64, TYPE_STRING, TYPE_BYTES
        };
        for (size_t i = 0; i < 13; i++) {
            assert(ast.structs[0].fields[i].type.primitive == expected[i]);
        }
        PASS();
    }

    TEST(parser_struct_with_custom_type) {
        std::string src =
            "struct Outer {\n"
            "    Point position;\n"
            "    string name;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.structs.size() == 1);
        assert(ast.structs[0].fields.size() == 2);
        assert(ast.structs[0].fields[0].type.primitive == TYPE_CUSTOM);
        assert(ast.structs[0].fields[0].type.custom_name == "Point");
        assert(ast.structs[0].fields[1].type.primitive == TYPE_STRING);
        PASS();
    }

    TEST(parser_struct_with_array) {
        std::string src =
            "struct Container {\n"
            "    array<int32> values;\n"
            "    array<string> names;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.structs.size() == 1);
        assert(ast.structs[0].fields.size() == 2);

        assert(ast.structs[0].fields[0].type.primitive == TYPE_ARRAY);
        assert(ast.structs[0].fields[0].type.element_type != NULL);
        assert(ast.structs[0].fields[0].type.element_type->primitive == TYPE_INT32);

        assert(ast.structs[0].fields[1].type.primitive == TYPE_ARRAY);
        assert(ast.structs[0].fields[1].type.element_type != NULL);
        assert(ast.structs[0].fields[1].type.element_type->primitive == TYPE_STRING);
        PASS();
    }

    TEST(parser_service_with_methods) {
        std::string src =
            "service Calculator {\n"
            "    int32 add(int32 a);\n"
            "    float64 compute(string expr);\n"
            "    void reset();\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());
        assert(ast.services.size() == 1);
        assert(ast.services[0].name == "Calculator");
        assert(ast.services[0].methods.size() == 3);

        // int32 add(int32 a);
        assert(ast.services[0].methods[0].name == "add");
        assert(ast.services[0].methods[0].return_type.primitive == TYPE_INT32);
        assert(ast.services[0].methods[0].has_param == true);
        assert(ast.services[0].methods[0].param.type.primitive == TYPE_INT32);
        assert(ast.services[0].methods[0].param.name == "a");

        // float64 compute(string expr);
        assert(ast.services[0].methods[1].name == "compute");
        assert(ast.services[0].methods[1].return_type.primitive == TYPE_FLOAT64);
        assert(ast.services[0].methods[1].has_param == true);
        assert(ast.services[0].methods[1].param.type.primitive == TYPE_STRING);
        assert(ast.services[0].methods[1].param.name == "expr");

        // void reset();
        assert(ast.services[0].methods[2].name == "reset");
        assert(ast.services[0].methods[2].return_type.primitive == TYPE_VOID);
        assert(ast.services[0].methods[2].has_param == false);
        PASS();
    }

    TEST(parser_service_with_publishes) {
        std::string src =
            "service Sensor {\n"
            "    void start();\n"
            "    publishes TemperatureUpdate;\n"
            "    publishes PressureUpdate;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.services.size() == 1);
        assert(ast.services[0].name == "Sensor");
        assert(ast.services[0].methods.size() == 1);
        assert(ast.services[0].methods[0].name == "start");
        assert(ast.services[0].publishes.size() == 2);
        assert(ast.services[0].publishes[0] == "TemperatureUpdate");
        assert(ast.services[0].publishes[1] == "PressureUpdate");
        PASS();
    }

    TEST(parser_topic_definition) {
        std::string src =
            "topic SensorData {\n"
            "    float64 temperature;\n"
            "    float64 pressure;\n"
            "    uint64 timestamp;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());
        assert(ast.topics.size() == 1);
        assert(ast.topics[0].name == "SensorData");
        assert(ast.topics[0].fields.size() == 3);

        assert(ast.topics[0].fields[0].name == "temperature");
        assert(ast.topics[0].fields[0].type.primitive == TYPE_FLOAT64);

        assert(ast.topics[0].fields[1].name == "pressure");
        assert(ast.topics[0].fields[1].type.primitive == TYPE_FLOAT64);

        assert(ast.topics[0].fields[2].name == "timestamp");
        assert(ast.topics[0].fields[2].type.primitive == TYPE_UINT64);
        PASS();
    }

    TEST(parser_full_idl_file) {
        std::string src =
            "package sensors;\n"
            "\n"
            "// Data structures\n"
            "struct Reading {\n"
            "    float64 value;\n"
            "    uint64 timestamp;\n"
            "}\n"
            "\n"
            "/* Topic for sensor updates */\n"
            "topic SensorUpdate {\n"
            "    string sensor_id;\n"
            "    Reading reading;\n"
            "}\n"
            "\n"
            "service SensorService {\n"
            "    Reading getLatest(string sensor_id);\n"
            "    void calibrate();\n"
            "    publishes SensorUpdate;\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());
        assert(ast.package_name == "sensors");
        assert(ast.structs.size() == 1);
        assert(ast.topics.size() == 1);
        assert(ast.services.size() == 1);

        // Struct
        assert(ast.structs[0].name == "Reading");
        assert(ast.structs[0].fields.size() == 2);

        // Topic
        assert(ast.topics[0].name == "SensorUpdate");
        assert(ast.topics[0].fields.size() == 2);
        assert(ast.topics[0].fields[1].type.primitive == TYPE_CUSTOM);
        assert(ast.topics[0].fields[1].type.custom_name == "Reading");

        // Service
        assert(ast.services[0].name == "SensorService");
        assert(ast.services[0].methods.size() == 2);
        assert(ast.services[0].methods[0].name == "getLatest");
        assert(ast.services[0].methods[0].return_type.primitive == TYPE_CUSTOM);
        assert(ast.services[0].methods[0].return_type.custom_name == "Reading");
        assert(ast.services[0].methods[0].has_param == true);
        assert(ast.services[0].methods[0].param.type.primitive == TYPE_STRING);
        assert(ast.services[0].methods[1].name == "calibrate");
        assert(ast.services[0].methods[1].return_type.primitive == TYPE_VOID);
        assert(ast.services[0].methods[1].has_param == false);
        assert(ast.services[0].publishes.size() == 1);
        assert(ast.services[0].publishes[0] == "SensorUpdate");
        PASS();
    }

    TEST(parser_multiple_structs) {
        std::string src =
            "struct A { int32 x; }\n"
            "struct B { string name; bool flag; }";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.structs.size() == 2);
        assert(ast.structs[0].name == "A");
        assert(ast.structs[0].fields.size() == 1);
        assert(ast.structs[1].name == "B");
        assert(ast.structs[1].fields.size() == 2);
        PASS();
    }

    TEST(parser_empty_struct) {
        std::string src = "struct Empty {}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.structs.size() == 1);
        assert(ast.structs[0].name == "Empty");
        assert(ast.structs[0].fields.size() == 0);
        PASS();
    }

    TEST(parser_service_custom_param_and_return) {
        std::string src =
            "service Storage {\n"
            "    Record lookup(Query q);\n"
            "}";
        Lexer lexer(src);
        Parser parser(lexer);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.services.size() == 1);
        assert(ast.services[0].methods.size() == 1);
        assert(ast.services[0].methods[0].return_type.primitive == TYPE_CUSTOM);
        assert(ast.services[0].methods[0].return_type.custom_name == "Record");
        assert(ast.services[0].methods[0].has_param == true);
        assert(ast.services[0].methods[0].param.type.primitive == TYPE_CUSTOM);
        assert(ast.services[0].methods[0].param.type.custom_name == "Query");
        assert(ast.services[0].methods[0].param.name == "q");
        PASS();
    }

    // ============================================================
    // Import feature tests
    // ============================================================
    printf("\n--- Import Feature Tests ---\n");

    TEST(lexer_import_keyword) {
        std::string src = "import \"file.bidl\";";
        std::vector<Token> toks = tokenize(src);

        // import, "file.bidl", ;, EOF
        assert(toks.size() == 4);
        assert(toks[0].type == TOK_IMPORT);
        assert(toks[0].value == "import");
        assert(toks[1].type == TOK_STRING);
        assert(toks[1].value == "file.bidl");
        assert(toks[2].type == TOK_SEMICOLON);
        assert(toks[3].type == TOK_EOF);
        PASS();
    }

    TEST(lexer_dot_token) {
        std::string src = "common.Point";
        std::vector<Token> toks = tokenize(src);

        // common, ., Point, EOF
        assert(toks.size() == 4);
        assert(toks[0].type == TOK_IDENTIFIER);
        assert(toks[0].value == "common");
        assert(toks[1].type == TOK_DOT);
        assert(toks[1].value == ".");
        assert(toks[2].type == TOK_IDENTIFIER);
        assert(toks[2].value == "Point");
        assert(toks[3].type == TOK_EOF);
        PASS();
    }

    TEST(parser_import_statement) {
        // Parser should parse import statements and store paths in ast.imports
        std::string src =
            "package demo;\n"
            "import \"common.bidl\";\n"
            "import \"utils.bidl\";\n"
            "struct Foo { int32 x; }";
        Lexer lexer(src);
        // Use default constructor (no file context, won't actually load files)
        // Import will fail to open files, but we can test with file-based approach
        // Instead, test that import paths are parsed correctly before file loading
        // We need to create temp files for this test
        
        // Write temp files
        const char* dir = "/tmp/omni-idlc_test_import";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);
        
        {
            std::ofstream f(std::string(dir) + "/common.bidl");
            f << "package common;\nstruct Point { float32 x; float32 y; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/utils.bidl");
            f << "package utils;\nstruct Config { int32 value; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/main.bidl");
            f << "package demo;\n"
                 "import \"common.bidl\";\n"
                 "import \"utils.bidl\";\n"
                 "struct Foo { common.Point p; utils.Config c; }\n";
        }
        
        // Read and parse main.bidl
        std::ifstream ifs(std::string(dir) + "/main.bidl");
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();
        
        ParseContext ctx;
        Lexer lex(source);
        Parser parser(lex, ctx, std::string(dir) + "/main.bidl");
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.package_name == "demo");
        assert(ast.imports.size() == 2);
        assert(ast.imports[0] == "common.bidl");
        assert(ast.imports[1] == "utils.bidl");
        
        // Verify dependencies were loaded
        assert(ctx.loaded_packages.find("common") != ctx.loaded_packages.end());
        assert(ctx.loaded_packages.find("utils") != ctx.loaded_packages.end());
        assert(ctx.loaded_packages["common"].structs.size() == 1);
        assert(ctx.loaded_packages["common"].structs[0].name == "Point");
        assert(ctx.loaded_packages["utils"].structs.size() == 1);
        assert(ctx.loaded_packages["utils"].structs[0].name == "Config");
        
        // Verify cross-package type references
        assert(ast.structs.size() == 1);
        assert(ast.structs[0].fields.size() == 2);
        assert(ast.structs[0].fields[0].type.primitive == TYPE_CUSTOM);
        assert(ast.structs[0].fields[0].type.package_name == "common");
        assert(ast.structs[0].fields[0].type.custom_name == "Point");
        assert(ast.structs[0].fields[1].type.primitive == TYPE_CUSTOM);
        assert(ast.structs[0].fields[1].type.package_name == "utils");
        assert(ast.structs[0].fields[1].type.custom_name == "Config");
        
        // Verify type registry
        assert(ctx.type_registry.find("Point") != ctx.type_registry.end());
        assert(ctx.type_registry["Point"] == "common");
        assert(ctx.type_registry.find("Config") != ctx.type_registry.end());
        assert(ctx.type_registry["Config"] == "utils");
        assert(ctx.type_registry.find("Foo") != ctx.type_registry.end());
        assert(ctx.type_registry["Foo"] == "demo");
        
        // Verify all_files contains dependency paths
        assert(ctx.all_files.size() >= 2);
        PASS();
    }

    TEST(parser_cross_package_type_in_service) {
        const char* dir = "/tmp/omni-idlc_test_import";
        {
            std::ofstream f(std::string(dir) + "/types.bidl");
            f << "package types;\n"
                 "struct Response { int32 code; string msg; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/svc.bidl");
            f << "package svc;\n"
                 "import \"types.bidl\";\n"
                 "service MyService {\n"
                 "    types.Response doWork(int32 id);\n"
                 "}\n";
        }
        
        std::ifstream ifs(std::string(dir) + "/svc.bidl");
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();
        
        ParseContext ctx;
        Lexer lex(source);
        Parser parser(lex, ctx, std::string(dir) + "/svc.bidl");
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.services.size() == 1);
        assert(ast.services[0].methods.size() == 1);
        assert(ast.services[0].methods[0].return_type.primitive == TYPE_CUSTOM);
        assert(ast.services[0].methods[0].return_type.package_name == "types");
        assert(ast.services[0].methods[0].return_type.custom_name == "Response");
        assert(ast.services[0].methods[0].has_param == true);
        assert(ast.services[0].methods[0].param.type.primitive == TYPE_INT32);
        PASS();
    }

    TEST(parser_circular_import_detection) {
        const char* dir = "/tmp/omni-idlc_test_circular";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);
        
        {
            std::ofstream f(std::string(dir) + "/a.bidl");
            f << "package a;\nimport \"b.bidl\";\nstruct Foo { int32 x; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/b.bidl");
            f << "package b;\nimport \"a.bidl\";\nstruct Bar { int32 y; }\n";
        }
        
        std::ifstream ifs(std::string(dir) + "/a.bidl");
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();
        
        ParseContext ctx;
        Lexer lex(source);
        Parser parser(lex, ctx, std::string(dir) + "/a.bidl");
        AstFile ast;
        assert(!parser.parse(ast));
        assert(parser.hasError());
        // Error message should mention circular import
        assert(parser.errorMessage().find("Circular import") != std::string::npos);
        PASS();
    }

    TEST(parser_duplicate_import_dedup) {
        const char* dir = "/tmp/omni-idlc_test_dedup";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);
        
        {
            std::ofstream f(std::string(dir) + "/base.bidl");
            f << "package base;\nstruct Item { int32 id; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/main.bidl");
            f << "package main;\n"
                 "import \"base.bidl\";\n"
                 "import \"base.bidl\";\n"
                 "struct Container { base.Item item; }\n";
        }
        
        std::ifstream ifs(std::string(dir) + "/main.bidl");
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();
        
        ParseContext ctx;
        Lexer lex(source);
        Parser parser(lex, ctx, std::string(dir) + "/main.bidl");
        AstFile ast;
        assert(parser.parse(ast));
        // base.bidl should only be loaded once
        assert(ctx.loaded_packages.size() == 2); // base + main
        assert(ctx.loaded_packages.find("base") != ctx.loaded_packages.end());
        // all_files should contain base.bidl only once
        int base_count = 0;
        for (size_t i = 0; i < ctx.all_files.size(); ++i) {
            if (ctx.all_files[i].find("base.bidl") != std::string::npos) base_count++;
        }
        assert(base_count == 1);
        PASS();
    }

    TEST(parser_missing_import_file) {
        std::string src =
            "package test;\n"
            "import \"nonexistent.bidl\";\n"
            "struct Foo { int32 x; }";
        
        ParseContext ctx;
        Lexer lex(src);
        Parser parser(lex, ctx, "/tmp/omni-idlc_test_missing/test.bidl");
        AstFile ast;
        assert(!parser.parse(ast));
        assert(parser.hasError());
        assert(parser.errorMessage().find("Cannot open") != std::string::npos);
        PASS();
    }

    TEST(parser_relative_path_import) {
        const char* dir = "/tmp/omni-idlc_test_relpath";
        std::string cmd = std::string("mkdir -p ") + dir + "/sub";
        assert(system(cmd.c_str()) == 0);
        
        {
            std::ofstream f(std::string(dir) + "/shared.bidl");
            f << "package shared;\nstruct Config { int32 val; string name; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/sub/app.bidl");
            f << "package app;\n"
                 "import \"../shared.bidl\";\n"
                 "struct AppData { shared.Config cfg; int32 id; }\n";
        }
        
        std::ifstream ifs(std::string(dir) + "/sub/app.bidl");
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();
        
        ParseContext ctx;
        Lexer lex(source);
        Parser parser(lex, ctx, std::string(dir) + "/sub/app.bidl");
        AstFile ast;
        assert(parser.parse(ast));
        assert(ctx.loaded_packages.find("shared") != ctx.loaded_packages.end());
        assert(ast.structs[0].fields[0].type.package_name == "shared");
        assert(ast.structs[0].fields[0].type.custom_name == "Config");
        PASS();
    }

    TEST(parser_transitive_import) {
        // a imports b, b imports c — a should have access to all types
        const char* dir = "/tmp/omni-idlc_test_transitive";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);
        
        {
            std::ofstream f(std::string(dir) + "/c.bidl");
            f << "package cpkg;\nstruct Base { int32 id; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/b.bidl");
            f << "package bpkg;\nimport \"c.bidl\";\nstruct Mid { cpkg.Base base; }\n";
        }
        {
            std::ofstream f(std::string(dir) + "/a.bidl");
            f << "package apkg;\nimport \"b.bidl\";\nstruct Top { bpkg.Mid mid; }\n";
        }
        
        std::ifstream ifs(std::string(dir) + "/a.bidl");
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();
        
        ParseContext ctx;
        Lexer lex(source);
        Parser parser(lex, ctx, std::string(dir) + "/a.bidl");
        AstFile ast;
        assert(parser.parse(ast));
        // All three packages should be loaded
        assert(ctx.loaded_packages.find("cpkg") != ctx.loaded_packages.end());
        assert(ctx.loaded_packages.find("bpkg") != ctx.loaded_packages.end());
        assert(ctx.loaded_packages.find("apkg") != ctx.loaded_packages.end());
        
        // All types should be in the registry
        assert(ctx.type_registry["Base"] == "cpkg");
        assert(ctx.type_registry["Mid"] == "bpkg");
        assert(ctx.type_registry["Top"] == "apkg");
        PASS();
    }

    TEST(parser_no_import_backward_compat) {
        // Files without import should still work exactly as before
        std::string src =
            "package legacy;\n"
            "struct Data { int32 x; float64 y; }\n"
            "topic Update { Data d; int64 ts; }\n"
            "service Svc {\n"
            "    Data get();\n"
            "    void reset(int32 id);\n"
            "    publishes Update;\n"
            "}";
        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(ast.package_name == "legacy");
        assert(ast.imports.empty());
        assert(ast.structs.size() == 1);
        assert(ast.topics.size() == 1);
        assert(ast.services.size() == 1);
        assert(ast.services[0].methods.size() == 2);
        assert(ast.services[0].publishes.size() == 1);
        PASS();
    }

    printf("\n--- Code Generation Regression Tests ---\n");

    TEST(codegen_cpp_array_uses_buffer_suffix_methods) {
        std::string src =
            "package demo;\n"
            "struct Arrays {\n"
            "    array<int32> ids;\n"
            "    array<string> names;\n"
            "    array<bytes> blobs;\n"
            "}";

        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());

        const char* dir = "/tmp/omni-idlc_test_codegen_cpp_arrays";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);

        CppCodeGen gen;
        assert(gen.generate(ast, dir, "arrays"));

        std::string cpp = readFile(std::string(dir) + "/arrays.cpp");

        assert(cpp.find("buf.writeInt32(ids[i]);") != std::string::npos);
        assert(cpp.find("ids[i] = buf.readInt32();") != std::string::npos);
        assert(cpp.find("buf.writeString(names[i]);") != std::string::npos);
        assert(cpp.find("names[i] = buf.readString();") != std::string::npos);
        assert(cpp.find("buf.writeBytes(blobs[i]);") != std::string::npos);
        assert(cpp.find("blobs[i] = buf.readBytes();") != std::string::npos);

        assert(cpp.find("writeint32_t") == std::string::npos);
        assert(cpp.find("readint32_t") == std::string::npos);
        assert(cpp.find("writestd::string") == std::string::npos);
        assert(cpp.find("readstd::string") == std::string::npos);
        assert(cpp.find("writestd::vector<uint8_t>") == std::string::npos);
        assert(cpp.find("readstd::vector<uint8_t>") == std::string::npos);
        PASS();
    }

    TEST(codegen_cpp_method_arrays_use_recursive_buffer_logic) {
        std::string src =
            "package demo;\n"
            "service ArrayService {\n"
            "    array<int32> getIds();\n"
            "    array<string> getNames();\n"
            "    array<bytes> echoBlobs(array<bytes> blobs);\n"
            "}";

        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());

        const char* dir = "/tmp/omni-idlc_test_codegen_cpp_method_arrays";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);

        CppCodeGen gen;
        assert(gen.generate(ast, dir, "array_service"));

        std::string cpp = readFile(std::string(dir) + "/array_service.cpp");

        assert(cpp.find("{ uint32_t cnt0 = req.readUint32(); blobs.resize(cnt0);") != std::string::npos);
        assert(cpp.find("blobs[i0] = req.readBytes();") != std::string::npos);
        assert(cpp.find("response.writeUint32(static_cast<uint32_t>(result.size()));") != std::string::npos);
        assert(cpp.find("response.writeInt32(result[i0]);") != std::string::npos);
        assert(cpp.find("response.writeString(result[i0]);") != std::string::npos);
        assert(cpp.find("req.writeUint32(static_cast<uint32_t>(blobs.size()));") != std::string::npos);
        assert(cpp.find("req.writeBytes(blobs[i0]);") != std::string::npos);
        assert(cpp.find("result[i0] = resp.readInt32();") != std::string::npos);
        assert(cpp.find("result[i0] = resp.readString();") != std::string::npos);

        assert(cpp.find("req.write(") == std::string::npos);
        assert(cpp.find("resp.read()") == std::string::npos);
        PASS();
    }

    TEST(codegen_cpp_custom_arrays_use_recursive_custom_logic) {
        std::string src =
            "package demo;\n"
            "struct Item {\n"
            "    int32 id;\n"
            "}\n"
            "struct ItemList {\n"
            "    array<Item> items;\n"
            "}\n"
            "service CustomArrayService {\n"
            "    array<Item> getItems();\n"
            "    array<Item> echoItems(array<Item> items);\n"
            "}";

        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());

        const char* dir = "/tmp/omni-idlc_test_codegen_cpp_custom_arrays";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);

        CppCodeGen gen;
        assert(gen.generate(ast, dir, "custom_arrays"));

        std::string cpp = readFile(std::string(dir) + "/custom_arrays.cpp");

        assert(cpp.find("items[i0].serialize(buf);") != std::string::npos);
        assert(cpp.find("items[i0].deserialize(buf);") != std::string::npos);
        assert(cpp.find("items[i0].deserialize(req);") != std::string::npos);
        assert(cpp.find("response.writeUint32(static_cast<uint32_t>(result.size()));") != std::string::npos);
        assert(cpp.find("result[i0].serialize(response);") != std::string::npos);
        assert(cpp.find("req.writeUint32(static_cast<uint32_t>(items.size()));") != std::string::npos);
        assert(cpp.find("items[i0].serialize(req);") != std::string::npos);
        assert(cpp.find("result[i0].deserialize(resp);") != std::string::npos);

        assert(cpp.find("req.write(items)") == std::string::npos);
        assert(cpp.find("response.write(result)") == std::string::npos);
        assert(cpp.find("result = resp.read()") == std::string::npos);
        PASS();
    }

    TEST(codegen_cpp_nested_arrays_use_distinct_loop_depths) {
        std::string src =
            "package demo;\n"
            "struct Nested {\n"
            "    array<array<int32>> matrix;\n"
            "}\n"
            "service NestedService {\n"
            "    array<array<int32>> getMatrix();\n"
            "    array<array<int32>> echoMatrix(array<array<int32>> matrix);\n"
            "}";

        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());

        const char* dir = "/tmp/omni-idlc_test_codegen_cpp_nested_arrays";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);

        CppCodeGen gen;
        assert(gen.generate(ast, dir, "nested_arrays"));

        std::string cpp = readFile(std::string(dir) + "/nested_arrays.cpp");

        assert(cpp.find("for (size_t i0 = 0; i0 < matrix.size(); ++i0) {") != std::string::npos);
        assert(cpp.find("for (size_t i1 = 0; i1 < matrix[i0].size(); ++i1) {") != std::string::npos);
        assert(cpp.find("buf.writeInt32(matrix[i0][i1]);") != std::string::npos);
        assert(cpp.find("{ uint32_t cnt0 = buf.readUint32(); matrix.resize(cnt0);") != std::string::npos);
        assert(cpp.find("{ uint32_t cnt1 = buf.readUint32(); matrix[i0].resize(cnt1);") != std::string::npos);
        assert(cpp.find("matrix[i0][i1] = buf.readInt32();") != std::string::npos);
        assert(cpp.find("req.writeUint32(static_cast<uint32_t>(matrix.size()));") != std::string::npos);
        assert(cpp.find("req.writeInt32(matrix[i0][i1]);") != std::string::npos);
        assert(cpp.find("result[i0][i1] = resp.readInt32();") != std::string::npos);

        assert(cpp.find("req.write(matrix)") == std::string::npos);
        assert(cpp.find("resp.read()") == std::string::npos);
        PASS();
    }

    TEST(codegen_c_string_bytes_signatures_and_metadata) {
        std::string src =
            "package demo;\n"
            "struct Payload {\n"
            "    string name;\n"
            "    bytes data;\n"
            "}\n"
            "service BlobService {\n"
            "    string echoName(string name);\n"
            "    bytes echoData(bytes data);\n"
            "    Payload roundTrip(Payload payload);\n"
            "}";

        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());

        const char* dir = "/tmp/omni-idlc_test_codegen_c_strings";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);

        CCodeGen gen;
        assert(gen.generate(ast, dir, "blob_service"));

        std::string header = readFile(std::string(dir) + "/blob_service_c.h");
        std::string source = readFile(std::string(dir) + "/blob_service.c");

        assert(header.find("char* name;") != std::string::npos);
        assert(header.find("uint32_t name_len;") != std::string::npos);
        assert(header.find("uint8_t* data;") != std::string::npos);
        assert(header.find("uint32_t data_len;") != std::string::npos);

        assert(header.find("(*echoName)(const char* name, uint32_t name_len, char** result, uint32_t* result_len, void* user_data);") != std::string::npos);
        assert(header.find("(*echoData)(const uint8_t* data, uint32_t data_len, uint8_t** result, uint32_t* result_len, void* user_data);") != std::string::npos);
        assert(header.find("int demo_BlobService_proxy_echoName(demo_BlobService_proxy* p, const char* name, uint32_t name_len, char** result, uint32_t* result_len);") != std::string::npos);
        assert(header.find("int demo_BlobService_proxy_echoData(demo_BlobService_proxy* p, const uint8_t* data, uint32_t data_len, uint8_t** result, uint32_t* result_len);") != std::string::npos);

        assert(source.find("omni_service_add_method_ex(svc, demo_BlobService_METHOD_ECHO_NAME, \"echoName\", \"std::string\", \"std::string\");") != std::string::npos);
        assert(source.find("omni_service_add_method_ex(svc, demo_BlobService_METHOD_ECHO_DATA, \"echoData\", \"std::vector<uint8_t>\", \"std::vector<uint8_t>\");") != std::string::npos);
        assert(source.find("omni_service_add_method_ex(svc, demo_BlobService_METHOD_ROUND_TRIP, \"roundTrip\", \"Payload\", \"Payload\");") != std::string::npos);

        assert(source.find("name = omni_buffer_read_string(req, &name_len);") != std::string::npos);
        assert(source.find("data = omni_buffer_read_bytes(req, &data_len);") != std::string::npos);
        assert(source.find("omni_buffer_write_string(response, result, result_len);") != std::string::npos);
        assert(source.find("omni_buffer_write_bytes(response, result, result_len);") != std::string::npos);
        assert(source.find("*result = omni_buffer_read_string(resp, result_len);") != std::string::npos);
        assert(source.find("*result = omni_buffer_read_bytes(resp, result_len);") != std::string::npos);
        PASS();
    }

    TEST(codegen_cpp_and_c_malformed_deserialize_guards_all_type_classes) {
        std::string src =
            "package demo;\n"
            "struct Inner {\n"
            "    int32 value;\n"
            "}\n"
            "struct AllTypes {\n"
            "    bool flag;\n"
            "    int8 i8;\n"
            "    uint8 u8;\n"
            "    int16 i16;\n"
            "    uint16 u16;\n"
            "    int32 i32;\n"
            "    uint32 u32;\n"
            "    int64 i64;\n"
            "    uint64 u64;\n"
            "    float32 f32;\n"
            "    float64 f64;\n"
            "    string name;\n"
            "    bytes blob;\n"
            "    Inner inner;\n"
            "    array<int32> ids;\n"
            "    array<string> names;\n"
            "    array<bytes> blobs;\n"
            "    array<Inner> inners;\n"
            "    array<array<int32>> matrix;\n"
            "}\n"
            "topic AllTopic {\n"
            "    AllTypes payload;\n"
            "}\n"
            "service GuardedService {\n"
            "    AllTypes echo(AllTypes input);\n"
            "    publish AllTopic;\n"
            "}";

        Lexer lex(src);
        Parser parser(lex);
        AstFile ast;
        assert(parser.parse(ast));
        assert(!parser.hasError());

        const char* dir = "/tmp/omni-idlc_test_codegen_guards";
        std::string cmd = std::string("mkdir -p ") + dir;
        assert(system(cmd.c_str()) == 0);

        CppCodeGen cpp_gen;
        assert(cpp_gen.generate(ast, dir, "guarded"));
        CCodeGen c_gen;
        assert(c_gen.generate(ast, dir, "guarded"));

        std::string cpp = readFile(std::string(dir) + "/guarded.cpp");
        std::string c_header = readFile(std::string(dir) + "/guarded_c.h");
        std::string c_source = readFile(std::string(dir) + "/guarded.c");

        assert(cpp.find("if (!input.deserialize(req)) { throw std::runtime_error(\"deserialize failed\"); }") != std::string::npos);
        assert(cpp.find("if (!payload.deserialize(buf)) { throw std::runtime_error(\"deserialize failed\"); }") != std::string::npos);
        assert(cpp.find("if (!result.deserialize(resp)) { throw std::runtime_error(\"deserialize failed\"); }") != std::string::npos);
        assert(cpp.find("if (!msg.deserialize(buf)) return;") != std::string::npos);
        assert(cpp.find("if (!inners[i0].deserialize(buf)) { throw std::runtime_error(\"deserialize failed\"); }") != std::string::npos);
        assert(cpp.find("int echo(const AllTypes& input, AllTypes* out);") != std::string::npos);
        assert(cpp.find("int ret = runtime_.invoke(\"GuardedService\"") != std::string::npos);
        assert(cpp.find("if (ret != 0) return ret;") != std::string::npos);
        assert(cpp.find("if (!out) return static_cast<int>(omnibinder::ErrorCode::ERR_INVALID_PARAM);") != std::string::npos);
        assert(cpp.find("return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);
        assert(cpp.find("try {") != std::string::npos);
        assert(cpp.find("} catch (...) {") != std::string::npos);

        assert(c_header.find("int demo_Inner_deserialize(demo_Inner* self, omni_buffer_t* buf);") != std::string::npos);
        assert(c_header.find("int demo_AllTypes_deserialize(demo_AllTypes* self, omni_buffer_t* buf);") != std::string::npos);
        assert(c_header.find("int demo_AllTopic_deserialize(demo_AllTopic* self, omni_buffer_t* buf);") != std::string::npos);
        assert(c_source.find("if (!demo_AllTypes_deserialize(&input, req)) { demo_AllTypes_destroy(&input); omni_buffer_mark_error(response, -501); omni_buffer_destroy(req); return; }") != std::string::npos);
        assert(c_source.find("if (!omni_buffer_read_ok(req)) { if (name) free(name); omni_buffer_mark_error(response, -501); omni_buffer_destroy(req); return; }") == std::string::npos);
        assert(c_source.find("if (!demo_AllTypes_deserialize(result, resp)) { demo_AllTypes_destroy(result); ret = -501; }") != std::string::npos);
        assert(c_source.find("if (!demo_AllTopic_deserialize(&msg, buf)) {") != std::string::npos);
        assert(c_source.find("if (!omni_buffer_read_ok(buf)) { goto fail; }") != std::string::npos);
        assert(c_source.find("return 1;") != std::string::npos);
        PASS();
    }

    TEST(codegen_c_custom_struct_naming_is_forward_decl_safe) {
        char dir_template[] = "/tmp/omni-idlc_test_codegen_c_custom_XXXXXX";
        char* dir_path = mkdtemp(dir_template);
        assert(dir_path != NULL);
        std::string dir(dir_path);

        const std::string common_src =
            "package common;\n"
            "struct Shared {\n"
            "    int32 code;\n"
            "}\n";
        const std::string main_src =
            "package demo;\n"
            "import \"common_types.bidl\";\n"
            "struct Item {\n"
            "    int32 id;\n"
            "}\n"
            "struct Wrapper {\n"
            "    Item item;\n"
            "    array<Item> items;\n"
            "    common.Shared shared;\n"
            "    array<common.Shared> shared_items;\n"
            "}\n"
            "topic ItemTopic {\n"
            "    Item item;\n"
            "    array<Item> items;\n"
            "}\n"
            "service ItemService {\n"
            "    Item echoItem(Item item);\n"
            "    array<Item> echoItems(array<Item> items);\n"
            "    common.Shared echoShared(common.Shared shared);\n"
            "    array<common.Shared> echoSharedItems(array<common.Shared> items);\n"
            "    publish ItemTopic;\n"
            "}";

        std::string common_path = dir + "/common_types.bidl";
        std::string main_path = dir + "/item_service.bidl";
        {
            std::ofstream out(common_path.c_str());
            out << common_src;
        }
        {
            std::ofstream out(main_path.c_str());
            out << main_src;
        }

        AstFile common_ast;
        ParseContext common_ctx = parseFile(common_path, common_ast);
        assert(common_ctx.loaded_packages.count("common") == 1);
        CCodeGen common_gen;
        assert(common_gen.generate(common_ast, dir, "common_types.bidl"));

        AstFile main_ast;
        ParseContext main_ctx = parseFile(main_path, main_ast);
        assert(main_ctx.loaded_packages.count("demo") == 1);
        assert(main_ctx.loaded_packages.count("common") == 1);
        CCodeGen main_gen;
        assert(main_gen.generate(main_ast, dir, "item_service.bidl"));

        std::string header = readFile(dir + "/item_service.bidl_c.h");

        assert(header.find("struct demo_Item;") != std::string::npos);
        assert(header.find("struct demo_Item item;") != std::string::npos);
        assert(header.find("struct demo_Item* data;") != std::string::npos);
        assert(header.find("common_Shared shared;") != std::string::npos);
        assert(header.find("common_Shared* data;") != std::string::npos);
        assert(header.find("const struct demo_Item* item") != std::string::npos);
        assert(header.find("struct demo_Item* result") != std::string::npos);
        assert(header.find("const demo_demo_Item_array* items") != std::string::npos);
        assert(header.find("demo_demo_Item_array* result") != std::string::npos);
        assert(header.find("const common_Shared* shared") != std::string::npos);
        assert(header.find("common_Shared* result") != std::string::npos);
        assert(header.find("demo_Item item;") == std::string::npos);

        unlink(common_path.c_str());
        unlink(main_path.c_str());
        PASS();
    }

    printf("\nAll IDL compiler tests passed!\n");
    return 0;
}
