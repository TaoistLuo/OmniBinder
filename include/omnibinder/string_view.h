/**************************************************************************************************
 * @file        string_view.h
 * @brief       非拥有字符串视图（C++11）
 * @details     仅保存指针 + 长度，不分配、不释放；用于零拷贝消息视图。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-10-30
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
 *************************************************************************************************/
#ifndef OMNIBINDER_STRING_VIEW_H
#define OMNIBINDER_STRING_VIEW_H

#include <stddef.h>
#include <stdint.h>

namespace omnibinder {

/* @brief 非拥有字符串视图
 * @details 不保证以 '\0' 结尾，调用方必须同时使用 data() 与 size()。 */
class StringView {
public:
    StringView() noexcept : data_(NULL), size_(0) {}
    StringView(const char* data, size_t size) noexcept : data_(data), size_(size) {}

    const char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    const char* data_;
    size_t size_;
};

/* @brief 非拥有 bytes 视图 */
class ByteView {
public:
    ByteView() noexcept : data_(NULL), size_(0) {}
    ByteView(const uint8_t* data, size_t size) noexcept : data_(data), size_(size) {}

    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    const uint8_t* data_;
    size_t size_;
};

} // namespace omnibinder

#endif // OMNIBINDER_STRING_VIEW_H
