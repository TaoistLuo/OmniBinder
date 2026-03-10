#include "../tools/omni_cli/simple_json.h"
#include "../tools/omni_cli/type_codec.h"
#include "../tools/omni_idlc/lexer.h"
#include "../tools/omni_idlc/parser.h"

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
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/%sXXXXXX%s", prefix.c_str(), suffix.c_str());

    std::string pattern(tmpl);
    size_t suffix_len = suffix.size();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    int fd = mkstemps(writable.data(), static_cast<int>(suffix_len));
    assert(fd >= 0);

    std::ofstream out(writable.data());
    out << contents;
    out.close();
    close(fd);
    return std::string(writable.data());
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

    printf("\nAll omni-cli TypeCodec tests passed.\n");
    return 0;
}
