/**************************************************************************************************
 * @file        buffer_view.h
 * @brief       非拥有只读缓冲区视图
 * @details     提供与 Buffer 相同的 tryRead 接口，但不持有/不分配/不释放内存。
 *              适用于"从已有内存区域读取数据"的场景，避免不必要的 malloc/memcpy。
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
#ifndef OMNIBINDER_BUFFER_VIEW_H
#define OMNIBINDER_BUFFER_VIEW_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace omnibinder {

/**
 * BufferView - 非拥有只读缓冲区视图
 *
 * 只持有外部指针 + 长度 + 游标，零分配。
 * 提供 Buffer 的全部 tryRead 方法，签名一致。
 * 不提供任何写入方法。
 */
class BufferView {
public:
    BufferView(const uint8_t* data, size_t length) noexcept
        : data_(data), length_(length), read_pos_(0) {}

    // ---- 读取（与 Buffer 签名完全一致）----
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

    // ---- 缓冲区信息 ----
    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return length_; }
    size_t readPosition() const noexcept { return read_pos_; }
    bool hasRemaining() const noexcept { return read_pos_ < length_; }
    size_t remaining() const noexcept { return length_ - read_pos_; }

private:
    const uint8_t* data_;
    size_t         length_;
    size_t         read_pos_;
};

} // namespace omnibinder

#endif // OMNIBINDER_BUFFER_VIEW_H
