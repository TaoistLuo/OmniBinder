/**************************************************************************************************
 * @file        buffer_read_utils.h
 * @brief       Buffer / BufferView 共享的读取原语
 * @details     将小端序、带边界检查的基础类型读取逻辑收敛到一组自由函数，供 Buffer（拥有
 *              内存，读取上限为 write_pos_）与 BufferView（只读视图，读取上限为 length_）
 *              共用。所有函数都要求 pos <= limit，越界时返回 false 且不推进 pos；
 *              长度计算使用减法形式，避免 pos + n 溢出。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-09-12
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
#ifndef OMNIBINDER_BUFFER_READ_UTILS_H
#define OMNIBINDER_BUFFER_READ_UTILS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>

namespace omnibinder {
namespace buffer_read {

/* @brief 判断 [pos, pos + n) 是否完整落在 limit 之内（pos <= limit 时不会下溢） */
inline bool canRead(size_t limit, size_t pos, size_t n) noexcept {
    return pos <= limit && n <= limit - pos;
}

/* @brief 读取 1 字节无符号整数 */
inline bool readUint8(const uint8_t* data, size_t limit, size_t& pos, uint8_t& out) noexcept {
    if (!canRead(limit, pos, 1)) {
        return false;
    }
    out = data[pos++];
    return true;
}

/* @brief 读取 1 字节布尔值（非 0 为 true） */
inline bool readBool(const uint8_t* data, size_t limit, size_t& pos, bool& out) noexcept {
    uint8_t byte = 0;
    if (!readUint8(data, limit, pos, byte)) {
        return false;
    }
    out = (byte != 0);
    return true;
}

/* @brief 读取 1 字节有符号整数 */
inline bool readInt8(const uint8_t* data, size_t limit, size_t& pos, int8_t& out) noexcept {
    uint8_t byte = 0;
    if (!readUint8(data, limit, pos, byte)) {
        return false;
    }
    out = static_cast<int8_t>(byte);
    return true;
}

/* @brief 读取小端序 2 字节无符号整数 */
inline bool readUint16(const uint8_t* data, size_t limit, size_t& pos, uint16_t& out) noexcept {
    if (!canRead(limit, pos, 2)) {
        return false;
    }
    out = static_cast<uint16_t>(data[pos])
        | (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
}

/* @brief 读取小端序 2 字节有符号整数 */
inline bool readInt16(const uint8_t* data, size_t limit, size_t& pos, int16_t& out) noexcept {
    uint16_t temp = 0;
    if (!readUint16(data, limit, pos, temp)) {
        return false;
    }
    out = static_cast<int16_t>(temp);
    return true;
}

/* @brief 读取小端序 4 字节无符号整数 */
inline bool readUint32(const uint8_t* data, size_t limit, size_t& pos, uint32_t& out) noexcept {
    if (!canRead(limit, pos, 4)) {
        return false;
    }
    out = static_cast<uint32_t>(data[pos])
        | (static_cast<uint32_t>(data[pos + 1]) << 8)
        | (static_cast<uint32_t>(data[pos + 2]) << 16)
        | (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

/* @brief 读取小端序 4 字节有符号整数 */
inline bool readInt32(const uint8_t* data, size_t limit, size_t& pos, int32_t& out) noexcept {
    uint32_t temp = 0;
    if (!readUint32(data, limit, pos, temp)) {
        return false;
    }
    out = static_cast<int32_t>(temp);
    return true;
}

/* @brief 读取小端序 8 字节无符号整数 */
inline bool readUint64(const uint8_t* data, size_t limit, size_t& pos, uint64_t& out) noexcept {
    if (!canRead(limit, pos, 8)) {
        return false;
    }
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
    }
    pos += 8;
    return true;
}

/* @brief 读取小端序 8 字节有符号整数 */
inline bool readInt64(const uint8_t* data, size_t limit, size_t& pos, int64_t& out) noexcept {
    uint64_t temp = 0;
    if (!readUint64(data, limit, pos, temp)) {
        return false;
    }
    out = static_cast<int64_t>(temp);
    return true;
}

/* @brief 读取 4 字节 IEEE754 单精度浮点数 */
inline bool readFloat32(const uint8_t* data, size_t limit, size_t& pos, float& out) noexcept {
    uint32_t bits = 0;
    if (!readUint32(data, limit, pos, bits)) {
        return false;
    }
    memcpy(&out, &bits, sizeof(out));
    return true;
}

/* @brief 读取 8 字节 IEEE754 双精度浮点数 */
inline bool readFloat64(const uint8_t* data, size_t limit, size_t& pos, double& out) noexcept {
    uint64_t bits = 0;
    if (!readUint64(data, limit, pos, bits)) {
        return false;
    }
    memcpy(&out, &bits, sizeof(out));
    return true;
}

/* @brief 读取 uint32 长度前缀 + 字符串数据；长度为 0 时清空 out */
inline bool readString(const uint8_t* data, size_t limit, size_t& pos,
                       std::string& out) noexcept {
    uint32_t len = 0;
    if (!readUint32(data, limit, pos, len)) {
        return false;
    }
    if (len == 0) {
        out.clear();
        return true;
    }
    if (!canRead(limit, pos, len)) {
        return false;
    }
    try {
        out.assign(reinterpret_cast<const char*>(data + pos), len);
    } catch (...) {
        return false;
    }
    pos += len;
    return true;
}

/* @brief 读取 uint32 长度前缀 + 字节数组；长度为 0 时清空 out */
inline bool readBytes(const uint8_t* data, size_t limit, size_t& pos,
                      std::vector<uint8_t>& out) noexcept {
    uint32_t len = 0;
    if (!readUint32(data, limit, pos, len)) {
        return false;
    }
    if (len == 0) {
        out.clear();
        return true;
    }
    if (!canRead(limit, pos, len)) {
        return false;
    }
    try {
        out.assign(data + pos, data + pos + len);
    } catch (...) {
        return false;
    }
    pos += len;
    return true;
}

} // namespace buffer_read
} // namespace omnibinder

#endif // OMNIBINDER_BUFFER_READ_UTILS_H
