#include "codegen_c.h"
#include "codegen_c_internal.h"
#include <cctype>
#include <vector>

namespace omnic {

/* @brief 获取自定义类型的 C 函数名前缀
 * @param[in] type 类型引用
 * @param[in] pkg 包名
 * @return C 函数名前缀
 * @note 跨包时用 package_name，本包时用 pkg
 */
static std::string cTypePrefix(const TypeRef& type, const std::string& pkg) {
    if (!type.package_name.empty()) {
        return type.package_name + "_" + type.custom_name;
    }
    return pkg + "_" + type.custom_name;
}

static std::string cDestroyPrefix(const TypeRef& type, const std::string& pkg) {
    if (type.isArray()) {
        return cArrayTypeName(type, pkg);
    }
    if (type.isCustom()) {
        return cTypePrefix(type, pkg);
    }
    return std::string();
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
        os << indent << "if (" << expr << ") { omni_free(" << expr << "); }\n";
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

static void emitArrayTypeDefinitions(std::ostream& os, const TypeRef& type,
                                     const std::string& pkg) {
    std::string type_name = cArrayTypeName(type, pkg);
    const TypeRef& element = *type.element_type;
    std::string element_type_name = cTypeName(element, pkg);
    const size_t min_wire_size = minimumWireSize(element);

    os << "void " << type_name << "_init(" << type_name << "* self) {\n";
    os << "    memset(self, 0, sizeof(*self));\n";
    os << "}\n\n";

    os << "void " << type_name << "_destroy(" << type_name << "* self) {\n";
    if (element.primitive == TYPE_STRING || element.primitive == TYPE_BYTES) {
        os << "    uint32_t i = 0;\n";
        os << "    if (self->data) {\n";
        os << "        for (i = 0; i < self->count; ++i) {\n";
        os << "            if (self->data[i]) { omni_free(self->data[i]); }\n";
        os << "        }\n";
        os << "        omni_free(self->data);\n";
        os << "    }\n";
        os << "    if (self->lens) { omni_free(self->lens); }\n";
    } else if (element.isCustom() || element.isArray()) {
        os << "    uint32_t i = 0;\n";
        os << "    if (self->data) {\n";
        os << "        for (i = 0; i < self->count; ++i) {\n";
        emitValueDestroy(os, element, "self->data[i]", pkg, "            ");
        os << "        }\n";
        os << "        omni_free(self->data);\n";
        os << "    }\n";
    } else {
        os << "    if (self->data) { omni_free(self->data); }\n";
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
    os << "    " << type_name << "_destroy(self);\n";
    os << "    " << type_name << "_init(self);\n";
    os << "    omni_buffer_clear_error(buf);\n";
    os << "    self->count = omni_buffer_read_uint32(buf);\n";
    os << "    if (!omni_buffer_read_ok(buf)) { goto fail; }\n";
    os << "    if (self->count > OMNI_MAX_ARRAY_ELEMENTS) { omni_buffer_mark_error(buf, OMNI_ERR_DESERIALIZE); goto fail; }\n";
    if (min_wire_size != 0) {
        os << "    if ((size_t)self->count > omni_buffer_remaining(buf) / " << min_wire_size
           << "u) { omni_buffer_mark_error(buf, OMNI_ERR_DESERIALIZE); goto fail; }\n";
    } else {
        os << "    if (self->count > OMNI_MAX_ZERO_WIRE_ARRAY_ELEMENTS) { omni_buffer_mark_error(buf, OMNI_ERR_DESERIALIZE); goto fail; }\n";
    }
    os << "    if ((size_t)self->count > (size_t)OMNI_MAX_MESSAGE_SIZE / sizeof("
       << element_type_name << ")) { omni_buffer_mark_error(buf, OMNI_ERR_DESERIALIZE); goto fail; }\n";
    os << "    if (self->count == 0) {\n";
    os << "        self->data = NULL;\n";
    if (arrayElementNeedsLengths(element)) {
        os << "        self->lens = NULL;\n";
    }
    os << "        return 1;\n";
    os << "    }\n";
    os << "    self->data = (" << element_type_name << "*)omni_malloc(sizeof(" << element_type_name << ") * (size_t)self->count);\n";
    os << "    if (!self->data) { self->count = 0; return 0; }\n";
    os << "    memset(self->data, 0, sizeof(" << element_type_name << ") * (size_t)self->count);\n";
    if (arrayElementNeedsLengths(element)) {
        os << "    self->lens = (uint32_t*)omni_malloc(sizeof(uint32_t) * (size_t)self->count);\n";
        os << "    if (!self->lens) { omni_free(self->data); self->data = NULL; self->count = 0; return 0; }\n";
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

    // 结构体初始化/销毁/序列化/反序列化
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        const StructDef& s = ast.structs[i];
        std::string tname = pkg_ + "_" + s.name;

        // 初始化
        os << "void " << tname << "_init(" << tname << "* self) {\n";
        os << "    memset(self, 0, sizeof(*self));\n";
        os << "}\n\n";

        // 销毁
        os << "void " << tname << "_destroy(" << tname << "* self) {\n";
        bool has_destroy_logic = false;
        for (size_t j = 0; j < s.fields.size(); ++j) {
            const FieldDef& f = s.fields[j];
            if (f.type.primitive == TYPE_STRING || f.type.primitive == TYPE_BYTES) {
                has_destroy_logic = true;
                os << "    if (self->" << f.name << ") { omni_free(self->" << f.name << "); self->" << f.name << " = NULL; }\n";
            } else if (f.type.isArray()) {
                has_destroy_logic = true;
                os << "    " << cArrayTypeName(f.type, pkg_) << "_destroy(&self->" << f.name << ");\n";
            } else if (f.type.primitive == TYPE_CUSTOM) {
                has_destroy_logic = true;
                std::string cpkg = f.type.package_name.empty() ? pkg_ : f.type.package_name;
                os << "    " << cpkg << "_" << f.type.custom_name << "_destroy(&self->" << f.name << ");\n";
            }
        }
        if (!has_destroy_logic) {
            os << "    (void)self;\n";
        }
        os << "}\n\n";

        genStructSerialize(s, os);
        genStructDeserialize(s, os);
    }

    // topic 初始化/销毁/序列化/反序列化
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        const TopicDef& t = ast.topics[i];
        std::string tname = pkg_ + "_" + t.name;

        // 初始化
        os << "void " << tname << "_init(" << tname << "* self) {\n";
        os << "    memset(self, 0, sizeof(*self));\n";
        os << "}\n\n";

        // 销毁
        os << "void " << tname << "_destroy(" << tname << "* self) {\n";
        bool has_destroy_logic = false;
        for (size_t j = 0; j < t.fields.size(); ++j) {
            const FieldDef& f = t.fields[j];
            if (f.type.primitive == TYPE_STRING || f.type.primitive == TYPE_BYTES) {
                has_destroy_logic = true;
                os << "    if (self->" << f.name << ") { omni_free(self->" << f.name << "); self->" << f.name << " = NULL; }\n";
            } else if (f.type.isArray()) {
                has_destroy_logic = true;
                os << "    " << cArrayTypeName(f.type, pkg_) << "_destroy(&self->" << f.name << ");\n";
            } else if (f.type.primitive == TYPE_CUSTOM) {
                has_destroy_logic = true;
                std::string cpkg = f.type.package_name.empty() ? pkg_ : f.type.package_name;
                os << "    " << cpkg << "_" << f.type.custom_name << "_destroy(&self->" << f.name << ");\n";
            }
        }
        if (!has_destroy_logic) {
            os << "    (void)self;\n";
        }
        os << "}\n\n";

        genTopicSerialize(t, os);
        genTopicDeserialize(t, os);
    }

    // 服务 Stub 与 Proxy 实现
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
    os << "    " << tname << "_destroy(self);\n";
    os << "    " << tname << "_init(self);\n";
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
    os << "    " << tname << "_destroy(self);\n";
    os << "    " << tname << "_init(self);\n";
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

// ---- Stub 源码：单方法请求分发片段 ----

static void emitStubParamDeserialize(std::ostream& os, const MethodDef& m, const std::string& pkg) {
    if (!m.has_param) {
        return;
    }
    if (isCompositePointerType(m.param.type)) {
        std::string ptype = cTypeName(m.param.type, pkg);
        std::string ppfx = cDestroyPrefix(m.param.type, pkg);
        os << "        " << ptype << " " << m.param.name << ";\n";
        os << "        " << ppfx << "_init(&" << m.param.name << ");\n";
        os << "        if (!" << ppfx << "_deserialize(&" << m.param.name << ", req)) { " << ppfx << "_destroy(&" << m.param.name << "); omni_buffer_destroy(req); return OMNI_ERR_DESERIALIZE; }\n";
    } else if (isCStringLike(m.param.type)) {
        std::string ptype = cTypeName(m.param.type, pkg);
        os << "        " << ptype << " " << m.param.name << " = NULL;\n";
        os << "        uint32_t " << m.param.name << "_len = 0;\n";
        if (m.param.type.primitive == TYPE_STRING) {
            os << "        " << m.param.name << " = omni_buffer_read_string(req, &" << m.param.name << "_len);\n";
        } else {
            os << "        " << m.param.name << " = omni_buffer_read_bytes(req, &" << m.param.name << "_len);\n";
        }
        os << "        if (!omni_buffer_read_ok(req)) { if (" << m.param.name << ") omni_free(" << m.param.name << "); omni_buffer_destroy(req); return OMNI_ERR_DESERIALIZE; }\n";
    } else {
        std::string ptype = cTypeName(m.param.type, pkg);
        os << "        " << ptype << " " << m.param.name << " = ";
        const char* rfn = primitiveReadFunc(m.param.type.primitive);
        if (rfn) {
            os << rfn << "(req);\n";
        } else {
            os << "0;\n";
        }
        os << "        if (!omni_buffer_read_ok(req)) { omni_buffer_destroy(req); return OMNI_ERR_DESERIALIZE; }\n";
    }
}

static void emitStubResultDecl(std::ostream& os, const MethodDef& m, const std::string& pkg) {
    if (m.return_type.isVoid()) {
        return;
    }
    if (m.return_type.isArray() || m.return_type.isCustom()) {
        std::string rtype = cTypeName(m.return_type, pkg);
        std::string rpfx = cDestroyPrefix(m.return_type, pkg);
        os << "        " << rtype << " result;\n";
        os << "        " << rpfx << "_init(&result);\n";
    } else if (isCStringLike(m.return_type)) {
        os << "        " << cTypeName(m.return_type, pkg) << " result = NULL;\n";
        os << "        uint32_t result_len = 0;\n";
    } else {
        os << "        " << cTypeName(m.return_type, pkg) << " result = 0;\n";
    }
}

static void emitStubCallbackCall(std::ostream& os, const MethodDef& m) {
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
}

static void emitStubResultSerialize(std::ostream& os, const MethodDef& m, const std::string& pkg) {
    if (m.return_type.isVoid()) {
        return;
    }
    if (m.return_type.isArray() || m.return_type.isCustom()) {
        std::string rpfx = cDestroyPrefix(m.return_type, pkg);
        os << "        " << rpfx << "_serialize(&result, response);\n";
        os << "        " << rpfx << "_destroy(&result);\n";
    } else if (isCStringLike(m.return_type)) {
        if (m.return_type.primitive == TYPE_STRING) {
            os << "        omni_buffer_write_string(response, result, result_len);\n";
        } else {
            os << "        omni_buffer_write_bytes(response, result, result_len);\n";
        }
        os << "        if (result) omni_free(result);\n";
    } else {
        emitPrimitiveWrite(os, m.return_type.primitive, "result", "response", "        ");
    }
}

static void emitStubParamCleanup(std::ostream& os, const MethodDef& m, const std::string& pkg) {
    if (m.has_param && isCompositePointerType(m.param.type)) {
        os << "        " << cDestroyPrefix(m.param.type, pkg) << "_destroy(&" << m.param.name << ");\n";
    } else if (m.has_param && isCStringLike(m.param.type)) {
        os << "        if (" << m.param.name << ") omni_free(" << m.param.name << ");\n";
    }
}

void CCodeGen::genServiceStubSource(const ServiceDef& svc, const AstFile& ast, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;

    // 保存回调的内部结构体
    os << "/* " << svc.name << " Stub internal */\n";
    os << "typedef struct " << prefix << "_stub_data {\n";
    os << "    " << prefix << "_callbacks cbs;\n";
    os << "} " << prefix << "_stub_data;\n\n";

    // onInvoke 分发函数
    os << "static int " << prefix << "_on_invoke(uint32_t method_id,\n";
    os << "    const omni_buffer_t* request, omni_buffer_t* response, void* user_data)\n";
    os << "{\n";
    os << "    " << prefix << "_stub_data* data = (" << prefix << "_stub_data*)user_data;\n";
    os << "    omni_buffer_t* req = omni_buffer_create_from(omni_buffer_data(request), omni_buffer_size(request));\n\n";

    bool first = true;
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string upper_name = cToSnakeCase(m.name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);

        if (first) {
            os << "    if (method_id == " << prefix << "_METHOD_" << upper_name << ") {\n";
            first = false;
        } else {
            os << "    } else if (method_id == " << prefix << "_METHOD_" << upper_name << ") {\n";
        }

        emitStubParamDeserialize(os, m, pkg_);
        emitStubResultDecl(os, m, pkg_);
        emitStubCallbackCall(os, m);
        emitStubResultSerialize(os, m, pkg_);
        emitStubParamCleanup(os, m, pkg_);
    }
    if (!svc.methods.empty()) {
        os << "    }\n";
    }
    os << "    omni_buffer_destroy(req);\n";
    os << "    return 0;\n";
    os << "}\n\n";

    os << "omni_service_t* " << prefix << "_stub_create_from_callbacks(const " << prefix << "_callbacks* cbs) {\n";
    os << "    " << prefix << "_stub_data* data = (" << prefix << "_stub_data*)omni_malloc(sizeof(" << prefix << "_stub_data));\n";
    os << "    if (!data) return NULL;\n";
    os << "    data->cbs = *cbs;\n";
    os << "    omni_service_t* svc = omni_service_create(\"" << svc.name << "\",\n";
    os << "        " << prefix << "_INTERFACE_ID, " << prefix << "_on_invoke, data);\n";

    // 添加方法
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        std::string upper_name = cToSnakeCase(svc.methods[i].name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);
        std::string param_type_str = svc.methods[i].has_param ? cppTypeName(svc.methods[i].param.type) : "";
        std::string return_type_str = cppTypeName(svc.methods[i].return_type);
        uint32_t idl_hash = computeMethodHash(svc.methods[i], ast);
        os << "    omni_service_add_method_ex(svc, " << prefix << "_METHOD_" << upper_name
           << ", \"" << svc.methods[i].name << "\", \"" << param_type_str
           << "\", \"" << return_type_str << "\", 0x" << std::hex << idl_hash << std::dec << "u);\n";
    }
    os << "    return svc;\n";
    os << "}\n\n";

    // _stub_destroy
    os << "void " << prefix << "_stub_destroy(omni_service_t* svc) {\n";
    os << "    void* stub_data = omni_service_get_user_data(svc);\n";
    os << "    omni_service_destroy(svc);\n";
    os << "    omni_free(stub_data);\n";
    os << "}\n\n";

    // 广播辅助函数
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_broadcast_" + cToSnakeCase(topic);
        uint32_t tid = fnv1a_hash(topic);

        os << "void " << fn_name << "(omni_runtime_t* runtime, const " << topic_type << "* msg) {\n";
        os << "    omni_buffer_t* buf = omni_buffer_create();\n";
        os << "    " << topic_type << "_serialize(msg, buf);\n";
        os << "    omni_runtime_broadcast(runtime, 0x" << std::hex << tid << std::dec << "u, buf);\n";
        os << "    omni_buffer_destroy(buf);\n";
        os << "}\n\n";
    }
}

// ---- Proxy 源码：生命周期 / 单方法调用 / 订阅 ----

static void emitProxyLifecycleSource(std::ostream& os, const ServiceDef& svc,
                                     const std::string& prefix) {
    os << "static void " << prefix << "_internal_death_cb(const char* service_name, void* user_data) {\n";
    os << "    (void)service_name;\n";
    os << "    " << prefix << "_proxy* p = (" << prefix << "_proxy*)user_data;\n";
    os << "    p->connected = 0;\n";
    os << "    if (p->death_callback) {\n";
    os << "        p->death_callback(p->death_user_data);\n";
    os << "    }\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_init(" << prefix << "_proxy* p, omni_runtime_t* runtime) {\n";
    os << "    p->runtime = runtime;\n";
    os << "    p->connected = 0;\n";
    os << "    p->auto_reconnect_enabled = 0;\n";
    os << "    p->reconnect_interval_ms = 1000;\n";
    os << "    p->death_callback = NULL;\n";
    os << "    p->death_user_data = NULL;\n";
    os << "    memset(p->_sub_ctxs, 0, sizeof(p->_sub_ctxs));\n";
    os << "    memset(p->_sub_topics, 0, sizeof(p->_sub_topics));\n";
    os << "    p->_sub_ctx_count = 0;\n";
    os << "}\n\n";

    os << "int " << prefix << "_proxy_connect(" << prefix << "_proxy* p) {\n";
    os << "    int ret = omni_runtime_connect_service(p->runtime, \"" << svc.name << "\");\n";
    os << "    if (ret == 0) {\n";
    os << "        p->connected = 1;\n";
    os << "        omni_runtime_enable_auto_reconnect(p->runtime, \"" << svc.name << "\", 1);\n";
    os << "        p->auto_reconnect_enabled = 1;\n";
    os << "        omni_runtime_start_heartbeat(p->runtime, \"" << svc.name << "\", 5000, 10000);\n";
    os << "        omni_runtime_subscribe_death(p->runtime, \"" << svc.name << "\",\n";
    os << "            " << prefix << "_internal_death_cb, p);\n";
    os << "    }\n";
    os << "    return ret;\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_disconnect(" << prefix << "_proxy* p) {\n";
    os << "    if (p->connected) {\n";
    os << "        omni_runtime_stop_heartbeat(p->runtime, \"" << svc.name << "\");\n";
    os << "        omni_runtime_unsubscribe_death(p->runtime, \"" << svc.name << "\");\n";
    os << "        omni_runtime_disconnect_service(p->runtime, \"" << svc.name << "\");\n";
    os << "        p->connected = 0;\n";
    os << "    }\n";
    os << "}\n\n";

    os << "int " << prefix << "_proxy_is_connected(const " << prefix << "_proxy* p) {\n";
    os << "    return p->connected;\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_enable_auto_reconnect(" << prefix << "_proxy* p, int enable) {\n";
    os << "    omni_runtime_enable_auto_reconnect(p->runtime, \"" << svc.name << "\", enable);\n";
    os << "    p->auto_reconnect_enabled = enable;\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_set_reconnect_interval(" << prefix << "_proxy* p, uint32_t interval_ms) {\n";
    os << "    omni_runtime_set_reconnect_interval(p->runtime, \"" << svc.name << "\", interval_ms);\n";
    os << "    p->reconnect_interval_ms = interval_ms;\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_start_heartbeat(" << prefix << "_proxy* p, uint32_t interval_ms, uint32_t timeout_ms) {\n";
    os << "    omni_runtime_start_heartbeat(p->runtime, \"" << svc.name << "\", interval_ms, timeout_ms);\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_stop_heartbeat(" << prefix << "_proxy* p) {\n";
    os << "    omni_runtime_stop_heartbeat(p->runtime, \"" << svc.name << "\");\n";
    os << "}\n\n";
}

static void emitProxyMethodSource(std::ostream& os, const ServiceDef& svc, const MethodDef& m,
                                  uint32_t iface_id, const std::string& prefix,
                                  const AstFile& ast, const std::string& pkg) {
    uint32_t mid = fnv1a_hash(m.name);
    std::string fn_name = prefix + "_proxy_" + cToSnakeCase(m.name);

    if (m.return_type.isVoid()) {
        os << "void " << fn_name << "(" << prefix << "_proxy* p";
    } else {
        os << "int " << fn_name << "(" << prefix << "_proxy* p";
    }
    emitProxyMethodParams(os, m, pkg, false);
    os << ") {\n";

    os << "    omni_buffer_t* req = omni_buffer_create();\n";
    os << "    omni_buffer_t* resp = omni_buffer_create();\n";

    // 序列化参数
    if (m.has_param) {
        if (isCompositePointerType(m.param.type)) {
            std::string ppfx = cDestroyPrefix(m.param.type, pkg);
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

    uint32_t idl_hash = computeMethodHash(m, ast);
    if (m.return_type.isVoid()) {
        os << "    omni_runtime_invoke_oneway(p->runtime, \"" << svc.name << "\",\n";
        os << "        0x" << std::hex << iface_id << std::dec << "u, 0x" << std::hex << mid << std::dec << "u, 0x" << std::hex << idl_hash << std::dec << "u, req);\n";
    } else {
        os << "    int ret = omni_runtime_invoke(p->runtime, \"" << svc.name << "\",\n";
        os << "        0x" << std::hex << iface_id << std::dec << "u, 0x" << std::hex << mid << std::dec << "u, 0x" << std::hex << idl_hash << std::dec << "u, req, resp, 0);\n";
    }

    // 反序列化结果
    if (!m.return_type.isVoid()) {
        os << "    if (ret == 0 && result) {\n";
        if (m.return_type.isArray() || m.return_type.isCustom()) {
            std::string rpfx = cDestroyPrefix(m.return_type, pkg);
            os << "        " << rpfx << "_init(result);\n";
            os << "        if (!" << rpfx << "_deserialize(result, resp)) { " << rpfx << "_destroy(result); ret = OMNI_ERR_DESERIALIZE; }\n";
        } else if (isCStringLike(m.return_type)) {
            if (m.return_type.primitive == TYPE_STRING) {
                os << "        *result = omni_buffer_read_string(resp, result_len);\n";
            } else {
                os << "        *result = omni_buffer_read_bytes(resp, result_len);\n";
            }
            os << "        if (!omni_buffer_read_ok(resp)) { if (*result) omni_free(*result); *result = NULL; if (result_len) *result_len = 0; ret = OMNI_ERR_DESERIALIZE; }\n";
        } else {
            emitPrimitiveRead(os, m.return_type.primitive, "*result", "resp", "        ");
            os << "        if (!omni_buffer_read_ok(resp)) { ret = OMNI_ERR_DESERIALIZE; }\n";
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

static void emitProxySubscribeSource(std::ostream& os, const std::string& topic,
                                     const std::string& prefix, const std::string& pkg) {
    std::string topic_type = pkg + "_" + topic;
    std::string topic_snake = cToSnakeCase(topic);
    std::string fn_name = prefix + "_proxy_subscribe_" + topic_snake;

    // 需要一个小的包装结构体，把类型化回调传入通用 topic 回调
    os << "typedef struct " << prefix << "_" << topic_snake << "_sub_ctx {\n";
    os << "    void (*callback)(const " << topic_type << "* msg, void* user_data);\n";
    os << "    void* user_data;\n";
    os << "} " << prefix << "_" << topic_snake << "_sub_ctx;\n\n";

    os << "static void " << prefix << "_" << topic_snake << "_topic_cb(\n";
    os << "    uint32_t topic_id, const omni_buffer_t* data, void* user_data)\n";
    os << "{\n";
    os << "    (void)topic_id;\n";
    os << "    " << prefix << "_" << topic_snake << "_sub_ctx* ctx =\n";
    os << "        (" << prefix << "_" << topic_snake << "_sub_ctx*)user_data;\n";
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

    os << "int  " << fn_name << "(" << prefix << "_proxy* p,\n";
    os << "    void (*callback)(const " << topic_type << "* msg, void* user_data), void* user_data)\n";
    os << "{\n";
    os << "    if (p->_sub_ctx_count >= 8 || !p->runtime) return -1;\n";
    os << "    " << prefix << "_" << topic_snake << "_sub_ctx* ctx =\n";
    os << "        (" << prefix << "_" << topic_snake << "_sub_ctx*)omni_malloc(\n";
    os << "            sizeof(" << prefix << "_" << topic_snake << "_sub_ctx));\n";
    os << "    if (!ctx) return -1;\n";
    os << "    ctx->callback = callback;\n";
    os << "    ctx->user_data = user_data;\n";
    os << "    if (omni_runtime_subscribe_topic(p->runtime, \"" << topic << "\", "
       << pkg << "_" << topic << "_TOPIC_IDL_HASH,\n";
    os << "        " << prefix << "_" << topic_snake << "_topic_cb, ctx) != 0) {\n";
    os << "        omni_free(ctx);\n";
    os << "        return -1;\n";
    os << "    }\n";
    os << "    p->_sub_ctxs[p->_sub_ctx_count] = ctx;\n";
    os << "    p->_sub_topics[p->_sub_ctx_count] = \"" << topic << "\";\n";
    os << "    p->_sub_ctx_count++;\n";
    os << "    return 0;\n";
    os << "}\n\n";
}

void CCodeGen::genServiceProxySource(const ServiceDef& svc, const AstFile& ast, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;
    uint32_t iface_id = fnv1a_hash(pkg_ + "." + svc.name);

    emitProxyLifecycleSource(os, svc, prefix);

    for (size_t i = 0; i < svc.methods.size(); ++i) {
        emitProxyMethodSource(os, svc, svc.methods[i], iface_id, prefix, ast, pkg_);
    }

    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        emitProxySubscribeSource(os, svc.publishes[i], prefix, pkg_);
    }

    os << "void " << prefix << "_proxy_on_service_died(" << prefix << "_proxy* p,\n";
    os << "    void (*callback)(void* user_data), void* user_data)\n";
    os << "{\n";
    os << "    p->death_callback = callback;\n";
    os << "    p->death_user_data = user_data;\n";
    os << "}\n\n";

    os << "void " << prefix << "_proxy_destroy(" << prefix << "_proxy* p)\n";
    os << "{\n";
    os << "    for (int i = 0; i < p->_sub_ctx_count; ++i) {\n";
    os << "        if (p->runtime && p->_sub_topics[i]) {\n";
    os << "            omni_runtime_unsubscribe_topic(p->runtime, p->_sub_topics[i]);\n";
    os << "        }\n";
    os << "        omni_free(p->_sub_ctxs[i]);\n";
    os << "    }\n";
    os << "    p->_sub_ctx_count = 0;\n";
    os << "    p->runtime = NULL;\n";
    os << "}\n\n";
}

} // namespace omnic
