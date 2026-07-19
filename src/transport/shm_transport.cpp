#include "transport/shm_transport.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"
#include "platform/platform.h"

#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <sstream>
#include <iomanip>

#define LOG_TAG "ShmTransport"

namespace omnibinder {

namespace {

const size_t MIN_RING_CAPACITY = 64;

bool isValidRingCapacity(size_t capacity)
{
    return capacity >= MIN_RING_CAPACITY
        && capacity <= static_cast<size_t>(std::numeric_limits<uint32_t>::max());
}

size_t normalizeRingCapacity(size_t capacity)
{
    if (capacity == 0) {
        return MIN_RING_CAPACITY;
    }
    if (capacity < MIN_RING_CAPACITY) {
        return MIN_RING_CAPACITY;
    }
    return capacity;
}

size_t alignUp(size_t value, size_t alignment)
{
    return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
}

size_t requestRingOffset()
{
    return alignUp(sizeof(ShmControlBlock), alignof(ShmRingHeader));
}

size_t requestDataOffset()
{
    return requestRingOffset() + sizeof(ShmRingHeader);
}

size_t responseRingOffset(size_t req_ring_capacity)
{
    return alignUp(requestDataOffset() + req_ring_capacity, alignof(ShmRingHeader));
}

size_t responseDataOffset(size_t req_ring_capacity)
{
    return responseRingOffset(req_ring_capacity) + sizeof(ShmRingHeader);
}

} // namespace

// ============================================================
// Utility functions
// ============================================================

size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity,
                        uint32_t max_clients)
{
    (void)max_clients;  // deprecated, kept for backward compat
    req_ring_capacity = normalizeRingCapacity(req_ring_capacity);
    resp_ring_capacity = normalizeRingCapacity(resp_ring_capacity);
    if (!isValidRingCapacity(req_ring_capacity) || !isValidRingCapacity(resp_ring_capacity)) {
        return 0;
    }
    size_t offset = responseDataOffset(req_ring_capacity);
    if (offset > std::numeric_limits<size_t>::max() - resp_ring_capacity) {
        return 0;
    }
    return offset + resp_ring_capacity;
}

std::string generateShmName(const std::string& service_name)
{
    // Use truncated prefix + FNV-1a hash to produce a unique,
    // bounded-length SHM name.  This prevents truncation when the
    // service name approaches MAX_SERVICE_NAME_LENGTH (256).
    const size_t kMaxPrefix = 48;
    std::string prefix = service_name.substr(0, kMaxPrefix);
    uint32_t hash = fnv1a_32(service_name);

    std::ostringstream os;
    os << "/binder_" << prefix << "_" << std::hex << hash;
    return os.str();
}

// ============================================================
// ShmTransport constructor / destructor
// ============================================================

ShmTransport::ShmTransport(const std::string& shm_name, bool is_server,
                           size_t req_ring_capacity, size_t resp_ring_capacity)
    : shm_name_(shm_name)
    , is_server_(is_server)
    , state_(ConnectionState::DISCONNECTED)
    , client_id_(0)
    , requested_req_ring_capacity_(normalizeRingCapacity(req_ring_capacity))
    , requested_resp_ring_capacity_(normalizeRingCapacity(resp_ring_capacity))
    , shm_addr_(NULL)
    , shm_size_(0)
    , ctrl_(NULL)
    , req_eventfd_(-1)
    , event_fd_(-1)
    , peer_notify_fd_(-1)
    , handshake_listener_(NULL)
    , handshake_channel_(NULL)
    , eventfd_enabled_(false)
    , next_client_id_(1)
{
    if (is_server_) {
        if (initServer()) {
            state_ = ConnectionState::CONNECTED;
        } else {
            state_ = ConnectionState::ERROR;
        }
    }
}

ShmTransport::~ShmTransport()
{
    close();
}

// ============================================================
// Server initialization: create 握手 listener only
// ============================================================
bool ShmTransport::initServer()
{
    // Create 握手 listen socket for client handshake
    handshake_path_ = getHandshakePath(shm_name_);
    handshake_listener_ = platform::handshakeListen(handshake_path_);
    if (!handshake_listener_) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create 握手 listen for '%s'", shm_name_.c_str());
        return false;
    }

    // Create a master eventfd for caller epoll registration
    req_eventfd_ = platform::createEventFd();
    if (req_eventfd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create master req_eventfd for '%s'", shm_name_.c_str());
        platform::handshakeCloseListener(handshake_listener_);
        handshake_listener_ = NULL;
        handshake_path_.clear();
        return false;
    }

    eventfd_enabled_ = true;

    OMNI_LOG_INFO(LOG_TAG, "Server listening on 握手通道 '%s' (fd=%d, req_eventfd=%d)",
                    handshake_path_.c_str(), handshakeListenFd(), req_eventfd_);
    return true;
}

// ============================================================
// Client initialization: create own SHM + 握手 name exchange
// ============================================================
bool ShmTransport::initClient()
{
    // Save the server identifier (used for handshake path)
    std::string server_name = shm_name_;

    // Generate unique SHM name for this client (atomic counter prevents
    // collisions when multiple ShmTransport instances exist in same process)
    {
        static std::atomic<uint64_t> s_instance_counter(0);
        uint64_t unique_id = s_instance_counter.fetch_add(1, std::memory_order_relaxed);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s_cli_%d_%lu",
                 server_name.c_str(), platform::getPid(),
                 static_cast<unsigned long>(unique_id));
        shm_name_ = std::string(buf);
    }

    size_t req_cap = requested_req_ring_capacity_;
    size_t resp_cap = requested_resp_ring_capacity_;
    if (!isValidRingCapacity(req_cap) || !isValidRingCapacity(resp_cap)) {
        OMNI_LOG_ERROR(LOG_TAG, "invalid SHM ring capacity req=%zu resp=%zu", req_cap, resp_cap);
        return false;
    }
    shm_size_ = calculateShmSize(req_cap, resp_cap, 1);
    if (shm_size_ == 0) {
        OMNI_LOG_ERROR(LOG_TAG, "invalid SHM size for req=%zu resp=%zu", req_cap, resp_cap);
        return false;
    }

    // Create the SHM region
    size_t mapped_size = 0;
    shm_addr_ = platform::shmCreate(shm_name_, shm_size_, true, &mapped_size);
    if (shm_addr_ == NULL) {
        OMNI_LOG_ERROR(LOG_TAG, "shmCreate failed for client '%s'", shm_name_.c_str());
        return false;
    }
    if (mapped_size < shm_size_) {
        OMNI_LOG_ERROR(LOG_TAG, "created SHM mapping too small: need=%zu mapped=%zu",
                       shm_size_, mapped_size);
        platform::shmDetach(shm_addr_, mapped_size);
        shm_addr_ = NULL;
        shm_size_ = 0;
        platform::shmUnlink(shm_name_);
        return false;
    }
    shm_size_ = mapped_size;

    // Zero-init control block + both ring headers
    memset(shm_addr_, 0, shm_size_);

    // Set up control block
    ctrl_ = reinterpret_cast<ShmControlBlock*>(shm_addr_);
    ctrl_->magic = SHM_MAGIC;
    ctrl_->version = 1;
    ctrl_->req_ring_capacity = static_cast<uint32_t>(req_cap);
    ctrl_->resp_ring_capacity = static_cast<uint32_t>(resp_cap);

    // Initialize request ring
    ShmRingHeader* req_ring = getRequestRing();
    req_ring->write_pos.store(0, std::memory_order_relaxed);
    req_ring->read_pos.store(0, std::memory_order_relaxed);
    req_ring->capacity = static_cast<uint32_t>(req_cap);
    req_ring->reserved = 0;

    // Initialize response ring
    ShmRingHeader* resp_ring = getResponseRing();
    resp_ring->write_pos.store(0, std::memory_order_relaxed);
    resp_ring->read_pos.store(0, std::memory_order_relaxed);
    resp_ring->capacity = static_cast<uint32_t>(resp_cap);
    resp_ring->reserved = 0;

    // Set feature flag
    ctrl_->reserved[0] |= SHM_FEATURE_EVENTFD;

    // Mark SHM as ready
    platform::memoryBarrier();
    ctrl_->ready_flag = 1;

    // Connect to server via handshake
    std::string path = getHandshakePath(server_name);
    platform::handshake_channel* ch = platform::handshakeConnect(path);
    if (!ch) {
        OMNI_LOG_WARN(LOG_TAG, "握手 connect failed for '%s', falling back from SHM",
                        server_name.c_str());
        cleanup();
        return false;
    }

    // Step 1: Send SHM name to server (no fds — server-side master eventfd
    //         will be sent to us in Step 2 for client→server notification)
    {
        if (!platform::handshakeSend(ch, shm_name_.data(), shm_name_.size(), NULL, 0)) {
            OMNI_LOG_WARN(LOG_TAG, "握手 send name failed for client '%s', falling back",
                            shm_name_.c_str());
            platform::handshakeClose(ch);
            cleanup();
            return false;
        }
    }

    // Step 2: Receive [resp_eventfd, master_eventfd] from server.
    //         resp_eventfd   → client epoll wakes when server writes response
    //         master_eventfd → client notifies this when writing request → wakes server epoll
    {
        int recv_fds[2] = {-1, -1};
        size_t response_len = 0;
        int recv_fd_count = 0;
        if (!platform::handshakeRecv(ch, NULL, 0, &response_len,
                                     recv_fds, 2, &recv_fd_count)
            || response_len != 0 || recv_fd_count != 2) {
            OMNI_LOG_WARN(LOG_TAG, "握手 recv fds failed for client '%s', falling back",
                            shm_name_.c_str());
            for (int i = 0; i < recv_fd_count; ++i) {
                if (recv_fds[i] >= 0) platform::closeEventFd(recv_fds[i]);
            }
            platform::handshakeClose(ch);
            cleanup();
            return false;
        }
        event_fd_ = recv_fds[0];
        peer_notify_fd_ = recv_fds[1];
    }

    handshake_channel_ = ch;

    eventfd_enabled_ = (event_fd_ >= 0 && peer_notify_fd_ >= 0);

    OMNI_LOG_INFO(LOG_TAG, "Client connected: shm='%s', event_fd=%d, notify_fd=%d",
                    shm_name_.c_str(), event_fd_, peer_notify_fd_);
    return true;
}

// ============================================================
// Pointer helpers: navigate SHM layout
// ============================================================

ShmRingHeader* ShmTransport::getRequestRingFromBase(uint8_t* base)
{
    return reinterpret_cast<ShmRingHeader*>(base + requestRingOffset());
}

uint8_t* ShmTransport::getRequestDataFromBase(uint8_t* base)
{
    return base + requestDataOffset();
}

ShmRingHeader* ShmTransport::getResponseRingFromBase(uint8_t* base, uint32_t req_capacity)
{
    return reinterpret_cast<ShmRingHeader*>(
        base + responseRingOffset(req_capacity));
}

uint8_t* ShmTransport::getResponseDataFromBase(uint8_t* base, uint32_t req_capacity)
{
    return base + responseDataOffset(req_capacity);
}

ShmRingHeader* ShmTransport::getRequestRing() const
{
    if (!shm_addr_) return NULL;
    return getRequestRingFromBase(static_cast<uint8_t*>(shm_addr_));
}

uint8_t* ShmTransport::getRequestData() const
{
    if (!shm_addr_) return NULL;
    return getRequestDataFromBase(static_cast<uint8_t*>(shm_addr_));
}

ShmRingHeader* ShmTransport::getResponseRing() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return getResponseRingFromBase(static_cast<uint8_t*>(shm_addr_),
                                   static_cast<uint32_t>(requested_req_ring_capacity_));
}

uint8_t* ShmTransport::getResponseData() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return getResponseDataFromBase(static_cast<uint8_t*>(shm_addr_),
                                   static_cast<uint32_t>(requested_req_ring_capacity_));
}

ShmRingHeader* ShmTransport::getRequestRing(const ClientShmContext& ctx) const
{
    if (!ctx.shm_addr) return NULL;
    return getRequestRingFromBase(static_cast<uint8_t*>(ctx.shm_addr));
}

uint8_t* ShmTransport::getRequestData(const ClientShmContext& ctx) const
{
    if (!ctx.shm_addr) return NULL;
    return getRequestDataFromBase(static_cast<uint8_t*>(ctx.shm_addr));
}

ShmRingHeader* ShmTransport::getResponseRing(const ClientShmContext& ctx) const
{
    if (!ctx.shm_addr || !ctx.ctrl) return NULL;
    return getResponseRingFromBase(static_cast<uint8_t*>(ctx.shm_addr),
                                   ctx.req_ring_capacity);
}

uint8_t* ShmTransport::getResponseData(const ClientShmContext& ctx) const
{
    if (!ctx.shm_addr || !ctx.ctrl) return NULL;
    return getResponseDataFromBase(static_cast<uint8_t*>(ctx.shm_addr),
                                   ctx.req_ring_capacity);
}

// ============================================================
// Ring buffer operations
// ============================================================

uint32_t ShmTransport::ringAvailableRead(const ShmRingHeader* ring, uint32_t cap) const
{
    if (!ring || cap < MIN_RING_CAPACITY || ring->capacity != cap) return 0;
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

uint32_t ShmTransport::ringAvailableWrite(const ShmRingHeader* ring, uint32_t capacity) const
{
    if (!ring || capacity < MIN_RING_CAPACITY || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_acquire) >= capacity
        || ring->read_pos.load(std::memory_order_acquire) >= capacity) {
        return 0;
    }
    uint32_t used = ringAvailableRead(ring, capacity);
    return capacity - 1 - used;
}

int ShmTransport::inspectFrame(const ShmRingHeader* ring, const uint8_t* ring_data,
                               uint32_t trusted_capacity, size_t& out_length) const
{
    out_length = 0;
    if (!ring || !ring_data || trusted_capacity < MIN_RING_CAPACITY
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

bool ShmTransport::validateMappedLayout(void* addr, size_t mapped_size,
                                        ShmControlBlock*& out_ctrl,
                                        uint32_t& out_req_capacity,
                                        uint32_t& out_resp_capacity) const
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
    size_t layout_size = calculateShmSize(req_capacity, resp_capacity, 1);
    if (layout_size == 0 || layout_size > mapped_size) {
        return false;
    }
    ShmRingHeader* req_ring = getRequestRingFromBase(static_cast<uint8_t*>(addr));
    ShmRingHeader* resp_ring = getResponseRingFromBase(static_cast<uint8_t*>(addr),
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

uint32_t ShmTransport::ringWrite(ShmRingHeader* ring, uint8_t* ring_data,
                                  const uint8_t* data, uint32_t length, uint32_t capacity)
{
    if (!ring || !ring_data || !data || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_relaxed) >= capacity) {
        return 0;
    }
    uint32_t avail = ringAvailableWrite(ring, capacity);
    uint32_t to_write = std::min(length, avail);
    if (to_write == 0) {
        return 0;
    }

    uint32_t w = ring->write_pos.load(std::memory_order_relaxed);
    uint32_t cap = capacity;

    uint32_t first = std::min(to_write, cap - w);
    memcpy(ring_data + w, data, first);

    if (first < to_write) {
        memcpy(ring_data, data + first, to_write - first);
    }

    ring->write_pos.store((w + to_write) % cap, std::memory_order_release);

    return to_write;
}

uint32_t ShmTransport::ringWriteFrame(ShmRingHeader* ring, uint8_t* ring_data,
                                      const uint8_t* data, uint32_t length,
                                      uint32_t capacity)
{
    if (!ring || !ring_data || !data || ring->capacity != capacity
        || ring->write_pos.load(std::memory_order_relaxed) >= capacity) {
        return 0;
    }
    uint32_t total = static_cast<uint32_t>(sizeof(uint32_t)) + length;
    if (ringAvailableWrite(ring, capacity) < total) return 0;

    uint32_t pos = ring->write_pos.load(std::memory_order_relaxed);
    uint32_t first = std::min(static_cast<uint32_t>(sizeof(uint32_t)), capacity - pos);
    memcpy(ring_data + pos, &length, first);
    if (first < sizeof(uint32_t)) {
        memcpy(ring_data, reinterpret_cast<const uint8_t*>(&length) + first,
               sizeof(uint32_t) - first);
    }
    pos = (pos + sizeof(uint32_t)) % capacity;
    first = std::min(length, capacity - pos);
    memcpy(ring_data + pos, data, first);
    if (first < length) memcpy(ring_data, data + first, length - first);
    ring->write_pos.store((pos + length) % capacity, std::memory_order_release);
    return total;
}

uint32_t ShmTransport::ringRead(ShmRingHeader* ring, const uint8_t* ring_data,
                                uint8_t* buf, uint32_t length, uint32_t capacity)
{
    if (!ring || !ring_data || !buf || ring->capacity != capacity
        || ring->read_pos.load(std::memory_order_relaxed) >= capacity) {
        return 0;
    }
    uint32_t avail = ringAvailableRead(ring, capacity);
    uint32_t to_read = std::min(length, avail);
    if (to_read == 0) {
        return 0;
    }

    uint32_t r = ring->read_pos.load(std::memory_order_relaxed);
    uint32_t cap = capacity;

    uint32_t first = std::min(to_read, cap - r);
    memcpy(buf, ring_data + r, first);

    if (first < to_read) {
        memcpy(buf + first, ring_data, to_read - first);
    }

    ring->read_pos.store((r + to_read) % cap, std::memory_order_release);

    return to_read;
}

// ============================================================
// Cleanup
// ============================================================

void ShmTransport::cleanupClientContext(ClientShmContext& ctx)
{
    if (ctx.shm_addr != NULL) {
        platform::shmDetach(ctx.shm_addr, ctx.shm_size);
        ctx.shm_addr = NULL;
    }
    // 客户端已断开时，服务端负责清理 SHM 对象避免 /dev/shm 残留
    if (is_server_ && !ctx.shm_name.empty()) {
        platform::shmUnlink(ctx.shm_name);
    }
    if (ctx.resp_eventfd >= 0) {
        platform::closeEventFd(ctx.resp_eventfd);
        ctx.resp_eventfd = -1;
    }
    if (ctx.request_notify_fd >= 0) {
        platform::closeEventFd(ctx.request_notify_fd);
        ctx.request_notify_fd = -1;
    }
    if (ctx.liveness_channel) {
        platform::handshakeClose(ctx.liveness_channel);
        ctx.liveness_channel = NULL;
    }
    ctx.ctrl = NULL;
    ctx.shm_size = 0;
}

void ShmTransport::cleanup()
{
    if (is_server_) {
        // Close all client contexts
        for (std::map<uint32_t, ClientShmContext>::iterator it = client_contexts_.begin();
             it != client_contexts_.end(); ++it) {
            cleanupClientContext(it->second);
        }
        client_contexts_.clear();

        // Close master eventfd
        if (req_eventfd_ >= 0) {
            platform::closeEventFd(req_eventfd_);
            req_eventfd_ = -1;
        }

        // Close handshake listener and remove its endpoint.
        if (handshake_listener_) {
            platform::handshakeCloseListener(handshake_listener_);
            handshake_listener_ = NULL;
        }
        handshake_path_.clear();
    } else {
        if (handshake_channel_) {
            platform::handshakeClose(handshake_channel_);
            handshake_channel_ = NULL;
        }
        // Client: close eventfds
        if (peer_notify_fd_ >= 0) {
            platform::closeEventFd(peer_notify_fd_);
            peer_notify_fd_ = -1;
        }
        if (event_fd_ >= 0) {
            platform::closeEventFd(event_fd_);
            event_fd_ = -1;
        }

        // Detach SHM
        if (shm_addr_ != NULL) {
            platform::shmDetach(shm_addr_, shm_size_);
            shm_addr_ = NULL;
        }

        // Unlink client's SHM
        if (!shm_name_.empty()) {
            platform::shmUnlink(shm_name_);
        }

        ctrl_ = NULL;
        shm_size_ = 0;
    }

    eventfd_enabled_ = false;
}

// ============================================================
// ITransport interface - Client side
// ============================================================

int ShmTransport::connect(const std::string& host, uint16_t port)
{
    (void)host;
    (void)port;

    if (is_server_) {
        OMNI_LOG_ERROR(LOG_TAG, "connect() should not be called on server side");
        return -1;
    }

    if (state_ == ConnectionState::CONNECTED) {
        return 0;
    }

    if (!initClient()) {
        state_ = ConnectionState::ERROR;
        return -1;
    }

    state_ = ConnectionState::CONNECTED;
    return 0;
}

int ShmTransport::send(const uint8_t* data, size_t length)
{
    if (state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    if (is_server_) {
        OMNI_LOG_ERROR(LOG_TAG, "Client send() called on server-side transport");
        return -1;
    }

    if (length == 0) {
        return 0;
    }

    if (length > MAX_MESSAGE_SIZE || length > 0x7FFFFFFF) {
        OMNI_LOG_ERROR(LOG_TAG, "send() message too large: %zu bytes", length);
        return -1;
    }

    // Message format in request ring: [length(4B)] [payload(NB)]
    uint32_t total_needed = static_cast<uint32_t>(sizeof(uint32_t) + length);

    ShmRingHeader* req_ring = getRequestRing();
    uint8_t* req_data = getRequestData();

    if (!req_ring || !req_data) {
        OMNI_LOG_ERROR(LOG_TAG, "send() missing request ring");
        return -1;
    }

    uint32_t req_capacity = static_cast<uint32_t>(requested_req_ring_capacity_);
    bool was_empty = (ringAvailableRead(req_ring, req_capacity) == 0);
    uint32_t avail = ringAvailableWrite(req_ring, req_capacity);
    if (avail < total_needed) {
        OMNI_LOG_DEBUG(LOG_TAG, "send() request ring full: need %u, avail %u",
                         total_needed, avail);
        return 0;
    }

    uint32_t write_start = req_ring->write_pos.load(std::memory_order_relaxed);
    ringWriteFrame(req_ring, req_data, data, static_cast<uint32_t>(length), req_capacity);

    if (!was_empty) {
        was_empty = (req_ring->read_pos.load(std::memory_order_acquire) == write_start);
    }

    // Notify only on the empty-to-nonempty transition. If enqueue races with
    // a drain of the previous contents, the post-write read position detects
    // that this frame became the new empty-to-nonempty transition.
    if (was_empty || req_ring->read_pos.load(std::memory_order_acquire) == write_start) {
        if (eventfd_enabled_ && peer_notify_fd_ >= 0) {
        platform::eventFdNotify(peer_notify_fd_);
        }
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Client sent %zu bytes on shm '%s'",
                     length, shm_name_.c_str());
    return static_cast<int>(length);
}

int ShmTransport::recv(uint8_t* buf, size_t buf_size)
{
    if (state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    if (is_server_) {
        OMNI_LOG_ERROR(LOG_TAG, "Client recv() called on server-side transport");
        return -1;
    }

    if (buf_size == 0) {
        return 0;
    }

    {
        ShmRingHeader* resp_ring = getResponseRing();
        const uint8_t* resp_data = getResponseData();

        if (!resp_ring || !resp_data) {
            OMNI_LOG_ERROR(LOG_TAG, "recv() missing response ring");
            return -1;
        }

        size_t frame_size = 0;
        int inspect = inspectFrame(resp_ring, resp_data,
                                   static_cast<uint32_t>(requested_resp_ring_capacity_),
                                   frame_size);
        if (inspect <= 0) {
            if (inspect < 0) {
                OMNI_LOG_ERROR(LOG_TAG, "recv() malformed response ring");
                state_ = ConnectionState::ERROR;
            }
            return inspect;
        }
        uint32_t msg_len = static_cast<uint32_t>(frame_size);

        if (buf_size < msg_len) {
            OMNI_LOG_WARN(LOG_TAG, "recv() buffer too small: need %u, have %zu",
                            msg_len, buf_size);
            return -1;
        }

        uint32_t r = resp_ring->read_pos.load(std::memory_order_relaxed);
        uint32_t cap = static_cast<uint32_t>(requested_resp_ring_capacity_);
        resp_ring->read_pos.store((r + sizeof(uint32_t)) % cap, std::memory_order_release);

        uint32_t read_bytes = ringRead(resp_ring, resp_data, buf, msg_len, cap);
        if (read_bytes != msg_len) {
            OMNI_LOG_ERROR(LOG_TAG, "recv() failed to read payload (%u of %u)",
                             read_bytes, msg_len);
            state_ = ConnectionState::ERROR;
            return -1;
        }

        OMNI_LOG_DEBUG(LOG_TAG, "Client received %u bytes on shm '%s'",
                         msg_len, shm_name_.c_str());
        return static_cast<int>(msg_len);
    }
}

int ShmTransport::nextRecvSize(size_t& out_length)
{
    out_length = 0;
    if (is_server_ || state_ != ConnectionState::CONNECTED || !ctrl_) return -1;
    int ret = inspectFrame(getResponseRing(), getResponseData(),
                           static_cast<uint32_t>(requested_resp_ring_capacity_), out_length);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "nextRecvSize() malformed response ring");
        state_ = ConnectionState::ERROR;
    }
    return ret;
}

void ShmTransport::close()
{
    if (state_ == ConnectionState::DISCONNECTED && shm_addr_ == NULL
        && client_contexts_.empty()) {
        return;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Closing shm transport '%s' (server=%d)",
                     shm_name_.c_str(), is_server_);

    cleanup();
    state_ = ConnectionState::DISCONNECTED;
}

ConnectionState ShmTransport::state() const
{
    return state_;
}

int ShmTransport::fd() const
{
    if (is_server_) {
        return req_eventfd_;
    }
    return event_fd_;
}

TransportType ShmTransport::type() const
{
    return TransportType::SHM;
}

// ============================================================
// Server-side operations (per-client model)
// ============================================================

int ShmTransport::serverRecv(uint8_t* buf, size_t buf_size, uint32_t& out_client_id)
{
    if (!is_server_ || state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    // Iterate all connected client contexts
    for (std::map<uint32_t, ClientShmContext>::iterator it = client_contexts_.begin();
         it != client_contexts_.end(); ++it) {
        uint32_t cid = it->first;
        ClientShmContext& ctx = it->second;

        ShmRingHeader* req_ring = getRequestRing(ctx);
        const uint8_t* req_data = getRequestData(ctx);

        if (!req_ring || !req_data) {
            continue;
        }

        size_t frame_size = 0;
        int inspect = inspectFrame(req_ring, req_data, ctx.req_ring_capacity,
                                   frame_size);
        if (inspect < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "serverRecv() malformed request ring from client[%u], disconnecting",
                           cid);
            // ShmTransport reports the abnormal client; the callback is
            // contractually responsible for the full cleanup sequence
            // (deregister fds → drop topics → removeClient).  See header.
            if (on_client_disconnected_cb_) {
                on_client_disconnected_cb_(cid, platform::handshakeGetFd(ctx.liveness_channel),
                                           ctx.request_notify_fd);
            }
            return -1;
        }
        if (inspect == 0) continue;
        uint32_t msg_len = static_cast<uint32_t>(frame_size);

        if (msg_len > 0 && buf_size < msg_len) {
            OMNI_LOG_WARN(LOG_TAG, "serverRecv() buffer too small: need %u, have %zu",
                            msg_len, buf_size);
            continue;
        }

        uint32_t r = req_ring->read_pos.load(std::memory_order_relaxed);
        uint32_t cap = ctx.req_ring_capacity;
        req_ring->read_pos.store((r + sizeof(uint32_t)) % cap, std::memory_order_release);

        out_client_id = cid;

        uint32_t read_bytes = ringRead(req_ring, req_data, buf, msg_len, cap);
        if (read_bytes != msg_len) {
            OMNI_LOG_ERROR(LOG_TAG, "serverRecv() failed to read payload (%u of %u) from client[%u]",
                             read_bytes, msg_len, cid);
            // Roll back the length-prefix consumption so the message can be
            // retried on the next poll cycle rather than leaving the ring in
            // an inconsistent state.
            req_ring->read_pos.store(r, std::memory_order_release);
            continue;
        }

        OMNI_LOG_DEBUG(LOG_TAG, "Server received %u bytes from client[%u] on shm '%s'",
                         msg_len, cid, ctx.shm_name.c_str());
        return static_cast<int>(msg_len);
    }

    return 0;
}

int ShmTransport::nextServerRecvSize(size_t& out_length, uint32_t& out_client_id)
{
    out_length = 0;
    if (!is_server_ || state_ != ConnectionState::CONNECTED) return -1;
    for (std::map<uint32_t, ClientShmContext>::iterator it = client_contexts_.begin();
         it != client_contexts_.end(); ++it) {
        ClientShmContext& ctx = it->second;
        int ret = inspectFrame(getRequestRing(ctx), getRequestData(ctx),
                               ctx.req_ring_capacity, out_length);
        if (ret < 0) {
            uint32_t client_id = it->first;
            OMNI_LOG_ERROR(LOG_TAG, "nextServerRecvSize() malformed ring from client[%u], disconnecting",
                           client_id);
            // Same cleanup contract as serverRecv(): callback → owner-loop
            // cleanup → removeClient().  See header.
            if (on_client_disconnected_cb_) {
                on_client_disconnected_cb_(client_id,
                                           platform::handshakeGetFd(ctx.liveness_channel),
                                           ctx.request_notify_fd);
            }
            out_client_id = client_id;
            return -1;
        }
        if (ret > 0) {
            out_client_id = it->first;
            return 1;
        }
    }
    return 0;
}

int ShmTransport::serverSend(uint32_t client_id, const uint8_t* data, size_t length)
{
    if (!is_server_ || state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    std::map<uint32_t, ClientShmContext>::iterator it = client_contexts_.find(client_id);
    if (it == client_contexts_.end()) {
        OMNI_LOG_DEBUG(LOG_TAG, "serverSend() client[%u] not found", client_id);
        return 0;
    }

    return serverSendToContext(it->second, client_id, data, length);
}

bool ShmTransport::removeClient(uint32_t client_id)
{
    if (!is_server_) return false;
    std::map<uint32_t, ClientShmContext>::iterator it = client_contexts_.find(client_id);
    if (it == client_contexts_.end()) return false;
    cleanupClientContext(it->second);
    client_contexts_.erase(it);
    OMNI_LOG_INFO(LOG_TAG, "client[%u] disconnected from shm '%s'",
                  client_id, shm_name_.c_str());
    return true;
}

int ShmTransport::serverSendToContext(ClientShmContext& ctx, uint32_t client_id,
                                       const uint8_t* data, size_t length)
{
    if (length == 0) {
        return 0;
    }

    if (length > MAX_MESSAGE_SIZE || length > 0x7FFFFFFF) {
        return -1;
    }

    ShmRingHeader* resp_ring = getResponseRing(ctx);
    uint8_t* resp_data = getResponseData(ctx);

    if (!resp_ring || !resp_data) {
        OMNI_LOG_ERROR(LOG_TAG, "serverSend() missing response ring for client[%u]", client_id);
        return -1;
    }

    uint32_t total_needed = static_cast<uint32_t>(sizeof(uint32_t) + length);
    uint32_t resp_capacity = ctx.resp_ring_capacity;
    bool was_empty = (ringAvailableRead(resp_ring, resp_capacity) == 0);
    uint32_t write_start = resp_ring->write_pos.load(std::memory_order_relaxed);
    uint32_t avail = ringAvailableWrite(resp_ring, resp_capacity);

    if (avail < total_needed) {
        OMNI_LOG_DEBUG(LOG_TAG, "serverSend() response ring full for client[%u]: need %u, avail %u",
                         client_id, total_needed, avail);
        return 0;
    }

    ringWriteFrame(resp_ring, resp_data, data, static_cast<uint32_t>(length), resp_capacity);

    if (!was_empty) {
        was_empty = (resp_ring->read_pos.load(std::memory_order_acquire) == write_start);
    }

    if ((was_empty || resp_ring->read_pos.load(std::memory_order_acquire) == write_start)
        && eventfd_enabled_ && ctx.resp_eventfd >= 0) {
        platform::eventFdNotify(ctx.resp_eventfd);
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Server sent %zu bytes to client[%u] on shm '%s'",
                     length, client_id, ctx.shm_name.c_str());
    return static_cast<int>(length);
}

bool ShmTransport::waitReady(uint32_t timeout_ms)
{
    if (is_server_) {
        return state_ == ConnectionState::CONNECTED;
    }

    uint32_t elapsed = 0;
    const uint32_t interval = 1;
    while (state_ != ConnectionState::CONNECTED && elapsed < timeout_ms) {
        platform::sleepMs(interval);
        elapsed += interval;
    }
    return state_ == ConnectionState::CONNECTED;
}

uint32_t ShmTransport::clientCount() const
{
    if (is_server_) {
        return static_cast<uint32_t>(client_contexts_.size());
    }
    return 0;
}

std::vector<uint32_t> ShmTransport::activeClientIds() const
{
    std::vector<uint32_t> ids;
    if (!is_server_) {
        return ids;
    }

    for (std::map<uint32_t, ClientShmContext>::const_iterator it = client_contexts_.begin();
         it != client_contexts_.end(); ++it) {
        ids.push_back(it->first);
    }
    return ids;
}

// ============================================================
// 握手 eventfd + SHM name exchange (server side)
// ============================================================

void ShmTransport::onHandshakeClientConnect()
{
    platform::handshake_channel* ch = platform::handshakeAccept(handshake_listener_);
    if (!ch) {
        return;
    }

    // Step 1: Receive SHM name from the client; no handles are allowed.
    char name_buf[256] = {0};
    size_t data_len = 0;
    int fd_count = 0;
    if (!platform::handshakeRecv(ch, name_buf, sizeof(name_buf) - 1, &data_len,
                                 NULL, 0, &fd_count)
        || fd_count != 0 || data_len == 0) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: invalid client request");
        platform::handshakeClose(ch);
        return;
    }

    std::string client_shm_name(name_buf, data_len);

    if (client_shm_name.empty()) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: empty SHM name from client");
        platform::handshakeClose(ch);
        return;
    }

    // Open the client's SHM
    size_t mapped_size = 0;
    void* addr = platform::shmCreate(client_shm_name, sizeof(ShmControlBlock), false,
                                     &mapped_size);
    if (addr == NULL) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: failed to open client SHM '%s'",
                        client_shm_name.c_str());
        platform::handshakeClose(ch);
        return;
    }

    ShmControlBlock* ctrl = NULL;
    uint32_t req_capacity = 0;
    uint32_t resp_capacity = 0;
    if (!validateMappedLayout(addr, mapped_size, ctrl, req_capacity, resp_capacity)) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: SHM object does not cover a valid layout for '%s'",
                      client_shm_name.c_str());
        platform::shmDetach(addr, mapped_size);
        platform::handshakeClose(ch);
        return;
    }

    // Step 2: Create resp_eventfd for this client
    int resp_efd = platform::createEventFd();
    if (resp_efd < 0) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: failed to create resp_eventfd for client '%s'",
                        client_shm_name.c_str());
        platform::shmDetach(addr, mapped_size);
        platform::handshakeClose(ch);
        return;
    }

    // Step 3: Send exactly [response notify, request notify]. The platform
    // may substitute a server-owned local request notification internally.
    //         resp_eventfd → client epoll wakes when server writes response
    //         Linux: platform sends shared master_eventfd
    //         Windows: platform creates and exposes a per-client local notify descriptor
    {
        int response_fds[2] = {resp_efd, req_eventfd_};
        if (!platform::handshakeSend(ch, NULL, 0, response_fds, 2)) {
            OMNI_LOG_WARN(LOG_TAG, "handshake: failed to send eventfds to client '%s'",
                            client_shm_name.c_str());
            platform::shmDetach(addr, mapped_size);
            platform::closeEventFd(resp_efd);
            platform::handshakeClose(ch);
            return;
        }
    }

    int local_notify_fd = platform::handshakeTakeLocalNotifyFd(ch);

    // Assign client ID and store context
    uint32_t assigned_id = next_client_id_++;

    ClientShmContext ctx;
    ctx.client_id = assigned_id;
    ctx.shm_name = client_shm_name;
    ctx.shm_addr = addr;
    ctx.shm_size = mapped_size;
    ctx.ctrl = ctrl;
    ctx.req_ring_capacity = req_capacity;
    ctx.resp_ring_capacity = resp_capacity;
    ctx.resp_eventfd = resp_efd;
    ctx.request_notify_fd = local_notify_fd;
    ctx.liveness_channel = ch;

    client_contexts_[assigned_id] = ctx;

    // The context owns every cleanup resource before the owner loop can observe it.
    if (on_client_connected_cb_) {
        on_client_connected_cb_(assigned_id, platform::handshakeGetFd(ch), local_notify_fd);
    }

    // Notify the master eventfd to wake up the event loop (data from new client
    // may need servicing if server uses polling/scanning callback)
    if (req_eventfd_ >= 0) {
        platform::eventFdNotify(req_eventfd_);
    }

    OMNI_LOG_INFO(LOG_TAG, "handshake: client[%u] connected, shm='%s', resp_efd=%d",
                    assigned_id, client_shm_name.c_str(), resp_efd);
}

// ============================================================
// getHandshakePath
// ============================================================

std::string ShmTransport::getHandshakePath(const std::string& shm_name)
{
    // Use a hash-based path to stay safely within the Linux sun_path
    // limit (typically 108 bytes).  The full shm_name may be too long.
    uint32_t hash = fnv1a_32(shm_name);
    std::ostringstream os;
    os << "/tmp/omni_" << std::hex << hash << ".sock";
    return os.str();
}

} // namespace omnibinder
