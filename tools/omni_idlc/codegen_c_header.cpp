#include "codegen_c.h"
#include "codegen_c_internal.h"
#include <cctype>
#include <cstdio>
#include <fstream>
#include <vector>

namespace omnic {

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

/* @brief 生成 stub 回调函数参数列表
 * @param[in,out] os 输出流
 * @param[in] m 方法定义
 * @param[in] pkg 包名
 * @note typedef 与 impl 声明共用同一份签名
 */
static void emitStubHandlerParams(std::ostream& os, const MethodDef& m, const std::string& pkg) {
    bool has_args = false;
    if (m.has_param) {
        if (isCompositePointerType(m.param.type)) {
            os << "const " << cDeclaredTypeName(m.param.type, pkg) << "* " << m.param.name;
        } else if (isCStringLike(m.param.type)) {
            os << "const " << cDeclaredTypeName(m.param.type, pkg) << " " << m.param.name << ", uint32_t " << m.param.name << "_len";
        } else {
            os << cDeclaredTypeName(m.param.type, pkg) << " " << m.param.name;
        }
        has_args = true;
    }
    if (!m.return_type.isVoid()) {
        if (has_args) os << ", ";
        if (m.return_type.isArray() || m.return_type.isCustom()) {
            os << cDeclaredTypeName(m.return_type, pkg) << "* result";
        } else if (isCStringLike(m.return_type)) {
            os << cDeclaredTypeName(m.return_type, pkg) << "* result, uint32_t* result_len";
        } else {
            os << cDeclaredTypeName(m.return_type, pkg) << "* result";
        }
        has_args = true;
    }
    if (has_args) os << ", ";
    os << "void* user_data";
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
        reportError(context + " uses unsupported C codegen type '" + cppTypeName(type) + "'");
        return false;
    }

    if (type.isArray()) {
        if (!type.element_type) {
            reportError(context + " uses unsupported C codegen type '" + cppTypeName(type) + "'");
            return false;
        }
        return validateTypeSupported(*type.element_type, context + " element type", false);
    }

    if (primitiveReadFunc(type.primitive) != NULL &&
        primitiveWriteFunc(type.primitive) != NULL) {
        return true;
    }

    reportError(context + " uses unsupported C codegen type '" + cppTypeName(type) + "'");
    return false;
}

bool CCodeGen::validateAst(const AstFile& ast) {
    return validateAstCommon(ast, [this](const TypeRef& t, const std::string& ctx, bool allow_void) {
        return this->validateTypeSupported(t, ctx, allow_void);
    });
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

    // 结构体
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        genStruct(ast.structs[i], os);
    }

    // 话题
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        genTopic(ast.topics[i], os);
        uint32_t topic_idl_hash = computeTopicHash(ast.topics[i], ast);
        os << "#define " << pkg_ << "_" << ast.topics[i].name << "_TOPIC_IDL_HASH"
           << " 0x" << std::hex << topic_idl_hash << std::dec << "u\n\n";
    }

    // 服务
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
    uint32_t topic_id = fnv1a_hash(t.name);

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

void CCodeGen::genServiceStubHeader(const ServiceDef& svc, const AstFile& ast, std::ostream& os) {
    std::string prefix = pkg_ + "_" + svc.name;
    uint32_t iface_id = fnv1a_hash(pkg_ + "." + svc.name);

    os << "/* ---- " << svc.name << " Stub (Server Side) ---- */\n\n";

    os << "#define " << prefix << "_INTERFACE_ID 0x"
       << std::hex << iface_id << std::dec << "u\n\n";

    // Method ID 宏定义
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        uint32_t mid = fnv1a_hash(svc.methods[i].name);
        std::string upper_name = cToSnakeCase(svc.methods[i].name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);
        os << "#define " << prefix << "_METHOD_" << upper_name
           << " 0x" << std::hex << mid << std::dec << "u\n";
    }
    os << "\n";

    // Method IDL hash 宏定义（用于运行时 IDL 兼容性校验）
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        uint32_t idl_hash = computeMethodHash(m, ast);
        std::string upper_name = cToSnakeCase(m.name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);
        os << "#define " << prefix << "_METHOD_" << upper_name << "_IDL_HASH"
           << " 0x" << std::hex << idl_hash << std::dec << "u\n";
    }
    os << "\n";

    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string handler_typedef = prefix + "_" + cToSnakeCase(m.name) + "_handler_t";
        std::string handler_decl = prefix + "_impl_" + cToSnakeCase(m.name);
        os << "typedef void (*" << handler_typedef << ")(";
        emitStubHandlerParams(os, m, pkg_);
        os << ");\n";

        os << "void " << handler_decl << "(";
        emitStubHandlerParams(os, m, pkg_);
        os << ");\n\n";
    }

    os << "typedef struct " << prefix << "_callbacks {\n";
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string handler_typedef = prefix + "_" + cToSnakeCase(m.name) + "_handler_t";
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
        os << "    cbs." << m.name << " = " << prefix << "_impl_" << cToSnakeCase(m.name) << ";\n";
    }
    os << "    cbs.user_data = user_data;\n";
    os << "    return " << prefix << "_stub_create_from_callbacks(&cbs);\n";
    os << "}\n\n";
    os << "void " << prefix << "_stub_destroy(omni_service_t* svc);\n\n";

    // 广播辅助函数
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_broadcast_" + cToSnakeCase(topic);
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
    os << "    int auto_reconnect_enabled;\n";
    os << "    uint32_t reconnect_interval_ms;\n";
    os << "    void (*death_callback)(void* user_data);\n";
    os << "    void* death_user_data;\n";
    os << "    void* _sub_ctxs[8];\n";
    os << "    const char* _sub_topics[8];\n";
    os << "    int _sub_ctx_count;\n";
    os << "} " << prefix << "_proxy;\n\n";

    os << "void " << prefix << "_proxy_init(" << prefix << "_proxy* p, omni_runtime_t* runtime);\n";
    os << "int  " << prefix << "_proxy_connect(" << prefix << "_proxy* p);\n";
    os << "void " << prefix << "_proxy_disconnect(" << prefix << "_proxy* p);\n";
    os << "void " << prefix << "_proxy_destroy(" << prefix << "_proxy* p);\n";
    os << "int  " << prefix << "_proxy_is_connected(const " << prefix << "_proxy* p);\n";
    os << "void " << prefix << "_proxy_enable_auto_reconnect(" << prefix << "_proxy* p, int enable);\n";
    os << "void " << prefix << "_proxy_set_reconnect_interval(" << prefix << "_proxy* p, uint32_t interval_ms);\n";
    os << "void " << prefix << "_proxy_start_heartbeat(" << prefix << "_proxy* p, uint32_t interval_ms, uint32_t timeout_ms);\n";
    os << "void " << prefix << "_proxy_stop_heartbeat(" << prefix << "_proxy* p);\n\n";

    // 方法 Proxy
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        std::string fn_name = prefix + "_proxy_" + cToSnakeCase(m.name);

        if (m.return_type.isVoid()) {
            os << "void " << fn_name << "(" << prefix << "_proxy* p";
        } else {
            os << "int  " << fn_name << "(" << prefix << "_proxy* p";
        }
        emitProxyMethodParams(os, m, pkg_, true);
        os << ");\n";
    }
    os << "\n";

    // 订阅辅助函数
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        const std::string& topic = svc.publishes[i];
        std::string topic_type = pkg_ + "_" + topic;
        std::string fn_name = prefix + "_proxy_subscribe_" + cToSnakeCase(topic);
        os << "int  " << fn_name << "(" << prefix << "_proxy* p,\n";
        os << "    void (*callback)(const " << topic_type << "* msg, void* user_data), void* user_data);\n";
    }

    // 死亡通知
    os << "void " << prefix << "_proxy_on_service_died(" << prefix << "_proxy* p,\n";
    os << "    void (*callback)(void* user_data), void* user_data);\n\n";
}

} // namespace omnic
