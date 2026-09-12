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
#include "buffer_read_utils.h"

namespace omnibinder {

// 读取方法统一委托给共享原语，上限为 length_（public 行为与 Buffer 完全一致）

bool BufferView::tryReadBool(bool& value) noexcept {
    return buffer_read::readBool(data_, length_, read_pos_, value);
}

bool BufferView::tryReadInt8(int8_t& value) noexcept {
    return buffer_read::readInt8(data_, length_, read_pos_, value);
}

bool BufferView::tryReadUint8(uint8_t& value) noexcept {
    return buffer_read::readUint8(data_, length_, read_pos_, value);
}

bool BufferView::tryReadInt16(int16_t& value) noexcept {
    return buffer_read::readInt16(data_, length_, read_pos_, value);
}

bool BufferView::tryReadUint16(uint16_t& value) noexcept {
    return buffer_read::readUint16(data_, length_, read_pos_, value);
}

bool BufferView::tryReadInt32(int32_t& value) noexcept {
    return buffer_read::readInt32(data_, length_, read_pos_, value);
}

bool BufferView::tryReadUint32(uint32_t& value) noexcept {
    return buffer_read::readUint32(data_, length_, read_pos_, value);
}

bool BufferView::tryReadInt64(int64_t& value) noexcept {
    return buffer_read::readInt64(data_, length_, read_pos_, value);
}

bool BufferView::tryReadUint64(uint64_t& value) noexcept {
    return buffer_read::readUint64(data_, length_, read_pos_, value);
}

bool BufferView::tryReadFloat32(float& value) noexcept {
    return buffer_read::readFloat32(data_, length_, read_pos_, value);
}

bool BufferView::tryReadFloat64(double& value) noexcept {
    return buffer_read::readFloat64(data_, length_, read_pos_, value);
}

bool BufferView::tryReadString(std::string& value) noexcept {
    return buffer_read::readString(data_, length_, read_pos_, value);
}

bool BufferView::tryReadBytes(std::vector<uint8_t>& value) noexcept {
    return buffer_read::readBytes(data_, length_, read_pos_, value);
}

} // namespace omnibinder
