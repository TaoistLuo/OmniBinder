#include "codegen_c.h"
#include <fstream>
#include <iomanip>
#include <cctype>

namespace omnic {

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
    default: return "void";
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
    std::string header_path = output_dir + "/" + filename + "_c.h";
    std::string source_path = output_dir + "/" + filename + ".c";

    std::ofstream hdr(header_path.c_str());
    std::ofstream src(source_path.c_str());
    if (!hdr.is_open() || !src.is_open()) return false;

    generateHeader(ast, hdr, filename);
    generateSource(ast, src, filename);
    return true;
}

void CCodeGen::generateHeader(const AstFile& ast, std::ostream& os, const std::string& filename) {
    std::string guard = filename;
    for (size_t i = 0; i < guard.size(); ++i) {
        if (guard[i] == '.') guard[i] = '_';
        guard[i] = toupper(guard[i]);
    }
    guard += "_C_H";

    os << "#ifndef " << guard << "\n#define " << guard << "\n\n";
    os << "#include <omnibinder/omnibinder_c.h>\n";
    os << "#include <stdint.h>\n#include <stddef.h>\n\n";
    
    // 生成被导入文件的 #include
    for (size_t i = 0; i < ast.imports.size(); ++i) {
        std::string imp = ast.imports[i];
        size_t slash = imp.find_last_of("/\\");
        if (slash != std::string::npos) imp = imp.substr(slash + 1);
        os << "#include \"" << imp << "_c.h\"\n";
    }
    if (!ast.imports.empty()) os << "\n";
    
    os << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

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
        os << "    " << cTypeName(f.type, pkg_) << " " << f.name << ";\n";
        if (f.type.primitive == TYPE_STRING) {
            os << "    uint32_t " << f.name << "_len;\n";
        } else if (f.type.primitive == TYPE_BYTES) {
            os << "    uint32_t " << f.name << "_len;\n";
        } else if (f.type.primitive == TYPE_ARRAY) {
            os << "    uint32_t " << f.name << "_count;\n";
        }
    }
    os << "} " << tname << ";\n\n";

    os << "void " << tname << "_init(" << tname << "* self);\n";
    os << "void " << tname << "_destroy(" << tname << "* self);\n";
    os << "void " << tname << "_serialize(const " << tname << "* self, omni_buffer_t* buf);\n";
    os << "void " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf);\n\n";
}

void CCodeGen::genTopic(const TopicDef& t, std::ostream& os) {
    std::string tname = pkg_ + "_" + t.name;
    uint32_t topic_id = fnv1a_hash(pkg_ + "." + t.name);

    os << "typedef struct " << tname << " {\n";
    for (size_t j = 0; j < t.fields.size(); ++j) {
        const FieldDef& f = t.fields[j];
        os << "    " << cTypeName(f.type, pkg_) << " " << f.name << ";\n";
        if (f.type.primitive == TYPE_STRING) {
            os << "    uint32_t " << f.name << "_len;\n";
        } else if (f.type.primitive == TYPE_BYTES) {
            os << "    uint32_t " << f.name << "_len;\n";
        } else if (f.type.primitive == TYPE_ARRAY) {
            os << "    uint32_t " << f.name << "_count;\n";
        }
    }
    os << "} " << tname << ";\n\n";

    os << "#define " << pkg_ << "_" << t.name << "_TOPIC_ID 0x"
       << std::hex << topic_id << std::dec << "u\n\n";

    os << "void " << tname << "_init(" << tname << "* self);\n";
    os << "void " << tname << "_destroy(" << tname << "* self);\n";
    os << "void " << tname << "_serialize(const " << tname << "* self, omni_buffer_t* buf);\n";
    os << "void " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf);\n\n";
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

    // Callbacks struct
    os << "typedef struct " << prefix << "_callbacks {\n";
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        os << "    ";
        if (m.return_type.isVoid()) {
            os << "void";
        } else {
            os << "void";
        }
        os << " (*" << m.name << ")(";

        // Parameters
        bool has_args = false;
        if (m.has_param) {
            if (m.param.type.isCustom()) {
                os << "const " << cTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else {
                os << cTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
            has_args = true;
        }
        if (!m.return_type.isVoid()) {
            if (has_args) os << ", ";
            if (m.return_type.isCustom()) {
                os << cTypeName(m.return_type, pkg_) << "* result";
            } else {
                os << cTypeName(m.return_type, pkg_) << "* result";
            }
            has_args = true;
        }
        if (has_args) os << ", ";
        os << "void* user_data);\n";
    }
    os << "    void* user_data;\n";
    os << "} " << prefix << "_callbacks;\n\n";

    os << "omni_service_t* " << prefix << "_stub_create(const " << prefix << "_callbacks* cbs);\n";
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
            if (m.param.type.isCustom()) {
                os << ", const " << cTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else {
                os << ", " << cTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
        }
        if (!m.return_type.isVoid()) {
            if (m.return_type.isCustom()) {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result";
            } else {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result";
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
    os << "#include \"" << filename << "_c.h\"\n";
    os << "#include <string.h>\n#include <stdlib.h>\n\n";

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
    switch (f.type.primitive) {
    case TYPE_BOOL:    os << "    omni_buffer_write_bool(buf, " << name << ");\n"; break;
    case TYPE_INT8:    os << "    omni_buffer_write_int8(buf, " << name << ");\n"; break;
    case TYPE_UINT8:   os << "    omni_buffer_write_uint8(buf, " << name << ");\n"; break;
    case TYPE_INT16:   os << "    omni_buffer_write_int16(buf, " << name << ");\n"; break;
    case TYPE_UINT16:  os << "    omni_buffer_write_uint16(buf, " << name << ");\n"; break;
    case TYPE_INT32:   os << "    omni_buffer_write_int32(buf, " << name << ");\n"; break;
    case TYPE_UINT32:  os << "    omni_buffer_write_uint32(buf, " << name << ");\n"; break;
    case TYPE_INT64:   os << "    omni_buffer_write_int64(buf, " << name << ");\n"; break;
    case TYPE_UINT64:  os << "    omni_buffer_write_uint64(buf, " << name << ");\n"; break;
    case TYPE_FLOAT32: os << "    omni_buffer_write_float32(buf, " << name << ");\n"; break;
    case TYPE_FLOAT64: os << "    omni_buffer_write_float64(buf, " << name << ");\n"; break;
    case TYPE_STRING:
        os << "    omni_buffer_write_string(buf, " << name << ", " << name << "_len);\n";
        break;
    case TYPE_BYTES:
        os << "    omni_buffer_write_bytes(buf, " << name << ", " << name << "_len);\n";
        break;
    case TYPE_CUSTOM: {
        std::string cpkg = f.type.package_name.empty() ? pkg_ : f.type.package_name;
        os << "    " << cpkg << "_" << f.type.custom_name << "_serialize(&" << name << ", buf);\n";
        break;
    }
    default: break;
    }
}

void CCodeGen::genFieldDeserialize(const FieldDef& f, const std::string& obj, std::ostream& os) {
    std::string name = obj + f.name;
    switch (f.type.primitive) {
    case TYPE_BOOL:    os << "    " << name << " = omni_buffer_read_bool(buf);\n"; break;
    case TYPE_INT8:    os << "    " << name << " = omni_buffer_read_int8(buf);\n"; break;
    case TYPE_UINT8:   os << "    " << name << " = omni_buffer_read_uint8(buf);\n"; break;
    case TYPE_INT16:   os << "    " << name << " = omni_buffer_read_int16(buf);\n"; break;
    case TYPE_UINT16:  os << "    " << name << " = omni_buffer_read_uint16(buf);\n"; break;
    case TYPE_INT32:   os << "    " << name << " = omni_buffer_read_int32(buf);\n"; break;
    case TYPE_UINT32:  os << "    " << name << " = omni_buffer_read_uint32(buf);\n"; break;
    case TYPE_INT64:   os << "    " << name << " = omni_buffer_read_int64(buf);\n"; break;
    case TYPE_UINT64:  os << "    " << name << " = omni_buffer_read_uint64(buf);\n"; break;
    case TYPE_FLOAT32: os << "    " << name << " = omni_buffer_read_float32(buf);\n"; break;
    case TYPE_FLOAT64: os << "    " << name << " = omni_buffer_read_float64(buf);\n"; break;
    case TYPE_STRING:
        os << "    " << name << " = omni_buffer_read_string(buf, &" << name << "_len);\n";
        break;
    case TYPE_BYTES:
        os << "    " << name << " = omni_buffer_read_bytes(buf, &" << name << "_len);\n";
        break;
    case TYPE_CUSTOM: {
        std::string cpkg = f.type.package_name.empty() ? pkg_ : f.type.package_name;
        os << "    " << cpkg << "_" << f.type.custom_name << "_deserialize(&" << name << ", buf);\n";
        break;
    }
    default: break;
    }
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
    os << "void " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf) {\n";
    for (size_t i = 0; i < s.fields.size(); ++i) {
        genFieldDeserialize(s.fields[i], "self->", os);
    }
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
    os << "void " << tname << "_deserialize(" << tname << "* self, omni_buffer_t* buf) {\n";
    for (size_t i = 0; i < t.fields.size(); ++i) {
        genFieldDeserialize(t.fields[i], "self->", os);
    }
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
            if (m.param.type.isCustom()) {
                std::string ptype = cTypeName(m.param.type, pkg_);
                std::string ppfx = cTypePrefix(m.param.type, pkg_);
                os << "        " << ptype << " " << m.param.name << ";\n";
                os << "        " << ppfx << "_init(&" << m.param.name << ");\n";
                os << "        " << ppfx << "_deserialize(&" << m.param.name << ", req);\n";
            } else {
                std::string ptype = cTypeName(m.param.type, pkg_);
                os << "        " << ptype << " " << m.param.name << " = ";
                // Read primitive from buffer
                switch (m.param.type.primitive) {
                case TYPE_BOOL:    os << "omni_buffer_read_bool(req);\n"; break;
                case TYPE_INT8:    os << "omni_buffer_read_int8(req);\n"; break;
                case TYPE_UINT8:   os << "omni_buffer_read_uint8(req);\n"; break;
                case TYPE_INT16:   os << "omni_buffer_read_int16(req);\n"; break;
                case TYPE_UINT16:  os << "omni_buffer_read_uint16(req);\n"; break;
                case TYPE_INT32:   os << "omni_buffer_read_int32(req);\n"; break;
                case TYPE_UINT32:  os << "omni_buffer_read_uint32(req);\n"; break;
                case TYPE_INT64:   os << "omni_buffer_read_int64(req);\n"; break;
                case TYPE_UINT64:  os << "omni_buffer_read_uint64(req);\n"; break;
                case TYPE_FLOAT32: os << "omni_buffer_read_float32(req);\n"; break;
                case TYPE_FLOAT64: os << "omni_buffer_read_float64(req);\n"; break;
                default: os << "0;\n"; break;
                }
            }
        }

        // Prepare result variable
        if (!m.return_type.isVoid()) {
            if (m.return_type.isCustom()) {
                std::string rtype = cTypeName(m.return_type, pkg_);
                std::string rpfx = cTypePrefix(m.return_type, pkg_);
                os << "        " << rtype << " result;\n";
                os << "        " << rpfx << "_init(&result);\n";
            } else {
                os << "        " << cTypeName(m.return_type, pkg_) << " result = 0;\n";
            }
        }

        // Call the callback
        os << "        if (data->cbs." << m.name << ") {\n";
        os << "            data->cbs." << m.name << "(";
        bool has_args = false;
        if (m.has_param) {
            if (m.param.type.isCustom()) {
                os << "&" << m.param.name;
            } else {
                os << m.param.name;
            }
            has_args = true;
        }
        if (!m.return_type.isVoid()) {
            if (has_args) os << ", ";
            os << "&result";
            has_args = true;
        }
        if (has_args) os << ", ";
        os << "data->cbs.user_data);\n";
        os << "        }\n";

        // Serialize result
        if (!m.return_type.isVoid()) {
            if (m.return_type.isCustom()) {
                std::string rpfx2 = cTypePrefix(m.return_type, pkg_);
                os << "        " << rpfx2 << "_serialize(&result, response);\n";
                os << "        " << rpfx2 << "_destroy(&result);\n";
            } else {
                switch (m.return_type.primitive) {
                case TYPE_BOOL:    os << "        omni_buffer_write_bool(response, result);\n"; break;
                case TYPE_INT8:    os << "        omni_buffer_write_int8(response, result);\n"; break;
                case TYPE_UINT8:   os << "        omni_buffer_write_uint8(response, result);\n"; break;
                case TYPE_INT16:   os << "        omni_buffer_write_int16(response, result);\n"; break;
                case TYPE_UINT16:  os << "        omni_buffer_write_uint16(response, result);\n"; break;
                case TYPE_INT32:   os << "        omni_buffer_write_int32(response, result);\n"; break;
                case TYPE_UINT32:  os << "        omni_buffer_write_uint32(response, result);\n"; break;
                case TYPE_INT64:   os << "        omni_buffer_write_int64(response, result);\n"; break;
                case TYPE_UINT64:  os << "        omni_buffer_write_uint64(response, result);\n"; break;
                case TYPE_FLOAT32: os << "        omni_buffer_write_float32(response, result);\n"; break;
                case TYPE_FLOAT64: os << "        omni_buffer_write_float64(response, result);\n"; break;
                default: break;
                }
            }
        }

        // Cleanup param
        if (m.has_param && m.param.type.isCustom()) {
            std::string ppfx2 = cTypePrefix(m.param.type, pkg_);
            os << "        " << ppfx2 << "_destroy(&" << m.param.name << ");\n";
        }
    }
    if (!svc.methods.empty()) {
        os << "    }\n";
    }
    os << "    omni_buffer_destroy(req);\n";
    os << "}\n\n";

    // _stub_create
    os << "omni_service_t* " << prefix << "_stub_create(const " << prefix << "_callbacks* cbs) {\n";
    os << "    " << prefix << "_stub_data* data = (" << prefix << "_stub_data*)malloc(sizeof(" << prefix << "_stub_data));\n";
    os << "    if (!data) return NULL;\n";
    os << "    data->cbs = *cbs;\n";
    os << "    omni_service_t* svc = omni_service_create(\"" << svc.name << "\",\n";
    os << "        " << prefix << "_INTERFACE_ID, " << prefix << "_on_invoke, data);\n";

    // Add methods
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        std::string upper_name = toSnakeCase(svc.methods[i].name);
        for (size_t j = 0; j < upper_name.size(); ++j) upper_name[j] = toupper(upper_name[j]);
        os << "    omni_service_add_method(svc, " << prefix << "_METHOD_" << upper_name
           << ", \"" << svc.methods[i].name << "\");\n";
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
            if (m.param.type.isCustom()) {
                os << ", const " << cTypeName(m.param.type, pkg_) << "* " << m.param.name;
            } else {
                os << ", " << cTypeName(m.param.type, pkg_) << " " << m.param.name;
            }
        }
        if (!m.return_type.isVoid()) {
            if (m.return_type.isCustom()) {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result";
            } else {
                os << ", " << cTypeName(m.return_type, pkg_) << "* result";
            }
        }
        os << ") {\n";

        os << "    omni_buffer_t* req = omni_buffer_create();\n";
        os << "    omni_buffer_t* resp = omni_buffer_create();\n";

        // Serialize parameter
        if (m.has_param) {
            if (m.param.type.isCustom()) {
                std::string ppfx = cTypePrefix(m.param.type, pkg_);
                os << "    " << ppfx << "_serialize(" << m.param.name << ", req);\n";
            } else {
                switch (m.param.type.primitive) {
                case TYPE_BOOL:    os << "    omni_buffer_write_bool(req, " << m.param.name << ");\n"; break;
                case TYPE_INT8:    os << "    omni_buffer_write_int8(req, " << m.param.name << ");\n"; break;
                case TYPE_UINT8:   os << "    omni_buffer_write_uint8(req, " << m.param.name << ");\n"; break;
                case TYPE_INT16:   os << "    omni_buffer_write_int16(req, " << m.param.name << ");\n"; break;
                case TYPE_UINT16:  os << "    omni_buffer_write_uint16(req, " << m.param.name << ");\n"; break;
                case TYPE_INT32:   os << "    omni_buffer_write_int32(req, " << m.param.name << ");\n"; break;
                case TYPE_UINT32:  os << "    omni_buffer_write_uint32(req, " << m.param.name << ");\n"; break;
                case TYPE_INT64:   os << "    omni_buffer_write_int64(req, " << m.param.name << ");\n"; break;
                case TYPE_UINT64:  os << "    omni_buffer_write_uint64(req, " << m.param.name << ");\n"; break;
                case TYPE_FLOAT32: os << "    omni_buffer_write_float32(req, " << m.param.name << ");\n"; break;
                case TYPE_FLOAT64: os << "    omni_buffer_write_float64(req, " << m.param.name << ");\n"; break;
                default: break;
                }
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
            if (m.return_type.isCustom()) {
                std::string rpfx = cTypePrefix(m.return_type, pkg_);
                os << "        " << rpfx << "_deserialize(result, resp);\n";
            } else {
                switch (m.return_type.primitive) {
                case TYPE_BOOL:    os << "        *result = omni_buffer_read_bool(resp);\n"; break;
                case TYPE_INT8:    os << "        *result = omni_buffer_read_int8(resp);\n"; break;
                case TYPE_UINT8:   os << "        *result = omni_buffer_read_uint8(resp);\n"; break;
                case TYPE_INT16:   os << "        *result = omni_buffer_read_int16(resp);\n"; break;
                case TYPE_UINT16:  os << "        *result = omni_buffer_read_uint16(resp);\n"; break;
                case TYPE_INT32:   os << "        *result = omni_buffer_read_int32(resp);\n"; break;
                case TYPE_UINT32:  os << "        *result = omni_buffer_read_uint32(resp);\n"; break;
                case TYPE_INT64:   os << "        *result = omni_buffer_read_int64(resp);\n"; break;
                case TYPE_UINT64:  os << "        *result = omni_buffer_read_uint64(resp);\n"; break;
                case TYPE_FLOAT32: os << "        *result = omni_buffer_read_float32(resp);\n"; break;
                case TYPE_FLOAT64: os << "        *result = omni_buffer_read_float64(resp);\n"; break;
                default: break;
                }
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
        os << "    " << topic_type << "_deserialize(&msg, buf);\n";
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
