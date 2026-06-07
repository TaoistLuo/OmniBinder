#include <gtest/gtest.h>
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
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

using namespace omnic;

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

static bool parseFile(const std::string& file_path, AstFile& ast, ParseContext& ctx) {
    std::ifstream in(file_path.c_str());
    if (!in.good()) {
        fprintf(stderr, "parseFile: cannot open '%s'\n", file_path.c_str());
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Lexer lexer(source);
    Parser parser(lexer, ctx, file_path);
    if (!parser.parse(ast) || parser.hasError()) {
        fprintf(stderr, "parseFile: parse failed for '%s'\n", file_path.c_str());
        return false;
    }
    return true;
}

// ============================================================
// Lexer Tests
// ============================================================

TEST(IdlCompilerTest, LexerSimpleStruct) {
    std::string src = "struct Point { int32 x; int32 y; }";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 11u);
    ASSERT_EQ(toks[0].type, TOK_STRUCT);
    ASSERT_EQ(toks[0].value, "struct");
    ASSERT_EQ(toks[1].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[1].value, "Point");
    ASSERT_EQ(toks[2].type, TOK_LBRACE);
    ASSERT_EQ(toks[3].type, TOK_INT32);
    ASSERT_EQ(toks[3].value, "int32");
    ASSERT_EQ(toks[4].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[4].value, "x");
    ASSERT_EQ(toks[5].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[6].type, TOK_INT32);
    ASSERT_EQ(toks[7].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[7].value, "y");
    ASSERT_EQ(toks[8].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[9].type, TOK_RBRACE);
    ASSERT_EQ(toks[10].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerKeywords) {
    std::string src = "package struct service topic publishes array";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 7u);
    ASSERT_EQ(toks[0].type, TOK_PACKAGE);
    ASSERT_EQ(toks[1].type, TOK_STRUCT);
    ASSERT_EQ(toks[2].type, TOK_SERVICE);
    ASSERT_EQ(toks[3].type, TOK_TOPIC);
    ASSERT_EQ(toks[4].type, TOK_PUBLISHES);
    ASSERT_EQ(toks[5].type, TOK_ARRAY);
    ASSERT_EQ(toks[6].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerPrimitiveTypes) {
    std::string src =
        "bool int8 uint8 int16 uint16 int32 uint32 "
        "int64 uint64 float32 float64 string bytes void";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 15u);
    ASSERT_EQ(toks[0].type,  TOK_BOOL);
    ASSERT_EQ(toks[1].type,  TOK_INT8);
    ASSERT_EQ(toks[2].type,  TOK_UINT8);
    ASSERT_EQ(toks[3].type,  TOK_INT16);
    ASSERT_EQ(toks[4].type,  TOK_UINT16);
    ASSERT_EQ(toks[5].type,  TOK_INT32);
    ASSERT_EQ(toks[6].type,  TOK_UINT32);
    ASSERT_EQ(toks[7].type,  TOK_INT64);
    ASSERT_EQ(toks[8].type,  TOK_UINT64);
    ASSERT_EQ(toks[9].type,  TOK_FLOAT32);
    ASSERT_EQ(toks[10].type, TOK_FLOAT64);
    ASSERT_EQ(toks[11].type, TOK_STRING_TYPE);
    ASSERT_EQ(toks[12].type, TOK_BYTES);
    ASSERT_EQ(toks[13].type, TOK_VOID);
    ASSERT_EQ(toks[14].type, TOK_EOF);

    EXPECT_EQ(toks[0].value,  "bool");
    EXPECT_EQ(toks[1].value,  "int8");
    EXPECT_EQ(toks[2].value,  "uint8");
    EXPECT_EQ(toks[3].value,  "int16");
    EXPECT_EQ(toks[4].value,  "uint16");
    EXPECT_EQ(toks[5].value,  "int32");
    EXPECT_EQ(toks[6].value,  "uint32");
    EXPECT_EQ(toks[7].value,  "int64");
    EXPECT_EQ(toks[8].value,  "uint64");
    EXPECT_EQ(toks[9].value,  "float32");
    EXPECT_EQ(toks[10].value, "float64");
    EXPECT_EQ(toks[11].value, "string");
    EXPECT_EQ(toks[12].value, "bytes");
    EXPECT_EQ(toks[13].value, "void");
}

TEST(IdlCompilerTest, LexerLineComment) {
    std::string src =
        "// this is a comment\n"
        "int32 x;";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 4u);
    ASSERT_EQ(toks[0].type, TOK_INT32);
    ASSERT_EQ(toks[1].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[1].value, "x");
    ASSERT_EQ(toks[2].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[3].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerBlockComment) {
    std::string src =
        "/* multi\n"
        "   line\n"
        "   comment */\n"
        "uint64 counter;";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 4u);
    ASSERT_EQ(toks[0].type, TOK_UINT64);
    ASSERT_EQ(toks[1].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[1].value, "counter");
    ASSERT_EQ(toks[2].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[3].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerMixedComments) {
    std::string src =
        "// line comment\n"
        "struct /* inline block */ Foo {\n"
        "  // field comment\n"
        "  bool active; /* trailing */\n"
        "}";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 8u);
    ASSERT_EQ(toks[0].type, TOK_STRUCT);
    ASSERT_EQ(toks[1].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[1].value, "Foo");
    ASSERT_EQ(toks[2].type, TOK_LBRACE);
    ASSERT_EQ(toks[3].type, TOK_BOOL);
    ASSERT_EQ(toks[4].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[4].value, "active");
    ASSERT_EQ(toks[5].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[6].type, TOK_RBRACE);
    ASSERT_EQ(toks[7].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerPunctuation) {
    std::string src = "{ } ( ) < > ; ,";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 9u);
    ASSERT_EQ(toks[0].type, TOK_LBRACE);
    ASSERT_EQ(toks[1].type, TOK_RBRACE);
    ASSERT_EQ(toks[2].type, TOK_LPAREN);
    ASSERT_EQ(toks[3].type, TOK_RPAREN);
    ASSERT_EQ(toks[4].type, TOK_LANGLE);
    ASSERT_EQ(toks[5].type, TOK_RANGLE);
    ASSERT_EQ(toks[6].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[7].type, TOK_COMMA);
    ASSERT_EQ(toks[8].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerNumbersAndStrings) {
    std::string src = "42 \"hello world\"";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 3u);
    ASSERT_EQ(toks[0].type, TOK_NUMBER);
    ASSERT_EQ(toks[0].value, "42");
    ASSERT_EQ(toks[1].type, TOK_STRING);
    ASSERT_EQ(toks[1].value, "hello world");
    ASSERT_EQ(toks[2].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerLineTracking) {
    std::string src =
        "package\n"
        "mypackage\n"
        ";";
    Lexer lexer(src);
    Token t1 = lexer.nextToken();
    Token t2 = lexer.nextToken();
    Token t3 = lexer.nextToken();

    ASSERT_EQ(t1.line, 1);
    ASSERT_EQ(t2.line, 2);
    ASSERT_EQ(t3.line, 3);
}

TEST(IdlCompilerTest, LexerNoErrorOnValidInput) {
    std::string src = "package test; struct Foo { int32 x; }";
    Lexer lexer(src);
    while (true) {
        Token tok = lexer.nextToken();
        if (tok.type == TOK_EOF) break;
        ASSERT_NE(tok.type, TOK_ERROR);
    }
    EXPECT_FALSE(lexer.hasError());
}

TEST(IdlCompilerTest, LexerErrorOnInvalidChar) {
    std::string src = "struct Foo { @invalid; }";
    Lexer lexer(src);
    while (true) {
        Token tok = lexer.nextToken();
        if (tok.type == TOK_ERROR) { break; }
        if (tok.type == TOK_EOF) break;
    }
    EXPECT_TRUE(lexer.hasError());
}

// ============================================================
// Parser Tests
// ============================================================

TEST(IdlCompilerTest, ParserSimpleStruct) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());
    ASSERT_EQ(ast.package_name, "myapp");
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.structs[0].name, "Point");
    ASSERT_EQ(ast.structs[0].fields.size(), 3u);

    ASSERT_EQ(ast.structs[0].fields[0].name, "x");
    ASSERT_EQ(ast.structs[0].fields[0].type.primitive, TYPE_INT32);

    ASSERT_EQ(ast.structs[0].fields[1].name, "y");
    ASSERT_EQ(ast.structs[0].fields[1].type.primitive, TYPE_INT32);

    ASSERT_EQ(ast.structs[0].fields[2].name, "z");
    ASSERT_EQ(ast.structs[0].fields[2].type.primitive, TYPE_FLOAT64);
}

TEST(IdlCompilerTest, ParserStructAllPrimitives) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.structs[0].fields.size(), 13u);

    static const PrimitiveType expected[] = {
        TYPE_BOOL, TYPE_INT8, TYPE_UINT8, TYPE_INT16, TYPE_UINT16,
        TYPE_INT32, TYPE_UINT32, TYPE_INT64, TYPE_UINT64,
        TYPE_FLOAT32, TYPE_FLOAT64, TYPE_STRING, TYPE_BYTES
    };
    for (size_t i = 0; i < 13; i++) {
        ASSERT_EQ(ast.structs[0].fields[i].type.primitive, expected[i]);
    }
}

TEST(IdlCompilerTest, ParserStructWithCustomType) {
    std::string src =
        "struct Outer {\n"
        "    Point position;\n"
        "    string name;\n"
        "}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.structs[0].fields.size(), 2u);
    ASSERT_EQ(ast.structs[0].fields[0].type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.structs[0].fields[0].type.custom_name, "Point");
    ASSERT_EQ(ast.structs[0].fields[1].type.primitive, TYPE_STRING);
}

TEST(IdlCompilerTest, ParserStructWithArray) {
    std::string src =
        "struct Container {\n"
        "    array<int32> values;\n"
        "    array<string> names;\n"
        "}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.structs[0].fields.size(), 2u);

    ASSERT_EQ(ast.structs[0].fields[0].type.primitive, TYPE_ARRAY);
    ASSERT_TRUE(ast.structs[0].fields[0].type.element_type != NULL);
    ASSERT_EQ(ast.structs[0].fields[0].type.element_type->primitive, TYPE_INT32);

    ASSERT_EQ(ast.structs[0].fields[1].type.primitive, TYPE_ARRAY);
    ASSERT_TRUE(ast.structs[0].fields[1].type.element_type != NULL);
    ASSERT_EQ(ast.structs[0].fields[1].type.element_type->primitive, TYPE_STRING);
}

TEST(IdlCompilerTest, ParserServiceWithMethods) {
    std::string src =
        "service Calculator {\n"
        "    int32 add(int32 a);\n"
        "    float64 compute(string expr);\n"
        "    void reset();\n"
        "}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());
    ASSERT_EQ(ast.services.size(), 1u);
    ASSERT_EQ(ast.services[0].name, "Calculator");
    ASSERT_EQ(ast.services[0].methods.size(), 3u);

    ASSERT_EQ(ast.services[0].methods[0].name, "add");
    ASSERT_EQ(ast.services[0].methods[0].return_type.primitive, TYPE_INT32);
    ASSERT_TRUE(ast.services[0].methods[0].has_param);
    ASSERT_EQ(ast.services[0].methods[0].param.type.primitive, TYPE_INT32);
    ASSERT_EQ(ast.services[0].methods[0].param.name, "a");

    ASSERT_EQ(ast.services[0].methods[1].name, "compute");
    ASSERT_EQ(ast.services[0].methods[1].return_type.primitive, TYPE_FLOAT64);
    ASSERT_TRUE(ast.services[0].methods[1].has_param);
    ASSERT_EQ(ast.services[0].methods[1].param.type.primitive, TYPE_STRING);
    ASSERT_EQ(ast.services[0].methods[1].param.name, "expr");

    ASSERT_EQ(ast.services[0].methods[2].name, "reset");
    ASSERT_EQ(ast.services[0].methods[2].return_type.primitive, TYPE_VOID);
    ASSERT_FALSE(ast.services[0].methods[2].has_param);
}

TEST(IdlCompilerTest, ParserServiceWithPublishes) {
    std::string src =
        "service Sensor {\n"
        "    void start();\n"
        "    publishes TemperatureUpdate;\n"
        "    publishes PressureUpdate;\n"
        "}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.services.size(), 1u);
    ASSERT_EQ(ast.services[0].name, "Sensor");
    ASSERT_EQ(ast.services[0].methods.size(), 1u);
    ASSERT_EQ(ast.services[0].methods[0].name, "start");
    ASSERT_EQ(ast.services[0].publishes.size(), 2u);
    ASSERT_EQ(ast.services[0].publishes[0], "TemperatureUpdate");
    ASSERT_EQ(ast.services[0].publishes[1], "PressureUpdate");
}

TEST(IdlCompilerTest, ParserTopicDefinition) {
    std::string src =
        "topic SensorData {\n"
        "    float64 temperature;\n"
        "    float64 pressure;\n"
        "    uint64 timestamp;\n"
        "}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());
    ASSERT_EQ(ast.topics.size(), 1u);
    ASSERT_EQ(ast.topics[0].name, "SensorData");
    ASSERT_EQ(ast.topics[0].fields.size(), 3u);

    ASSERT_EQ(ast.topics[0].fields[0].name, "temperature");
    ASSERT_EQ(ast.topics[0].fields[0].type.primitive, TYPE_FLOAT64);

    ASSERT_EQ(ast.topics[0].fields[1].name, "pressure");
    ASSERT_EQ(ast.topics[0].fields[1].type.primitive, TYPE_FLOAT64);

    ASSERT_EQ(ast.topics[0].fields[2].name, "timestamp");
    ASSERT_EQ(ast.topics[0].fields[2].type.primitive, TYPE_UINT64);
}

TEST(IdlCompilerTest, ParserFullIdlFile) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());
    ASSERT_EQ(ast.package_name, "sensors");
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.topics.size(), 1u);
    ASSERT_EQ(ast.services.size(), 1u);

    ASSERT_EQ(ast.structs[0].name, "Reading");
    ASSERT_EQ(ast.structs[0].fields.size(), 2u);

    ASSERT_EQ(ast.topics[0].name, "SensorUpdate");
    ASSERT_EQ(ast.topics[0].fields.size(), 2u);
    ASSERT_EQ(ast.topics[0].fields[1].type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.topics[0].fields[1].type.custom_name, "Reading");

    ASSERT_EQ(ast.services[0].name, "SensorService");
    ASSERT_EQ(ast.services[0].methods.size(), 2u);
    ASSERT_EQ(ast.services[0].methods[0].name, "getLatest");
    ASSERT_EQ(ast.services[0].methods[0].return_type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.services[0].methods[0].return_type.custom_name, "Reading");
    ASSERT_TRUE(ast.services[0].methods[0].has_param);
    ASSERT_EQ(ast.services[0].methods[0].param.type.primitive, TYPE_STRING);
    ASSERT_EQ(ast.services[0].methods[1].name, "calibrate");
    ASSERT_EQ(ast.services[0].methods[1].return_type.primitive, TYPE_VOID);
    ASSERT_FALSE(ast.services[0].methods[1].has_param);
    ASSERT_EQ(ast.services[0].publishes.size(), 1u);
    ASSERT_EQ(ast.services[0].publishes[0], "SensorUpdate");
}

TEST(IdlCompilerTest, ParserMultipleStructs) {
    std::string src =
        "struct A { int32 x; }\n"
        "struct B { string name; bool flag; }";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.structs.size(), 2u);
    ASSERT_EQ(ast.structs[0].name, "A");
    ASSERT_EQ(ast.structs[0].fields.size(), 1u);
    ASSERT_EQ(ast.structs[1].name, "B");
    ASSERT_EQ(ast.structs[1].fields.size(), 2u);
}

TEST(IdlCompilerTest, ParserEmptyStruct) {
    std::string src = "struct Empty {}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.structs[0].name, "Empty");
    ASSERT_EQ(ast.structs[0].fields.size(), 0u);
}

TEST(IdlCompilerTest, ParserServiceCustomParamAndReturn) {
    std::string src =
        "service Storage {\n"
        "    Record lookup(Query q);\n"
        "}";
    Lexer lexer(src);
    Parser parser(lexer);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.services.size(), 1u);
    ASSERT_EQ(ast.services[0].methods.size(), 1u);
    ASSERT_EQ(ast.services[0].methods[0].return_type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.services[0].methods[0].return_type.custom_name, "Record");
    ASSERT_TRUE(ast.services[0].methods[0].has_param);
    ASSERT_EQ(ast.services[0].methods[0].param.type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.services[0].methods[0].param.type.custom_name, "Query");
    ASSERT_EQ(ast.services[0].methods[0].param.name, "q");
}

// ============================================================
// Import Feature Tests
// ============================================================

TEST(IdlCompilerTest, LexerImportKeyword) {
    std::string src = "import \"file.bidl\";";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 4u);
    ASSERT_EQ(toks[0].type, TOK_IMPORT);
    ASSERT_EQ(toks[0].value, "import");
    ASSERT_EQ(toks[1].type, TOK_STRING);
    ASSERT_EQ(toks[1].value, "file.bidl");
    ASSERT_EQ(toks[2].type, TOK_SEMICOLON);
    ASSERT_EQ(toks[3].type, TOK_EOF);
}

TEST(IdlCompilerTest, LexerDotToken) {
    std::string src = "common.Point";
    std::vector<Token> toks = tokenize(src);

    ASSERT_EQ(toks.size(), 4u);
    ASSERT_EQ(toks[0].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[0].value, "common");
    ASSERT_EQ(toks[1].type, TOK_DOT);
    ASSERT_EQ(toks[1].value, ".");
    ASSERT_EQ(toks[2].type, TOK_IDENTIFIER);
    ASSERT_EQ(toks[2].value, "Point");
    ASSERT_EQ(toks[3].type, TOK_EOF);
}

TEST(IdlCompilerTest, ParserImportStatement) {
    const char* dir = "/tmp/omni-idlc_test_import";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

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

    std::ifstream ifs(std::string(dir) + "/main.bidl");
    std::string source((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ifs.close();

    ParseContext ctx;
    Lexer lex(source);
    Parser parser(lex, ctx, std::string(dir) + "/main.bidl");
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.package_name, "demo");
    ASSERT_EQ(ast.imports.size(), 2u);
    ASSERT_EQ(ast.imports[0], "common.bidl");
    ASSERT_EQ(ast.imports[1], "utils.bidl");

    ASSERT_TRUE(ctx.loaded_packages.find("common") != ctx.loaded_packages.end());
    ASSERT_TRUE(ctx.loaded_packages.find("utils") != ctx.loaded_packages.end());
    ASSERT_EQ(ctx.loaded_packages["common"].structs.size(), 1u);
    ASSERT_EQ(ctx.loaded_packages["common"].structs[0].name, "Point");
    ASSERT_EQ(ctx.loaded_packages["utils"].structs.size(), 1u);
    ASSERT_EQ(ctx.loaded_packages["utils"].structs[0].name, "Config");

    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.structs[0].fields.size(), 2u);
    ASSERT_EQ(ast.structs[0].fields[0].type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.structs[0].fields[0].type.package_name, "common");
    ASSERT_EQ(ast.structs[0].fields[0].type.custom_name, "Point");
    ASSERT_EQ(ast.structs[0].fields[1].type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.structs[0].fields[1].type.package_name, "utils");
    ASSERT_EQ(ast.structs[0].fields[1].type.custom_name, "Config");

    ASSERT_TRUE(ctx.type_registry.find("Point") != ctx.type_registry.end());
    ASSERT_EQ(ctx.type_registry["Point"], "common");
    ASSERT_TRUE(ctx.type_registry.find("Config") != ctx.type_registry.end());
    ASSERT_EQ(ctx.type_registry["Config"], "utils");
    ASSERT_TRUE(ctx.type_registry.find("Foo") != ctx.type_registry.end());
    ASSERT_EQ(ctx.type_registry["Foo"], "demo");

    ASSERT_GE(ctx.all_files.size(), 2u);
}

TEST(IdlCompilerTest, ParserCrossPackageTypeInService) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.services.size(), 1u);
    ASSERT_EQ(ast.services[0].methods.size(), 1u);
    ASSERT_EQ(ast.services[0].methods[0].return_type.primitive, TYPE_CUSTOM);
    ASSERT_EQ(ast.services[0].methods[0].return_type.package_name, "types");
    ASSERT_EQ(ast.services[0].methods[0].return_type.custom_name, "Response");
    ASSERT_TRUE(ast.services[0].methods[0].has_param);
    ASSERT_EQ(ast.services[0].methods[0].param.type.primitive, TYPE_INT32);
}

TEST(IdlCompilerTest, ParserCircularImportDetection) {
    const char* dir = "/tmp/omni-idlc_test_circular";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

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
    ASSERT_FALSE(parser.parse(ast));
    ASSERT_TRUE(parser.hasError());
    ASSERT_TRUE(parser.errorMessage().find("Circular import") != std::string::npos);
}

TEST(IdlCompilerTest, ParserDuplicateImportDedup) {
    const char* dir = "/tmp/omni-idlc_test_dedup";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ctx.loaded_packages.size(), 2u);
    ASSERT_TRUE(ctx.loaded_packages.find("base") != ctx.loaded_packages.end());
    int base_count = 0;
    for (size_t i = 0; i < ctx.all_files.size(); ++i) {
        if (ctx.all_files[i].find("base.bidl") != std::string::npos) base_count++;
    }
    ASSERT_EQ(base_count, 1);
}

TEST(IdlCompilerTest, ParserMissingImportFile) {
    std::string src =
        "package test;\n"
        "import \"nonexistent.bidl\";\n"
        "struct Foo { int32 x; }";

    ParseContext ctx;
    Lexer lex(src);
    Parser parser(lex, ctx, "/tmp/omni-idlc_test_missing/test.bidl");
    AstFile ast;
    ASSERT_FALSE(parser.parse(ast));
    ASSERT_TRUE(parser.hasError());
    ASSERT_TRUE(parser.errorMessage().find("Cannot open") != std::string::npos);
}

TEST(IdlCompilerTest, ParserRelativePathImport) {
    const char* dir = "/tmp/omni-idlc_test_relpath";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir) + "/sub").c_str()), 0);

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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_TRUE(ctx.loaded_packages.find("shared") != ctx.loaded_packages.end());
    ASSERT_EQ(ast.structs[0].fields[0].type.package_name, "shared");
    ASSERT_EQ(ast.structs[0].fields[0].type.custom_name, "Config");
}

TEST(IdlCompilerTest, ParserTransitiveImport) {
    const char* dir = "/tmp/omni-idlc_test_transitive";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_TRUE(ctx.loaded_packages.find("cpkg") != ctx.loaded_packages.end());
    ASSERT_TRUE(ctx.loaded_packages.find("bpkg") != ctx.loaded_packages.end());
    ASSERT_TRUE(ctx.loaded_packages.find("apkg") != ctx.loaded_packages.end());

    ASSERT_EQ(ctx.type_registry["Base"], "cpkg");
    ASSERT_EQ(ctx.type_registry["Mid"], "bpkg");
    ASSERT_EQ(ctx.type_registry["Top"], "apkg");
}

TEST(IdlCompilerTest, ParserNoImportBackwardCompat) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_EQ(ast.package_name, "legacy");
    ASSERT_TRUE(ast.imports.empty());
    ASSERT_EQ(ast.structs.size(), 1u);
    ASSERT_EQ(ast.topics.size(), 1u);
    ASSERT_EQ(ast.services.size(), 1u);
    ASSERT_EQ(ast.services[0].methods.size(), 2u);
    ASSERT_EQ(ast.services[0].publishes.size(), 1u);
}

// ============================================================
// Code Generation Regression Tests
// ============================================================

TEST(IdlCompilerTest, CodegenCppArrayUsesBufferSuffixMethods) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());

    const char* dir = "/tmp/omni-idlc_test_codegen_cpp_arrays";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

    CppCodeGen gen;
    ASSERT_TRUE(gen.generate(ast, dir, "arrays"));

    std::string cpp = readFile(std::string(dir) + "/arrays.cpp");

    ASSERT_TRUE(cpp.find("if (!buf.writeInt32(ids[i0])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.tryReadInt32(ids[i0])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.writeString(names[i0])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.tryReadString(names[i0])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.writeBytes(blobs[i0])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.tryReadBytes(blobs[i0])) return false;") != std::string::npos);

    ASSERT_TRUE(cpp.find("writeint32_t") == std::string::npos);
    ASSERT_TRUE(cpp.find("readint32_t") == std::string::npos);
    ASSERT_TRUE(cpp.find("writestd::string") == std::string::npos);
    ASSERT_TRUE(cpp.find("readstd::string") == std::string::npos);
    ASSERT_TRUE(cpp.find("writestd::vector<uint8_t>") == std::string::npos);
    ASSERT_TRUE(cpp.find("readstd::vector<uint8_t>") == std::string::npos);
}

TEST(IdlCompilerTest, CodegenCppMethodArraysUseRecursiveBufferLogic) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());

    const char* dir = "/tmp/omni-idlc_test_codegen_cpp_method_arrays";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

    CppCodeGen gen;
    ASSERT_TRUE(gen.generate(ast, dir, "array_service"));

    std::string cpp = readFile(std::string(dir) + "/array_service.cpp");

    ASSERT_TRUE(cpp.find("int ArrayServiceStub::onInvoke(uint32_t method_id, const omnibinder::Buffer& request, omnibinder::Buffer& response)") != std::string::npos);
    ASSERT_TRUE(cpp.find("{ uint32_t cnt0 = 0; if (!req.tryReadUint32(cnt0)) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE); blobs.resize(cnt0);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!req.tryReadBytes(blobs[i0])) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!response.writeUint32(static_cast<uint32_t>(result.size()))) return static_cast<int>(omnibinder::ErrorCode::ERR_SERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!response.writeInt32(result[i0])) return static_cast<int>(omnibinder::ErrorCode::ERR_SERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!response.writeString(result[i0])) return static_cast<int>(omnibinder::ErrorCode::ERR_SERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!req.writeUint32(static_cast<uint32_t>(blobs.size()))) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!req.writeBytes(blobs[i0])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!resp.tryReadInt32(out[i0])) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!resp.tryReadString(out[i0])) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);

    ASSERT_TRUE(cpp.find("req.write(") == std::string::npos);
    ASSERT_TRUE(cpp.find("resp.read()") == std::string::npos);
}

TEST(IdlCompilerTest, CodegenCppCustomArraysUseRecursiveCustomLogic) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());

    const char* dir = "/tmp/omni-idlc_test_codegen_cpp_custom_arrays";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

    CppCodeGen gen;
    ASSERT_TRUE(gen.generate(ast, dir, "custom_arrays"));

    std::string cpp = readFile(std::string(dir) + "/custom_arrays.cpp");

    ASSERT_TRUE(cpp.find("if (!items[i0].serialize(buf)) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!items[i0].deserialize(buf)) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!items[i0].deserialize(req)) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!response.writeUint32(static_cast<uint32_t>(result.size()))) return static_cast<int>(omnibinder::ErrorCode::ERR_SERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!result[i0].serialize(response)) return static_cast<int>(omnibinder::ErrorCode::ERR_SERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!req.writeUint32(static_cast<uint32_t>(items.size()))) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!items[i0].serialize(req)) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!out[i0].deserialize(resp)) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);

    ASSERT_TRUE(cpp.find("req.write(items)") == std::string::npos);
    ASSERT_TRUE(cpp.find("response.write(result)") == std::string::npos);
    ASSERT_TRUE(cpp.find("result = resp.read()") == std::string::npos);
}

TEST(IdlCompilerTest, CodegenCppNestedArraysUseDistinctLoopDepths) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());

    const char* dir = "/tmp/omni-idlc_test_codegen_cpp_nested_arrays";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

    CppCodeGen gen;
    ASSERT_TRUE(gen.generate(ast, dir, "nested_arrays"));

    std::string cpp = readFile(std::string(dir) + "/nested_arrays.cpp");

    ASSERT_TRUE(cpp.find("for (size_t i0 = 0; i0 < matrix.size(); ++i0) {") != std::string::npos);
    ASSERT_TRUE(cpp.find("for (size_t i1 = 0; i1 < matrix[i0].size(); ++i1) {") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.writeInt32(matrix[i0][i1])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("{ uint32_t cnt0 = 0; if (!buf.tryReadUint32(cnt0)) return false; matrix.resize(cnt0);") != std::string::npos);
    ASSERT_TRUE(cpp.find("{ uint32_t cnt1 = 0; if (!buf.tryReadUint32(cnt1)) return false; matrix[i0].resize(cnt1);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!buf.tryReadInt32(matrix[i0][i1])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!req.writeUint32(static_cast<uint32_t>(matrix.size()))) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!req.writeInt32(matrix[i0][i1])) return false;") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!resp.tryReadInt32(out[i0][i1])) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);

    ASSERT_TRUE(cpp.find("req.write(matrix)") == std::string::npos);
    ASSERT_TRUE(cpp.find("resp.read()") == std::string::npos);
}

TEST(IdlCompilerTest, CodegenCStringBytesSignaturesAndMetadata) {
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
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());

    const char* dir = "/tmp/omni-idlc_test_codegen_c_strings";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

    CCodeGen gen;
    ASSERT_TRUE(gen.generate(ast, dir, "blob_service"));

    std::string header = readFile(std::string(dir) + "/blob_service_c.h");
    std::string source = readFile(std::string(dir) + "/blob_service.c");

    ASSERT_TRUE(header.find("char* name;") != std::string::npos);
    ASSERT_TRUE(header.find("uint32_t name_len;") != std::string::npos);
    ASSERT_TRUE(header.find("uint8_t* data;") != std::string::npos);
    ASSERT_TRUE(header.find("uint32_t data_len;") != std::string::npos);

    ASSERT_TRUE(header.find("demo_BlobService_echo_name_handler_t echoName;") != std::string::npos);
    ASSERT_TRUE(header.find("demo_BlobService_echo_data_handler_t echoData;") != std::string::npos);
    ASSERT_TRUE(header.find("int  demo_BlobService_proxy_echo_name(demo_BlobService_proxy* p, const char* name, uint32_t name_len, char** result, uint32_t* result_len);") != std::string::npos);
    ASSERT_TRUE(header.find("int  demo_BlobService_proxy_echo_data(demo_BlobService_proxy* p, const uint8_t* data, uint32_t data_len, uint8_t** result, uint32_t* result_len);") != std::string::npos);

    ASSERT_TRUE(source.find("omni_service_add_method_ex(svc, demo_BlobService_METHOD_ECHO_NAME, \"echoName\", \"std::string\", \"std::string\");") != std::string::npos);
    ASSERT_TRUE(source.find("omni_service_add_method_ex(svc, demo_BlobService_METHOD_ECHO_DATA, \"echoData\", \"std::vector<uint8_t>\", \"std::vector<uint8_t>\");") != std::string::npos);
    ASSERT_TRUE(source.find("omni_service_add_method_ex(svc, demo_BlobService_METHOD_ROUND_TRIP, \"roundTrip\", \"Payload\", \"Payload\");") != std::string::npos);

    ASSERT_TRUE(source.find("name = omni_buffer_read_string(req, &name_len);") != std::string::npos);
    ASSERT_TRUE(source.find("data = omni_buffer_read_bytes(req, &data_len);") != std::string::npos);
    ASSERT_TRUE(source.find("omni_buffer_write_string(response, result, result_len);") != std::string::npos);
    ASSERT_TRUE(source.find("omni_buffer_write_bytes(response, result, result_len);") != std::string::npos);
    ASSERT_TRUE(source.find("*result = omni_buffer_read_string(resp, result_len);") != std::string::npos);
    ASSERT_TRUE(source.find("*result = omni_buffer_read_bytes(resp, result_len);") != std::string::npos);
}

TEST(IdlCompilerTest, CodegenCppAndCMalformedDeserializeGuardsAllTypeClasses) {
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
        "    publishes AllTopic;\n"
        "}";

    Lexer lex(src);
    Parser parser(lex);
    AstFile ast;
    ASSERT_TRUE(parser.parse(ast));
    ASSERT_FALSE(parser.hasError());

    const char* dir = "/tmp/omni-idlc_test_codegen_guards";
    ASSERT_EQ(system(std::string("mkdir -p " + std::string(dir)).c_str()), 0);

    CppCodeGen cpp_gen;
    ASSERT_TRUE(cpp_gen.generate(ast, dir, "guarded"));
    CCodeGen c_gen;
    ASSERT_TRUE(c_gen.generate(ast, dir, "guarded"));

    std::string cpp = readFile(std::string(dir) + "/guarded.cpp");
    std::string c_header = readFile(std::string(dir) + "/guarded_c.h");
    std::string c_source = readFile(std::string(dir) + "/guarded.c");

    ASSERT_TRUE(cpp.find("if (!input.deserialize(req)) return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (!msg.deserialize(buf)) return;") != std::string::npos);
    ASSERT_TRUE(cpp.find("GuardedServiceProxy::echo(const AllTypes& input, AllTypes& out)") != std::string::npos);
    ASSERT_TRUE(cpp.find("int ret = runtime_.invoke(\"GuardedService\"") != std::string::npos);
    ASSERT_TRUE(cpp.find("if (ret != 0) return ret;") != std::string::npos);
    ASSERT_TRUE(cpp.find("return static_cast<int>(omnibinder::ErrorCode::ERR_DESERIALIZE);") != std::string::npos);

    ASSERT_TRUE(c_header.find("int demo_Inner_deserialize(demo_Inner* self, omni_buffer_t* buf);") != std::string::npos);
    ASSERT_TRUE(c_header.find("int demo_AllTypes_deserialize(demo_AllTypes* self, omni_buffer_t* buf);") != std::string::npos);
    ASSERT_TRUE(c_header.find("int demo_AllTopic_deserialize(demo_AllTopic* self, omni_buffer_t* buf);") != std::string::npos);
    ASSERT_TRUE(c_source.find("if (!demo_AllTypes_deserialize(&input, req)) { demo_AllTypes_destroy(&input); omni_buffer_destroy(req); return -501; }") != std::string::npos);
    ASSERT_TRUE(c_source.find("if (!demo_AllTypes_deserialize(result, resp)) { demo_AllTypes_destroy(result); ret = -501; }") != std::string::npos);
    ASSERT_TRUE(c_source.find("if (!demo_AllTopic_deserialize(&msg, buf)) {") != std::string::npos);
    ASSERT_TRUE(c_source.find("if (!omni_buffer_read_ok(buf)) { goto fail; }") != std::string::npos);
    ASSERT_TRUE(c_source.find("return 1;") != std::string::npos);
}

TEST(IdlCompilerTest, CodegenCCustomStructNamingIsForwardDeclSafe) {
#ifdef _WIN32
    std::string dir = "omni-idlc_test_codegen_c_custom";
    mkdir(dir.c_str());
#else
    char dir_template[] = "/tmp/omni-idlc_test_codegen_c_custom_XXXXXX";
    char* dir_path = mkdtemp(dir_template);
    ASSERT_TRUE(dir_path != NULL);
    std::string dir(dir_path);
#endif

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
        "    publishes ItemTopic;\n"
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
    ParseContext common_ctx;
    ASSERT_TRUE(parseFile(common_path, common_ast, common_ctx));
    ASSERT_EQ(common_ctx.loaded_packages.count("common"), 1u);
    CCodeGen common_gen;
    ASSERT_TRUE(common_gen.generate(common_ast, dir, "common_types.bidl"));

    AstFile main_ast;
    ParseContext main_ctx;
    ASSERT_TRUE(parseFile(main_path, main_ast, main_ctx));
    ASSERT_EQ(main_ctx.loaded_packages.count("demo"), 1u);
    ASSERT_EQ(main_ctx.loaded_packages.count("common"), 1u);
    CCodeGen main_gen;
    ASSERT_TRUE(main_gen.generate(main_ast, dir, "item_service.bidl"));

    std::string header = readFile(dir + "/item_service.bidl_c.h");

    ASSERT_TRUE(header.find("struct demo_Item;") != std::string::npos);
    ASSERT_TRUE(header.find("struct demo_Item item;") != std::string::npos);
    ASSERT_TRUE(header.find("struct demo_Item* data;") != std::string::npos);

    unlink(common_path.c_str());
    unlink(main_path.c_str());
}
