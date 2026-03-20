#include "simple_json.h"
#include "type_codec.h"
#include "type_resolver.h"
#include "lexer.h"
#include "parser.h"

#include <omnibinder/buffer.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

using namespace omnic;

#define TEST(name) printf("  TEST %-42s ", #name); fflush(stdout)
#define PASS() printf("PASS\n"); fflush(stdout)

static std::string writeTempFile(const std::string& prefix, const std::string& suffix,
                                 const std::string& contents) {
    char dir_template[] = "/tmp/omnibinder_tmp_XXXXXX";
    char* dir_path = mkdtemp(dir_template);
    assert(dir_path != NULL);

    std::string file_path = std::string(dir_path) + "/" + prefix + suffix;
    std::ofstream out(file_path.c_str());
    out << contents;
    out.close();
    return file_path;
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
    printf("=== omni-cli TypeCodec Tests ===\n\n");

    TEST(nested_same_package_struct_roundtrip);
    {
        const std::string idl =
            "package demo;\n"
            "struct Child {\n"
            "    int32 value;\n"
            "}\n"
            "struct Parent {\n"
            "    Child child;\n"
            "    string label;\n"
            "}\n";

        std::string file_path = writeTempFile("omnibinder_demo_", ".bidl", idl);
        AstFile ast;
        ParseContext ctx = parseFile(file_path, ast);
        assert(ctx.loaded_packages.count("demo") == 1);

        omnic::TypeRef parentType;
        parentType.primitive = omnic::TYPE_CUSTOM;
        parentType.custom_name = "Parent";

        simple_json::Value json;
        json.setObject();
        simple_json::Value child;
        child.setObject();
        child.set("value", simple_json::Value(42.0));
        json.set("child", child);
        json.set("label", simple_json::Value(std::string("nested-ok")));

        type_codec::TypeCodec codec(ctx);
        omnibinder::Buffer buf;
        bool encoded = codec.encodeToBuffer(json, parentType, "demo", buf);
        assert(encoded);

        omnibinder::Buffer decodeBuf(buf.data(), buf.size());
        simple_json::Value decoded;
        bool decoded_ok = codec.decodeFromBuffer(decodeBuf, parentType, "demo", decoded);
        assert(decoded_ok);
        assert(decoded.isObject());
        assert(decoded.get("label").asString() == "nested-ok");
        assert(decoded.get("child").isObject());
        assert(decoded.get("child").get("value").asInt64() == 42);

        unlink(file_path.c_str());
        PASS();
    }

    TEST(cross_package_struct_roundtrip);
    {
        const std::string common =
            "package common;\n"
            "struct Point {\n"
            "    int32 x;\n"
            "    int32 y;\n"
            "}\n";
        const std::string main =
            "package demo;\n"
            "import \"common_types.bidl\";\n"
            "struct Shape {\n"
            "    common.Point center;\n"
            "    string name;\n"
            "}\n";

        char dir_template[] = "/tmp/omnibinder_codec_XXXXXX";
        char* dir_path = mkdtemp(dir_template);
        assert(dir_path != NULL);
        std::string dir(dir_path);
        std::string common_path = dir + "/common_types.bidl";
        std::string main_path = dir + "/shape.bidl";

        {
            std::ofstream out(common_path.c_str());
            out << common;
        }
        {
            std::ofstream out(main_path.c_str());
            out << main;
        }

        AstFile ast;
        ParseContext ctx = parseFile(main_path, ast);
        assert(ctx.loaded_packages.count("demo") == 1);
        assert(ctx.loaded_packages.count("common") == 1);

        omnic::TypeRef shapeType;
        shapeType.primitive = omnic::TYPE_CUSTOM;
        shapeType.custom_name = "Shape";

        simple_json::Value json;
        json.setObject();
        simple_json::Value center;
        center.setObject();
        center.set("x", simple_json::Value(7.0));
        center.set("y", simple_json::Value(9.0));
        json.set("center", center);
        json.set("name", simple_json::Value(std::string("origin-shifted")));

        type_codec::TypeCodec codec(ctx);
        omnibinder::Buffer buf;
        assert(codec.encodeToBuffer(json, shapeType, "demo", buf));

        omnibinder::Buffer decodeBuf(buf.data(), buf.size());
        simple_json::Value decoded;
        assert(codec.decodeFromBuffer(decodeBuf, shapeType, "demo", decoded));
        assert(decoded.get("name").asString() == "origin-shifted");
        assert(decoded.get("center").get("x").asInt64() == 7);
        assert(decoded.get("center").get("y").asInt64() == 9);

        unlink(common_path.c_str());
        unlink(main_path.c_str());
        rmdir(dir.c_str());
        PASS();
    }

    TEST(type_resolver_primitives_and_cross_package);
    {
        const std::string common =
            "package common;\n"
            "struct StatusResponse {\n"
            "    int32 code;\n"
            "    string message;\n"
            "}\n";
        const std::string main =
            "package demo;\n"
            "import \"common_types.bidl\";\n"
            "struct Wrapper {\n"
            "    common.StatusResponse status;\n"
            "}\n";

        char dir_template[] = "/tmp/omnibinder_resolver_XXXXXX";
        char* dir_path = mkdtemp(dir_template);
        assert(dir_path != NULL);
        std::string dir(dir_path);
        std::string common_path = dir + "/common_types.bidl";
        std::string main_path = dir + "/wrapper.bidl";

        {
            std::ofstream out(common_path.c_str());
            out << common;
        }
        {
            std::ofstream out(main_path.c_str());
            out << main;
        }

        AstFile ast;
        ParseContext ctx = parseFile(main_path, ast);

        omnic::TypeRef type_ref;
        assert(omni_cli::fillPrimitiveTypeRef("std::string", type_ref));
        assert(type_ref.primitive == omnic::TYPE_STRING);

        assert(omni_cli::fillPrimitiveTypeRef("std::vector<uint8_t>", type_ref));
        assert(type_ref.primitive == omnic::TYPE_BYTES);

        assert(omni_cli::fillPrimitiveTypeRef("int32_t", type_ref));
        assert(type_ref.primitive == omnic::TYPE_INT32);

        assert(omni_cli::findTypeRef(&ctx, "common::StatusResponse", "demo", type_ref));
        assert(type_ref.primitive == omnic::TYPE_CUSTOM);
        assert(type_ref.package_name == "common");
        assert(type_ref.custom_name == "StatusResponse");

        assert(omni_cli::findTypeRef(&ctx, "Wrapper", "demo", type_ref));
        assert(type_ref.primitive == omnic::TYPE_CUSTOM);
        assert(type_ref.package_name.empty());
        assert(type_ref.custom_name == "Wrapper");

        assert(!omni_cli::findTypeRef(&ctx, "common::Missing", "demo", type_ref));

        unlink(common_path.c_str());
        unlink(main_path.c_str());
        rmdir(dir.c_str());
        PASS();
    }

    TEST(type_codec_scalar_cli_friendly_values);
    {
        omnic::TypeRef intType;
        assert(omni_cli::fillPrimitiveTypeRef("int32_t", intType));
        assert(omni_cli::isScalarCliType(intType));

        omnic::TypeRef stringType;
        assert(omni_cli::fillPrimitiveTypeRef("std::string", stringType));
        assert(omni_cli::isScalarCliType(stringType));

        omnic::TypeRef bytesType;
        assert(omni_cli::fillPrimitiveTypeRef("std::vector<uint8_t>", bytesType));
        assert(!omni_cli::isScalarCliType(bytesType));

        omnic::TypeRef structType;
        structType.primitive = omnic::TYPE_CUSTOM;
        structType.custom_name = "Wrapper";
        assert(!omni_cli::isScalarCliType(structType));

        assert(std::string(omni_cli::primitiveTypeName(omnic::TYPE_INT32)) == "int32");
        assert(std::string(omni_cli::primitiveTypeName(omnic::TYPE_STRING)) == "string");
        assert(std::string(omni_cli::primitiveTypeName(omnic::TYPE_BYTES)) == "bytes");

        simple_json::Value scalar;
        assert(omni_cli::parseScalarCliValue("123", intType, scalar));
        assert(scalar.isNumber());
        assert(scalar.asInt64() == 123);

        assert(omni_cli::parseScalarCliValue("hello", stringType, scalar));
        assert(scalar.isString());
        assert(scalar.asString() == "hello");

        assert(!omni_cli::parseScalarCliValue("ff", bytesType, scalar));
        assert(!omni_cli::parseScalarCliValue("123", structType, scalar));
        PASS();
    }

    TEST(type_codec_rejects_object_for_scalar);
    {
        omnic::TypeRef intType;
        assert(omni_cli::fillPrimitiveTypeRef("int32_t", intType));

        simple_json::Value badJson;
        badJson.setObject();
        badJson.set("in", simple_json::Value(32.0));

        ParseContext ctx;
        type_codec::TypeCodec codec(ctx);
        omnibinder::Buffer buf;
        assert(!codec.encodeToBuffer(badJson, intType, "demo", buf));
        PASS();
    }

    printf("\nAll omni-cli TypeCodec tests passed.\n");
    return 0;
}
