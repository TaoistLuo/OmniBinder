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
    Buffer() noexcept;
    explicit Buffer(size_t initial_capacity) noexcept;
    Buffer(const uint8_t* data, size_t length) noexcept;
    ~Buffer() noexcept;

    // C++11: 支持移动语义
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    // ---- 写入（序列化）----
    bool writeBool(bool value) noexcept;
    bool writeInt8(int8_t value) noexcept;
    bool writeUint8(uint8_t value) noexcept;
    bool writeInt16(int16_t value) noexcept;
    bool writeUint16(uint16_t value) noexcept;
    bool writeInt32(int32_t value) noexcept;
    bool writeUint32(uint32_t value) noexcept;
    bool writeInt64(int64_t value) noexcept;
    bool writeUint64(uint64_t value) noexcept;
    bool writeFloat32(float value) noexcept;
    bool writeFloat64(double value) noexcept;
    bool writeString(const std::string& value) noexcept;
    bool writeBytes(const void* data, size_t length) noexcept;
    bool writeBytes(const std::vector<uint8_t>& data) noexcept;
    bool writeRaw(const void* data, size_t length) noexcept;

    bool tryReadBool(bool& value) noexcept;
    bool tryReadInt8(int8_t& value) noexcept;
    bool tryReadUint8(uint8_t& value) noexcept;
    bool tryReadInt16(int16_t& value) noexcept;
    bool tryReadUint16(uint16_t& value) noexcept;
    bool tryReadInt32(int32_t& value) noexcept;
    bool tryReadUint32(uint32_t& value) noexcept;
    bool tryReadInt64(int64_t& value) noexcept;
    bool tryReadUint64(uint64_t& value) noexcept;
    bool tryReadFloat32(float& value) noexcept;
    bool tryReadFloat64(double& value) noexcept;
    bool tryReadString(std::string& value) noexcept;
    bool tryReadBytes(std::vector<uint8_t>& value) noexcept;

    // ---- 缓冲区管理 ----
    const uint8_t* data() const noexcept;
    uint8_t* mutableData() noexcept;
    size_t size() const noexcept;
    size_t capacity() const noexcept;
    void reset() noexcept;
    void clear() noexcept;
    size_t readPosition() const noexcept;
    bool trySetReadPosition(size_t pos) noexcept;
    size_t writePosition() const noexcept;
    void setWritePosition(size_t pos) noexcept;
    bool hasRemaining() const noexcept;
    size_t remaining() const noexcept;
    void assign(const uint8_t* data, size_t length) noexcept;
    void reserve(size_t capacity) noexcept;
    void resize(size_t new_size) noexcept;

    // ---- 写入状态检查 ----
    bool writeOk() const noexcept;

private:
    // 禁止拷贝
    Buffer(const Buffer&);
    Buffer& operator=(const Buffer&);

    bool ensureCapacity(size_t additional) noexcept;
    void grow(size_t min_capacity) noexcept;

    uint8_t* data_;
    size_t   capacity_;
    size_t   write_pos_;
    size_t   read_pos_;
    bool     write_failed_;
};

} // namespace omnibinder

#endif // OMNIBINDER_BUFFER_H
