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

/* @brief Buffer — 二进制序列化/反序列化缓冲区
 * @details 支持自动扩容，小端字节序；提供所有基础类型的读写方法。 */
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
    /* @brief 写入 bool（1 字节，0/1） */
    bool writeBool(bool value) noexcept;
    /* @brief 写入 int8 */
    bool writeInt8(int8_t value) noexcept;
    /* @brief 写入 uint8 */
    bool writeUint8(uint8_t value) noexcept;
    /* @brief 写入 int16（小端） */
    bool writeInt16(int16_t value) noexcept;
    /* @brief 写入 uint16（小端） */
    bool writeUint16(uint16_t value) noexcept;
    /* @brief 写入 int32（小端） */
    bool writeInt32(int32_t value) noexcept;
    /* @brief 写入 uint32（小端） */
    bool writeUint32(uint32_t value) noexcept;
    /* @brief 写入 int64（小端） */
    bool writeInt64(int64_t value) noexcept;
    /* @brief 写入 uint64（小端） */
    bool writeUint64(uint64_t value) noexcept;
    /* @brief 写入 float32（小端） */
    bool writeFloat32(float value) noexcept;
    /* @brief 写入 float64（小端） */
    bool writeFloat64(double value) noexcept;
    /* @brief 写入字符串（uint32 长度前缀 + 字节） */
    bool writeString(const std::string& value) noexcept;
    /* @brief 写入字节数组（uint32 长度前缀 + 字节） */
    bool writeBytes(const void* data, size_t length) noexcept;
    /* @brief 写入字节数组（uint32 长度前缀 + 字节） */
    bool writeBytes(const std::vector<uint8_t>& data) noexcept;
    /* @brief 原样写入字节（无长度前缀） */
    bool writeRaw(const void* data, size_t length) noexcept;

    /* @brief 读取 bool */
    bool tryReadBool(bool& value) noexcept;
    /* @brief 读取 int8 */
    bool tryReadInt8(int8_t& value) noexcept;
    /* @brief 读取 uint8 */
    bool tryReadUint8(uint8_t& value) noexcept;
    /* @brief 读取 int16 */
    bool tryReadInt16(int16_t& value) noexcept;
    /* @brief 读取 uint16 */
    bool tryReadUint16(uint16_t& value) noexcept;
    /* @brief 读取 int32 */
    bool tryReadInt32(int32_t& value) noexcept;
    /* @brief 读取 uint32 */
    bool tryReadUint32(uint32_t& value) noexcept;
    /* @brief 读取 int64 */
    bool tryReadInt64(int64_t& value) noexcept;
    /* @brief 读取 uint64 */
    bool tryReadUint64(uint64_t& value) noexcept;
    /* @brief 读取 float32 */
    bool tryReadFloat32(float& value) noexcept;
    /* @brief 读取 float64 */
    bool tryReadFloat64(double& value) noexcept;
    /* @brief 读取字符串（长度前缀 + 字节）
     * @note 越界/长度超限返回 false；内存分配失败返回 false（无异常传播） */
    bool tryReadString(std::string& value) noexcept;
    /* @brief 读取字节数组（长度前缀 + 字节）
     * @note 越界/长度超限返回 false；内存分配失败返回 false（无异常传播） */
    bool tryReadBytes(std::vector<uint8_t>& value) noexcept;

    // ---- 缓冲区管理 ----
    /* @brief 返回已写入数据的起始指针 */
    const uint8_t* data() const noexcept;
    /* @brief 返回可写起始指针 */
    uint8_t* mutableData() noexcept;
    /* @brief 返回已写入数据长度（write_pos_ - read_pos_） */
    size_t size() const noexcept;
    /* @brief 返回当前容量 */
    size_t capacity() const noexcept;
    /* @brief 回绕读游标：仅重置 read_pos_ 与写失败标志，保留已写入数据 */
    void reset() noexcept;
    /* @brief 清空缓冲：同时重置读/写游标与写失败标志 */
    void clear() noexcept;
    /* @brief 返回当前读游标位置 */
    size_t readPosition() const noexcept;
    /* @brief 设置读游标位置；越界返回 false */
    bool trySetReadPosition(size_t pos) noexcept;
    /* @brief 返回当前写游标位置 */
    size_t writePosition() const noexcept;
    /* @brief 设置写游标位置（void；越界/分配失败置 writeOk()=false） */
    void setWritePosition(size_t pos) noexcept;
    /* @brief 是否还有可读数据 */
    bool hasRemaining() const noexcept;
    /* @brief 剩余可读字节数 */
    size_t remaining() const noexcept;
    /* @brief 以外部数据覆盖缓冲内容（void；分配失败置 writeOk()=false） */
    void assign(const uint8_t* data, size_t length) noexcept;
    /* @brief 预分配至少 capacity 字节（void；分配失败置 writeOk()=false） */
    void reserve(size_t capacity) noexcept;
    /* @brief 调整已写入数据大小（void；分配失败置 writeOk()=false） */
    void resize(size_t new_size) noexcept;
    /* @brief 压缩缓冲，把未读数据移到起始处（void；失败置 writeOk()=false） */
    void compact() noexcept;

    /* @brief 返回尾部可直接写入（例如 recv 落盘）的连续区域
     * @param[out] out_writable 本次可写字节数，保证 >= 1（容量不足时内部按需增长）
     * @return 可写起始指针；分配失败时返回 NULL 且 out_writable = 0
     * @note 写入后必须调用 commitWritten() 提交实际写入长度，否则 size() 不前进 */
    uint8_t* writableTail(size_t& out_writable) noexcept;

    /* @brief 提交 writableTail() 区域实际写入的 n 字节，推进写游标
     * @param[in] n 实际写入字节数，不得超过上次 writableTail 返回的 out_writable
     * @return true 成功；false 表示 n 越界或缓冲区已失效（writeOk() 为 false） */
    bool commitWritten(size_t n) noexcept;

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
