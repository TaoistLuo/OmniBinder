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
#ifdef _WIN32
#include <direct.h>
#define mkdtemp(p) (_mkdir("omnibinder_tmp") == 0 ? (char*)"omnibinder_tmp" : NULL)
#else
#include <unistd.h>
#endif
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
    if (!parser.parse(ast)) {
        abort();
    }
    assert(!parser.hasError());
    return ctx;
}

static void appendTruncatedBuffers(const std::vector<uint8_t>& full,
                                   std::vector< std::vector<uint8_t> >& out) {
    for (size_t i = 0; i < full.size(); ++i) {
        out.push_back(std::vector<uint8_t>(full.begin(), full.begin() + i));
    }
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
        assert(codec.encodeToBuffer(json, parentType, "demo", buf));

        omnibinder::Buffer decodeBuf(buf.data(), buf.size());
        simple_json::Value decoded;
        assert(codec.decodeFromBuffer(decodeBuf, parentType, "demo", decoded));
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

    TEST(type_codec_decode_rejects_truncated_buffers_for_all_supported_type_classes);
    {
        const std::string common =
            "package common;\n"
            "struct External {\n"
            "    int32 code;\n"
            "    string note;\n"
            "}\n";
        const std::string main =
            "package demo;\n"
            "import \"common_types.bidl\";\n"
            "struct Inner {\n"
            "    int32 value;\n"
            "}\n"
            "struct Wrapper {\n"
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
            "    Inner inner;\n"
            "    array<int32> ids;\n"
            "    array<string> names;\n"
            "    array<Inner> inners;\n"
            "    array<array<int32>> matrix;\n"
            "    common.External ext;\n"
            "}\n";

        char dir_template[] = "/tmp/omnibinder_codec_matrix_XXXXXX";
        char* dir_path = mkdtemp(dir_template);
        assert(dir_path != NULL);
        std::string dir(dir_path);
        std::string common_path = dir + "/common_types.bidl";
        std::string main_path = dir + "/matrix.bidl";
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
        type_codec::TypeCodec codec(ctx);

        auto runTruncationCase = [&codec](const omnic::TypeRef& type,
                                          const std::string& package,
                                          const simple_json::Value& value) {
            omnibinder::Buffer full;
            if (!codec.encodeToBuffer(value, type, package, full)) {
                abort();
            }
            std::vector<uint8_t> bytes(full.data(), full.data() + full.size());
            std::vector< std::vector<uint8_t> > truncated;
            appendTruncatedBuffers(bytes, truncated);
            for (size_t j = 0; j < truncated.size(); ++j) {
                omnibinder::Buffer decode_buf(truncated[j].data(), truncated[j].size());
                simple_json::Value decoded;
                if (codec.decodeFromBuffer(decode_buf, type, package, decoded)) {
                    abort();
                }
            }
        };

        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_BOOL;
            runTruncationCase(type, "demo", simple_json::Value(true));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_INT8;
            runTruncationCase(type, "demo", simple_json::Value(-8.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_UINT8;
            runTruncationCase(type, "demo", simple_json::Value(8.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_INT16;
            runTruncationCase(type, "demo", simple_json::Value(-16.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_UINT16;
            runTruncationCase(type, "demo", simple_json::Value(16.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_INT32;
            runTruncationCase(type, "demo", simple_json::Value(-32.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_UINT32;
            runTruncationCase(type, "demo", simple_json::Value(32.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_INT64;
            runTruncationCase(type, "demo", simple_json::Value(-64.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_UINT64;
            runTruncationCase(type, "demo", simple_json::Value(64.0));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_FLOAT32;
            runTruncationCase(type, "demo", simple_json::Value(3.25));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_FLOAT64;
            runTruncationCase(type, "demo", simple_json::Value(6.5));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_STRING;
            runTruncationCase(type, "demo", simple_json::Value(std::string("hello")));
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_CUSTOM;
            type.custom_name = "Inner";
            simple_json::Value value;
            value.setObject();
            value.set("value", simple_json::Value(7.0));
            runTruncationCase(type, "demo", value);
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_CUSTOM;
            type.package_name = "common";
            type.custom_name = "External";
            simple_json::Value value;
            value.setObject();
            value.set("code", simple_json::Value(5.0));
            value.set("note", simple_json::Value(std::string("ext")));
            runTruncationCase(type, "demo", value);
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_ARRAY;
            type.element_type = new omnic::TypeRef();
            type.element_type->primitive = omnic::TYPE_INT32;
            simple_json::Value value;
            value.setArray();
            value.push(simple_json::Value(1.0));
            value.push(simple_json::Value(2.0));
            runTruncationCase(type, "demo", value);
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_ARRAY;
            type.element_type = new omnic::TypeRef();
            type.element_type->primitive = omnic::TYPE_STRING;
            simple_json::Value value;
            value.setArray();
            value.push(simple_json::Value(std::string("a")));
            value.push(simple_json::Value(std::string("b")));
            runTruncationCase(type, "demo", value);
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_ARRAY;
            type.element_type = new omnic::TypeRef();
            type.element_type->primitive = omnic::TYPE_CUSTOM;
            type.element_type->custom_name = "Inner";
            simple_json::Value value;
            value.setArray();
            simple_json::Value item;
            item.setObject();
            item.set("value", simple_json::Value(9.0));
            value.push(item);
            runTruncationCase(type, "demo", value);
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_ARRAY;
            type.element_type = new omnic::TypeRef();
            type.element_type->primitive = omnic::TYPE_ARRAY;
            type.element_type->element_type = new omnic::TypeRef();
            type.element_type->element_type->primitive = omnic::TYPE_INT32;
            simple_json::Value value;
            value.setArray();
            simple_json::Value row;
            row.setArray();
            row.push(simple_json::Value(1.0));
            row.push(simple_json::Value(2.0));
            value.push(row);
            runTruncationCase(type, "demo", value);
        }
        {
            omnic::TypeRef type;
            type.primitive = omnic::TYPE_CUSTOM;
            type.custom_name = "Wrapper";
            simple_json::Value value;
            value.setObject();
            value.set("flag", simple_json::Value(true));
            value.set("i8", simple_json::Value(-8.0));
            value.set("u8", simple_json::Value(8.0));
            value.set("i16", simple_json::Value(-16.0));
            value.set("u16", simple_json::Value(16.0));
            value.set("i32", simple_json::Value(-32.0));
            value.set("u32", simple_json::Value(32.0));
            value.set("i64", simple_json::Value(-64.0));
            value.set("u64", simple_json::Value(64.0));
            value.set("f32", simple_json::Value(3.25));
            value.set("f64", simple_json::Value(6.5));
            value.set("name", simple_json::Value(std::string("wrapper")));
            {
                simple_json::Value inner;
                inner.setObject();
                inner.set("value", simple_json::Value(11.0));
                value.set("inner", inner);
            }
            {
                simple_json::Value ids;
                ids.setArray();
                ids.push(simple_json::Value(1.0));
                ids.push(simple_json::Value(2.0));
                value.set("ids", ids);
            }
            {
                simple_json::Value names;
                names.setArray();
                names.push(simple_json::Value(std::string("x")));
                names.push(simple_json::Value(std::string("y")));
                value.set("names", names);
            }
            {
                simple_json::Value inners;
                inners.setArray();
                simple_json::Value item;
                item.setObject();
                item.set("value", simple_json::Value(12.0));
                inners.push(item);
                value.set("inners", inners);
            }
            {
                simple_json::Value matrix;
                matrix.setArray();
                simple_json::Value row;
                row.setArray();
                row.push(simple_json::Value(3.0));
                row.push(simple_json::Value(4.0));
                matrix.push(row);
                value.set("matrix", matrix);
            }
            {
                simple_json::Value ext;
                ext.setObject();
                ext.set("code", simple_json::Value(21.0));
                ext.set("note", simple_json::Value(std::string("note")));
                value.set("ext", ext);
            }
            runTruncationCase(type, "demo", value);
        }

        unlink(common_path.c_str());
        unlink(main_path.c_str());
        rmdir(dir.c_str());
        PASS();
    }

    printf("\nAll omni-cli TypeCodec tests passed.\n");
    return 0;
}
