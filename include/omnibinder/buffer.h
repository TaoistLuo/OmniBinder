/**************************************************************************************************
 * @file        buffer.h
 * @brief       序列化反序列化缓冲区
 * @details     提供二进制序列化/反序列化能力的缓冲区类。支持所有基础类型（bool、
 *              int8~int64、float32/64、string、bytes）的读写操作，采用小端字节序，
 *              自动扩容。用于 RPC 请求/响应的载荷编解码以及消息协议的序列化。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
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
#ifndef OMNIBINDER_BUFFER_H
#define OMNIBINDER_BUFFER_H

#include "omnibinder/types.h"
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <cstring>

namespace omnibinder {

/**
 * Buffer - 二进制序列化/反序列化缓冲区
 *
 * 支持自动扩容，小端字节序。
 * 提供所有基础类型的读写方法。
 */
class Buffer {
public:
    Buffer();
    explicit Buffer(size_t initial_capacity);
    Buffer(const uint8_t* data, size_t length);
    ~Buffer();

    // C++11: 支持移动语义
    Buffer(Buffer&& other);
    Buffer& operator=(Buffer&& other);

    // ---- 写入（序列化）----
    void writeBool(bool value);
    void writeInt8(int8_t value);
    void writeUint8(uint8_t value);
    void writeInt16(int16_t value);
    void writeUint16(uint16_t value);
    void writeInt32(int32_t value);
    void writeUint32(uint32_t value);
    void writeInt64(int64_t value);
    void writeUint64(uint64_t value);
    void writeFloat32(float value);
    void writeFloat64(double value);
    void writeString(const std::string& value);
    void writeBytes(const void* data, size_t length);
    void writeBytes(const std::vector<uint8_t>& data);
    void writeRaw(const void* data, size_t length);

    // ---- 读取（反序列化）----
    bool readBool();
    int8_t readInt8();
    uint8_t readUint8();
    int16_t readInt16();
    uint16_t readUint16();
    int32_t readInt32();
    uint32_t readUint32();
    int64_t readInt64();
    uint64_t readUint64();
    float readFloat32();
    double readFloat64();
    std::string readString();
    std::vector<uint8_t> readBytes();
    bool readRaw(void* buf, size_t length);

    // ---- 缓冲区管理 ----
    const uint8_t* data() const;
    uint8_t* mutableData();
    size_t size() const;
    size_t capacity() const;
    void reset();
    void clear();
    size_t readPosition() const;
    void setReadPosition(size_t pos);
    size_t writePosition() const;
    void setWritePosition(size_t pos);
    bool hasRemaining() const;
    size_t remaining() const;
    void assign(const uint8_t* data, size_t length);
    void reserve(size_t capacity);
    void resize(size_t new_size);

private:
    // 禁止拷贝
    Buffer(const Buffer&);
    Buffer& operator=(const Buffer&);

    void ensureCapacity(size_t additional);
    void grow(size_t min_capacity);

    uint8_t* data_;
    size_t   capacity_;
    size_t   write_pos_;
    size_t   read_pos_;
};

} // namespace omnibinder

#endif // OMNIBINDER_BUFFER_H
