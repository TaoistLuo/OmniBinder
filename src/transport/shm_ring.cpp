#include "transport/shm_ring.h"
#include "omnibinder/types.h"
#include "platform/platform.h"

#include <string.h>
#include <limits>
#include <sstream>

namespace omnibinder {

// ============================================================
// 容量与布局
// ============================================================

bool shmIsValidRingCapacity(size_t capacity)
{
    return capacity >= SHM_MIN_RING_CAPACITY
        && capacity <= static_cast<size_t>(std::numeric_limits<uint32_t>::max());
}

size_t shmNormalizeRingCapacity(size_t capacity)
{
    if (capacity == 0) {
        return SHM_MIN_RING_CAPACITY;
    }
    if (capacity < SHM_MIN_RING_CAPACITY) {
        return SHM_MIN_RING_CAPACITY;
    }
    return capacity;
}

uint32_t shmAdvancePos(uint32_t pos, uint32_t delta, uint32_t capacity)
{
    if (delta >= capacity - pos) {
        return delta - (capacity - pos);
    }
    return pos + delta;
}

size_t shmAlignUp(size_t value, size_t alignment)
{
    return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
}

size_t shmRequestRingOffset()
{
    return shmAlignUp(sizeof(ShmControlBlock), alignof(ShmRingHeader));
}

size_t shmRequestDataOffset()
{
    return shmRequestRingOffset() + sizeof(ShmRingHeader);
}

size_t shmResponseRingOffset(size_t req_ring_capacity)
{
    return shmAlignUp(shmRequestDataOffset() + req_ring_capacity, alignof(ShmRingHeader));
}

size_t shmResponseDataOffset(size_t req_ring_capacity)
{
    return shmResponseRingOffset(req_ring_capacity) + sizeof(ShmRingHeader);
}

size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity)
{
    req_ring_capacity = shmNormalizeRingCapacity(req_ring_capacity);
    resp_ring_capacity = shmNormalizeRingCapacity(resp_ring_capacity);
    if (!shmIsValidRingCapacity(req_ring_capacity)
        || !shmIsValidRingCapacity(resp_ring_capacity)) {
        return 0;
    }
    size_t offset = shmResponseDataOffset(req_ring_capacity);
    if (offset > std::numeric_limits<size_t>::max() - resp_ring_capacity) {
        return 0;
    }
    return offset + resp_ring_capacity;
}

std::string generateShmName(const std::string& service_name)
{
    // 截断前缀 + FNV-1a 哈希，生成有界长度且唯一的 SHM 名称，
    // 避免服务名接近 MAX_SERVICE_NAME_LENGTH（256）时被截断。
    const size_t kMaxPrefix = 48;
    std::string prefix = service_name.substr(0, kMaxPrefix);
    uint32_t hash = fnv1a_32(service_name);

    std::ostringstream os;
    os << "/binder_" << prefix << "_" << std::hex << hash;
    return os.str();
}

std::string shmHandshakePath(const std::string& shm_name)
{
    // 使用哈希路径以保持在 Linux sun_path 长度限制（通常 108 字节）内，
    // 完整 shm_name 可能过长。
    uint32_t hash = fnv1a_32(shm_name);
    std::ostringstream os;
    os << "/tmp/omni_" << std::hex << hash << ".sock";
    return os.str();
}

// ============================================================
// 基址导航
// ============================================================

ShmRingHeader* shmRequestRingFromBase(uint8_t* base)
{
    return reinterpret_cast<ShmRingHeader*>(base + shmRequestRingOffset());
}

uint8_t* shmRequestDataFromBase(uint8_t* base)
{
    return base + shmRequestDataOffset();
}

ShmRingHeader* shmResponseRingFromBase(uint8_t* base, uint32_t req_capacity)
{
    return reinterpret_cast<ShmRingHeader*>(base + shmResponseRingOffset(req_capacity));
}

uint8_t* shmResponseDataFromBase(uint8_t* base, uint32_t req_capacity)
{
    return base + shmResponseDataOffset(req_capacity);
}

// ============================================================
// ring 读写
// ============================================================

uint32_t shmRingAvailableRead(const ShmRingHeader* ring, uint32_t cap)
{
    if (!ring || cap < SHM_MIN_RING_CAPACITY || ring->capacity != cap) return 0;
    uint32_t w = ring->write_pos.load(std::memory_order_acquire);
    uint32_t r = ring->read_pos.load(std::memory_order_acquire);
    if (w >= cap || r >= cap) {
        return 0;
    }
    if (w >= r) {
        return w - r;
    }
    return cap - r + w;
}

uint32_t shmRingAvailableWrite(const ShmRingHeader* ring, uint32_t capacity)
{
    if (!ring || capacity < SHM_MIN_RING_CAPACITY || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_acquire) >= capacity
        || ring->read_pos.load(std::memory_order_acquire) >= capacity) {
        return 0;
    }
    uint32_t used = shmRingAvailableRead(ring, capacity);
    return capacity - 1 - used;
}

uint32_t shmRingWrite(ShmRingHeader* ring, uint8_t* ring_data,
                      const uint8_t* data, uint32_t length, uint32_t capacity)
{
    if (!ring || !ring_data || !data || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_relaxed) >= capacity) {
        return 0;
    }
    uint32_t avail = shmRingAvailableWrite(ring, capacity);
    uint32_t to_write = (length < avail) ? length : avail;
    if (to_write == 0) {
        return 0;
    }

    uint32_t w = ring->write_pos.load(std::memory_order_relaxed);
    uint32_t cap = capacity;

    uint32_t first = (to_write < cap - w) ? to_write : (cap - w);
    memcpy(ring_data + w, data, first);

    if (first < to_write) {
        memcpy(ring_data, data + first, to_write - first);
    }

    ring->write_pos.store(shmAdvancePos(w, to_write, cap), std::memory_order_release);

    return to_write;
}

uint32_t shmRingWriteFrame(ShmRingHeader* ring, uint8_t* ring_data,
                           const uint8_t* data, uint32_t length,
                           uint32_t capacity)
{
    if (!ring || !ring_data || !data || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_relaxed) >= capacity) {
        return 0;
    }
    uint32_t total = static_cast<uint32_t>(sizeof(uint32_t)) + length;
    if (shmRingAvailableWrite(ring, capacity) < total) return 0;

    uint32_t pos = ring->write_pos.load(std::memory_order_relaxed);
    uint32_t first = (static_cast<uint32_t>(sizeof(uint32_t)) < capacity - pos)
        ? static_cast<uint32_t>(sizeof(uint32_t)) : (capacity - pos);
    memcpy(ring_data + pos, &length, first);
    if (first < sizeof(uint32_t)) {
        memcpy(ring_data, reinterpret_cast<const uint8_t*>(&length) + first,
               sizeof(uint32_t) - first);
    }
    pos = shmAdvancePos(pos, static_cast<uint32_t>(sizeof(uint32_t)), capacity);
    first = (length < capacity - pos) ? length : (capacity - pos);
    memcpy(ring_data + pos, data, first);
    if (first < length) memcpy(ring_data, data + first, length - first);
    ring->write_pos.store(shmAdvancePos(pos, length, capacity), std::memory_order_release);
    return total;
}

uint32_t shmRingSendFrame(ShmRingHeader* ring, uint8_t* ring_data, uint32_t capacity,
                          const uint8_t* frame, uint32_t length, int notify_fd)
{
    if (!ring || !ring_data || !frame || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_relaxed) >= capacity
        || length > 0x7FFFFFFFu) {
        return 0;
    }

    uint32_t total = static_cast<uint32_t>(sizeof(uint32_t)) + length;
    bool was_empty = (shmRingAvailableRead(ring, capacity) == 0);
    uint32_t write_start = ring->write_pos.load(std::memory_order_relaxed);

    uint32_t written = shmRingWriteFrame(ring, ring_data, frame, length, capacity);
    if (written != total) {
        return 0;
    }

    // 入队与消费可能竞争：写入前非空时，若消费方已排空到本帧起点，
    // 本帧就是新的 空→非空 跃迁，仍需通知。
    if (!was_empty) {
        was_empty = (ring->read_pos.load(std::memory_order_acquire) == write_start);
    }
    if (was_empty && notify_fd >= 0) {
        platform::eventFdNotify(notify_fd);
    }
    return written;
}

int shmRingSendFrameWithinTimeout(ShmRingHeader* ring, uint8_t* ring_data, uint32_t capacity,
                                  const uint8_t* frame, uint32_t length, int notify_fd,
                                  uint32_t timeout_ms)
{
    if (!ring || !ring_data || length > 0x7FFFFFFFu) {
        return -1;
    }
    uint32_t total_needed = static_cast<uint32_t>(sizeof(uint32_t)) + length;
    if (capacity == 0 || total_needed > capacity - 1u) {
        return -1;
    }

    int64_t start_ms = platform::currentTimeMs();
    for (;;) {
        uint32_t written = shmRingSendFrame(ring, ring_data, capacity, frame, length, notify_fd);
        if (written == total_needed) {
            return 0;
        }
        if (timeout_ms == 0 || platform::currentTimeMs() - start_ms >= timeout_ms) {
            return -2;
        }
        platform::sleepMs(1);
    }
}

int shmRingRecvFrame(ShmRingHeader* ring, const uint8_t* ring_data, uint32_t capacity,
                     uint8_t* out, size_t out_size, size_t& out_length)
{
    out_length = 0;
    size_t frame_size = 0;
    int inspect = shmInspectFrame(ring, ring_data, capacity, frame_size);
    if (inspect <= 0) {
        return inspect;
    }
    if (!out || out_size < frame_size) {
        return -1;
    }

    uint32_t msg_len = static_cast<uint32_t>(frame_size);
    uint32_t r = ring->read_pos.load(std::memory_order_relaxed);
    ring->read_pos.store(
        shmAdvancePos(r, static_cast<uint32_t>(sizeof(uint32_t)), capacity),
        std::memory_order_release);

    uint32_t read_bytes = shmRingRead(ring, ring_data, out, msg_len, capacity);
    if (read_bytes != msg_len) {
        // 回滚长度前缀消费，避免 ring 状态不一致
        ring->read_pos.store(r, std::memory_order_release);
        return -1;
    }

    out_length = frame_size;
    return static_cast<int>(msg_len);
}

uint32_t shmRingRead(ShmRingHeader* ring, const uint8_t* ring_data,
                     uint8_t* buf, uint32_t length, uint32_t capacity)
{
    if (!ring || !ring_data || !buf || ring->capacity != capacity
        || ring->read_pos.load(std::memory_order_relaxed) >= capacity) {
        return 0;
    }
    uint32_t avail = shmRingAvailableRead(ring, capacity);
    uint32_t to_read = (length < avail) ? length : avail;
    if (to_read == 0) {
        return 0;
    }

    uint32_t r = ring->read_pos.load(std::memory_order_relaxed);
    uint32_t cap = capacity;

    uint32_t first = (to_read < cap - r) ? to_read : (cap - r);
    memcpy(buf, ring_data + r, first);

    if (first < to_read) {
        memcpy(buf + first, ring_data, to_read - first);
    }

    ring->read_pos.store(shmAdvancePos(r, to_read, cap), std::memory_order_release);

    return to_read;
}

int shmInspectFrame(const ShmRingHeader* ring, const uint8_t* ring_data,
                    uint32_t trusted_capacity, size_t& out_length)
{
    out_length = 0;
    if (!ring || !ring_data || trusted_capacity < SHM_MIN_RING_CAPACITY
        || ring->capacity != trusted_capacity) {
        return -1;
    }

    uint32_t r = ring->read_pos.load(std::memory_order_acquire);
    uint32_t w = ring->write_pos.load(std::memory_order_acquire);
    if (r >= trusted_capacity || w >= trusted_capacity) {
        return -1;
    }

    uint32_t avail = w >= r ? w - r : trusted_capacity - r + w;
    if (avail < sizeof(uint32_t)) {
        return 0;
    }

    uint32_t msg_len = 0;
    uint8_t* len_bytes = reinterpret_cast<uint8_t*>(&msg_len);
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        len_bytes[i] = ring_data[(r + i) % trusted_capacity];
    }

    const uint32_t max_frame = trusted_capacity - 1u - sizeof(uint32_t);
    if (msg_len == 0 || msg_len > MAX_MESSAGE_SIZE || msg_len > max_frame) {
        return -1;
    }
    uint64_t total_needed = static_cast<uint64_t>(sizeof(uint32_t)) + msg_len;
    if (total_needed > avail) {
        return 0;
    }
    out_length = msg_len;
    return 1;
}

bool shmValidateMappedLayout(void* addr, size_t mapped_size,
                             ShmControlBlock*& out_ctrl,
                             uint32_t& out_req_capacity,
                             uint32_t& out_resp_capacity)
{
    out_ctrl = NULL;
    out_req_capacity = 0;
    out_resp_capacity = 0;
    if (!addr || mapped_size < sizeof(ShmControlBlock)) {
        return false;
    }
    ShmControlBlock* ctrl = reinterpret_cast<ShmControlBlock*>(addr);
    if (ctrl->magic != SHM_MAGIC || ctrl->version != 1 || ctrl->ready_flag != 1) {
        return false;
    }
    const uint32_t req_capacity = ctrl->req_ring_capacity;
    const uint32_t resp_capacity = ctrl->resp_ring_capacity;
    size_t layout_size = calculateShmSize(req_capacity, resp_capacity);
    if (layout_size == 0 || layout_size > mapped_size) {
        return false;
    }
    ShmRingHeader* req_ring = shmRequestRingFromBase(static_cast<uint8_t*>(addr));
    ShmRingHeader* resp_ring = shmResponseRingFromBase(static_cast<uint8_t*>(addr),
                                                       req_capacity);
    if (ctrl->req_ring_capacity != req_capacity
        || ctrl->resp_ring_capacity != resp_capacity
        || req_ring->capacity != req_capacity
        || resp_ring->capacity != resp_capacity
        || req_ring->read_pos.load(std::memory_order_relaxed) >= req_ring->capacity
        || req_ring->write_pos.load(std::memory_order_relaxed) >= req_ring->capacity
        || resp_ring->read_pos.load(std::memory_order_relaxed) >= resp_ring->capacity
        || resp_ring->write_pos.load(std::memory_order_relaxed) >= resp_ring->capacity) {
        return false;
    }
    out_ctrl = ctrl;
    out_req_capacity = req_capacity;
    out_resp_capacity = resp_capacity;
    return true;
}

void shmInitLayout(void* addr, size_t mapped_size,
                   uint32_t req_capacity, uint32_t resp_capacity)
{
    if (!addr || mapped_size == 0) {
        return;
    }
    memset(addr, 0, mapped_size);

    ShmControlBlock* ctrl = reinterpret_cast<ShmControlBlock*>(addr);
    ctrl->magic = SHM_MAGIC;
    ctrl->version = 1;
    ctrl->req_ring_capacity = req_capacity;
    ctrl->resp_ring_capacity = resp_capacity;

    ShmRingHeader* req_ring = shmRequestRingFromBase(static_cast<uint8_t*>(addr));
    req_ring->write_pos.store(0, std::memory_order_relaxed);
    req_ring->read_pos.store(0, std::memory_order_relaxed);
    req_ring->capacity = req_capacity;
    req_ring->reserved = 0;

    ShmRingHeader* resp_ring = shmResponseRingFromBase(static_cast<uint8_t*>(addr),
                                                       req_capacity);
    resp_ring->write_pos.store(0, std::memory_order_relaxed);
    resp_ring->read_pos.store(0, std::memory_order_relaxed);
    resp_ring->capacity = resp_capacity;
    resp_ring->reserved = 0;

    platform::memoryBarrier();
    ctrl->ready_flag = 1;
}

} // namespace omnibinder
