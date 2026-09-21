/**************************************************************************************************
 * @file        type_codec.cpp
 * @brief       类型编解码器实现
 *************************************************************************************************/
#include "type_codec.h"
#include "type_resolver.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace type_codec {

namespace {

bool checkIntegral(const simple_json::Value& json, double& out_value, const char* type_name) {
    if (!json.isNumber() || !std::isfinite(json.asNumber())) {
        fprintf(stderr, "Error: Expected finite numeric value for %s\n", type_name);
        return false;
    }
    out_value = json.asNumber();
    if (std::floor(out_value) != out_value) {
        fprintf(stderr, "Error: Non-integer value for %s\n", type_name);
        return false;
    }
    return true;
}

bool checkIntegerRange(const simple_json::Value& json, double min_value, double max_value,
                       const char* type_name) {
    double value = 0.0;
    if (!checkIntegral(json, value, type_name)) return false;
    if (value < min_value || value > max_value) {
        fprintf(stderr, "Error: Value out of range for %s\n", type_name);
        return false;
    }
    return true;
}

bool checkFiniteNumber(const simple_json::Value& json, const char* type_name) {
    if (!json.isNumber() || !std::isfinite(json.asNumber())) {
        fprintf(stderr, "Error: Expected finite numeric value for %s\n", type_name);
        return false;
    }
    return true;
}

bool checkIntegerRangeSafe(const simple_json::Value& json, double min_inclusive,
                           double max_exclusive, const char* type_name) {
    double value = 0.0;
    if (!checkIntegral(json, value, type_name)) return false;
    if (value < min_inclusive || value >= max_exclusive) {
        fprintf(stderr, "Error: Value out of range for %s\n", type_name);
        return false;
    }
    return true;
}

} // namespace

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
        if (!checkIntegerRange(json, -128.0, 127.0, "int8")) return false;
        buf.writeInt8(static_cast<int8_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT8:
        if (!checkIntegerRange(json, 0.0, 255.0, "uint8")) return false;
        buf.writeUint8(static_cast<uint8_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT16:
        if (!checkIntegerRange(json, -32768.0, 32767.0, "int16")) return false;
        buf.writeInt16(static_cast<int16_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT16:
        if (!checkIntegerRange(json, 0.0, 65535.0, "uint16")) return false;
        buf.writeUint16(static_cast<uint16_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT32:
        if (!checkIntegerRange(json, -2147483648.0, 2147483647.0, "int32")) return false;
        buf.writeInt32(static_cast<int32_t>(json.asInt64()));
        return true;
    case omnic::TYPE_UINT32:
        if (!checkIntegerRange(json, 0.0, 4294967295.0, "uint32")) return false;
        buf.writeUint32(static_cast<uint32_t>(json.asInt64()));
        return true;
    case omnic::TYPE_INT64: {
        if (!checkFiniteNumber(json, "int64")) return false;
        if (json.numberKind() == simple_json::NUM_UINT) {
            if (json.asUint64() > static_cast<uint64_t>(INT64_MAX)) {
                fprintf(stderr, "Error: Value out of range for int64\n");
                return false;
            }
        } else if (json.numberKind() == simple_json::NUM_FP) {
            if (!checkIntegerRangeSafe(json, -9223372036854775808.0, 9223372036854775808.0, "int64")) {
                return false;
            }
        }
        buf.writeInt64(json.asInt64());
        return true;
    }
    case omnic::TYPE_UINT64: {
        if (!checkFiniteNumber(json, "uint64")) return false;
        if (json.numberKind() == simple_json::NUM_INT) {
            if (json.asInt64() < 0) {
                fprintf(stderr, "Error: Value out of range for uint64\n");
                return false;
            }
        } else if (json.numberKind() == simple_json::NUM_FP) {
            if (!checkIntegerRangeSafe(json, 0.0, 18446744073709551616.0, "uint64")) {
                return false;
            }
        }
        buf.writeUint64(json.asUint64());
        return true;
    }
    case omnic::TYPE_FLOAT32:
        if (!checkFiniteNumber(json, "float32")) return false;
        buf.writeFloat32(static_cast<float>(json.asNumber()));
        return true;
    case omnic::TYPE_FLOAT64:
        if (!checkFiniteNumber(json, "float64")) return false;
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
        fprintf(stderr, "Error: bytes value is only supported as a hex string parameter\n");
        return false;
    default:
        return false;
    }
}

bool TypeCodec::decodePrimitive(omnibinder::Buffer& buf, const omnic::TypeRef& type, simple_json::Value& json) {
    switch (type.primitive) {
    case omnic::TYPE_BOOL: {
        bool value = false;
        if (!buf.tryReadBool(value)) return false;
        json = simple_json::Value(value);
        return true;
    }
    case omnic::TYPE_INT8: {
        int8_t value = 0;
        if (!buf.tryReadInt8(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_UINT8: {
        uint8_t value = 0;
        if (!buf.tryReadUint8(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_INT16: {
        int16_t value = 0;
        if (!buf.tryReadInt16(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_UINT16: {
        uint16_t value = 0;
        if (!buf.tryReadUint16(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_INT32: {
        int32_t value = 0;
        if (!buf.tryReadInt32(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_UINT32: {
        uint32_t value = 0;
        if (!buf.tryReadUint32(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_INT64: {
        int64_t value = 0;
        if (!buf.tryReadInt64(value)) return false;
        json = simple_json::Value(value);
        return true;
    }
    case omnic::TYPE_UINT64: {
        uint64_t value = 0;
        if (!buf.tryReadUint64(value)) return false;
        json = simple_json::Value(value);
        return true;
    }
    case omnic::TYPE_FLOAT32: {
        float value = 0.0f;
        if (!buf.tryReadFloat32(value)) return false;
        json = simple_json::Value(static_cast<double>(value));
        return true;
    }
    case omnic::TYPE_FLOAT64: {
        double value = 0.0;
        if (!buf.tryReadFloat64(value)) return false;
        json = simple_json::Value(value);
        return true;
    }
    case omnic::TYPE_STRING: {
        std::string value;
        if (!buf.tryReadString(value)) return false;
        json = simple_json::Value(value);
        return true;
    }
    case omnic::TYPE_BYTES:
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
            fprintf(stderr, "Error: Failed to encode field '%s' of struct %s\n",
                    field.name.c_str(), structDef.name.c_str());
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
    uint32_t count = 0;
    if (!buf.tryReadUint32(count)) {
        return false;
    }
    if (static_cast<size_t>(count) > omnibinder::MAX_ARRAY_ELEMENTS) {
        fprintf(stderr, "Error: Array element count %u exceeds limit\n", count);
        return false;
    }
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

} // namespace type_codec
