#include "codegen_c_internal.h"
#include <cctype>
#include <cstddef>

namespace omnic {

static std::string cTypeToken(const TypeRef& type, const std::string& pkg) {
    switch (type.primitive) {
    case TYPE_BOOL:    return "bool";
    case TYPE_INT8:    return "int8_t";
    case TYPE_UINT8:   return "uint8_t";
    case TYPE_INT16:   return "int16_t";
    case TYPE_UINT16:  return "uint16_t";
    case TYPE_INT32:   return "int32_t";
    case TYPE_UINT32:  return "uint32_t";
    case TYPE_INT64:   return "int64_t";
    case TYPE_UINT64:  return "uint64_t";
    case TYPE_FLOAT32: return "float";
    case TYPE_FLOAT64: return "double";
    case TYPE_STRING:  return "string";
    case TYPE_BYTES:   return "bytes";
    case TYPE_VOID:    return "void";
    case TYPE_CUSTOM:
        if (!type.package_name.empty()) {
            return type.package_name + "_" + type.custom_name;
        }
        return pkg + "_" + type.custom_name;
    case TYPE_ARRAY:
        if (type.element_type) {
            return cTypeToken(*type.element_type, pkg) + "_array";
        }
        return "void_array";
    default:
        return "void";
    }
}

std::string cTypeName(const TypeRef& type, const std::string& pkg) {
    switch (type.primitive) {
    case TYPE_BOOL:    return "uint8_t";
    case TYPE_INT8:    return "int8_t";
    case TYPE_UINT8:   return "uint8_t";
    case TYPE_INT16:   return "int16_t";
    case TYPE_UINT16:  return "uint16_t";
    case TYPE_INT32:   return "int32_t";
    case TYPE_UINT32:  return "uint32_t";
    case TYPE_INT64:   return "int64_t";
    case TYPE_UINT64:  return "uint64_t";
    case TYPE_FLOAT32: return "float";
    case TYPE_FLOAT64: return "double";
    case TYPE_STRING:  return "char*";
    case TYPE_BYTES:   return "uint8_t*";
    case TYPE_VOID:    return "void";
    case TYPE_CUSTOM:
        if (!type.package_name.empty()) {
            return type.package_name + "_" + type.custom_name;
        }
        return pkg + "_" + type.custom_name;
    case TYPE_ARRAY:
        return cArrayTypeName(type, pkg);
    default: return "void";
    }
}

std::string cDeclaredTypeName(const TypeRef& type, const std::string& pkg) {
    if (type.isCustom() && type.package_name.empty()) {
        return "struct " + pkg + "_" + type.custom_name;
    }
    return cTypeName(type, pkg);
}

std::string cArrayTypeName(const TypeRef& type, const std::string& pkg) {
    return pkg + "_" + cTypeToken(type, pkg);
}

bool arrayElementNeedsLengths(const TypeRef& type) {
    return type.primitive == TYPE_STRING || type.primitive == TYPE_BYTES;
}

bool isCompositePointerType(const TypeRef& type) {
    return type.isCustom() || type.isArray();
}

bool isCStringLike(const TypeRef& type) {
    return type.primitive == TYPE_STRING || type.primitive == TYPE_BYTES;
}

static bool containsString(const std::vector<std::string>& values, const std::string& value) {
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] == value) {
            return true;
        }
    }
    return false;
}

static void collectArrayTypes(const TypeRef& type, const std::string& pkg,
                              std::vector<TypeRef>& arrays,
                              std::vector<std::string>& names) {
    if (!type.isArray()) {
        return;
    }
    if (type.element_type) {
        collectArrayTypes(*type.element_type, pkg, arrays, names);
    }
    std::string name = cArrayTypeName(type, pkg);
    if (!containsString(names, name)) {
        names.push_back(name);
        arrays.push_back(type);
    }
}

std::vector<TypeRef> collectArrayTypesFromAst(const AstFile& ast, const std::string& pkg) {
    std::vector<TypeRef> arrays;
    std::vector<std::string> names;
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        for (size_t j = 0; j < ast.structs[i].fields.size(); ++j) {
            collectArrayTypes(ast.structs[i].fields[j].type, pkg, arrays, names);
        }
    }
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        for (size_t j = 0; j < ast.topics[i].fields.size(); ++j) {
            collectArrayTypes(ast.topics[i].fields[j].type, pkg, arrays, names);
        }
    }
    for (size_t i = 0; i < ast.services.size(); ++i) {
        for (size_t j = 0; j < ast.services[i].methods.size(); ++j) {
            const MethodDef& method = ast.services[i].methods[j];
            collectArrayTypes(method.return_type, pkg, arrays, names);
            if (method.has_param) {
                collectArrayTypes(method.param.type, pkg, arrays, names);
            }
        }
    }
    return arrays;
}

const char* primitiveReadFunc(PrimitiveType p) {
    switch (p) {
    case TYPE_BOOL:    return "omni_buffer_read_bool";
    case TYPE_INT8:    return "omni_buffer_read_int8";
    case TYPE_UINT8:   return "omni_buffer_read_uint8";
    case TYPE_INT16:   return "omni_buffer_read_int16";
    case TYPE_UINT16:  return "omni_buffer_read_uint16";
    case TYPE_INT32:   return "omni_buffer_read_int32";
    case TYPE_UINT32:  return "omni_buffer_read_uint32";
    case TYPE_INT64:   return "omni_buffer_read_int64";
    case TYPE_UINT64:  return "omni_buffer_read_uint64";
    case TYPE_FLOAT32: return "omni_buffer_read_float32";
    case TYPE_FLOAT64: return "omni_buffer_read_float64";
    default: return NULL;
    }
}

const char* primitiveWriteFunc(PrimitiveType p) {
    switch (p) {
    case TYPE_BOOL:    return "omni_buffer_write_bool";
    case TYPE_INT8:    return "omni_buffer_write_int8";
    case TYPE_UINT8:   return "omni_buffer_write_uint8";
    case TYPE_INT16:   return "omni_buffer_write_int16";
    case TYPE_UINT16:  return "omni_buffer_write_uint16";
    case TYPE_INT32:   return "omni_buffer_write_int32";
    case TYPE_UINT32:  return "omni_buffer_write_uint32";
    case TYPE_INT64:   return "omni_buffer_write_int64";
    case TYPE_UINT64:  return "omni_buffer_write_uint64";
    case TYPE_FLOAT32: return "omni_buffer_write_float32";
    case TYPE_FLOAT64: return "omni_buffer_write_float64";
    default: return NULL;
    }
}

void emitProxyMethodParams(std::ostream& os, const MethodDef& m, const std::string& pkg,
                           bool declared_names) {
    if (m.has_param) {
        std::string ptype = declared_names ? cDeclaredTypeName(m.param.type, pkg)
                                           : cTypeName(m.param.type, pkg);
        if (isCompositePointerType(m.param.type)) {
            os << ", const " << ptype << "* " << m.param.name;
        } else if (isCStringLike(m.param.type)) {
            os << ", const " << ptype << " " << m.param.name << ", uint32_t " << m.param.name << "_len";
        } else {
            os << ", " << ptype << " " << m.param.name;
        }
    }
    if (!m.return_type.isVoid()) {
        std::string rtype = declared_names ? cDeclaredTypeName(m.return_type, pkg)
                                           : cTypeName(m.return_type, pkg);
        if (m.return_type.isArray() || m.return_type.isCustom()) {
            os << ", " << rtype << "* result";
        } else if (isCStringLike(m.return_type)) {
            os << ", " << rtype << "* result, uint32_t* result_len";
        } else {
            os << ", " << rtype << "* result";
        }
    }
}

std::string cToSnakeCase(const std::string& name) {
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        if (isupper(name[i])) {
            if (i > 0 && !isupper(name[i-1])) result += '_';
            result += (char)tolower(name[i]);
        } else {
            result += name[i];
        }
    }
    return result;
}

} // namespace omnic
