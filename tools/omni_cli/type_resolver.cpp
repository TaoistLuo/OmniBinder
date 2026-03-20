#include "type_resolver.h"
#include "simple_json.h"

namespace omni_cli {

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

bool findTypeRef(const omnic::ParseContext* parse_ctx, const std::string& type_name,
                 const std::string& package, omnic::TypeRef& type_ref)
{
    if (!parse_ctx) return false;

    if (fillPrimitiveTypeRef(type_name, type_ref)) {
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
    case omnic::TYPE_UINT8:
    case omnic::TYPE_INT16:
    case omnic::TYPE_UINT16:
    case omnic::TYPE_INT32:
    case omnic::TYPE_UINT32:
    case omnic::TYPE_INT64:
    case omnic::TYPE_UINT64:
    case omnic::TYPE_FLOAT32:
    case omnic::TYPE_FLOAT64:
        try {
            value = simple_json::Value(std::stod(input));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    case omnic::TYPE_STRING:
        value = simple_json::Value(input);
        return true;
    default:
        return false;
    }
}

} // namespace omni_cli
