/**************************************************************************************************
 * @file        type_codec.h
 * @brief       类型编解码器 — JSON ↔ Buffer 互转
 * @details     基于 IDL AST 定义，实现 JSON 到 Buffer 的编码和 Buffer 到 JSON 的解码。
 *              支持基础类型、结构体、数组和嵌套类型的双向转换。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-03-05
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef TYPE_CODEC_H
#define TYPE_CODEC_H

#include "simple_json.h"
#include "ast.h"
#include "parser.h"
#include <omnibinder/buffer.h>
#include <string>
#include <map>

namespace type_codec {

class TypeCodec {
public:
    TypeCodec(const omnic::ParseContext& ctx) : ctx_(ctx) {}
    
    // 将 JSON 编码为 Buffer（用于 call 命令的输入）
    bool encodeToBuffer(const simple_json::Value& json, const omnic::TypeRef& type, 
                        const std::string& package, omnibinder::Buffer& buf);
    
    // 将 Buffer 解码为 JSON（用于 call 命令的输出）
    bool decodeFromBuffer(omnibinder::Buffer& buf, const omnic::TypeRef& type,
                          const std::string& package, simple_json::Value& json);
    
    // 生成类型的 JSON schema 描述（用于 info 命令）
    simple_json::Value generateSchema(const omnic::TypeRef& type, const std::string& package);
    
private:
    const omnic::ParseContext& ctx_;
    
    // 查找结构体定义
    const omnic::StructDef* findStruct(const std::string& name, const std::string& package);
    
    // 编码基础类型
    bool encodePrimitive(const simple_json::Value& json, const omnic::TypeRef& type, omnibinder::Buffer& buf);
    
    // 解码基础类型
    bool decodePrimitive(omnibinder::Buffer& buf, const omnic::TypeRef& type, simple_json::Value& json);
    
    // 编码自定义结构体
    bool encodeStruct(const simple_json::Value& json, const omnic::StructDef& structDef,
                      const std::string& package, omnibinder::Buffer& buf);
    
    // 解码自定义结构体
    bool decodeStruct(omnibinder::Buffer& buf, const omnic::StructDef& structDef,
                      const std::string& package, simple_json::Value& json);
    
    // 编码数组
    bool encodeArray(const simple_json::Value& json, const omnic::TypeRef& elementType, 
                     const std::string& package, omnibinder::Buffer& buf);
    
    // 解码数组
    bool decodeArray(omnibinder::Buffer& buf, const omnic::TypeRef& elementType,
                     const std::string& package, simple_json::Value& json);
};

} // namespace type_codec

#endif // TYPE_CODEC_H
