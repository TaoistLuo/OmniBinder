#include "codegen_cpp.h"
#include <fstream>
#include <iomanip>

namespace omnic {

std::string cppTypeName(const TypeRef& type) {
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
            return "std::vector<" + cppTypeName(*type.element_type) + ">";
        }
        return "std::vector<void*>";
    default: return "void";
    }
}

bool isReferenceType(const TypeRef& type) {
    return type.primitive == TYPE_STRING || type.primitive == TYPE_BYTES ||
           type.primitive == TYPE_CUSTOM || type.primitive == TYPE_ARRAY;
}

// Maps primitive type to Buffer read/write method suffix (e.g., "Int32", "String")
std::string bufferMethodSuffix(const TypeRef& type) {
    switch (type.primitive) {
    case TYPE_BOOL:    return "Bool";
    case TYPE_INT8:    return "Int8";
    case TYPE_UINT8:   return "Uint8";
    case TYPE_INT16:   return "Int16";
    case TYPE_UINT16:  return "Uint16";
    case TYPE_INT32:   return "Int32";
    case TYPE_UINT32:  return "Uint32";
    case TYPE_INT64:   return "Int64";
    case TYPE_UINT64:  return "Uint64";
    case TYPE_FLOAT32: return "Float32";
    case TYPE_FLOAT64: return "Float64";
    case TYPE_STRING:  return "String";
    case TYPE_BYTES:   return "Bytes";
    default: return "";
    }
}

bool CppCodeGen::generate(const AstFile& ast, const std::string& output_dir,
                          const std::string& filename) {
    pkg_ = ast.package_name;
    
    std::string header_path = output_dir + "/" + filename + ".h";
    std::string source_path = output_dir + "/" + filename + ".cpp";
    
    std::ofstream hdr(header_path.c_str());
    std::ofstream src(source_path.c_str());
    
    if (!hdr.is_open() || !src.is_open()) return false;
    
    generateHeader(ast, hdr, filename);
    generateSource(ast, src, filename);
    return true;
}

void CppCodeGen::generateHeader(const AstFile& ast, std::ostream& os, const std::string& filename) {
    std::string guard = filename;
    for (size_t i = 0; i < guard.size(); ++i) {
        if (guard[i] == '.') guard[i] = '_';
        guard[i] = toupper(guard[i]);
    }
    guard += "_H";
    
    os << "#ifndef " << guard << "\n";
    os << "#define " << guard << "\n\n";
    os << "#include <omnibinder/omnibinder.h>\n";
    os << "#include <string>\n#include <vector>\n#include <functional>\n\n";
    
    // 生成被导入文件的 #include
    for (size_t i = 0; i < ast.imports.size(); ++i) {
        // 从 import 路径提取文件名
        std::string imp = ast.imports[i];
        size_t slash = imp.find_last_of("/\\");
        if (slash != std::string::npos) imp = imp.substr(slash + 1);
        os << "#include \"" << imp << ".h\"\n";
    }
    if (!ast.imports.empty()) os << "\n";
    
    os << "namespace " << pkg_ << " {\n\n";
    
    for (size_t i = 0; i < ast.structs.size(); ++i) genStruct(ast.structs[i], os);
    for (size_t i = 0; i < ast.topics.size(); ++i) genTopic(ast.topics[i], os);
    for (size_t i = 0; i < ast.services.size(); ++i) {
        genStub(ast.services[i], ast, os);
        genProxy(ast.services[i], ast, os);
    }
    
    os << "} // namespace " << pkg_ << "\n\n";
    os << "#endif // " << guard << "\n";
}

void CppCodeGen::genStruct(const StructDef& s, std::ostream& os) {
    os << "struct " << s.name << " {\n";
    for (size_t i = 0; i < s.fields.size(); ++i) {
        os << "    " << cppTypeName(s.fields[i].type) << " " << s.fields[i].name << ";\n";
    }
    os << "\n    " << s.name << "()";
    bool first = true;
    for (size_t i = 0; i < s.fields.size(); ++i) {
        if (!isReferenceType(s.fields[i].type)) {
            os << (first ? " : " : ", ") << s.fields[i].name << "(0)";
            first = false;
        }
    }
    os << " {}\n";
    os << "    bool serialize(omnibinder::Buffer& buf) const;\n";
    os << "    bool deserialize(omnibinder::Buffer& buf);\n";
    os << "};\n\n";
}

void CppCodeGen::genTopic(const TopicDef& t, std::ostream& os) {
    os << "struct " << t.name << " {\n";
    for (size_t i = 0; i < t.fields.size(); ++i) {
        os << "    " << cppTypeName(t.fields[i].type) << " " << t.fields[i].name << ";\n";
    }
    os << "\n    static const uint32_t TOPIC_ID = 0x" << std::hex << fnv1a_hash(pkg_ + "." + t.name) << std::dec << "u;\n";
    os << "    bool serialize(omnibinder::Buffer& buf) const;\n";
    os << "    bool deserialize(omnibinder::Buffer& buf);\n";
    os << "};\n\n";
}

void CppCodeGen::genStub(const ServiceDef& svc, const AstFile& ast, std::ostream& os) {
    uint32_t iface_id = fnv1a_hash(pkg_ + "." + svc.name);
    
    os << "class " << svc.name << "Stub : public omnibinder::Service {\n";
    os << "public:\n";
    os << "    " << svc.name << "Stub() : Service(\"" << svc.name << "\") {}\n";
    os << "    virtual ~" << svc.name << "Stub() {}\n\n";
    os << "    const char* serviceName() const override { return \"" << svc.name << "\"; }\n";
    os << "    const omnibinder::InterfaceInfo& interfaceInfo() const override;\n\n";
    
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        os << "    virtual " << cppTypeName(m.return_type) << " " << m.name << "(";
        if (m.has_param) {
            if (isReferenceType(m.param.type)) {
                os << "const " << cppTypeName(m.param.type) << "& " << m.param.name;
            } else {
                os << cppTypeName(m.param.type) << " " << m.param.name;
            }
        }
        os << ") = 0;\n";
    }
    
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        os << "    void Broadcast" << svc.publishes[i] << "(const " << svc.publishes[i] << "& msg);\n";
    }
    
    os << "\nprotected:\n";
    os << "    void onInvoke(uint32_t method_id, const omnibinder::Buffer& request, omnibinder::Buffer& response) override;\n";
    os << "};\n\n";
}

void CppCodeGen::genProxy(const ServiceDef& svc, const AstFile& ast, std::ostream& os) {
    os << "class " << svc.name << "Proxy {\n";
    os << "public:\n";
    os << "    explicit " << svc.name << "Proxy(omnibinder::OmniRuntime& runtime);\n";
    os << "    ~" << svc.name << "Proxy();\n";
    os << "    int connect();\n";
    os << "    void disconnect();\n";
    os << "    bool isConnected() const;\n\n";
    
    for (size_t i = 0; i < svc.methods.size(); ++i) {
        const MethodDef& m = svc.methods[i];
        os << "    " << cppTypeName(m.return_type) << " " << m.name << "(";
        if (m.has_param) {
            if (isReferenceType(m.param.type)) {
                os << "const " << cppTypeName(m.param.type) << "& " << m.param.name;
            } else {
                os << cppTypeName(m.param.type) << " " << m.param.name;
            }
        }
        os << ");\n";
    }
    
    for (size_t i = 0; i < svc.publishes.size(); ++i) {
        os << "    void Subscribe" << svc.publishes[i] << "(const std::function<void(const " << svc.publishes[i] << "&)>& callback);\n";
    }
    os << "    void OnServiceDied(const std::function<void()>& callback);\n";
    
    os << "\nprivate:\n";
    os << "    omnibinder::OmniRuntime& runtime_;\n";
    os << "    bool connected_;\n";
    os << "    std::function<void()> death_callback_;\n";
    os << "};\n\n";
}

void CppCodeGen::generateSource(const AstFile& ast, std::ostream& os, const std::string& filename) {
    os << "#include \"" << filename << ".h\"\n\n";
    os << "namespace " << pkg_ << " {\n\n";
    
    // Struct serialize/deserialize
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        const StructDef& s = ast.structs[i];
        os << "bool " << s.name << "::serialize(omnibinder::Buffer& buf) const {\n";
        genSerialize(s.fields, "", os);
        os << "    return true;\n}\n\n";
        
        os << "bool " << s.name << "::deserialize(omnibinder::Buffer& buf) {\n";
        os << "    try {\n";
        genDeserialize(s.fields, "", os);
        os << "        return true;\n";
        os << "    } catch (...) { return false; }\n}\n\n";
    }
    
    // Topic serialize/deserialize
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        const TopicDef& t = ast.topics[i];
        os << "bool " << t.name << "::serialize(omnibinder::Buffer& buf) const {\n";
        genSerialize(t.fields, "", os);
        os << "    return true;\n}\n\n";
        
        os << "bool " << t.name << "::deserialize(omnibinder::Buffer& buf) {\n";
        os << "    try {\n";
        genDeserialize(t.fields, "", os);
        os << "        return true;\n";
        os << "    } catch (...) { return false; }\n}\n\n";
    }
    
    // Service Stub implementation
    for (size_t si = 0; si < ast.services.size(); ++si) {
        const ServiceDef& svc = ast.services[si];
        uint32_t iface_id = fnv1a_hash(pkg_ + "." + svc.name);
        
        os << "static omnibinder::InterfaceInfo s_" << svc.name << "_info;\n";
        os << "static bool s_" << svc.name << "_info_init = false;\n\n";
        
        os << "const omnibinder::InterfaceInfo& " << svc.name << "Stub::interfaceInfo() const {\n";
        os << "    if (!s_" << svc.name << "_info_init) {\n";
        os << "        s_" << svc.name << "_info.interface_id = 0x" << std::hex << iface_id << std::dec << "u;\n";
        os << "        s_" << svc.name << "_info.name = \"" << svc.name << "\";\n";
        for (size_t mi = 0; mi < svc.methods.size(); ++mi) {
            const MethodDef& m = svc.methods[mi];
            uint32_t mid = fnv1a_hash(m.name);
            
            // 生成参数类型字符串
            std::string param_type_str = m.has_param ? cppTypeName(m.param.type) : "";
            // 生成返回类型字符串
            std::string return_type_str = cppTypeName(m.return_type);
            
            os << "        s_" << svc.name << "_info.methods.push_back(omnibinder::MethodInfo(0x" 
               << std::hex << mid << std::dec << "u, \"" << m.name << "\", \""
               << param_type_str << "\", \"" << return_type_str << "\"));\n";
        }
        os << "        s_" << svc.name << "_info_init = true;\n";
        os << "    }\n    return s_" << svc.name << "_info;\n}\n\n";
        
        os << "void " << svc.name << "Stub::onInvoke(uint32_t method_id, const omnibinder::Buffer& request, omnibinder::Buffer& response) {\n";
        os << "    omnibinder::Buffer req(request.data(), request.size());\n";
        os << "    switch (method_id) {\n";
        for (size_t mi = 0; mi < svc.methods.size(); ++mi) {
            const MethodDef& m = svc.methods[mi];
            uint32_t mid = fnv1a_hash(m.name);
            os << "    case 0x" << std::hex << mid << std::dec << "u: {\n";
            if (m.has_param) {
                os << "        " << cppTypeName(m.param.type) << " " << m.param.name << ";\n";
                if (m.param.type.primitive == TYPE_CUSTOM) {
                    os << "        " << m.param.name << ".deserialize(req);\n";
                } else {
                    os << "        " << m.param.name << " = req.read" << bufferMethodSuffix(m.param.type) << "();\n";
                }
            }
            if (!m.return_type.isVoid()) {
                os << "        " << cppTypeName(m.return_type) << " result = " << m.name << "(";
            } else {
                os << "        " << m.name << "(";
            }
            if (m.has_param) os << m.param.name;
            os << ");\n";
            if (!m.return_type.isVoid()) {
                if (m.return_type.primitive == TYPE_CUSTOM) {
                    os << "        result.serialize(response);\n";
                } else {
                    os << "        response.write" << bufferMethodSuffix(m.return_type) << "(result);\n";
                }
            }
            os << "        break;\n    }\n";
        }
        os << "    default: break;\n    }\n}\n\n";
        
        for (size_t pi = 0; pi < svc.publishes.size(); ++pi) {
            const std::string& topic = svc.publishes[pi];
            uint32_t tid = fnv1a_hash(topic);
            os << "void " << svc.name << "Stub::Broadcast" << topic << "(const " << topic << "& msg) {\n";
            os << "    omnibinder::Buffer buf;\n";
            os << "    msg.serialize(buf);\n";
            os << "    if (runtime()) runtime()->broadcast(0x" << std::hex << tid << std::dec << "u, buf);\n";
            os << "}\n\n";
        }
        
        // Proxy implementation
os << svc.name << "Proxy::" << svc.name << "Proxy(omnibinder::OmniRuntime& runtime)\n";
        os << "    : runtime_(runtime), connected_(false) {}\n\n";
        os << svc.name << "Proxy::~" << svc.name << "Proxy() { disconnect(); }\n\n";
        os << "int " << svc.name << "Proxy::connect() {\n";
        os << "    omnibinder::ServiceInfo info;\n";
        os << "    int ret = runtime_.lookupService(\"" << svc.name << "\", info);\n";
        os << "    if (ret == 0) connected_ = true;\n";
        os << "    return ret;\n}\n\n";
        os << "void " << svc.name << "Proxy::disconnect() { connected_ = false; }\n";
        os << "bool " << svc.name << "Proxy::isConnected() const { return connected_; }\n\n";
        
        for (size_t mi = 0; mi < svc.methods.size(); ++mi) {
            const MethodDef& m = svc.methods[mi];
            uint32_t mid = fnv1a_hash(m.name);
            os << cppTypeName(m.return_type) << " " << svc.name << "Proxy::" << m.name << "(";
            if (m.has_param) {
                if (isReferenceType(m.param.type)) {
                    os << "const " << cppTypeName(m.param.type) << "& " << m.param.name;
                } else {
                    os << cppTypeName(m.param.type) << " " << m.param.name;
                }
            }
            os << ") {\n";
            os << "    omnibinder::Buffer req, resp;\n";
            if (m.has_param) {
                if (m.param.type.primitive == TYPE_CUSTOM) {
                    os << "    " << m.param.name << ".serialize(req);\n";
                } else {
                    os << "    req.write" << bufferMethodSuffix(m.param.type) << "(" << m.param.name << ");\n";
                }
            }
            os << "    runtime_.invoke(\"" << svc.name << "\", 0x" << std::hex << iface_id << std::dec << "u, 0x" << std::hex << mid << std::dec << "u, req, resp, 0);\n";
            if (!m.return_type.isVoid()) {
                os << "    " << cppTypeName(m.return_type) << " result;\n";
                if (m.return_type.primitive == TYPE_CUSTOM) {
                    os << "    result.deserialize(resp);\n";
                } else {
                    os << "    result = resp.read" << bufferMethodSuffix(m.return_type) << "();\n";
                }
                os << "    return result;\n";
            }
            os << "}\n\n";
        }
        
        for (size_t pi = 0; pi < svc.publishes.size(); ++pi) {
            const std::string& topic = svc.publishes[pi];
            os << "void " << svc.name << "Proxy::Subscribe" << topic << "(const std::function<void(const " << topic << "&)>& callback) {\n";
            os << "    runtime_.subscribeTopic(\"" << topic << "\", [callback](uint32_t, const omnibinder::Buffer& data) {\n";
            os << "        " << topic << " msg;\n";
            os << "        omnibinder::Buffer buf(data.data(), data.size());\n";
            os << "        msg.deserialize(buf);\n";
            os << "        callback(msg);\n";
            os << "    });\n}\n\n";
        }
        
        os << "void " << svc.name << "Proxy::OnServiceDied(const std::function<void()>& callback) {\n";
        os << "    death_callback_ = callback;\n";
        os << "    runtime_.subscribeServiceDeath(\"" << svc.name << "\", [this](const std::string&) {\n";
        os << "        connected_ = false;\n";
        os << "        if (death_callback_) death_callback_();\n";
        os << "    });\n}\n\n";
    }
    
    os << "} // namespace " << pkg_ << "\n";
}

void CppCodeGen::genSerialize(const std::vector<FieldDef>& fields, const std::string& obj, std::ostream& os) {
    for (size_t i = 0; i < fields.size(); ++i) {
        const FieldDef& f = fields[i];
        std::string name = obj.empty() ? f.name : obj + "." + f.name;
        switch (f.type.primitive) {
        case TYPE_BOOL:    os << "    buf.writeBool(" << name << ");\n"; break;
        case TYPE_INT8:    os << "    buf.writeInt8(" << name << ");\n"; break;
        case TYPE_UINT8:   os << "    buf.writeUint8(" << name << ");\n"; break;
        case TYPE_INT16:   os << "    buf.writeInt16(" << name << ");\n"; break;
        case TYPE_UINT16:  os << "    buf.writeUint16(" << name << ");\n"; break;
        case TYPE_INT32:   os << "    buf.writeInt32(" << name << ");\n"; break;
        case TYPE_UINT32:  os << "    buf.writeUint32(" << name << ");\n"; break;
        case TYPE_INT64:   os << "    buf.writeInt64(" << name << ");\n"; break;
        case TYPE_UINT64:  os << "    buf.writeUint64(" << name << ");\n"; break;
        case TYPE_FLOAT32: os << "    buf.writeFloat32(" << name << ");\n"; break;
        case TYPE_FLOAT64: os << "    buf.writeFloat64(" << name << ");\n"; break;
        case TYPE_STRING:  os << "    buf.writeString(" << name << ");\n"; break;
        case TYPE_BYTES:   os << "    buf.writeBytes(" << name << ");\n"; break;
        case TYPE_CUSTOM:  os << "    " << name << ".serialize(buf);\n"; break;
        case TYPE_ARRAY:
            os << "    buf.writeUint32(static_cast<uint32_t>(" << name << ".size()));\n";
            os << "    for (size_t i = 0; i < " << name << ".size(); ++i) {\n";
            if (f.type.element_type && f.type.element_type->primitive == TYPE_CUSTOM) {
                os << "        " << name << "[i].serialize(buf);\n";
            } else {
                os << "        buf.write" << cppTypeName(*f.type.element_type) << "(" << name << "[i]);\n";
            }
            os << "    }\n";
            break;
        default: break;
        }
    }
}

void CppCodeGen::genDeserialize(const std::vector<FieldDef>& fields, const std::string& obj, std::ostream& os) {
    for (size_t i = 0; i < fields.size(); ++i) {
        const FieldDef& f = fields[i];
        std::string name = obj.empty() ? f.name : obj + "." + f.name;
        switch (f.type.primitive) {
        case TYPE_BOOL:    os << "        " << name << " = buf.readBool();\n"; break;
        case TYPE_INT8:    os << "        " << name << " = buf.readInt8();\n"; break;
        case TYPE_UINT8:   os << "        " << name << " = buf.readUint8();\n"; break;
        case TYPE_INT16:   os << "        " << name << " = buf.readInt16();\n"; break;
        case TYPE_UINT16:  os << "        " << name << " = buf.readUint16();\n"; break;
        case TYPE_INT32:   os << "        " << name << " = buf.readInt32();\n"; break;
        case TYPE_UINT32:  os << "        " << name << " = buf.readUint32();\n"; break;
        case TYPE_INT64:   os << "        " << name << " = buf.readInt64();\n"; break;
        case TYPE_UINT64:  os << "        " << name << " = buf.readUint64();\n"; break;
        case TYPE_FLOAT32: os << "        " << name << " = buf.readFloat32();\n"; break;
        case TYPE_FLOAT64: os << "        " << name << " = buf.readFloat64();\n"; break;
        case TYPE_STRING:  os << "        " << name << " = buf.readString();\n"; break;
        case TYPE_BYTES:   os << "        " << name << " = buf.readBytes();\n"; break;
        case TYPE_CUSTOM:  os << "        " << name << ".deserialize(buf);\n"; break;
        case TYPE_ARRAY:
            os << "        { uint32_t cnt = buf.readUint32(); " << name << ".resize(cnt);\n";
            os << "          for (uint32_t i = 0; i < cnt; ++i) {\n";
            if (f.type.element_type && f.type.element_type->primitive == TYPE_CUSTOM) {
                os << "            " << name << "[i].deserialize(buf);\n";
            } else {
                os << "            " << name << "[i] = buf.read" << cppTypeName(*f.type.element_type) << "();\n";
            }
            os << "          }}\n";
            break;
        default: break;
        }
    }
}

} // namespace omnic
