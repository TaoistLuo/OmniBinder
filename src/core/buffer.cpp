#include "omnibinder/buffer.h"
#include <cstdlib>
#include <cstring>

namespace omnibinder {

Buffer::Buffer()
    : data_(NULL)
    , capacity_(0)
    , write_pos_(0)
    , read_pos_(0)
{
}

Buffer::Buffer(size_t initial_capacity)
    : data_(NULL)
    , capacity_(0)
    , write_pos_(0)
    , read_pos_(0)
{
    if (initial_capacity > 0) {
        reserve(initial_capacity);
    }
}

Buffer::Buffer(const uint8_t* data, size_t length)
    : data_(NULL)
    , capacity_(0)
    , write_pos_(0)
    , read_pos_(0)
{
    if (data && length > 0) {
        assign(data, length);
    }
}

Buffer::~Buffer() {
    if (data_) {
        free(data_);
        data_ = NULL;
    }
}

Buffer::Buffer(Buffer&& other)
    : data_(other.data_)
    , capacity_(other.capacity_)
    , write_pos_(other.write_pos_)
    , read_pos_(other.read_pos_)
{
    other.data_ = NULL;
    other.capacity_ = 0;
    other.write_pos_ = 0;
    other.read_pos_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) {
    if (this != &other) {
        if (data_) {
            free(data_);
        }
        data_ = other.data_;
        capacity_ = other.capacity_;
        write_pos_ = other.write_pos_;
        read_pos_ = other.read_pos_;
        other.data_ = NULL;
        other.capacity_ = 0;
        other.write_pos_ = 0;
        other.read_pos_ = 0;
    }
    return *this;
}

void Buffer::ensureCapacity(size_t additional) {
    size_t required = write_pos_ + additional;
    if (required > capacity_) {
        grow(required);
    }
}

void Buffer::grow(size_t min_capacity) {
    size_t new_capacity = capacity_ == 0 ? DEFAULT_BUFFER_SIZE : capacity_ * 2;
    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }
    uint8_t* new_data = static_cast<uint8_t*>(realloc(data_, new_capacity));
    if (!new_data) {
        return;
    }
    data_ = new_data;
    capacity_ = new_capacity;
}

void Buffer::reserve(size_t new_capacity) {
    if (new_capacity > capacity_) {
        grow(new_capacity);
    }
}

void Buffer::resize(size_t new_size) {
    if (new_size > capacity_) {
        reserve(new_size);
    }
    write_pos_ = new_size;
}

// ---- 写入方法 ----

void Buffer::writeBool(bool value) {
    writeUint8(value ? 1 : 0);
}

void Buffer::writeInt8(int8_t value) {
    ensureCapacity(1);
    data_[write_pos_++] = static_cast<uint8_t>(value);
}

void Buffer::writeUint8(uint8_t value) {
    ensureCapacity(1);
    data_[write_pos_++] = value;
}

void Buffer::writeInt16(int16_t value) {
    writeUint16(static_cast<uint16_t>(value));
}

void Buffer::writeUint16(uint16_t value) {
    ensureCapacity(2);
    // 小端序
    data_[write_pos_++] = static_cast<uint8_t>(value & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void Buffer::writeInt32(int32_t value) {
    writeUint32(static_cast<uint32_t>(value));
}

void Buffer::writeUint32(uint32_t value) {
    ensureCapacity(4);
    // 小端序
    data_[write_pos_++] = static_cast<uint8_t>(value & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data_[write_pos_++] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void Buffer::writeInt64(int64_t value) {
    writeUint64(static_cast<uint64_t>(value));
}

void Buffer::writeUint64(uint64_t value) {
    ensureCapacity(8);
    // 小端序
    for (int i = 0; i < 8; ++i) {
        data_[write_pos_++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

void Buffer::writeFloat32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    writeUint32(bits);
}

void Buffer::writeFloat64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    writeUint64(bits);
}

void Buffer::writeString(const std::string& value) {
    uint32_t len = static_cast<uint32_t>(value.size());
    writeUint32(len);
    if (len > 0) {
        writeRaw(value.data(), len);
    }
}

void Buffer::writeBytes(const void* data, size_t length) {
    uint32_t len = static_cast<uint32_t>(length);
    writeUint32(len);
    if (len > 0) {
        writeRaw(data, len);
    }
}

void Buffer::writeBytes(const std::vector<uint8_t>& data) {
    writeBytes(data.data(), data.size());
}

void Buffer::writeRaw(const void* data, size_t length) {
    if (length > 0) {
        ensureCapacity(length);
        memcpy(data_ + write_pos_, data, length);
        write_pos_ += length;
    }
}

bool Buffer::tryReadBool(bool& value) {
    uint8_t byte = 0;
    if (!tryReadUint8(byte)) {
        return false;
    }
    value = byte != 0;
    return true;
}

bool Buffer::tryReadInt8(int8_t& value) {
    uint8_t byte = 0;
    if (!tryReadUint8(byte)) {
        return false;
    }
    value = static_cast<int8_t>(byte);
    return true;
}

bool Buffer::tryReadUint8(uint8_t& value) {
    if (read_pos_ + 1 > write_pos_) {
        return false;
    }
    value = data_[read_pos_++];
    return true;
}

bool Buffer::tryReadInt16(int16_t& value) {
    uint16_t temp = 0;
    if (!tryReadUint16(temp)) {
        return false;
    }
    value = static_cast<int16_t>(temp);
    return true;
}

bool Buffer::tryReadUint16(uint16_t& value) {
    if (read_pos_ + 2 > write_pos_) {
        return false;
    }
    value = static_cast<uint16_t>(data_[read_pos_])
          | (static_cast<uint16_t>(data_[read_pos_ + 1]) << 8);
    read_pos_ += 2;
    return true;
}

bool Buffer::tryReadInt32(int32_t& value) {
    uint32_t temp = 0;
    if (!tryReadUint32(temp)) {
        return false;
    }
    value = static_cast<int32_t>(temp);
    return true;
}

bool Buffer::tryReadUint32(uint32_t& value) {
    if (read_pos_ + 4 > write_pos_) {
        return false;
    }
    value = static_cast<uint32_t>(data_[read_pos_])
          | (static_cast<uint32_t>(data_[read_pos_ + 1]) << 8)
          | (static_cast<uint32_t>(data_[read_pos_ + 2]) << 16)
          | (static_cast<uint32_t>(data_[read_pos_ + 3]) << 24);
    read_pos_ += 4;
    return true;
}

bool Buffer::tryReadInt64(int64_t& value) {
    uint64_t temp = 0;
    if (!tryReadUint64(temp)) {
        return false;
    }
    value = static_cast<int64_t>(temp);
    return true;
}

bool Buffer::tryReadUint64(uint64_t& value) {
    if (read_pos_ + 8 > write_pos_) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data_[read_pos_ + i]) << (i * 8);
    }
    read_pos_ += 8;
    return true;
}

bool Buffer::tryReadFloat32(float& value) {
    uint32_t bits = 0;
    if (!tryReadUint32(bits)) {
        return false;
    }
    memcpy(&value, &bits, sizeof(value));
    return true;
}

bool Buffer::tryReadFloat64(double& value) {
    uint64_t bits = 0;
    if (!tryReadUint64(bits)) {
        return false;
    }
    memcpy(&value, &bits, sizeof(value));
    return true;
}

bool Buffer::tryReadString(std::string& value) {
    uint32_t len = 0;
    if (!tryReadUint32(len)) {
        return false;
    }
    if (len == 0) {
        value.clear();
        return true;
    }
    if (read_pos_ + len > write_pos_) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(data_ + read_pos_), len);
    read_pos_ += len;
    return true;
}

bool Buffer::tryReadBytes(std::vector<uint8_t>& value) {
    uint32_t len = 0;
    if (!tryReadUint32(len)) {
        return false;
    }
    if (len == 0) {
        value.clear();
        return true;
    }
    if (read_pos_ + len > write_pos_) {
        return false;
    }
    value.assign(data_ + read_pos_, data_ + read_pos_ + len);
    read_pos_ += len;
    return true;
}

// ---- 缓冲区管理 ----

const uint8_t* Buffer::data() const {
    return data_;
}

uint8_t* Buffer::mutableData() {
    return data_;
}

size_t Buffer::size() const {
    return write_pos_;
}

size_t Buffer::capacity() const {
    return capacity_;
}

void Buffer::reset() {
    read_pos_ = 0;
}

void Buffer::clear() {
    write_pos_ = 0;
    read_pos_ = 0;
}

size_t Buffer::readPosition() const {
    return read_pos_;
}

bool Buffer::trySetReadPosition(size_t pos) {
    if (pos > write_pos_) {
        return false;
    }
    read_pos_ = pos;
    return true;
}

size_t Buffer::writePosition() const {
    return write_pos_;
}

void Buffer::setWritePosition(size_t pos) {
    if (pos > capacity_) {
        reserve(pos);
    }
    write_pos_ = pos;
}

bool Buffer::hasRemaining() const {
    return read_pos_ < write_pos_;
}

size_t Buffer::remaining() const {
    return write_pos_ - read_pos_;
}

void Buffer::assign(const uint8_t* data, size_t length) {
    clear();
    if (length > 0) {
        reserve(length);
        memcpy(data_, data, length);
        write_pos_ = length;
    }
}

} // namespace omnibinder
