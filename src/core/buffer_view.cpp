/**************************************************************************************************
 * @file        buffer_view.cpp
 * @brief       BufferView 实现
 * @details     BufferView 的 tryRead* 方法实现。头文件仅保留声明与简单内联。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-03-29
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

#include "omnibinder/buffer_view.h"
#include <cstring>

namespace omnibinder {

bool BufferView::tryReadBool(bool& value) noexcept {
    uint8_t byte = 0;
    if (!tryReadUint8(byte)) return false;
    value = (byte != 0);
    return true;
}

bool BufferView::tryReadInt8(int8_t& value) noexcept {
    uint8_t byte = 0;
    if (!tryReadUint8(byte)) return false;
    value = static_cast<int8_t>(byte);
    return true;
}

bool BufferView::tryReadUint8(uint8_t& value) noexcept {
    if (read_pos_ + 1 > length_) return false;
    value = data_[read_pos_++];
    return true;
}

bool BufferView::tryReadInt16(int16_t& value) noexcept {
    uint16_t temp = 0;
    if (!tryReadUint16(temp)) return false;
    value = static_cast<int16_t>(temp);
    return true;
}

bool BufferView::tryReadUint16(uint16_t& value) noexcept {
    if (read_pos_ + 2 > length_) return false;
    value = static_cast<uint16_t>(data_[read_pos_])
          | (static_cast<uint16_t>(data_[read_pos_ + 1]) << 8);
    read_pos_ += 2;
    return true;
}

bool BufferView::tryReadInt32(int32_t& value) noexcept {
    uint32_t temp = 0;
    if (!tryReadUint32(temp)) return false;
    value = static_cast<int32_t>(temp);
    return true;
}

bool BufferView::tryReadUint32(uint32_t& value) noexcept {
    if (read_pos_ + 4 > length_) return false;
    value = static_cast<uint32_t>(data_[read_pos_])
          | (static_cast<uint32_t>(data_[read_pos_ + 1]) << 8)
          | (static_cast<uint32_t>(data_[read_pos_ + 2]) << 16)
          | (static_cast<uint32_t>(data_[read_pos_ + 3]) << 24);
    read_pos_ += 4;
    return true;
}

bool BufferView::tryReadInt64(int64_t& value) noexcept {
    uint64_t temp = 0;
    if (!tryReadUint64(temp)) return false;
    value = static_cast<int64_t>(temp);
    return true;
}

bool BufferView::tryReadUint64(uint64_t& value) noexcept {
    if (read_pos_ + 8 > length_) return false;
    value = static_cast<uint64_t>(data_[read_pos_])
          | (static_cast<uint64_t>(data_[read_pos_ + 1]) << 8)
          | (static_cast<uint64_t>(data_[read_pos_ + 2]) << 16)
          | (static_cast<uint64_t>(data_[read_pos_ + 3]) << 24)
          | (static_cast<uint64_t>(data_[read_pos_ + 4]) << 32)
          | (static_cast<uint64_t>(data_[read_pos_ + 5]) << 40)
          | (static_cast<uint64_t>(data_[read_pos_ + 6]) << 48)
          | (static_cast<uint64_t>(data_[read_pos_ + 7]) << 56);
    read_pos_ += 8;
    return true;
}

bool BufferView::tryReadFloat32(float& value) noexcept {
    uint32_t bits = 0;
    if (!tryReadUint32(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

bool BufferView::tryReadFloat64(double& value) noexcept {
    uint64_t bits = 0;
    if (!tryReadUint64(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

bool BufferView::tryReadString(std::string& value) noexcept {
    uint32_t len = 0;
    if (!tryReadUint32(len)) return false;
    if (len == 0) { value.clear(); return true; }
    if (len > length_ - read_pos_) return false;
    try {
        value.assign(reinterpret_cast<const char*>(data_ + read_pos_), len);
    } catch (...) {
        return false;
    }
    read_pos_ += len;
    return true;
}

bool BufferView::tryReadBytes(std::vector<uint8_t>& value) noexcept {
    uint32_t len = 0;
    if (!tryReadUint32(len)) return false;
    if (len == 0) { value.clear(); return true; }
    if (len > length_ - read_pos_) return false;
    try {
        value.assign(data_ + read_pos_, data_ + read_pos_ + len);
    } catch (...) {
        return false;
    }
    read_pos_ += len;
    return true;
}

} // namespace omnibinder
