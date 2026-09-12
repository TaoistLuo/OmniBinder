#include "omnibinder/buffer.h"
#include "buffer_read_utils.h"
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" void* omni_malloc(size_t size);
extern "C" void  omni_free(void* ptr);

namespace omnibinder {

Buffer::Buffer() noexcept
    : data_(NULL)
    , capacity_(0)
    , write_pos_(0)
    , read_pos_(0)
    , write_failed_(false)
{
}

Buffer::Buffer(size_t initial_capacity) noexcept
    : data_(NULL)
    , capacity_(0)
    , write_pos_(0)
    , read_pos_(0)
    , write_failed_(false)
{
    if (initial_capacity > 0) {
        reserve(initial_capacity);
    }
}

Buffer::Buffer(const uint8_t* data, size_t length) noexcept
    : data_(NULL)
    , capacity_(0)
    , write_pos_(0)
    , read_pos_(0)
    , write_failed_(false)
{
    if (data && length > 0) {
        assign(data, length);
    }
}

Buffer::~Buffer() noexcept {
    if (data_) {
        omni_free(data_);
        data_ = NULL;
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : data_(other.data_)
    , capacity_(other.capacity_)
    , write_pos_(other.write_pos_)
    , read_pos_(other.read_pos_)
    , write_failed_(other.write_failed_)
{
    other.data_ = NULL;
    other.capacity_ = 0;
    other.write_pos_ = 0;
    other.read_pos_ = 0;
    other.write_failed_ = false;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (data_) {
            omni_free(data_);
        }
        data_ = other.data_;
        capacity_ = other.capacity_;
        write_pos_ = other.write_pos_;
        read_pos_ = other.read_pos_;
        write_failed_ = other.write_failed_;
        other.data_ = NULL;
        other.capacity_ = 0;
        other.write_pos_ = 0;
        other.read_pos_ = 0;
        other.write_failed_ = false;
    }
    return *this;
}

bool Buffer::ensureCapacity(size_t additional) noexcept {
    if (write_failed_) return false;
    if (additional > std::numeric_limits<size_t>::max() - write_pos_) {
        write_failed_ = true;
        return false;
    }
    size_t required = write_pos_ + additional;
    if (required > capacity_) {
        grow(required);
    }
    return !write_failed_;
}

void Buffer::grow(size_t min_capacity) noexcept {
    if (capacity_ > std::numeric_limits<size_t>::max() / 2) {
        write_failed_ = true;
        return;
    }
    size_t new_capacity = capacity_ == 0 ? DEFAULT_BUFFER_SIZE : capacity_ * 2;
    while (new_capacity < min_capacity) {
        if (new_capacity > std::numeric_limits<size_t>::max() / 2) {
            write_failed_ = true;
            return;
        }
        new_capacity *= 2;
    }
    uint8_t* new_data = static_cast<uint8_t*>(omni_malloc(new_capacity));
    if (!new_data) {
        write_failed_ = true;
        return;
    }
    if (data_ && write_pos_ > 0) {
        std::memcpy(new_data, data_, write_pos_);
    }
    omni_free(data_);
    data_ = new_data;
    capacity_ = new_capacity;
}

void Buffer::reserve(size_t new_capacity) noexcept {
    if (new_capacity > capacity_) {
        grow(new_capacity);
    }
}

void Buffer::resize(size_t new_size) noexcept {
    if (new_size > capacity_) {
        reserve(new_size);
        if (write_failed_) return;
    }
    write_pos_ = new_size;
}

// ---- 写入方法 ----

bool Buffer::writeBool(bool value) noexcept {
    return writeUint8(value ? 1 : 0);
}

bool Buffer::writeInt8(int8_t value) noexcept {
    if (!ensureCapacity(1)) return false;
    data_[write_pos_++] = static_cast<uint8_t>(value);
    return true;
}

bool Buffer::writeUint8(uint8_t value) noexcept {
    if (!ensureCapacity(1)) return false;
    data_[write_pos_++] = value;
    return true;
}

bool Buffer::writeInt16(int16_t value) noexcept {
    return writeUint16(static_cast<uint16_t>(value));
}

bool Buffer::writeUint16(uint16_t value) noexcept {
    if (!ensureCapacity(2)) return false;
    // 小端序
    data_[write_pos_++] = static_cast<uint8_t>(value & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    return true;
}

bool Buffer::writeInt32(int32_t value) noexcept {
    return writeUint32(static_cast<uint32_t>(value));
}

bool Buffer::writeUint32(uint32_t value) noexcept {
    if (!ensureCapacity(4)) return false;
    // 小端序
    data_[write_pos_++] = static_cast<uint8_t>(value & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 24) & 0xFF);
    return true;
}

bool Buffer::writeInt64(int64_t value) noexcept {
    return writeUint64(static_cast<uint64_t>(value));
}

bool Buffer::writeUint64(uint64_t value) noexcept {
    if (!ensureCapacity(8)) return false;
    // 小端序
    for (int i = 0; i < 8; ++i) {
        data_[write_pos_++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    return true;
}

bool Buffer::writeFloat32(float value) noexcept {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return writeUint32(bits);
}

bool Buffer::writeFloat64(double value) noexcept {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return writeUint64(bits);
}

bool Buffer::writeString(const std::string& value) noexcept {
    if (value.size() > std::numeric_limits<uint32_t>::max() - 4u) {
        write_failed_ = true;
        return false;
    }
    uint32_t len = static_cast<uint32_t>(value.size());
    // 原子性：一次性确保 length prefix + data 都有空间
    if (!ensureCapacity(4 + len)) return false;
    if (!writeUint32(len)) return false;
    if (len > 0) {
        if (!writeRaw(value.data(), len)) return false;
    }
    return true;
}

bool Buffer::writeBytes(const void* data, size_t length) noexcept {
    if (length > std::numeric_limits<uint32_t>::max() - 4u) {
        write_failed_ = true;
        return false;
    }
    uint32_t len = static_cast<uint32_t>(length);
    // 原子性：一次性确保 length prefix + data 都有空间
    if (!ensureCapacity(4 + len)) return false;
    if (!writeUint32(len)) return false;
    if (len > 0) {
        if (!writeRaw(data, len)) return false;
    }
    return true;
}

bool Buffer::writeBytes(const std::vector<uint8_t>& data) noexcept {
    return writeBytes(data.data(), data.size());
}

bool Buffer::writeRaw(const void* data, size_t length) noexcept {
    if (length > 0) {
        if (!ensureCapacity(length)) return false;
        memcpy(data_ + write_pos_, data, length);
        write_pos_ += length;
    }
    return true;
}

// ---- 读取方法（共享原语，上限为 write_pos_）----

bool Buffer::tryReadBool(bool& value) noexcept {
    return buffer_read::readBool(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadInt8(int8_t& value) noexcept {
    return buffer_read::readInt8(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadUint8(uint8_t& value) noexcept {
    return buffer_read::readUint8(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadInt16(int16_t& value) noexcept {
    return buffer_read::readInt16(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadUint16(uint16_t& value) noexcept {
    return buffer_read::readUint16(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadInt32(int32_t& value) noexcept {
    return buffer_read::readInt32(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadUint32(uint32_t& value) noexcept {
    return buffer_read::readUint32(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadInt64(int64_t& value) noexcept {
    return buffer_read::readInt64(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadUint64(uint64_t& value) noexcept {
    return buffer_read::readUint64(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadFloat32(float& value) noexcept {
    return buffer_read::readFloat32(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadFloat64(double& value) noexcept {
    return buffer_read::readFloat64(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadString(std::string& value) noexcept {
    return buffer_read::readString(data_, write_pos_, read_pos_, value);
}

bool Buffer::tryReadBytes(std::vector<uint8_t>& value) noexcept {
    return buffer_read::readBytes(data_, write_pos_, read_pos_, value);
}

// ---- 缓冲区管理 ----

const uint8_t* Buffer::data() const noexcept {
    return data_;
}

uint8_t* Buffer::mutableData() noexcept {
    return data_;
}

size_t Buffer::size() const noexcept {
    return write_pos_;
}

size_t Buffer::capacity() const noexcept {
    return capacity_;
}

void Buffer::reset() noexcept {
    read_pos_ = 0;
    write_failed_ = false;
}

void Buffer::clear() noexcept {
    write_pos_ = 0;
    read_pos_ = 0;
    write_failed_ = false;
}

size_t Buffer::readPosition() const noexcept {
    return read_pos_;
}

bool Buffer::trySetReadPosition(size_t pos) noexcept {
    if (pos > write_pos_) {
        return false;
    }
    read_pos_ = pos;
    return true;
}

size_t Buffer::writePosition() const noexcept {
    return write_pos_;
}

void Buffer::setWritePosition(size_t pos) noexcept {
    // 镜像 trySetReadPosition 的校验：写游标不得回退到读游标之前，
    // 否则 remaining()/compact() 会下溢（write_pos_ - read_pos_ 变为巨大值）
    if (pos < read_pos_) {
        return;
    }
    if (pos > capacity_) {
        reserve(pos);
        if (write_failed_) return;
    }
    write_pos_ = pos;
}

bool Buffer::hasRemaining() const noexcept {
    return read_pos_ < write_pos_;
}

size_t Buffer::remaining() const noexcept {
    return write_pos_ - read_pos_;
}

void Buffer::assign(const uint8_t* data, size_t length) noexcept {
    clear();
    if (length > 0) {
        reserve(length);
        if (write_failed_) return;
        memcpy(data_, data, length);
        write_pos_ = length;
    }
}

bool Buffer::writeOk() const noexcept {
    return !write_failed_;
}

void Buffer::compact() noexcept {
    size_t remaining = write_pos_ - read_pos_;
    if (remaining > 0 && read_pos_ > 0) {
        memmove(data_, data_ + read_pos_, remaining);
    }
    write_pos_ = remaining;
    read_pos_ = 0;
}

} // namespace omnibinder
