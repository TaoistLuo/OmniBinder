/**************************************************************************************************
 * @file        type_codec.cpp
 * @brief       类型编解码器实现
 *************************************************************************************************/
#include "type_codec.h"
#include <cstdio>
#include <cstring>

namespace type_codec {

const omnic::StructDef* TypeCodec::findStruct(const std::string& name, const std::string& package) {
    // 查找指定包中的结构体定义
    std::map<std::string, omnic::AstFile>::const_iterator it = ctx_.loaded_packages.find(package);
    if (it == ctx_.loaded_packages.end()) {
        return NULL;
    }
    
    const omnic::AstFile& ast = it->second;
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        if (ast.structs[i].name == name) {
            return &ast.structs[i];
        }
    }
    return NULL;
}

bool TypeCodec::encodePrimitive(const simple_json::Value& json, const omnic::TypeRef& type, omnibinder::Buffer& buf) {
    switch (type.primitive) {
    case omnic::TYPE_BOOL:
        buf.writeBool(json.asBool());
        return true;
    case omnic::TYPE_INT8:
        buf.writeInt8(static_cast<int8_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT8:
        buf.writeUint8(static_cast<uint8_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT16:
        buf.writeInt16(static_cast<int16_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT16:
        buf.writeUint16(static_cast<uint16_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT32:
        buf.writeInt32(static_cast<int32_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT32:
        buf.writeUint32(static_cast<uint32_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT64:
        buf.writeInt64(json.asInt64());
        return true;
    case omnic::TYPE_UINT64:
        buf.writeUint64(static_cast<uint64_t>(json.asInt64()));
        return true;
    case omnic::TYPE_FLOAT32:
        buf.writeFloat32(static_cast<float>(json.asNumber()));
        return true;
    case omnic::TYPE_FLOAT64:
        buf.writeFloat64(json.asNumber());
        return true;
    case omnic::TYPE_STRING:
        buf.writeString(json.asString());
        return true;
    case omnic::TYPE_BYTES:
        // TODO: 支持 base64 编码的字节数组
        return false;
    default:
        return false;
    }
}

bool TypeCodec::decodePrimitive(omnibinder::Buffer& buf, const omnic::TypeRef& type, simple_json::Value& json) {
    switch (type.primitive) {
    case omnic::TYPE_BOOL:
        json = simple_json::Value(buf.readBool());
        return true;
    case omnic::TYPE_INT8:
        json = simple_json::Value(static_cast<double>(buf.readInt8()));
        return true;
    case omnic::TYPE_UINT8:
        json = simple_json::Value(static_cast<double>(buf.readUint8()));
        return true;
    case omnic::TYPE_INT16:
        json = simple_json::Value(static_cast<double>(buf.readInt16()));
        return true;
    case omnic::TYPE_UINT16:
        json = simple_json::Value(static_cast<double>(buf.readUint16()));
        return true;
    case omnic::TYPE_INT32:
        json = simple_json::Value(static_cast<double>(buf.readInt32()));
        return true;
    case omnic::TYPE_UINT32:
        json = simple_json::Value(static_cast<double>(buf.readUint32()));
        return true;
    case omnic::TYPE_INT64:
        json = simple_json::Value(static_cast<double>(buf.readInt64()));
        return true;
    case omnic::TYPE_UINT64:
        json = simple_json::Value(static_cast<double>(buf.readUint64()));
        return true;
    case omnic::TYPE_FLOAT32:
        json = simple_json::Value(static_cast<double>(buf.readFloat32()));
        return true;
    case omnic::TYPE_FLOAT64:
        json = simple_json::Value(buf.readFloat64());
        return true;
    case omnic::TYPE_STRING:
        json = simple_json::Value(buf.readString());
        return true;
    case omnic::TYPE_BYTES:
        // TODO: 支持 base64 编码的字节数组
        return false;
    default:
        return false;
    }
}

bool TypeCodec::encodeStruct(const simple_json::Value& json, const omnic::StructDef& structDef,
                             const std::string& package, omnibinder::Buffer& buf) {
    if (!json.isObject()) {
        fprintf(stderr, "Error: Expected JSON object for struct %s\n", structDef.name.c_str());
        return false;
    }
    
    // 按字段顺序编码
    for (size_t i = 0; i < structDef.fields.size(); ++i) {
        const omnic::FieldDef& field = structDef.fields[i];
        
        if (!json.has(field.name)) {
            fprintf(stderr, "Error: Missing field '%s' in struct %s\n", 
                    field.name.c_str(), structDef.name.c_str());
            return false;
        }
        
        const simple_json::Value& fieldValue = json.get(field.name);
        
        // 确定字段所属的包
        std::string fieldPackage = field.type.package_name.empty() ?
                                   package : field.type.package_name;
        
        if (!encodeToBuffer(fieldValue, field.type, fieldPackage, buf)) {
            return false;
        }
    }
    
    return true;
}

bool TypeCodec::decodeStruct(omnibinder::Buffer& buf, const omnic::StructDef& structDef,
                             const std::string& package, simple_json::Value& json) {
    json.setObject();
    
    // 按字段顺序解码
    for (size_t i = 0; i < structDef.fields.size(); ++i) {
        const omnic::FieldDef& field = structDef.fields[i];
        simple_json::Value fieldValue;
        
        // 确定字段所属的包
        std::string fieldPackage = field.type.package_name.empty() ?
                                   package : field.type.package_name;
        
        if (!decodeFromBuffer(buf, field.type, fieldPackage, fieldValue)) {
            return false;
        }
        
        json.set(field.name, fieldValue);
    }
    
    return true;
}

bool TypeCodec::encodeArray(const simple_json::Value& json, const omnic::TypeRef& elementType,
                            const std::string& package, omnibinder::Buffer& buf) {
    if (!json.isArray()) {
        fprintf(stderr, "Error: Expected JSON array\n");
        return false;
    }
    
    // 写入数组长度
    uint32_t count = static_cast<uint32_t>(json.size());
    buf.writeUint32(count);
    
    // 编码每个元素
    for (size_t i = 0; i < json.size(); ++i) {
        if (!encodeToBuffer(json[i], elementType, package, buf)) {
            return false;
        }
    }
    
    return true;
}

bool TypeCodec::decodeArray(omnibinder::Buffer& buf, const omnic::TypeRef& elementType,
                            const std::string& package, simple_json::Value& json) {
    json.setArray();
    
    // 读取数组长度
    uint32_t count = buf.readUint32();
    
    // 解码每个元素
    for (uint32_t i = 0; i < count; ++i) {
        simple_json::Value element;
        if (!decodeFromBuffer(buf, elementType, package, element)) {
            return false;
        }
        json.push(element);
    }
    
    return true;
}

bool TypeCodec::encodeToBuffer(const simple_json::Value& json, const omnic::TypeRef& type,
                                const std::string& package, omnibinder::Buffer& buf) {
    if (type.primitive == omnic::TYPE_CUSTOM) {
        // 自定义结构体
        std::string structPackage = type.package_name.empty() ? package : type.package_name;
        const omnic::StructDef* structDef = findStruct(type.custom_name, structPackage);
        if (!structDef) {
            fprintf(stderr, "Error: Cannot find struct %s in package %s\n",
                    type.custom_name.c_str(), structPackage.c_str());
            return false;
        }
        return encodeStruct(json, *structDef, structPackage, buf);
    } else if (type.primitive == omnic::TYPE_ARRAY) {
        // 数组类型
        if (!type.element_type) {
            fprintf(stderr, "Error: Array type missing element type\n");
            return false;
        }
        return encodeArray(json, *type.element_type, package, buf);
    } else {
        // 基础类型
        return encodePrimitive(json, type, buf);
    }
}

bool TypeCodec::decodeFromBuffer(omnibinder::Buffer& buf, const omnic::TypeRef& type,
                                  const std::string& package, simple_json::Value& json) {
    if (type.primitive == omnic::TYPE_CUSTOM) {
        // 自定义结构体
        std::string structPackage = type.package_name.empty() ? package : type.package_name;
        const omnic::StructDef* structDef = findStruct(type.custom_name, structPackage);
        if (!structDef) {
            fprintf(stderr, "Error: Cannot find struct %s in package %s\n",
                    type.custom_name.c_str(), structPackage.c_str());
            return false;
        }
        return decodeStruct(buf, *structDef, structPackage, json);
    } else if (type.primitive == omnic::TYPE_ARRAY) {
        // 数组类型
        if (!type.element_type) {
            fprintf(stderr, "Error: Array type missing element type\n");
            return false;
        }
        return decodeArray(buf, *type.element_type, package, json);
    } else {
        // 基础类型
        return decodePrimitive(buf, type, json);
    }
}

simple_json::Value TypeCodec::generateSchema(const omnic::TypeRef& type, const std::string& package) {
    simple_json::Value schema;
    schema.setObject();
    
    if (type.primitive == omnic::TYPE_CUSTOM) {
        // 自定义结构体 - 展开字段
        std::string structPackage = type.package_name.empty() ? package : type.package_name;
        const omnic::StructDef* structDef = findStruct(type.custom_name, structPackage);
        if (structDef) {
            for (size_t i = 0; i < structDef->fields.size(); ++i) {
                const omnic::FieldDef& field = structDef->fields[i];
                simple_json::Value fieldSchema = generateSchema(field.type, structPackage);
                schema.set(field.name, fieldSchema);
            }
        }
    } else if (type.primitive == omnic::TYPE_ARRAY) {
        // 数组类型
        schema.set("type", simple_json::Value("array"));
        if (type.element_type) {
            schema.set("element", generateSchema(*type.element_type, package));
        }
    } else {
        // 基础类型 - 返回类型名
        std::string typeName;
        switch (type.primitive) {
        case omnic::TYPE_BOOL: typeName = "bool"; break;
        case omnic::TYPE_INT8: typeName = "int8"; break;
        case omnic::TYPE_UINT8: typeName = "uint8"; break;
        case omnic::TYPE_INT16: typeName = "int16"; break;
        case omnic::TYPE_UINT16: typeName = "uint16"; break;
        case omnic::TYPE_INT32: typeName = "int32"; break;
        case omnic::TYPE_UINT32: typeName = "uint32"; break;
        case omnic::TYPE_INT64: typeName = "int64"; break;
        case omnic::TYPE_UINT64: typeName = "uint64"; break;
        case omnic::TYPE_FLOAT32: typeName = "float32"; break;
        case omnic::TYPE_FLOAT64: typeName = "float64"; break;
        case omnic::TYPE_STRING: typeName = "string"; break;
        case omnic::TYPE_BYTES: typeName = "bytes"; break;
        case omnic::TYPE_VOID: typeName = "void"; break;
        default: typeName = "unknown"; break;
        }
        schema.set("type", simple_json::Value(typeName));
    }
    
    return schema;
}

} // namespace type_codec
