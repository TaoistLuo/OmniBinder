#include "type_resolver.h"
#include "simple_json.h"
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <cmath>

namespace omni_cli {

namespace {

bool startsLikeNumber(const std::string& input) {
    if (input.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(input[0]);
    return isdigit(first) || first == '-';
}

} // namespace

const char* primitiveTypeName(omnic::PrimitiveType primitive) {
    switch (primitive) {
    case omnic::TYPE_BOOL: return "bool";
    case omnic::TYPE_INT8: return "int8";
    case omnic::TYPE_UINT8: return "uint8";
    case omnic::TYPE_INT16: return "int16";
    case omnic::TYPE_UINT16: return "uint16";
    case omnic::TYPE_INT32: return "int32";
    case omnic::TYPE_UINT32: return "uint32";
    case omnic::TYPE_INT64: return "int64";
    case omnic::TYPE_UINT64: return "uint64";
    case omnic::TYPE_FLOAT32: return "float32";
    case omnic::TYPE_FLOAT64: return "float64";
    case omnic::TYPE_STRING: return "string";
    case omnic::TYPE_BYTES: return "bytes";
    case omnic::TYPE_VOID: return "void";
    default: return "unknown";
    }
}

bool fillPrimitiveTypeRef(const std::string& type_name, omnic::TypeRef& type_ref) {
    if (type_name == "bool") type_ref.primitive = omnic::TYPE_BOOL;
    else if (type_name == "int8" || type_name == "int8_t") type_ref.primitive = omnic::TYPE_INT8;
    else if (type_name == "uint8" || type_name == "uint8_t") type_ref.primitive = omnic::TYPE_UINT8;
    else if (type_name == "int16" || type_name == "int16_t") type_ref.primitive = omnic::TYPE_INT16;
    else if (type_name == "uint16" || type_name == "uint16_t") type_ref.primitive = omnic::TYPE_UINT16;
    else if (type_name == "int32" || type_name == "int32_t") type_ref.primitive = omnic::TYPE_INT32;
    else if (type_name == "uint32" || type_name == "uint32_t") type_ref.primitive = omnic::TYPE_UINT32;
    else if (type_name == "int64" || type_name == "int64_t") type_ref.primitive = omnic::TYPE_INT64;
    else if (type_name == "uint64" || type_name == "uint64_t") type_ref.primitive = omnic::TYPE_UINT64;
    else if (type_name == "float" || type_name == "float32") type_ref.primitive = omnic::TYPE_FLOAT32;
    else if (type_name == "double" || type_name == "float64") type_ref.primitive = omnic::TYPE_FLOAT64;
    else if (type_name == "string" || type_name == "std::string") type_ref.primitive = omnic::TYPE_STRING;
    else if (type_name == "bytes" || type_name == "std::vector<uint8_t>") type_ref.primitive = omnic::TYPE_BYTES;
    else if (type_name == "void") type_ref.primitive = omnic::TYPE_VOID;
    else return false;

    type_ref.package_name.clear();
    type_ref.custom_name.clear();
    delete type_ref.element_type;
    type_ref.element_type = NULL;
    return true;
}

namespace {

/* @brief 判断类型名是否为数组包装（std::vector<T> / array<T>），是则输出内层类型名
 * @param[in] type_name 类型名
 * @param[out] element_name 内层元素类型名
 * @return 是数组包装返回 true
 * @note 仅精确匹配包装前后缀，避免误吞普通自定义类型名
 */
bool unwrapArrayTypeName(const std::string& type_name, std::string& element_name) {
    static const char* kPrefixes[] = { "std::vector<", "array<" };
    for (size_t pi = 0; pi < sizeof(kPrefixes) / sizeof(kPrefixes[0]); ++pi) {
        const std::string prefix(kPrefixes[pi]);
        if (type_name.size() <= prefix.size() + 1) {
            continue;
        }
        if (type_name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        if (type_name[type_name.size() - 1] != '>') {
            continue;
        }
        size_t begin = prefix.size();
        size_t end = type_name.size() - 1;
        while (begin < end && type_name[begin] == ' ') ++begin;
        while (end > begin && type_name[end - 1] == ' ') --end;
        if (begin >= end) {
            continue;
        }
        element_name = type_name.substr(begin, end - begin);
        return true;
    }
    return false;
}

bool findTypeRefImpl(const omnic::ParseContext* parse_ctx, const std::string& type_name,
                     const std::string& package, omnic::TypeRef& type_ref, int depth) {
    if (!parse_ctx) return false;
    if (depth > 32) return false;

    // 基础类型优先（含 std::vector<uint8_t> → bytes），避免被数组包装语义覆盖
    if (fillPrimitiveTypeRef(type_name, type_ref)) {
        return true;
    }

    // 数组包装：递归解析元素类型后构造 element_type
    std::string element_name;
    if (unwrapArrayTypeName(type_name, element_name)) {
        omnic::TypeRef element_ref;
        if (!findTypeRefImpl(parse_ctx, element_name, package, element_ref, depth + 1)) {
            return false;
        }
        type_ref.primitive = omnic::TYPE_ARRAY;
        type_ref.custom_name.clear();
        type_ref.package_name.clear();
        delete type_ref.element_type;
        type_ref.element_type = new omnic::TypeRef(element_ref);
        return true;
    }

    std::string search_package = package;
    std::string search_type = type_name;
    size_t scope = type_name.find("::");
    if (scope != std::string::npos) {
        search_package = type_name.substr(0, scope);
        search_type = type_name.substr(scope + 2);
    }

    std::map<std::string, omnic::AstFile>::const_iterator pkg_it = parse_ctx->loaded_packages.find(search_package);
    if (pkg_it == parse_ctx->loaded_packages.end()) {
        return false;
    }

    const omnic::AstFile& ast = pkg_it->second;
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        if (ast.structs[i].name == search_type) {
            type_ref.primitive = omnic::TYPE_CUSTOM;
            type_ref.custom_name = search_type;
            type_ref.package_name = (search_package == package) ? "" : search_package;
            delete type_ref.element_type;
            type_ref.element_type = NULL;
            return true;
        }
    }

    return false;
}

} // namespace

bool findTypeRef(const omnic::ParseContext* parse_ctx, const std::string& type_name,
                 const std::string& package, omnic::TypeRef& type_ref)
{
    return findTypeRefImpl(parse_ctx, type_name, package, type_ref, 0);
}

bool isScalarCliType(const omnic::TypeRef& type_ref) {
    switch (type_ref.primitive) {
    case omnic::TYPE_BOOL:
    case omnic::TYPE_INT8:
    case omnic::TYPE_UINT8:
    case omnic::TYPE_INT16:
    case omnic::TYPE_UINT16:
    case omnic::TYPE_INT32:
    case omnic::TYPE_UINT32:
    case omnic::TYPE_INT64:
    case omnic::TYPE_UINT64:
    case omnic::TYPE_FLOAT32:
    case omnic::TYPE_FLOAT64:
    case omnic::TYPE_STRING:
        return true;
    default:
        return false;
    }
}

bool parseScalarCliValue(const char* text, const omnic::TypeRef& type_ref, simple_json::Value& value) {
    if (!text || !isScalarCliType(type_ref)) {
        return false;
    }

    std::string input(text);
    switch (type_ref.primitive) {
    case omnic::TYPE_BOOL:
        if (input == "true" || input == "1") {
            value = simple_json::Value(true);
            return true;
        }
        if (input == "false" || input == "0") {
            value = simple_json::Value(false);
            return true;
        }
        return false;
    case omnic::TYPE_INT8:
    case omnic::TYPE_INT16:
    case omnic::TYPE_INT32:
    case omnic::TYPE_INT64: {
        if (!startsLikeNumber(input)) {
            return false;
        }
        char* end = NULL;
        errno = 0;
        long long parsed = strtoll(input.c_str(), &end, 10);
        if (errno != 0 || end != input.c_str() + input.size()) {
            return false;
        }
        value = simple_json::Value(static_cast<int64_t>(parsed));
        return true;
    }
    case omnic::TYPE_UINT8:
    case omnic::TYPE_UINT16:
    case omnic::TYPE_UINT32:
    case omnic::TYPE_UINT64: {
        if (!startsLikeNumber(input) || input[0] == '-') {
            return false;
        }
        char* end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(input.c_str(), &end, 10);
        if (errno != 0 || end != input.c_str() + input.size()) {
            return false;
        }
        value = simple_json::Value(static_cast<uint64_t>(parsed));
        return true;
    }
    case omnic::TYPE_FLOAT32:
    case omnic::TYPE_FLOAT64: {
        if (!startsLikeNumber(input)) {
            return false;
        }
        char* end = NULL;
        errno = 0;
        double parsed = strtod(input.c_str(), &end);
        if (end != input.c_str() + input.size() || !std::isfinite(parsed)) {
            return false;
        }
        value = simple_json::Value(parsed);
        return true;
    }
    case omnic::TYPE_STRING:
        value = simple_json::Value(input);
        return true;
    default:
        return false;
    }
}

} // namespace omni_cli
