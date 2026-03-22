#include "codegen_c.h"
#include <fstream>
#include <cctype>
#include <cstdio>
#include <vector>

namespace omnic {

static void emitGeneratedFileBanner(std::ostream& os,
                                    const std::string& generated_name,
                                    const std::string& source_idl,
                                    const char* brief,
                                    const char* details) {
    os << "/**************************************************************************************************\n";
    os << " * @file        " << generated_name << "\n";
    os << " * @brief       " << brief << "\n";
    os << " * @details     " << details << "\n";
    os << " *              Source IDL: " << source_idl << ".bidl\n";
    os << " *              This file is auto-generated. DO NOT EDIT MANUALLY.\n";
    os << " *\n";
    os << " * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)\n";
    os << " *\n";
    os << " * MIT License\n";
    os << " *\n";
    os << " * Permission is hereby granted, free of charge, to any person obtaining a copy\n";
    os << " * of this software and associated documentation files (the \"Software\"), to deal\n";
    os << " * in the Software without restriction, including without limitation the rights\n";
    os << " * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n";
    os << " * copies of the Software, and to permit persons to whom the Software is\n";
    os << " * furnished to do so, subject to the following conditions:\n";
    os << " *\n";
    os << " * The above copyright notice and this permission notice shall be included in all\n";
    os << " * copies or substantial portions of the Software.\n";
    os << " *\n";
    os << " * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n";
    os << " * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n";
    os << " * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n";
    os << " * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n";
    os << " * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n";
    os << " * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n";
    os << " * SOFTWARE.\n";
    os << " *************************************************************************************************/\n";
}

static std::string cTypeToken(const TypeRef& type, const std::string& pkg);
static std::string cTypePrefix(const TypeRef& type, const std::string& pkg);
static std::string cDeclaredTypeName(const TypeRef& type, const std::string& pkg);
static void emitPrimitiveRead(std::ostream& os, PrimitiveType p,
                              const std::string& var, const std::string& buf,
                              const std::string& indent);
static void emitPrimitiveWrite(std::ostream& os, PrimitiveType p,
                               const std::string& var, const std::string& buf,
                               const std::string& indent);

static std::string cArrayTypeName(const TypeRef& type, const std::string& pkg) {
    return pkg + "_" + cTypeToken(type, pkg);
}

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

static std::vector<TypeRef> collectArrayTypesFromAst(const AstFile& ast, const std::string& pkg) {
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

static std::string cDestroyPrefix(const TypeRef& type, const std::string& pkg) {
    if (type.isArray()) {
        return cArrayTypeName(type, pkg);
    }
    if (type.isCustom()) {
        if (!type.package_name.empty()) {
            return type.package_name + "_" + type.custom_name;
        }
        return pkg + "_" + type.custom_name;
    }
    return std::string();
}

static bool arrayElementNeedsLengths(const TypeRef& type) {
    return type.primitive == TYPE_STRING || type.primitive == TYPE_BYTES;
}

static bool isCompositePointerType(const TypeRef& type) {
    return type.isCustom() || type.isArray();
}

static void emitValueInit(std::ostream& os, const TypeRef& type,
                          const std::string& expr, const std::string& pkg,
                          const std::string& indent) {
    if (type.isCustom() || type.isArray()) {
        os << indent << cDestroyPrefix(type, pkg) << "_init(&" << expr << ");\n";
    }
}

static void emitValueDestroy(std::ostream& os, const TypeRef& type,
                             const std::string& expr, const std::string& pkg,
                             const std::string& indent) {
    if (type.primitive == TYPE_STRING || type.primitive == TYPE_BYTES) {
        os << indent << "if (" << expr << ") { free(" << expr << "); }\n";
    } else if (type.isCustom() || type.isArray()) {
        os << indent << cDestroyPrefix(type, pkg) << "_destroy(&" << expr << ");\n";
    }
}

static void emitValueSerialize(std::ostream& os, const TypeRef& type,
                               const std::string& expr, const std::string& len_expr,
                               const std::string& buf, const std::string& pkg,
                               const std::string& indent) {
    if (type.isArray()) {
        os << indent << cArrayTypeName(type, pkg) << "_serialize(&" << expr << ", " << buf << ");\n";
        return;
    }
    if (type.isCustom()) {
        os << indent << cTypePrefix(type, pkg) << "_serialize(&" << expr << ", " << buf << ");\n";
        return;
    }
    if (type.primitive == TYPE_STRING) {
        os << indent << "omni_buffer_write_string(" << buf << ", " << expr << ", " << len_expr << ");\n";
        return;
    }
    if (type.primitive == TYPE_BYTES) {
        os << indent << "omni_buffer_write_bytes(" << buf << ", " << expr << ", " << len_expr << ");\n";
        return;
    }
    emitPrimitiveWrite(os, type.primitive, expr, buf, indent);
}

static void emitValueDeserialize(std::ostream& os, const TypeRef& type,
                                 const std::string& expr, const std::string& len_expr,
                                 const std::string& buf, const std::string& pkg,
                                 const std::string& indent, const std::string& fail_action) {
    if (type.isArray()) {
        os << indent << "if (!" << cArrayTypeName(type, pkg) << "_deserialize(&" << expr << ", " << buf << ")) { " << fail_action << "; }\n";
        return;
    }
    if (type.isCustom()) {
        os << indent << "if (!" << cTypePrefix(type, pkg) << "_deserialize(&" << expr << ", " << buf << ")) { " << fail_action << "; }\n";
        return;
    }
    if (type.primitive == TYPE_STRING) {
        os << indent << expr << " = omni_buffer_read_string(" << buf << ", &" << len_expr << ");\n";
        os << indent << "if (!omni_buffer_read_ok(" << buf << ")) { " << fail_action << "; }\n";
        return;
    }
    if (type.primitive == TYPE_BYTES) {
        os << indent << expr << " = omni_buffer_read_bytes(" << buf << ", &" << len_expr << ");\n";
        os << indent << "if (!omni_buffer_read_ok(" << buf << ")) { " << fail_action << "; }\n";
        return;
    }
    emitPrimitiveRead(os, type.primitive, expr, buf, indent);
    os << indent << "if (!omni_buffer_read_ok(" << buf << ")) { " << fail_action << "; }\n";
}

static void emitArrayTypeDeclaration(std::ostream& os, const TypeRef& type,
                                     const std::string& pkg) {
    std::string type_name = cArrayTypeName(type, pkg);
    const TypeRef& element = *type.element_type;
    std::string element_decl_type = cDeclaredTypeName(element, pkg);
    os << "typedef struct " << type_name << " {\n";
    os << "    " << element_decl_type << "* data;\n";
    if (arrayElementNeedsLengths(element)) {
        os << "    uint32_t* lens;\n";
    }
    os << "    uint32_t count;\n";
    os << "} " << type_name << ";\n\n";
    os << "void " << type_name << "_init(" << type_name << "* self);\n";
    os << "void " << type_name << "_destroy(" << type_name << "* self);\n";
    os << "void " << type_name << "_serialize(const " << type_name << "* self, omni_buffer_t* buf);\n";
    os << "int " << type_name << "_deserialize(" << type_name << "* self, omni_buffer_t* buf);\n\n";
}

static void emitArrayTypeDefinitions(std::ostream& os, const TypeRef& type,
                                     const std::string& pkg) {
    std::string type_name = cArrayTypeName(type, pkg);
    const TypeRef& element = *type.element_type;
    std::string element_type_name = cTypeName(element, pkg);

    os << "void " << type_name << "_init(" << type_name << "* self) {\n";
    os << "    memset(self, 0, sizeof(*self));\n";
    os << "}\n\n";

    os << "void " << type_name << "_destroy(" << type_name << "* self) {\n";
    os << "    uint32_t i = 0;\n";
    if (element.primitive == TYPE_STRING || element.primitive == TYPE_BYTES) {
        os << "    if (self->data) {\n";
        os << "        for (i = 0; i < self->count; ++i) {\n";
        os << "            if (self->data[i]) { free(self->data[i]); }\n";
        os << "        }\n";
        os << "        free(self->data);\n";
        os << "    }\n";
        os << "    if (self->lens) { free(self->lens); }\n";
    } else if (element.isCustom() || element.isArray()) {
        os << "    if (self->data) {\n";
        os << "        for (i = 0; i < self->count; ++i) {\n";
        emitValueDestroy(os, element, "self->data[i]", pkg, "            ");
        os << "        }\n";
        os << "        free(self->data);\n";
        os << "    }\n";
    } else {
        os << "    if (self->data) { free(self->data); }\n";
    }
    os << "    self->data = NULL;\n";
    if (arrayElementNeedsLengths(element)) {
        os << "    self->lens = NULL;\n";
    }
    os << "    self->count = 0;\n";
    os << "}\n\n";

    os << "void " << type_name << "_serialize(const " << type_name << "* self, omni_buffer_t* buf) {\n";
    os << "    uint32_t i = 0;\n";
    os << "    omni_buffer_write_uint32(buf, self->count);\n";
    os << "    for (i = 0; i < self->count; ++i) {\n";
    std::string len_expr = arrayElementNeedsLengths(element) ? "self->lens[i]" : "0";
    emitValueSerialize(os, element, "self->data[i]", len_expr, "buf", pkg, "        ");
    os << "    }\n";
    os << "}\n\n";

    os << "int " << type_name << "_deserialize(" << type_name << "* self, omni_buffer_t* buf) {\n";
    os << "    uint32_t i = 0;\n";
    os << "    if (!self || !buf) { return 0; }\n";
    os << "    omni_buffer_clear_error(buf);\n";
    os << "    self->count = omni_buffer_read_uint32(buf);\n";
    os << "    if (!omni_buffer_read_ok(buf)) { goto fail; }\n";
    os << "    if (self->count == 0) {\n";
    os << "        self->data = NULL;\n";
    if (arrayElementNeedsLengths(element)) {
        os << "        self->lens = NULL;\n";
    }
    os << "        return 1;\n";
    os << "    }\n";
    os << "    self->data = (" << element_type_name << "*)malloc(sizeof(" << element_type_name << ") * self->count);\n";
    os << "    if (!self->data) { self->count = 0; return 0; }\n";
    if (arrayElementNeedsLengths(element)) {
        os << "    self->lens = (uint32_t*)malloc(sizeof(uint32_t) * self->count);\n";
        os << "    if (!self->lens) { free(self->data); self->data = NULL; self->count = 0; return 0; }\n";
    }
    os << "    for (i = 0; i < self->count; ++i) {\n";
    if (element.isCustom() || element.isArray()) {
        emitValueInit(os, element, "self->data[i]", pkg, "        ");
    }
    std::string read_len_expr = arrayElementNeedsLengths(element) ? "self->lens[i]" : "0";
    emitValueDeserialize(os, element, "self->data[i]", read_len_expr, "buf", pkg, "        ", "goto fail");
    os << "    }\n";
    os << "    return 1;\n";
    os << "fail:\n";
    os << "    " << type_name << "_destroy(self);\n";
    os << "    " << type_name << "_init(self);\n";
    os << "    return 0;\n";
    os << "}\n\n";
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

static std::string cDeclaredTypeName(const TypeRef& type, const std::string& pkg) {
    if (type.isCustom() && type.package_name.empty()) {
        return "struct " + pkg + "_" + type.custom_name;
    }
    return cTypeName(type, pkg);
}

static const char* primitiveReadFunc(PrimitiveType p) {
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

static const char* primitiveWriteFunc(PrimitiveType p) {
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

static void emitPrimitiveRead(std::ostream& os, PrimitiveType p,
                              const std::string& var, const std::string& buf,
                              const std::string& indent) {
    const char* fn = primitiveReadFunc(p);
    if (fn) {
        os << indent << var << " = " << fn << "(" << buf << ");\n";
    } else {
        os << indent << var << " = 0;\n";
    }
}

static void emitPrimitiveWrite(std::ostream& os, PrimitiveType p,
                               const std::string& var, const std::string& buf,
                               const std::string& indent) {
    const char* fn = primitiveWriteFunc(p);
    if (fn) {
        os << indent << fn << "(" << buf << ", " << var << ");\n";
    }
}

static bool isCStringLike(const TypeRef& type) {
    return type.primitive == TYPE_STRING || type.primitive == TYPE_BYTES;
}

static std::string idlTypeName(const TypeRef& type) {
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
    case TYPE_STRING:  return "std::string";
    case TYPE_BYTES:   return "std::vector<uint8_t>";
    case TYPE_VOID:    return "void";
    case TYPE_CUSTOM:
        if (!type.package_name.empty()) {
            return type.package_name + "::" + type.custom_name;
        }
        return type.custom_name;
    case TYPE_ARRAY:
        if (type.element_type) {
            return "std::vector<" + idlTypeName(*type.element_type) + ">";
        }
        return "std::vector<void*>";
    default:
        return "void";
    }
}

// 获取自定义类型的 C 函数名前缀（跨包时用 package_name，本包时用 pkg）
static std::string cTypePrefix(const TypeRef& type, const std::string& pkg) {
    if (!type.package_name.empty()) {
        return type.package_name + "_" + type.custom_name;
    }
    return pkg + "_" + type.custom_name;
}

std::string CCodeGen::toSnakeCase(const std::string& name) {
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

bool CCodeGen::generate(const AstFile& ast, const std::string& output_dir,
                        const std::string& filename) {
    pkg_ = ast.package_name;
    has_error_ = false;

    if (!validateAst(ast)) {
        return false;
    }

    std::string header_path = output_dir + "/" + filename + "_c.h";
    std::string source_path = output_dir + "/" + filename + ".c";

    std::ofstream hdr(header_path.c_str());
    std::ofstream src(source_path.c_str());
    if (!hdr.is_open() || !src.is_open()) return false;

    generateHeader(ast, hdr, filename);
    generateSource(ast, src, filename);
    return true;
}

void CCodeGen::reportError(const std::string& message) {
    if (!has_error_) {
        std::fprintf(stderr, "C codegen error: %s\n", message.c_str());
        has_error_ = true;
    }
}

bool CCodeGen::validateTypeSupported(const TypeRef& type, const std::string& context,
                                     bool allow_void) {
    if (type.isCustom() || isCStringLike(type)) {
        return true;
    }

    if (type.isVoid()) {
        if (allow_void) {
            return true;
        }
        reportError(context + " uses unsupported C codegen type '" + idlTypeName(type) + "'");
        return false;
    }

    if (type.isArray()) {
        if (!type.element_type) {
            reportError(context + " uses unsupported C codegen type '" + idlTypeName(type) + "'");
            return false;
        }
        return validateTypeSupported(*type.element_type, context + " element type", false);
    }

    if (primitiveReadFunc(type.primitive) != NULL &&
        primitiveWriteFunc(type.primitive) != NULL) {
        return true;
    }

    reportError(context + " uses unsupported C codegen type '" + idlTypeName(type) + "'");
    return false;
}

bool CCodeGen::validateAst(const AstFile& ast) {
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        const StructDef& s = ast.structs[i];
        for (size_t j = 0; j < s.fields.size(); ++j) {
            const FieldDef& f = s.fields[j];
            if (!validateTypeSupported(f.type,
                                       "struct '" + s.name + "' field '" + f.name + "'",
                                       false)) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < ast.topics.size(); ++i) {
        const TopicDef& t = ast.topics[i];
        for (size_t j = 0; j < t.fields.size(); ++j) {
            const FieldDef& f = t.fields[j];
            if (!validateTypeSupported(f.type,
                                       "topic '" + t.name + "' field '" + f.name + "'",
                                       false)) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < ast.services.size(); ++i) {
        const ServiceDef& svc = ast.services[i];
        for (size_t j = 0; j < svc.methods.size(); ++j) {
            const MethodDef& m = svc.methods[j];
            if (!validateTypeSupported(m.return_type,
                                       "service '" + svc.name + "' method '" + m.name + "' return type",
                                       true)) {
                return false;
            }
            if (m.has_param &&
                !validateTypeSupported(m.param.type,
                                       "service '" + svc.name + "' method '" + m.name + "' parameter '" + m.param.name + "'",
                                       false)) {
                return false;
            }
        }
    }

    return true;
}

void CCodeGen::generateHeader(const AstFile& ast, std::ostream& os, const std::string& filename) {
    std::vector<TypeRef> array_types = collectArrayTypesFromAst(ast, pkg_);
    std::string guard = filename;
    for (size_t i = 0; i < guard.size(); ++i) {
        if (guard[i] == '.') guard[i] = '_';
        guard[i] = toupper(guard[i]);
    }
    guard += "_C_H";

    emitGeneratedFileBanner(os,
                            filename + "_c.h",
                            filename,
                            "Auto-generated OmniBinder C declarations",
                            "Generated from OmniBinder IDL for C declarations and runtime binding helpers.");

    os << "#ifndef " << guard << "\n#define " << guard << "\n\n";
    os << "#include <omnibinder/omnibinder_c.h>\n";
    os << "#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n\n";
    
    // 生成被导入文件的 #include
    for (size_t i = 0; i < ast.imports.size(); ++i) {
        std::string imp = ast.imports[i];
        size_t slash = imp.find_last_of("/\\");
        if (slash != std::string::npos) imp = imp.substr(slash + 1);
        os << "#include \"" << imp << "_c.h\"\n";
    }
    if (!ast.imports.empty()) os << "\n";
    
    os << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

    for (size_t i = 0; i < ast.structs.size(); ++i) {
        os << "struct " << pkg_ << "_" << ast.structs[i].name << ";\n";
    }
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        os << "struct " << pkg_ << "_" << ast.topics[i].name << ";\n";
    }
    if (!ast.structs.empty() || !ast.topics.empty()) {
        os << "\n";
    }

    for (size_t i = 0; i < array_types.size(); ++i) {
        emitArrayTypeDeclaration(os, array_types[i], pkg_);
    }

    // Structs
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        genStruct(ast.structs[i], os);
    }

    // Topics
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        genTopic(ast.topics[i], os);
    }

    // Services
    for (size_t i = 0; i < ast.services.size(); ++i) {
        genServiceStubHeader(ast.services[i], ast, os);
        genServiceProxyHeader(ast.services[i], ast, os);
    }

    os << "#ifdef __cplusplus\n}\n#endif\n\n";
    os << "#endif /* " << guard << " */\n";
}

void CCodeGen::genStruct(const StructDef& s, std::ostream& os) {
    std::string tname = pkg_ + "_" + s.name;

    os << "typedef struct " << tname << " {\n";
    for (size_t j = 0; j < s.fields.size(); ++j) {
        const FieldDef& f = s.fields[j];
        os << "    " << cDeclaredTypeName(f.type, pkg_) << " " << f.name << ";\n";
        if (f.type.primitive == TYPE_STRING) {
            os << "    uint32_t " << f.name << "_len;\n";
        } else if (f.type.primitive == TYPE_BYTES) {
            os << "    uint32_t " << f.name << "_len;\n";
        }
    }
    os << "} " << tname << ";\n\n";

    os << "void " << tname << "_init(" << tname << "* self);\n";
    os << "void " << tname << "_destroy(" << tname << "* self);\n";
    os << "void " << tname << "_serialize(const " << tname << "* self, omni_buffer_t* buf);\n";
    os << "int " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf);\n\n";
}

void CCodeGen::genTopic(const TopicDef& t, std::ostream& os) {
    std::string tname = pkg_ + "_" + t.name;
    uint32_t topic_id = fnv1a_hash(pkg_ + "." + t.name);

    os << "typedef struct " << tname << " {\n";
    for (size_t j = 0; j < t.fields.size(); ++j) {
        const FieldDef& f = t.fields[j];
        os << "    " << cDeclaredTypeName(f.type, pkg_) << " " << f.name << ";\n";
        if (f.type.primitive == TYPE_STRING) {
            os << "    uint32_t " << f.name << "_len;\n";
        } else if (f.type.primitive == TYPE_BYTES) {
            os << "    uint32_t " << f.name << "_len;\n";
        }
    }
    os << "} " << tname << ";\n\n";

    os << "#define " << pkg_ << "_" << t.name << "_TOPIC_ID 0x"
       << std::hex << topic_id << std::dec << "u\n\n";

    os << "void " << tname << "_init(" << tname << "* self);\n";
    os << "void " << tname << "_destroy(" << tname << "* self);\n";
    os << "void " << tname << "_serialize(const " << tname << "* self, omni_buffer_t* buf);\n";
    os << "int " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf);\n\n";
}

void CCodeGen::genServiceStubHeader(const ServiceDef& svc, const AstFile& /*ast*/, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;
    uint32_t iface_id = fnv1a_hash(pkg_ + "." + svc.name);

    os << "/* ---- " << svc.name << " Stub (Server Side) ---- */\n\n";

    os << "#define " << prefix << "_INTERFACE_ID 0x"
       << std::hex << iface_id << std::dec << "u\n\n";

    // Method ID defines
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        uint32_t mid = fnv1a_hash(svc.methods[i].name);
        std::string upper_name = toSnakeCase(svc.methods[i].name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);
        os << "#define " << prefix << "_METHOD_" << upper_name
           << " 0x" << std::hex << mid << std::dec << "u\n";
    }
    os << "\n";

    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string handler_typedef = prefix + "_" + toSnakeCase(m.name) + "_handler_t";
        std::string handler_decl = prefix + "_impl_" + toSnakeCase(m.name);
        os << "typedef void (*" << handler_typedef << ")(";

        bool has_args = false;
        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                os << "const " << cDeclaredTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else if (isCStringLike(m.param.type)) {
                os << "const " << cDeclaredTypeName(m.param.type, pkg_) << " " << m.param.name << ", uint32_t " << m.param.name << "_len";
            } else {
                os << cDeclaredTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
            has_args = true;
        }
        if (!m.return_type.isVoid()) {
            if (has_args) os << ", ";
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                os << cDeclaredTypeName(m.return_type, pkg_) << "* result";
            } else if (isCStringLike(m.return_type)) {
                os << cDeclaredTypeName(m.return_type, pkg_) << "* result, uint32_t* result_len";
            } else {
                os << cDeclaredTypeName(m.return_type, pkg_) << "* result";
            }
            has_args = true;
        }
        if (has_args) os << ", ";
        os << "void* user_data);\n";

        os << "void " << handler_decl << "(";
        has_args = false;
        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                os << "const " << cDeclaredTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else if (isCStringLike(m.param.type)) {
                os << "const " << cDeclaredTypeName(m.param.type, pkg_) << " " << m.param.name << ", uint32_t " << m.param.name << "_len";
            } else {
                os << cDeclaredTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
            has_args = true;
        }
        if (!m.return_type.isVoid()) {
            if (has_args) os << ", ";
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                os << cDeclaredTypeName(m.return_type, pkg_) << "* result";
            } else if (isCStringLike(m.return_type)) {
                os << cDeclaredTypeName(m.return_type, pkg_) << "* result, uint32_t* result_len";
            } else {
                os << cDeclaredTypeName(m.return_type, pkg_) << "* result";
            }
            has_args = true;
        }
        if (has_args) os << ", ";
        os << "void* user_data);\n\n";
    }

    os << "typedef struct " << prefix << "_callbacks {\n";
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string handler_typedef = prefix + "_" + toSnakeCase(m.name) + "_handler_t";
        if (m.has_param && isCStringLike(m.param.type)) {
            os << "    /* " << m.name << ": for string/bytes parameters, pass pointer + length. */\n";
        }
        if (!m.return_type.isVoid() && isCStringLike(m.return_type)) {
            os << "    /* " << m.name << ": allocate return buffer on heap, set *result_len, and the stub frees *result after reply serialization. */\n";
        }
        os << "    " << handler_typedef << " " << m.name << ";\n";
    }
    os << "    void* user_data;\n";
    os << "} " << prefix << "_callbacks;\n\n";

    os << "omni_service_t* " << prefix << "_stub_create_from_callbacks(const " << prefix << "_callbacks* cbs);\n";
    os << "static inline omni_service_t* " << prefix << "_stub_create(void* user_data) {\n";
    os << "    " << prefix << "_callbacks cbs;\n";
    os << "    memset(&cbs, 0, sizeof(cbs));\n";
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        os << "    cbs." << m.name << " = " << prefix << "_impl_" << toSnakeCase(m.name) << ";\n";
    }
    os << "    cbs.user_data = user_data;\n";
    os << "    return " << prefix << "_stub_create_from_callbacks(&cbs);\n";
    os << "}\n\n";
    os << "void " << prefix << "_stub_destroy(omni_service_t* svc);\n\n";

    // Broadcast helpers
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_broadcast_" + toSnakeCase(topic);
        os << "void " << fn_name << "(omni_runtime_t* runtime, const " << topic_type << "* msg);\n";
    }
    if (!svc.publishes.empty()) os << "\n";
}

void CCodeGen::genServiceProxyHeader(const ServiceDef& svc, const AstFile& /*ast*/, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;

    os << "/* ---- " << svc.name << " Proxy (Client Side) ---- */\n\n";

    os << "typedef struct " << prefix << "_proxy {\n";
    os << "    omni_runtime_t* runtime;\n";
    os << "    int connected;\n";
    os << "} " << prefix << "_proxy;\n\n";

    os << "void " << prefix << "_proxy_init(" << prefix << "_proxy* p, omni_runtime_t* runtime);\n";
    os << "int  " << prefix << "_proxy_connect(" << prefix << "_proxy* p);\n";
    os << "void " << prefix << "_proxy_disconnect(" << prefix << "_proxy* p);\n\n";

    // Method proxies
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string fn_name = prefix + "_proxy_" + toSnakeCase(m.name);

        if (m.return_type.isVoid()) {
            os << "void " << fn_name << "(" << prefix << "_proxy* p";
        } else {
            os << "int  " << fn_name << "(" << prefix << "_proxy* p";
        }

        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                os << ", const " << cDeclaredTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else if (isCStringLike(m.param.type)) {
                os << ", const " << cDeclaredTypeName(m.param.type, pkg_) << " " << m.param.name << ", uint32_t " << m.param.name << "_len";
            } else {
                os << ", " << cDeclaredTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
        }
        if (!m.return_type.isVoid()) {
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                os << ", " << cDeclaredTypeName(m.return_type, pkg_) << "* result";
            } else if (isCStringLike(m.return_type)) {
                os << ", " << cDeclaredTypeName(m.return_type, pkg_) << "* result, uint32_t* result_len";
            } else {
                os << ", " << cDeclaredTypeName(m.return_type, pkg_) << "* result";
            }
        }
        os << ");\n";
    }
    os << "\n";

    // Subscribe helpers
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_proxy_subscribe_" + toSnakeCase(topic);
        os << "void " << fn_name << "(" << prefix << "_proxy* p,\n";
        os << "    void (*callback)(const " << topic_type << "* msg, void* user_data), void* user_data);\n";
    }

    // Death notification
    os << "void " << prefix << "_proxy_on_service_died(" << prefix << "_proxy* p,\n";
    os << "    void (*callback)(void* user_data), void* user_data);\n\n";
}


/* ============================================================
 * Source file generation
 * ============================================================ */

void CCodeGen::generateSource(const AstFile& ast, std::ostream& os, const std::string& filename) {
    std::vector<TypeRef> array_types = collectArrayTypesFromAst(ast, pkg_);
    emitGeneratedFileBanner(os,
                            filename + ".c",
                            filename,
                            "Auto-generated OmniBinder C definitions",
                            "Generated from OmniBinder IDL for C serialization, proxy, and stub implementation glue.");
    os << "#include \"" << filename << "_c.h\"\n";
    os << "#include <string.h>\n#include <stdlib.h>\n\n";

    for (size_t i = 0; i < array_types.size(); ++i) {
        emitArrayTypeDefinitions(os, array_types[i], pkg_);
    }

    // Struct init/destroy/serialize/deserialize
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        const StructDef& s = ast.structs[i];
        std::string tname = pkg_ + "_" + s.name;

        // init
        os << "void " << tname << "_init(" << tname << "* self) {\n";
        os << "    memset(self, 0, sizeof(*self));\n";
        os << "}\n\n";

        // destroy
        os << "void " << tname << "_destroy(" << tname << "* self) {\n";
        for (size_t j = 0; j < s.fields.size(); ++j) {
            const FieldDef& f = s.fields[j];
            if (f.type.primitive == TYPE_STRING || f.type.primitive == TYPE_BYTES) {
                os << "    if (self->" << f.name << ") { free(self->" << f.name << "); self->" << f.name << " = NULL; }\n";
            } else if (f.type.isArray()) {
                os << "    " << cArrayTypeName(f.type, pkg_) << "_destroy(&self->" << f.name << ");\n";
            } else if (f.type.primitive == TYPE_CUSTOM) {
                std::string cpkg = f.type.package_name.empty() ? pkg_ : f.type.package_name;
                os << "    " << cpkg << "_" << f.type.custom_name << "_destroy(&self->" << f.name << ");\n";
            }
        }
        os << "}\n\n";

        genStructSerialize(s, os);
        genStructDeserialize(s, os);
    }

    // Topic init/destroy/serialize/deserialize
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        const TopicDef& t = ast.topics[i];
        std::string tname = pkg_ + "_" + t.name;

        // init
        os << "void " << tname << "_init(" << tname << "* self) {\n";
        os << "    memset(self, 0, sizeof(*self));\n";
        os << "}\n\n";

        // destroy
        os << "void " << tname << "_destroy(" << tname << "* self) {\n";
        for (size_t j = 0; j < t.fields.size(); ++j) {
            const FieldDef& f = t.fields[j];
            if (f.type.primitive == TYPE_STRING || f.type.primitive == TYPE_BYTES) {
                os << "    if (self->" << f.name << ") { free(self->" << f.name << "); self->" << f.name << " = NULL; }\n";
            } else if (f.type.isArray()) {
                os << "    " << cArrayTypeName(f.type, pkg_) << "_destroy(&self->" << f.name << ");\n";
            } else if (f.type.primitive == TYPE_CUSTOM) {
                std::string cpkg = f.type.package_name.empty() ? pkg_ : f.type.package_name;
                os << "    " << cpkg << "_" << f.type.custom_name << "_destroy(&self->" << f.name << ");\n";
            }
        }
        os << "}\n\n";

        genTopicSerialize(t, os);
        genTopicDeserialize(t, os);
    }

    // Service Stub and Proxy implementations
    for (size_t i = 0; i < ast.services.size(); ++i) {
        genServiceStubSource(ast.services[i], ast, os);
        genServiceProxySource(ast.services[i], ast, os);
    }
}

void CCodeGen::genFieldSerialize(const FieldDef& f, const std::string& obj, std::ostream& os) {
    std::string name = obj + f.name;
    std::string len_name = isCStringLike(f.type) ? name + "_len" : "0";
    emitValueSerialize(os, f.type, name, len_name, "buf", pkg_, "    ");
}

void CCodeGen::genFieldDeserialize(const FieldDef& f, const std::string& obj, std::ostream& os) {
    std::string name = obj + f.name;
    std::string len_name = isCStringLike(f.type) ? name + "_len" : "0";
    emitValueDeserialize(os, f.type, name, len_name, "buf", pkg_, "    ", "goto fail");
}

void CCodeGen::genStructSerialize(const StructDef& s, std::ostream& os) {
    std::string tname = pkg_ + "_" + s.name;
    os << "void " << tname << "_serialize(const " << tname << "* self, omni_buffer_t* buf) {\n";
    for (size_t i = 0; i < s.fields.size(); ++i) {
        genFieldSerialize(s.fields[i], "self->", os);
    }
    os << "}\n\n";
}

void CCodeGen::genStructDeserialize(const StructDef& s, std::ostream& os) {
    std::string tname = pkg_ + "_" + s.name;
    os << "int " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf) {\n";
    os << "    if (!self || !buf) { return 0; }\n";
    os << "    omni_buffer_clear_error(buf);\n";
    for (size_t i = 0; i < s.fields.size(); ++i) {
        genFieldDeserialize(s.fields[i], "self->", os);
    }
    os << "    return 1;\n";
    os << "fail:\n";
    os << "    " << tname << "_destroy(self);\n";
    os << "    " << tname << "_init(self);\n";
    os << "    return 0;\n";
    os << "}\n\n";
}

void CCodeGen::genTopicSerialize(const TopicDef& t, std::ostream& os) {
    std::string tname = pkg_ + "_" + t.name;
    os << "void " << tname << "_serialize(const " << tname << "* self, omni_buffer_t* buf) {\n";
    for (size_t i = 0; i < t.fields.size(); ++i) {
        genFieldSerialize(t.fields[i], "self->", os);
    }
    os << "}\n\n";
}

void CCodeGen::genTopicDeserialize(const TopicDef& t, std::ostream& os) {
    std::string tname = pkg_ + "_" + t.name;
    os << "int " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf) {\n";
    os << "    if (!self || !buf) { return 0; }\n";
    os << "    omni_buffer_clear_error(buf);\n";
    for (size_t i = 0; i < t.fields.size(); ++i) {
        genFieldDeserialize(t.fields[i], "self->", os);
    }
    os << "    return 1;\n";
    os << "fail:\n";
    os << "    " << tname << "_destroy(self);\n";
    os << "    " << tname << "_init(self);\n";
    os << "    return 0;\n";
    os << "}\n\n";
}


void CCodeGen::genServiceStubSource(const ServiceDef& svc, const AstFile& /*ast*/, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;

    // Internal struct to hold callbacks
    os << "/* " << svc.name << " Stub internal */\n";
    os << "typedef struct " << prefix << "_stub_data {\n";
    os << "    " << prefix << "_callbacks cbs;\n";
    os << "} " << prefix << "_stub_data;\n\n";

    // onInvoke dispatch function
    os << "static void " << prefix << "_on_invoke(uint32_t method_id,\n";
    os << "    const omni_buffer_t* request, omni_buffer_t* response, void* user_data)\n";
    os << "{\n";
    os << "    " << prefix << "_stub_data* data = (" << prefix << "_stub_data*)user_data;\n";
    os << "    omni_buffer_t* req = omni_buffer_create_from(omni_buffer_data(request), omni_buffer_size(request));\n\n";

    // Switch on method_id
    bool first = true;
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string upper_name = toSnakeCase(m.name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);

        if (first) {
            os << "    if (method_id == " << prefix << "_METHOD_" << upper_name << ") {\n";
            first = false;
        } else {
            os << "    } else if (method_id == " << prefix << "_METHOD_" << upper_name << ") {\n";
        }

        // Deserialize parameter
        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                std::string ptype = cTypeName(m.param.type, pkg_);
                std::string ppfx = cDestroyPrefix(m.param.type, pkg_);
                os << "        " << ptype << " " << m.param.name << ";\n";
                os << "        " << ppfx << "_init(&" << m.param.name << ");\n";
                os << "        if (!" << ppfx << "_deserialize(&" << m.param.name << ", req)) { " << ppfx << "_destroy(&" << m.param.name << "); omni_buffer_mark_error(response, -501); omni_buffer_destroy(req); return; }\n";
            } else if (isCStringLike(m.param.type)) {
                std::string ptype = cTypeName(m.param.type, pkg_);
                os << "        " << ptype << " " << m.param.name << " = NULL;\n";
                os << "        uint32_t " << m.param.name << "_len = 0;\n";
                if (m.param.type.primitive == TYPE_STRING) {
                    os << "        " << m.param.name << " = omni_buffer_read_string(req, &" << m.param.name << "_len);\n";
                } else {
                    os << "        " << m.param.name << " = omni_buffer_read_bytes(req, &" << m.param.name << "_len);\n";
                }
                os << "        if (!omni_buffer_read_ok(req)) { if (" << m.param.name << ") free(" << m.param.name << "); omni_buffer_mark_error(response, -501); omni_buffer_destroy(req); return; }\n";
            } else {
                std::string ptype = cTypeName(m.param.type, pkg_);
                os << "        " << ptype << " " << m.param.name << " = ";
                const char* rfn = primitiveReadFunc(m.param.type.primitive);
                if (rfn) {
                    os << rfn << "(req);\n";
                } else {
                    os << "0;\n";
                }
                os << "        if (!omni_buffer_read_ok(req)) { omni_buffer_mark_error(response, -501); omni_buffer_destroy(req); return; }\n";
            }
        }

        // Prepare result variable
        if (!m.return_type.isVoid()) {
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                std::string rtype = cTypeName(m.return_type, pkg_);
                std::string rpfx = cDestroyPrefix(m.return_type, pkg_);
                os << "        " << rtype << " result;\n";
                os << "        " << rpfx << "_init(&result);\n";
            } else if (isCStringLike(m.return_type)) {
                os << "        " << cTypeName(m.return_type, pkg_) << " result = NULL;\n";
                os << "        uint32_t result_len = 0;\n";
            } else {
                os << "        " << cTypeName(m.return_type, pkg_) << " result = 0;\n";
            }
        }

        // Call the callback
        os << "        if (data->cbs." << m.name << ") {\n";
        os << "            data->cbs." << m.name << "(";
        bool has_args = false;
        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                os << "&" << m.param.name;
            } else if (isCStringLike(m.param.type)) {
                os << m.param.name << ", " << m.param.name << "_len";
            } else {
                os << m.param.name;
            }
            has_args = true;
        }
        if (!m.return_type.isVoid()) {
            if (has_args) os << ", ";
            if (isCStringLike(m.return_type)) {
                os << "&result, &result_len";
            } else {
                os << "&result";
            }
            has_args = true;
        }
        if (has_args) os << ", ";
        os << "data->cbs.user_data);\n";
        os << "        }\n";

        // Serialize result
        if (!m.return_type.isVoid()) {
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                std::string rpfx2 = cDestroyPrefix(m.return_type, pkg_);
                os << "        " << rpfx2 << "_serialize(&result, response);\n";
                os << "        " << rpfx2 << "_destroy(&result);\n";
            } else if (isCStringLike(m.return_type)) {
                if (m.return_type.primitive == TYPE_STRING) {
                    os << "        omni_buffer_write_string(response, result, result_len);\n";
                } else {
                    os << "        omni_buffer_write_bytes(response, result, result_len);\n";
                }
                os << "        if (result) free(result);\n";
            } else {
                emitPrimitiveWrite(os, m.return_type.primitive, "result", "response", "        ");
            }
        }

        // Cleanup param
        if (m.has_param && isCompositePointerType(m.param.type)) {
            std::string ppfx2 = cDestroyPrefix(m.param.type, pkg_);
            os << "        " << ppfx2 << "_destroy(&" << m.param.name << ");\n";
        } else if (m.has_param && isCStringLike(m.param.type)) {
            os << "        if (" << m.param.name << ") free(" << m.param.name << ");\n";
        }
    }
    if (!svc.methods.empty()) {
        os << "    }\n";
    }
    os << "    omni_buffer_destroy(req);\n";
    os << "}\n\n";

    os << "omni_service_t* " << prefix << "_stub_create_from_callbacks(const " << prefix << "_callbacks* cbs) {\n";
    os << "    " << prefix << "_stub_data* data = (" << prefix << "_stub_data*)malloc(sizeof(" << prefix << "_stub_data));\n";
    os << "    if (!data) return NULL;\n";
    os << "    data->cbs = *cbs;\n";
    os << "    omni_service_t* svc = omni_service_create(\"" << svc.name << "\",\n";
    os << "        " << prefix << "_INTERFACE_ID, " << prefix << "_on_invoke, data);\n";

    // Add methods
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        std::string upper_name = toSnakeCase(svc.methods[i].name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);
        std::string param_type_str = svc.methods[i].has_param ? idlTypeName(svc.methods[i].param.type) : "";
        std::string return_type_str = idlTypeName(svc.methods[i].return_type);
        os << "    omni_service_add_method_ex(svc, " << prefix << "_METHOD_" << upper_name
           << ", \"" << svc.methods[i].name << "\", \"" << param_type_str
           << "\", \"" << return_type_str << "\");\n";
    }
    os << "    return svc;\n";
    os << "}\n\n";

    // _stub_destroy
    os << "void " << prefix << "_stub_destroy(omni_service_t* svc) {\n";
    os << "    /* Note: stub_data is freed here; the invoke callback user_data points to it */\n";
    os << "    omni_service_destroy(svc);\n";
    os << "}\n\n";

    // Broadcast helpers
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_broadcast_" + toSnakeCase(topic);
        uint32_t tid = fnv1a_hash(topic);

        os << "void " << fn_name << "(omni_runtime_t* runtime, const " << topic_type << "* msg) {\n";
        os << "    omni_buffer_t* buf = omni_buffer_create();\n";
        os << "    " << topic_type << "_serialize(msg, buf);\n";
        os << "    omni_runtime_broadcast(runtime, 0x" << std::hex << tid << std::dec << "u, buf);\n";
        os << "    omni_buffer_destroy(buf);\n";
        os << "}\n\n";
    }
}


void CCodeGen::genServiceProxySource(const ServiceDef& svc, const AstFile& /*ast*/, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;
    uint32_t iface_id = fnv1a_hash(pkg_ + "." + svc.name);

    // proxy_init
    os << "void " << prefix << "_proxy_init(" << prefix << "_proxy* p, omni_runtime_t* runtime) {\n";
    os << "    p->runtime = runtime;\n";
    os << "    p->connected = 0;\n";
    os << "}\n\n";

    // proxy_connect
    os << "int " << prefix << "_proxy_connect(" << prefix << "_proxy* p) {\n";
    os << "    /* Verify service exists via a dummy invoke or lookup */\n";
    os << "    p->connected = 1;\n";
    os << "    return 0;\n";
    os << "}\n\n";

    // proxy_disconnect
    os << "void " << prefix << "_proxy_disconnect(" << prefix << "_proxy* p) {\n";
    os << "    p->connected = 0;\n";
    os << "}\n\n";

    // Method proxies
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        uint32_t mid = fnv1a_hash(m.name);
        std::string fn_name = prefix + "_proxy_" + toSnakeCase(m.name);

        if (m.return_type.isVoid()) {
            os << "void " << fn_name << "(" << prefix << "_proxy* p";
        } else {
            os << "int " << fn_name << "(" << prefix << "_proxy* p";
        }

        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                os << ", const " << cTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else if (isCStringLike(m.param.type)) {
                os << ", const " << cTypeName(m.param.type, pkg_) << " " << m.param.name << ", uint32_t " << m.param.name << "_len";
            } else {
                os << ", " << cTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
        }
        if (!m.return_type.isVoid()) {
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result";
            } else if (isCStringLike(m.return_type)) {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result, uint32_t* result_len";
            } else {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result";
            }
        }
        os << ") {\n";

        os << "    omni_buffer_t* req = omni_buffer_create();\n";
        os << "    omni_buffer_t* resp = omni_buffer_create();\n";

        // Serialize parameter
        if (m.has_param) {
            if (isCompositePointerType(m.param.type)) {
                std::string ppfx = cDestroyPrefix(m.param.type, pkg_);
                os << "    " << ppfx << "_serialize(" << m.param.name << ", req);\n";
            } else if (isCStringLike(m.param.type)) {
                if (m.param.type.primitive == TYPE_STRING) {
                    os << "    omni_buffer_write_string(req, " << m.param.name << ", " << m.param.name << "_len);\n";
                } else {
                    os << "    omni_buffer_write_bytes(req, " << m.param.name << ", " << m.param.name << "_len);\n";
                }
            } else {
                emitPrimitiveWrite(os, m.param.type.primitive, m.param.name, "req", "    ");
            }
        }

        // Invoke
        if (m.return_type.isVoid()) {
            os << "    omni_runtime_invoke_oneway(p->runtime, \"" << svc.name << "\",\n";
            os << "        0x" << std::hex << iface_id << std::dec << "u, 0x" << std::hex << mid << std::dec << "u, req);\n";
        } else {
            os << "    int ret = omni_runtime_invoke(p->runtime, \"" << svc.name << "\",\n";
            os << "        0x" << std::hex << iface_id << std::dec << "u, 0x" << std::hex << mid << std::dec << "u, req, resp, 0);\n";
        }

        // Deserialize result
        if (!m.return_type.isVoid()) {
            os << "    if (ret == 0 && result) {\n";
            if (m.return_type.isArray() || m.return_type.isCustom()) {
                std::string rpfx = cDestroyPrefix(m.return_type, pkg_);
                os << "        " << rpfx << "_init(result);\n";
                os << "        if (!" << rpfx << "_deserialize(result, resp)) { " << rpfx << "_destroy(result); ret = -501; }\n";
            } else if (isCStringLike(m.return_type)) {
                if (m.return_type.primitive == TYPE_STRING) {
                    os << "        *result = omni_buffer_read_string(resp, result_len);\n";
                } else {
                    os << "        *result = omni_buffer_read_bytes(resp, result_len);\n";
                }
                os << "        if (!omni_buffer_read_ok(resp)) { if (*result) free(*result); *result = NULL; if (result_len) *result_len = 0; ret = -501; }\n";
            } else {
                emitPrimitiveRead(os, m.return_type.primitive, "*result", "resp", "        ");
                os << "        if (!omni_buffer_read_ok(resp)) { ret = -501; }\n";
            }
            os << "    }\n";
        }

        os << "    omni_buffer_destroy(req);\n";
        os << "    omni_buffer_destroy(resp);\n";

        if (!m.return_type.isVoid()) {
            os << "    return ret;\n";
        }
        os << "}\n\n";
    }

    // Subscribe helpers
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_proxy_subscribe_" + toSnakeCase(topic);

        // We need a small wrapper struct to pass the typed callback through the generic topic callback
        os << "typedef struct " << prefix << "_" << toSnakeCase(topic) << "_sub_ctx {\n";
        os << "    void (*callback)(const " << topic_type << "* msg, void* user_data);\n";
        os << "    void* user_data;\n";
        os << "} " << prefix << "_" << toSnakeCase(topic) << "_sub_ctx;\n\n";

        os << "static void " << prefix << "_" << toSnakeCase(topic) << "_topic_cb(\n";
        os << "    uint32_t topic_id, const omni_buffer_t* data, void* user_data)\n";
        os << "{\n";
        os << "    (void)topic_id;\n";
        os << "    " << prefix << "_" << toSnakeCase(topic) << "_sub_ctx* ctx =\n";
        os << "        (" << prefix << "_" << toSnakeCase(topic) << "_sub_ctx*)user_data;\n";
        os << "    " << topic_type << " msg;\n";
        os << "    " << topic_type << "_init(&msg);\n";
        os << "    omni_buffer_t* buf = omni_buffer_create_from(omni_buffer_data(data), omni_buffer_size(data));\n";
        os << "    if (!" << topic_type << "_deserialize(&msg, buf)) {\n";
        os << "        omni_buffer_destroy(buf);\n";
        os << "        " << topic_type << "_destroy(&msg);\n";
        os << "        return;\n";
        os << "    }\n";
        os << "    omni_buffer_destroy(buf);\n";
        os << "    if (ctx->callback) ctx->callback(&msg, ctx->user_data);\n";
        os << "    " << topic_type << "_destroy(&msg);\n";
        os << "}\n\n";

        os << "void " << fn_name << "(" << prefix << "_proxy* p,\n";
        os << "    void (*callback)(const " << topic_type << "* msg, void* user_data), void* user_data)\n";
        os << "{\n";
        os << "    /* Note: ctx is intentionally leaked for simplicity; in production code\n";
        os << "       you would track and free it on unsubscribe */\n";
        os << "    " << prefix << "_" << toSnakeCase(topic) << "_sub_ctx* ctx =\n";
        os << "        (" << prefix << "_" << toSnakeCase(topic) << "_sub_ctx*)malloc(\n";
        os << "            sizeof(" << prefix << "_" << toSnakeCase(topic) << "_sub_ctx));\n";
        os << "    ctx->callback = callback;\n";
        os << "    ctx->user_data = user_data;\n";
        os << "    omni_runtime_subscribe_topic(p->runtime, \"" << topic << "\",\n";
        os << "        " << prefix << "_" << toSnakeCase(topic) << "_topic_cb, ctx);\n";
        os << "}\n\n";
    }

    // Death notification
    os << "typedef struct " << prefix << "_death_ctx {\n";
    os << "    void (*callback)(void* user_data);\n";
    os << "    void* user_data;\n";
    os << "} " << prefix << "_death_ctx;\n\n";

    os << "static void " << prefix << "_death_cb(const char* service_name, void* user_data) {\n";
    os << "    " << prefix << "_death_ctx* ctx = (" << prefix << "_death_ctx*)user_data;\n";
    os << "    (void)service_name;\n";
    os << "    if (ctx->callback) ctx->callback(ctx->user_data);\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_on_service_died(" << prefix << "_proxy* p,\n";
    os << "    void (*callback)(void* user_data), void* user_data)\n";
    os << "{\n";
    os << "    " << prefix << "_death_ctx* ctx = (" << prefix << "_death_ctx*)malloc(\n";
    os << "        sizeof(" << prefix << "_death_ctx));\n";
    os << "    ctx->callback = callback;\n";
    os << "    ctx->user_data = user_data;\n";
    os << "    omni_runtime_subscribe_death(p->runtime, \"" << svc.name << "\",\n";
    os << "        " << prefix << "_death_cb, ctx);\n";
    os << "}\n\n";
}

} // namespace omnic
