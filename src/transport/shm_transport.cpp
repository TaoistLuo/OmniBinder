#include "transport/shm_transport.h"
#include "omnibinder/log.h"

#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <poll.h>

#if defined(__GNUC__)
#define OMNI_POPCOUNT32 __builtin_popcount
#endif

#define LOG_TAG "ShmTransport"

namespace omnibinder {

// ============================================================
// Utility functions
// ============================================================

size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity,
                        uint32_t max_clients)
{
    // Layout:
    //   [ShmControlBlock]
    //   [RequestQueue ring header + data]
    //   [ResponseSlot[0] ring header + data]
    //   [ResponseSlot[1] ring header + data]
    //   ...
    //   [ResponseSlot[max_clients-1] ring header + data]
    return sizeof(ShmControlBlock)
         + sizeof(ShmRingHeader) + req_ring_capacity
         + (sizeof(ShmRingHeader) + resp_ring_capacity) * max_clients;
}

std::string generateShmName(const std::string& service_name)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "/binder_%s", service_name.c_str());
    return std::string(buf);
}

uint32_t ShmTransport::countBits(uint32_t value) const
{
#if defined(OMNI_POPCOUNT32)
    return static_cast<uint32_t>(OMNI_POPCOUNT32(value));
#else
    uint32_t count = 0;
    while (value) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
#endif
}

// ============================================================
// ShmTransport implementation
// ============================================================

ShmTransport::ShmTransport(const std::string& shm_name, bool is_server,
                           size_t req_ring_capacity, size_t resp_ring_capacity)
    : shm_name_(shm_name)
    , is_server_(is_server)
    , state_(ConnectionState::DISCONNECTED)
    , client_id_(0)
    , requested_req_ring_capacity_(req_ring_capacity)
    , requested_resp_ring_capacity_(resp_ring_capacity)
    , shm_addr_(NULL)
    , shm_size_(0)
    , ctrl_(NULL)
    , req_eventfd_(-1)
    , peer_req_eventfd_(-1)
    , event_fd_(-1)
    , uds_listen_fd_(-1)
    , eventfd_enabled_(false)
{
    for (uint32_t i = 0; i < SHM_MAX_CLIENTS; ++i) {
        resp_eventfds_[i] = -1;
    }

    if (is_server_) {
        if (initServer()) {
            state_ = ConnectionState::CONNECTED;
            OMNI_LOG_INFO(LOG_TAG, "Server created shm '%s' (size=%zu, max_clients=%u)",
                            shm_name_.c_str(), shm_size_, SHM_MAX_CLIENTS);
        } else {
            state_ = ConnectionState::ERROR;
            OMNI_LOG_ERROR(LOG_TAG, "Failed to create server shm '%s'", shm_name_.c_str());
        }
    }
    // Client side: state stays DISCONNECTED until connect() is called
}

ShmTransport::~ShmTransport()
{
    close();
}

// ============================================================
// Server initialization: create and initialize the SHM region
// ============================================================
bool ShmTransport::initServer()
{
    size_t req_ring_capacity = requested_req_ring_capacity_;
    size_t resp_ring_capacity = requested_resp_ring_capacity_;
    shm_size_ = calculateShmSize(req_ring_capacity, resp_ring_capacity, SHM_MAX_CLIENTS);

    // Unlink any stale SHM from a previous crash
    platform::shmUnlink(shm_name_);

    shm_addr_ = platform::shmCreate(shm_name_, shm_size_, true);
    if (shm_addr_ == NULL) {
        OMNI_LOG_ERROR(LOG_TAG, "shmCreate failed for '%s'", shm_name_.c_str());
        return false;
    }

    // Only initialize metadata and request ring area.
    memset(shm_addr_, 0, sizeof(ShmControlBlock) + sizeof(ShmRingHeader) + req_ring_capacity);

    // Set up control block
    ctrl_ = reinterpret_cast<ShmControlBlock*>(shm_addr_);
    ctrl_->magic = SHM_MAGIC;
    ctrl_->version = 1;
    ctrl_->max_clients = SHM_MAX_CLIENTS;
    ctrl_->active_clients = 0;
    ctrl_->req_ring_capacity = static_cast<uint32_t>(req_ring_capacity);
    ctrl_->resp_ring_capacity = static_cast<uint32_t>(resp_ring_capacity);
    ctrl_->response_bitmap = 0;
    ctrl_->req_lock.init();

    // Initialize all slots as inactive
    for (uint32_t i = 0; i < SHM_MAX_CLIENTS; ++i) {
        ctrl_->slots[i].active = 0;
    }

    // Initialize RequestQueue ring header
    ShmRingHeader* req_ring = getRequestRing();
    req_ring->write_pos = 0;
    req_ring->read_pos = 0;
    req_ring->capacity = static_cast<uint32_t>(req_ring_capacity);
    req_ring->reserved = 0;

    // Create eventfds for notification
    req_eventfd_ = platform::createEventFd();
    if (req_eventfd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create req_eventfd for '%s'", shm_name_.c_str());
        return false;
    }

    for (uint32_t i = 0; i < SHM_MAX_CLIENTS; ++i) {
        resp_eventfds_[i] = platform::createEventFd();
        if (resp_eventfds_[i] < 0) {
            OMNI_LOG_ERROR(LOG_TAG, "Failed to create resp_eventfd[%u] for '%s'",
                             i, shm_name_.c_str());
            return false;
        }
    }

    // Create UDS listen socket for eventfd exchange
    uds_path_ = getShmUdsPath(shm_name_);
    uds_listen_fd_ = platform::udsBindListen(uds_path_);
    if (uds_listen_fd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS listen for '%s'", shm_name_.c_str());
        return false;
    }

    // Set feature flag
    ctrl_->reserved[0] |= SHM_FEATURE_EVENTFD;

    eventfd_enabled_ = true;

    // Mark as ready for clients
    __sync_synchronize();
    ctrl_->ready_flag = 1;

    OMNI_LOG_DEBUG(LOG_TAG, "Server eventfd enabled: req_eventfd=%d, uds=%s",
                     req_eventfd_, uds_path_.c_str());
    return true;
}

// ============================================================
// Client initialization: open existing SHM and allocate a slot
// ============================================================
bool ShmTransport::initClient()
{
    // Calculate the expected size (same constants as server)
    shm_size_ = calculateShmSize(SHM_DEFAULT_REQ_RING_CAPACITY,
                                 SHM_DEFAULT_RESP_RING_CAPACITY,
                                 SHM_MAX_CLIENTS);

    // Open existing SHM (don't create)
    shm_addr_ = platform::shmCreate(shm_name_, shm_size_, false);
    if (shm_addr_ == NULL) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to open shm '%s'", shm_name_.c_str());
        return false;
    }

    ctrl_ = reinterpret_cast<ShmControlBlock*>(shm_addr_);

    // Validate magic
    if (ctrl_->magic != SHM_MAGIC) {
        OMNI_LOG_ERROR(LOG_TAG, "Invalid shm magic: 0x%08X (expected 0x%08X)",
                         ctrl_->magic, SHM_MAGIC);
        return false;
    }

    // Calculate actual size for reference
    shm_size_ = calculateShmSize(ctrl_->req_ring_capacity,
                                 ctrl_->resp_ring_capacity,
                                 ctrl_->max_clients);

    uint32_t slot = ctrl_->max_clients;
    for (uint32_t i = 0; i < ctrl_->max_clients; ++i) {
        if (__sync_bool_compare_and_swap(&ctrl_->slots[i].active, 0, 1)) {
            slot = i;
            break;
        }
    }

    if (slot >= ctrl_->max_clients) {
        OMNI_LOG_ERROR(LOG_TAG, "SHM '%s' full: no free slot", shm_name_.c_str());
        return false;
    }

    if (!allocateResponseSlot(slot)) {
        ctrl_->slots[slot].active = 0;
        return false;
    }

    client_id_ = slot;
    __sync_fetch_and_add(&ctrl_->active_clients, 1);
    __sync_synchronize();

    // Exchange eventfds via UDS if server supports it
    if (ctrl_->reserved[0] & SHM_FEATURE_EVENTFD) {
        std::string uds_path = getShmUdsPath(shm_name_);
        int uds_fd = platform::udsConnect(uds_path);
        if (uds_fd < 0) {
            OMNI_LOG_WARN(LOG_TAG, "UDS connect failed for '%s', falling back from SHM",
                            shm_name_.c_str());
            cleanup();
            return false;
        } else {
            // Send our client_id
            uint32_t cid = client_id_;
            ssize_t n = ::send(uds_fd, &cid, sizeof(cid), 0);
            if (n != sizeof(cid)) {
                OMNI_LOG_WARN(LOG_TAG, "UDS send client_id failed for '%s', falling back from SHM",
                                shm_name_.c_str());
                platform::udsClose(uds_fd);
                cleanup();
                return false;
            } else {
                // Wait for server to accept and send fds (with short timeout)
                struct pollfd pfd;
                pfd.fd = uds_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                int poll_ret = ::poll(&pfd, 1, 100);  // 100ms timeout

                if (poll_ret > 0 && (pfd.revents & POLLIN)) {
                    // Receive 2 fds: [req_eventfd, resp_eventfd]
                    int fds[2] = {-1, -1};
                    if (platform::udsRecvFds(uds_fd, fds, 2, NULL, 0, NULL)) {
                        peer_req_eventfd_ = fds[0];
                        event_fd_ = fds[1];  // resp_eventfd, fd() returns this
                        eventfd_enabled_ = true;
                        OMNI_LOG_INFO(LOG_TAG, "Client[%u] eventfd exchange OK: "
                                        "peer_req=%d, resp=%d",
                                        slot, peer_req_eventfd_, event_fd_);
                    } else {
                        OMNI_LOG_WARN(LOG_TAG, "UDS recvfds failed for '%s', falling back from SHM",
                                        shm_name_.c_str());
                        platform::udsClose(uds_fd);
                        cleanup();
                        return false;
                    }
                } else {
                    OMNI_LOG_WARN(LOG_TAG, "UDS recv timeout for '%s', falling back from SHM",
                                    shm_name_.c_str());
                    platform::udsClose(uds_fd);
                    cleanup();
                    return false;
                }
                platform::udsClose(uds_fd);
            }
        }
    } else {
        OMNI_LOG_WARN(LOG_TAG, "SHM '%s' missing eventfd feature, falling back from SHM",
                        shm_name_.c_str());
        cleanup();
        return false;
    }

    OMNI_LOG_INFO(LOG_TAG, "Client connected to shm '%s', slot=%u, eventfd=yes",
                    shm_name_.c_str(), slot);
    return true;
}

// ============================================================
// Pointer helpers: navigate the SHM layout
// ============================================================

ShmRingHeader* ShmTransport::getRequestRing() const
{
    uint8_t* base = static_cast<uint8_t*>(shm_addr_);
    return reinterpret_cast<ShmRingHeader*>(base + sizeof(ShmControlBlock));
}

uint8_t* ShmTransport::getRequestData() const
{
    uint8_t* base = static_cast<uint8_t*>(shm_addr_);
    return base + sizeof(ShmControlBlock) + sizeof(ShmRingHeader);
}

uint8_t* ShmTransport::getResponseArenaBase() const
{
    uint8_t* base = static_cast<uint8_t*>(shm_addr_);
    return base + sizeof(ShmControlBlock) + sizeof(ShmRingHeader) + ctrl_->req_ring_capacity;
}

size_t ShmTransport::responseBlockSize() const
{
    return sizeof(ShmRingHeader) + ctrl_->resp_ring_capacity;
}

ShmRingHeader* ShmTransport::getResponseRing(uint32_t slot) const
{
    if (!ctrl_ || slot >= ctrl_->max_clients) {
        return NULL;
    }
    uint8_t* arena = getResponseArenaBase();
    return reinterpret_cast<ShmRingHeader*>(arena + ctrl_->slots[slot].response_offset);
}

uint8_t* ShmTransport::getResponseData(uint32_t slot) const
{
    ShmRingHeader* ring = getResponseRing(slot);
    if (!ring) {
        return NULL;
    }
    return reinterpret_cast<uint8_t*>(ring) + sizeof(ShmRingHeader);
}

// ============================================================
// Ring buffer operations
// ============================================================

uint32_t ShmTransport::ringAvailableRead(const ShmRingHeader* ring) const
{
    uint32_t w = ring->write_pos;
    uint32_t r = ring->read_pos;
    if (w >= r) {
        return w - r;
    }
    return ring->capacity - r + w;
}

uint32_t ShmTransport::ringAvailableWrite(const ShmRingHeader* ring) const
{
    // Reserve 1 byte to distinguish full from empty
    uint32_t used = ringAvailableRead(ring);
    return ring->capacity - 1 - used;
}

uint32_t ShmTransport::ringWrite(ShmRingHeader* ring, uint8_t* ring_data,
                                 const uint8_t* data, uint32_t length)
{
    uint32_t avail = ringAvailableWrite(ring);
    uint32_t to_write = std::min(length, avail);
    if (to_write == 0) {
        return 0;
    }

    uint32_t w = ring->write_pos;
    uint32_t cap = ring->capacity;

    uint32_t first = std::min(to_write, cap - w);
    memcpy(ring_data + w, data, first);

    if (first < to_write) {
        memcpy(ring_data, data + first, to_write - first);
    }

    __sync_synchronize();
    ring->write_pos = (w + to_write) % cap;

    return to_write;
}

uint32_t ShmTransport::ringRead(ShmRingHeader* ring, const uint8_t* ring_data,
                                uint8_t* buf, uint32_t length)
{
    uint32_t avail = ringAvailableRead(ring);
    uint32_t to_read = std::min(length, avail);
    if (to_read == 0) {
        return 0;
    }

    uint32_t r = ring->read_pos;
    uint32_t cap = ring->capacity;

    uint32_t first = std::min(to_read, cap - r);
    memcpy(buf, ring_data + r, first);

    if (first < to_read) {
        memcpy(buf + first, ring_data, to_read - first);
    }

    __sync_synchronize();
    ring->read_pos = (r + to_read) % cap;

    return to_read;
}

// ============================================================
// Cleanup
// ============================================================

void ShmTransport::cleanup()
{
    // If client, mark slot as inactive
    if (!is_server_ && ctrl_ && client_id_ < SHM_MAX_CLIENTS) {
        releaseResponseSlot(client_id_);
        ctrl_->slots[client_id_].active = 0;
        if (ctrl_->active_clients > 0) {
            __sync_fetch_and_sub(&ctrl_->active_clients, 1);
        }
        __sync_synchronize();
    }

    // Close eventfds
    if (is_server_) {
        if (req_eventfd_ >= 0) {
            platform::closeEventFd(req_eventfd_);
            req_eventfd_ = -1;
        }
        for (uint32_t i = 0; i < SHM_MAX_CLIENTS; ++i) {
            if (resp_eventfds_[i] >= 0) {
                platform::closeEventFd(resp_eventfds_[i]);
                resp_eventfds_[i] = -1;
            }
        }
        // Close UDS listen socket
        if (uds_listen_fd_ >= 0) {
            platform::udsClose(uds_listen_fd_);
            uds_listen_fd_ = -1;
        }
        if (!uds_path_.empty()) {
            platform::udsUnlink(uds_path_);
            uds_path_.clear();
        }
    } else {
        // Client: close peer_req_eventfd_ (received via UDS)
        if (peer_req_eventfd_ >= 0) {
            platform::closeEventFd(peer_req_eventfd_);
            peer_req_eventfd_ = -1;
        }
        // event_fd_ (resp_eventfd received via UDS, or dummy)
        if (event_fd_ >= 0) {
            platform::closeEventFd(event_fd_);
            event_fd_ = -1;
        }
    }

    // Detach SHM
    if (shm_addr_ != NULL) {
        platform::shmDetach(shm_addr_, shm_size_);
        shm_addr_ = NULL;
    }

    // Only the server unlinks SHM
    if (is_server_) {
        platform::shmUnlink(shm_name_);
        OMNI_LOG_DEBUG(LOG_TAG, "Server unlinked shm and eventfds for '%s'",
                         shm_name_.c_str());
    }

    ctrl_ = NULL;
    shm_size_ = 0;
    eventfd_enabled_ = false;
}

bool ShmTransport::allocateResponseSlot(uint32_t slot)
{
    if (!ctrl_ || slot >= ctrl_->max_clients) {
        return false;
    }

    uint32_t mask = 1u << slot;
    uint32_t old_bitmap = 0;
    do {
        old_bitmap = ctrl_->response_bitmap;
        if (old_bitmap & mask) {
            OMNI_LOG_ERROR(LOG_TAG, "SHM '%s' response block already allocated for slot %u",
                             shm_name_.c_str(), slot);
            return false;
        }
    } while (!__sync_bool_compare_and_swap(&ctrl_->response_bitmap, old_bitmap, old_bitmap | mask));

    ctrl_->slots[slot].response_capacity = ctrl_->resp_ring_capacity;
    ctrl_->slots[slot].response_offset = static_cast<uint32_t>(slot * responseBlockSize());

    ShmRingHeader* resp_ring = getResponseRing(slot);
    resp_ring->write_pos = 0;
    resp_ring->read_pos = 0;
    resp_ring->capacity = ctrl_->resp_ring_capacity;
    resp_ring->reserved = 0;
    return true;
}

void ShmTransport::releaseResponseSlot(uint32_t slot)
{
    if (!ctrl_ || slot >= ctrl_->max_clients) {
        return;
    }

    __sync_fetch_and_and(&ctrl_->response_bitmap, ~(1u << slot));
    ctrl_->slots[slot].response_offset = 0;
    ctrl_->slots[slot].response_capacity = 0;
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

    if (length > 0x7FFFFFFF) {
        OMNI_LOG_ERROR(LOG_TAG, "send() message too large: %zu bytes", length);
        return -1;
    }

    // Message format in RequestQueue: [client_id(4B)] [length(4B)] [payload(NB)]
    uint32_t total_needed = static_cast<uint32_t>(sizeof(uint32_t) * 2 + length);

    ShmRingHeader* req_ring = getRequestRing();
    uint8_t* req_data = getRequestData();

    // Acquire spinlock for multi-writer safety
    ctrl_->req_lock.acquire();

    uint32_t avail = ringAvailableWrite(req_ring);
    if (avail < total_needed) {
        ctrl_->req_lock.release();
        OMNI_LOG_DEBUG(LOG_TAG, "send() RequestQueue full: need %u, avail %u",
                         total_needed, avail);
        return 0;  // Would block
    }

    // Write client_id
    uint32_t cid = client_id_;
    ringWrite(req_ring, req_data, reinterpret_cast<const uint8_t*>(&cid), sizeof(uint32_t));

    // Write length
    uint32_t len32 = static_cast<uint32_t>(length);
    ringWrite(req_ring, req_data, reinterpret_cast<const uint8_t*>(&len32), sizeof(uint32_t));

    // Write payload
    ringWrite(req_ring, req_data, data, static_cast<uint32_t>(length));

    ctrl_->req_lock.release();

    // Notify server via eventfd
    if (eventfd_enabled_ && peer_req_eventfd_ >= 0) {
        platform::eventFdNotify(peer_req_eventfd_);
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Client[%u] sent %zu bytes on shm '%s'",
                     client_id_, length, shm_name_.c_str());
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

    // Read from our ResponseSlot
    ShmRingHeader* resp_ring = getResponseRing(client_id_);
    const uint8_t* resp_data = getResponseData(client_id_);

    uint32_t avail = ringAvailableRead(resp_ring);
    if (avail < sizeof(uint32_t)) {
        return 0;  // No complete message
    }

    // Peek at length prefix without advancing read_pos
    uint32_t r = resp_ring->read_pos;
    uint32_t cap = resp_ring->capacity;
    uint32_t msg_len = 0;
    uint8_t* len_bytes = reinterpret_cast<uint8_t*>(&msg_len);
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        len_bytes[i] = resp_data[(r + i) % cap];
    }

    if (msg_len == 0) {
        // Zero-length message: consume the prefix
        __sync_synchronize();
        resp_ring->read_pos = (r + sizeof(uint32_t)) % cap;
        return 0;
    }

    uint32_t total_needed = sizeof(uint32_t) + msg_len;
    if (avail < total_needed) {
        return 0;  // Partial message
    }

    if (buf_size < msg_len) {
        OMNI_LOG_WARN(LOG_TAG, "recv() buffer too small: need %u, have %zu",
                        msg_len, buf_size);
        return -1;
    }

    // Consume length prefix
    resp_ring->read_pos = (r + sizeof(uint32_t)) % cap;
    __sync_synchronize();

    // Read payload
    uint32_t read_bytes = ringRead(resp_ring, resp_data, buf, msg_len);
    if (read_bytes != msg_len) {
        OMNI_LOG_ERROR(LOG_TAG, "recv() failed to read payload (%u of %u)",
                         read_bytes, msg_len);
        state_ = ConnectionState::ERROR;
        return -1;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Client[%u] received %u bytes on shm '%s'",
                     client_id_, msg_len, shm_name_.c_str());
    return static_cast<int>(msg_len);
}

void ShmTransport::close()
{
    if (state_ == ConnectionState::DISCONNECTED && shm_addr_ == NULL) {
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
    // 服务端: 返回 req_eventfd_（请求到达通知）
    // 客户端: 返回 event_fd_（响应到达通知，通过 UDS 交换获得）
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
// Server-side operations
// ============================================================

int ShmTransport::serverRecv(uint8_t* buf, size_t buf_size, uint32_t& out_client_id)
{
    if (!is_server_ || state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    if (!ctrl_) {
        OMNI_LOG_ERROR(LOG_TAG, "serverRecv() missing control block");
        return -1;
    }

    ShmRingHeader* req_ring = getRequestRing();
    const uint8_t* req_data = getRequestData();

    if (!req_ring || !req_data) {
        OMNI_LOG_ERROR(LOG_TAG, "serverRecv() missing request ring/data");
        return -1;
    }

    if (req_ring->capacity == 0 || req_ring->capacity != ctrl_->req_ring_capacity) {
        OMNI_LOG_ERROR(LOG_TAG, "serverRecv() invalid request ring capacity: ring=%u ctrl=%u",
                         req_ring->capacity, ctrl_->req_ring_capacity);
        return -1;
    }

    if (req_ring->read_pos >= req_ring->capacity || req_ring->write_pos >= req_ring->capacity) {
        OMNI_LOG_ERROR(LOG_TAG, "serverRecv() invalid ring pointers: r=%u w=%u cap=%u",
                         req_ring->read_pos, req_ring->write_pos, req_ring->capacity);
        return -1;
    }

    ctrl_->req_lock.acquire();

    uint32_t avail = ringAvailableRead(req_ring);
    // Need at least: client_id(4) + length(4)
    if (avail < sizeof(uint32_t) * 2) {
        ctrl_->req_lock.release();
        return 0;  // No complete message
    }

    // Peek at client_id and length without advancing read_pos
    uint32_t r = req_ring->read_pos;
    uint32_t cap = req_ring->capacity;

    uint32_t client_id = 0;
    uint8_t* cid_bytes = reinterpret_cast<uint8_t*>(&client_id);
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        cid_bytes[i] = req_data[(r + i) % cap];
    }

    uint32_t msg_len = 0;
    uint8_t* len_bytes = reinterpret_cast<uint8_t*>(&msg_len);
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        len_bytes[i] = req_data[(r + sizeof(uint32_t) + i) % cap];
    }

    uint32_t total_needed = sizeof(uint32_t) * 2 + msg_len;
    if (msg_len > req_ring->capacity) {
        ctrl_->req_lock.release();
        OMNI_LOG_ERROR(LOG_TAG, "serverRecv() invalid msg_len=%u cap=%u",
                         msg_len, req_ring->capacity);
        return -1;
    }
    if (avail < total_needed) {
        ctrl_->req_lock.release();
        return 0;  // Partial message
    }

    if (msg_len > 0 && buf_size < msg_len) {
        OMNI_LOG_WARN(LOG_TAG, "serverRecv() buffer too small: need %u, have %zu",
                        msg_len, buf_size);
        ctrl_->req_lock.release();
        return -1;
    }

    // Consume client_id + length prefix
    req_ring->read_pos = (r + sizeof(uint32_t) * 2) % cap;
    __sync_synchronize();

    out_client_id = client_id;

    if (msg_len == 0) {
        return 0;
    }

    // Read payload
    uint32_t read_bytes = ringRead(req_ring, req_data, buf, msg_len);
    ctrl_->req_lock.release();
    if (read_bytes != msg_len) {
        OMNI_LOG_ERROR(LOG_TAG, "serverRecv() failed to read payload (%u of %u)",
                         read_bytes, msg_len);
        return -1;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Server received %u bytes from client[%u] on shm '%s'",
                     msg_len, client_id, shm_name_.c_str());
    return static_cast<int>(msg_len);
}

int ShmTransport::serverSend(uint32_t client_id, const uint8_t* data, size_t length)
{
    if (!is_server_ || state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    if (client_id >= ctrl_->max_clients) {
        OMNI_LOG_ERROR(LOG_TAG, "serverSend() invalid client_id=%u", client_id);
        return -1;
    }

    if (!ctrl_->slots[client_id].active) {
        OMNI_LOG_DEBUG(LOG_TAG, "serverSend() client[%u] not active, skipping", client_id);
        return 0;
    }

    if (length == 0) {
        return 0;
    }

    if (length > 0x7FFFFFFF) {
        return -1;
    }

    ShmRingHeader* resp_ring = getResponseRing(client_id);
    uint8_t* resp_data = getResponseData(client_id);

    // Length-prefixed: [length(4B)] [payload(NB)]
    uint32_t total_needed = static_cast<uint32_t>(sizeof(uint32_t) + length);
    uint32_t avail = ringAvailableWrite(resp_ring);

    if (avail < total_needed) {
        OMNI_LOG_DEBUG(LOG_TAG, "serverSend() ResponseSlot[%u] full: need %u, avail %u",
                         client_id, total_needed, avail);
        return 0;  // Would block
    }

    // Write length prefix
    uint32_t len32 = static_cast<uint32_t>(length);
    ringWrite(resp_ring, resp_data, reinterpret_cast<const uint8_t*>(&len32), sizeof(uint32_t));

    // Write payload
    ringWrite(resp_ring, resp_data, data, static_cast<uint32_t>(length));

    // Notify the client via eventfd
    if (eventfd_enabled_ && resp_eventfds_[client_id] >= 0) {
        platform::eventFdNotify(resp_eventfds_[client_id]);
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Server sent %zu bytes to client[%u] on shm '%s'",
                     length, client_id, shm_name_.c_str());
    return static_cast<int>(length);
}

int ShmTransport::serverBroadcast(const uint8_t* data, size_t length)
{
    if (!is_server_ || state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    int count = 0;

    for (uint32_t i = 0; i < ctrl_->max_clients; ++i) {
        if (ctrl_->slots[i].active) {
            int ret = serverSend(i, data, length);
            if (ret > 0) {
                ++count;
            }
        }
    }

    return count;
}

bool ShmTransport::waitReady(uint32_t timeout_ms)
{
    if (is_server_) {
        // Server is always ready after initServer()
        return state_ == ConnectionState::CONNECTED;
    }

    // Client waits for server's ready_flag
    if (shm_addr_ == NULL) {
        // Need to open SHM first to check ready_flag
        size_t sz = calculateShmSize(SHM_DEFAULT_REQ_RING_CAPACITY,
                                     SHM_DEFAULT_RESP_RING_CAPACITY,
                                     SHM_MAX_CLIENTS);
        shm_addr_ = platform::shmCreate(shm_name_, sz, false);
        if (shm_addr_ == NULL) {
            return false;
        }
        shm_size_ = sz;
        ctrl_ = reinterpret_cast<ShmControlBlock*>(shm_addr_);
    }

    int64_t start = platform::currentTimeMs();
    while (true) {
        __sync_synchronize();
        if (ctrl_ && ctrl_->ready_flag != 0) {
            return true;
        }

        int64_t elapsed = platform::currentTimeMs() - start;
        if (static_cast<uint32_t>(elapsed) >= timeout_ms) {
            OMNI_LOG_WARN(LOG_TAG, "waitReady() timed out after %u ms on '%s'",
                            timeout_ms, shm_name_.c_str());
            return false;
        }

        platform::sleepMs(1);
    }
}

uint32_t ShmTransport::clientCount() const
{
    if (ctrl_) {
        return ctrl_->active_clients;
    }
    return 0;
}

std::vector<uint32_t> ShmTransport::activeClientIds() const
{
    std::vector<uint32_t> ids;
    if (!ctrl_) {
        return ids;
    }

    for (uint32_t i = 0; i < ctrl_->max_clients; ++i) {
        if (ctrl_->slots[i].active) {
            ids.push_back(i);
        }
    }
    return ids;
}

uint32_t ShmTransport::responseSlotsInUse() const
{
    if (!ctrl_) {
        return 0;
    }
    return countBits(ctrl_->response_bitmap);
}

size_t ShmTransport::totalResponseArenaSize() const
{
    if (!ctrl_) {
        return 0;
    }
    return responseBlockSize() * ctrl_->max_clients;
}

size_t ShmTransport::activeResponseArenaSize() const
{
    if (!ctrl_) {
        return 0;
    }
    return responseBlockSize() * responseSlotsInUse();
}

// ============================================================
// UDS eventfd exchange (server side)
// ============================================================

void ShmTransport::onUdsClientConnect()
{
    int client_fd = platform::udsAccept(uds_listen_fd_);
    if (client_fd < 0) {
        return;
    }

    // Read client_id from the client
    uint32_t client_id = 0;
    ssize_t n = ::recv(client_fd, &client_id, sizeof(client_id), MSG_WAITALL);
    if (n != sizeof(client_id)) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: failed to read client_id (got %zd bytes)", n);
        platform::udsClose(client_fd);
        return;
    }

    if (client_id >= SHM_MAX_CLIENTS) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: invalid client_id=%u", client_id);
        platform::udsClose(client_fd);
        return;
    }

    // Send 2 fds: [req_eventfd_, resp_eventfds_[client_id]]
    int fds[2] = { req_eventfd_, resp_eventfds_[client_id] };
    if (!platform::udsSendFds(client_fd, fds, 2, NULL, 0)) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: failed to send fds to client[%u]", client_id);
    } else {
        OMNI_LOG_DEBUG(LOG_TAG, "UDS: sent eventfds to client[%u] (req=%d, resp=%d)",
                         client_id, req_eventfd_, resp_eventfds_[client_id]);
    }

    platform::udsClose(client_fd);
}

std::string ShmTransport::getShmUdsPath(const std::string& shm_name)
{
    // shm_name 格式为 "/binder_xxx"，去掉前导 '/' 用作文件名
    std::string name = shm_name;
    if (!name.empty() && name[0] == '/') {
        name = name.substr(1);
    }
    return "/tmp/" + name + ".sock";
}

} // namespace omnibinder
