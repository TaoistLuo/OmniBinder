/**************************************************************************************************
 * @file        type_codec.cpp
 * @brief       类型编解码器实现
 *************************************************************************************************/
#include "type_codec.h"
#include "type_resolver.h"
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
        if (!json.isBool()) {
            fprintf(stderr, "Error: Expected bool value\n");
            return false;
        }
        buf.writeBool(json.asBool());
        return true;
    case omnic::TYPE_INT8:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for int8\n");
            return false;
        }
        buf.writeInt8(static_cast<int8_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT8:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for uint8\n");
            return false;
        }
        buf.writeUint8(static_cast<uint8_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT16:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for int16\n");
            return false;
        }
        buf.writeInt16(static_cast<int16_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT16:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for uint16\n");
            return false;
        }
        buf.writeUint16(static_cast<uint16_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT32:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for int32\n");
            return false;
        }
        buf.writeInt32(static_cast<int32_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT32:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for uint32\n");
            return false;
        }
        buf.writeUint32(static_cast<uint32_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT64:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for int64\n");
            return false;
        }
        buf.writeInt64(json.asInt64());
        return true;
    case omnic::TYPE_UINT64:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for uint64\n");
            return false;
        }
        buf.writeUint64(static_cast<uint64_t>(json.asInt64()));
        return true;
    case omnic::TYPE_FLOAT32:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for float32\n");
            return false;
        }
        buf.writeFloat32(static_cast<float>(json.asNumber()));
        return true;
    case omnic::TYPE_FLOAT64:
        if (!json.isNumber()) {
            fprintf(stderr, "Error: Expected numeric value for float64\n");
            return false;
        }
        buf.writeFloat64(json.asNumber());
        return true;
    case omnic::TYPE_STRING:
        if (!json.isString()) {
            fprintf(stderr, "Error: Expected string value\n");
            return false;
        }
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
    try {
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
            return false;
        default:
            return false;
        }
    } catch (...) {
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
    try {
        json.setArray();
        uint32_t count = buf.readUint32();
        for (uint32_t i = 0; i < count; ++i) {
            simple_json::Value element;
            if (!decodeFromBuffer(buf, elementType, package, element)) {
                return false;
            }
            json.push(element);
        }
        return true;
    } catch (...) {
        return false;
    }
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
        schema.set("type", simple_json::Value(omni_cli::primitiveTypeName(type.primitive)));
    }
    
    return schema;
}

} // namespace type_codec
