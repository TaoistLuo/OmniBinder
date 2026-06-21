#include "transport/shm_transport.h"
#include "omnibinder/log.h"

#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <unistd.h>
#include <atomic>

#define LOG_TAG "ShmTransport"

namespace omnibinder {

// ============================================================
// Utility functions
// ============================================================

size_t calculateShmSize(size_t req_ring_capacity, size_t resp_ring_capacity,
                        uint32_t max_clients)
{
    (void)max_clients;  // deprecated, kept for backward compat
    return sizeof(ShmControlBlock)
         + sizeof(ShmRingHeader) + req_ring_capacity
         + sizeof(ShmRingHeader) + resp_ring_capacity;
}

std::string generateShmName(const std::string& service_name)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "/binder_%s", service_name.c_str());
    return std::string(buf);
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
    , requested_req_ring_capacity_(req_ring_capacity)
    , requested_resp_ring_capacity_(resp_ring_capacity)
    , shm_addr_(NULL)
    , shm_size_(0)
    , ctrl_(NULL)
    , req_eventfd_(-1)
    , event_fd_(-1)
    , peer_notify_fd_(-1)
    , uds_listen_fd_(-1)
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
// Server initialization: create UDS listener only
// ============================================================
bool ShmTransport::initServer()
{
    // Create UDS listen socket for client handshake
    uds_path_ = getShmUdsPath(shm_name_);
    uds_listen_fd_ = platform::udsBindListen(uds_path_);
    if (uds_listen_fd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create UDS listen for '%s'", shm_name_.c_str());
        return false;
    }

    // Create a master eventfd for caller epoll registration
    req_eventfd_ = platform::createEventFd();
    if (req_eventfd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create master req_eventfd for '%s'", shm_name_.c_str());
        platform::udsClose(uds_listen_fd_);
        uds_listen_fd_ = -1;
        platform::udsUnlink(uds_path_);
        uds_path_.clear();
        return false;
    }

    eventfd_enabled_ = true;

    OMNI_LOG_INFO(LOG_TAG, "Server listening on UDS '%s' (uds_fd=%d, req_eventfd=%d)",
                    uds_path_.c_str(), uds_listen_fd_, req_eventfd_);
    return true;
}

// ============================================================
// Client initialization: create own SHM + UDS name exchange
// ============================================================
bool ShmTransport::initClient()
{
    // Save the server identifier (used for UDS path)
    std::string server_name = shm_name_;

    // Generate unique SHM name for this client (atomic counter prevents
    // collisions when multiple ShmTransport instances exist in same process)
    {
        static std::atomic<uint64_t> s_instance_counter(0);
        uint64_t unique_id = s_instance_counter.fetch_add(1, std::memory_order_relaxed);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s_cli_%d_%lu",
                 server_name.c_str(), static_cast<int>(getpid()),
                 static_cast<unsigned long>(unique_id));
        shm_name_ = std::string(buf);
    }

    size_t req_cap = requested_req_ring_capacity_;
    size_t resp_cap = requested_resp_ring_capacity_;
    shm_size_ = calculateShmSize(req_cap, resp_cap, 1);

    // Create the SHM region
    shm_addr_ = platform::shmCreate(shm_name_, shm_size_, true);
    if (shm_addr_ == NULL) {
        OMNI_LOG_ERROR(LOG_TAG, "shmCreate failed for client '%s'", shm_name_.c_str());
        return false;
    }

    // Zero-init control block + both ring headers
    memset(shm_addr_, 0, sizeof(ShmControlBlock)
                         + sizeof(ShmRingHeader) + req_cap
                         + sizeof(ShmRingHeader) + resp_cap);

    // Set up control block
    ctrl_ = reinterpret_cast<ShmControlBlock*>(shm_addr_);
    ctrl_->magic = SHM_MAGIC;
    ctrl_->version = 1;
    ctrl_->req_ring_capacity = static_cast<uint32_t>(req_cap);
    ctrl_->resp_ring_capacity = static_cast<uint32_t>(resp_cap);

    // Initialize request ring
    ShmRingHeader* req_ring = getRequestRing();
    req_ring->write_pos = 0;
    req_ring->read_pos = 0;
    req_ring->capacity = static_cast<uint32_t>(req_cap);
    req_ring->reserved = 0;

    // Initialize response ring
    ShmRingHeader* resp_ring = getResponseRing();
    resp_ring->write_pos = 0;
    resp_ring->read_pos = 0;
    resp_ring->capacity = static_cast<uint32_t>(resp_cap);
    resp_ring->reserved = 0;

    // Set feature flag
    ctrl_->reserved[0] |= SHM_FEATURE_EVENTFD;

    // Mark SHM as ready
    platform::memoryBarrier();
    ctrl_->ready_flag = 1;

    // Connect to server via UDS
    std::string uds_path = getShmUdsPath(server_name);
    int uds_fd = platform::udsConnect(uds_path);
    if (uds_fd < 0) {
        OMNI_LOG_WARN(LOG_TAG, "UDS connect failed for '%s', falling back from SHM",
                        server_name.c_str());
        cleanup();
        return false;
    }

    // Step 1: Send SHM name to server (no fds — server-side master eventfd
    //         will be sent to us in Step 2 for client→server notification)
    {
        if (!platform::udsSendFds(uds_fd, NULL, 0,
                                   shm_name_.c_str(), shm_name_.size())) {
            OMNI_LOG_WARN(LOG_TAG, "UDS send name failed for client '%s', falling back",
                            shm_name_.c_str());
            platform::udsClose(uds_fd);
            cleanup();
            return false;
        }
    }

    // Step 2: Receive [resp_eventfd, master_eventfd] from server.
    //         resp_eventfd   → client epoll wakes when server writes response
    //         master_eventfd → client notifies this when writing request → wakes server epoll
    {
        int recv_fds[2] = {-1, -1};
        if (!platform::udsRecvFds(uds_fd, recv_fds, 2, NULL, 0, NULL)) {
            OMNI_LOG_WARN(LOG_TAG, "UDS recv fds failed for client '%s', falling back",
                            shm_name_.c_str());
            platform::udsClose(uds_fd);
            cleanup();
            return false;
        }
        event_fd_ = recv_fds[0];
        peer_notify_fd_ = recv_fds[1];
    }

    platform::udsClose(uds_fd);

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
    return reinterpret_cast<ShmRingHeader*>(base + sizeof(ShmControlBlock));
}

uint8_t* ShmTransport::getRequestDataFromBase(uint8_t* base)
{
    return base + sizeof(ShmControlBlock) + sizeof(ShmRingHeader);
}

ShmRingHeader* ShmTransport::getResponseRingFromBase(uint8_t* base, ShmControlBlock* ctrl)
{
    return reinterpret_cast<ShmRingHeader*>(
        base + sizeof(ShmControlBlock) + sizeof(ShmRingHeader) + ctrl->req_ring_capacity);
}

uint8_t* ShmTransport::getResponseDataFromBase(uint8_t* base, ShmControlBlock* ctrl)
{
    return base + sizeof(ShmControlBlock) + sizeof(ShmRingHeader)
               + ctrl->req_ring_capacity + sizeof(ShmRingHeader);
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
    return getResponseRingFromBase(static_cast<uint8_t*>(shm_addr_), ctrl_);
}

uint8_t* ShmTransport::getResponseData() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return getResponseDataFromBase(static_cast<uint8_t*>(shm_addr_), ctrl_);
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
    return getResponseRingFromBase(static_cast<uint8_t*>(ctx.shm_addr), ctx.ctrl);
}

uint8_t* ShmTransport::getResponseData(const ClientShmContext& ctx) const
{
    if (!ctx.shm_addr || !ctx.ctrl) return NULL;
    return getResponseDataFromBase(static_cast<uint8_t*>(ctx.shm_addr), ctx.ctrl);
}

size_t ShmTransport::responseBlockSize(const ShmControlBlock* ctrl) const
{
    return sizeof(ShmRingHeader) + ctrl->resp_ring_capacity;
}

size_t ShmTransport::responseBlockSize() const
{
    if (!ctrl_) return 0;
    return responseBlockSize(ctrl_);
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

    platform::memoryBarrier();
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

    platform::memoryBarrier();
    ring->read_pos = (r + to_read) % cap;

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
    if (ctx.resp_eventfd >= 0) {
        platform::closeEventFd(ctx.resp_eventfd);
        ctx.resp_eventfd = -1;
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

    if (length > 0x7FFFFFFF) {
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

    bool was_empty = (ringAvailableRead(req_ring) == 0);
    uint32_t avail = ringAvailableWrite(req_ring);
    if (avail < total_needed) {
        OMNI_LOG_DEBUG(LOG_TAG, "send() request ring full: need %u, avail %u",
                         total_needed, avail);
        return 0;
    }

    uint32_t len32 = static_cast<uint32_t>(length);
    ringWrite(req_ring, req_data, reinterpret_cast<const uint8_t*>(&len32), sizeof(uint32_t));
    ringWrite(req_ring, req_data, data, static_cast<uint32_t>(length));

    // Notify server via master eventfd (received during UDS handshake)
    if (was_empty && eventfd_enabled_ && peer_notify_fd_ >= 0) {
        platform::eventFdNotify(peer_notify_fd_);
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

    int result = 0;

    if (buf_size == 0) {
        goto consume_eventfd;
    }

    {
        ShmRingHeader* resp_ring = getResponseRing();
        const uint8_t* resp_data = getResponseData();

        if (!resp_ring || !resp_data) {
            OMNI_LOG_ERROR(LOG_TAG, "recv() missing response ring");
            return -1;
        }

        uint32_t avail = ringAvailableRead(resp_ring);
        if (avail < sizeof(uint32_t)) {
            goto consume_eventfd;
        }

        // Peek at length prefix
        uint32_t r = resp_ring->read_pos;
        uint32_t cap = resp_ring->capacity;
        uint32_t msg_len = 0;
        uint8_t* len_bytes = reinterpret_cast<uint8_t*>(&msg_len);
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
            len_bytes[i] = resp_data[(r + i) % cap];
        }

        if (msg_len == 0) {
            platform::memoryBarrier();
            resp_ring->read_pos = (r + sizeof(uint32_t)) % cap;
            goto consume_eventfd;
        }

        uint32_t total_needed = sizeof(uint32_t) + msg_len;
        if (avail < total_needed) {
            goto consume_eventfd;
        }

        if (buf_size < msg_len) {
            OMNI_LOG_WARN(LOG_TAG, "recv() buffer too small: need %u, have %zu",
                            msg_len, buf_size);
            return -1;
        }

        // Consume length prefix
        resp_ring->read_pos = (r + sizeof(uint32_t)) % cap;
        platform::memoryBarrier();

        uint32_t read_bytes = ringRead(resp_ring, resp_data, buf, msg_len);
        if (read_bytes != msg_len) {
            OMNI_LOG_ERROR(LOG_TAG, "recv() failed to read payload (%u of %u)",
                             read_bytes, msg_len);
            state_ = ConnectionState::ERROR;
            return -1;
        }

        OMNI_LOG_DEBUG(LOG_TAG, "Client received %u bytes on shm '%s'",
                         msg_len, shm_name_.c_str());
        result = static_cast<int>(msg_len);
    }

consume_eventfd:
    // Pair each notify with a consume so level-triggered epoll won't re-fire
    // when the ring is empty (which onConnectionData would misread as disconnect)
    if (event_fd_ >= 0) {
        platform::eventFdConsume(event_fd_);
    }
    return result;
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

        uint32_t avail = ringAvailableRead(req_ring);
        if (avail < sizeof(uint32_t)) {
            continue;
        }

        // Peek at length prefix
        uint32_t r = req_ring->read_pos;
        uint32_t cap = req_ring->capacity;

        uint32_t msg_len = 0;
        {
            uint8_t* len_bytes = reinterpret_cast<uint8_t*>(&msg_len);
            for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
                len_bytes[i] = req_data[(r + i) % cap];
            }
        }

        if (msg_len > req_ring->capacity) {
            OMNI_LOG_ERROR(LOG_TAG, "serverRecv() invalid msg_len=%u cap=%u from client[%u]",
                             msg_len, req_ring->capacity, cid);
            continue;
        }

        uint32_t total_needed = sizeof(uint32_t) + msg_len;
        if (avail < total_needed) {
            continue;
        }

        if (msg_len > 0 && buf_size < msg_len) {
            OMNI_LOG_WARN(LOG_TAG, "serverRecv() buffer too small: need %u, have %zu",
                            msg_len, buf_size);
            continue;
        }

        // Consume length prefix
        req_ring->read_pos = (r + sizeof(uint32_t)) % cap;
        platform::memoryBarrier();

        out_client_id = cid;

        if (msg_len == 0) {
            return 0;
        }

        uint32_t read_bytes = ringRead(req_ring, req_data, buf, msg_len);
        if (read_bytes != msg_len) {
            OMNI_LOG_ERROR(LOG_TAG, "serverRecv() failed to read payload (%u of %u) from client[%u]",
                             read_bytes, msg_len, cid);
            continue;
        }

        OMNI_LOG_DEBUG(LOG_TAG, "Server received %u bytes from client[%u] on shm '%s'",
                         msg_len, cid, ctx.shm_name.c_str());
        return static_cast<int>(msg_len);
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

int ShmTransport::serverSendToContext(ClientShmContext& ctx, uint32_t client_id,
                                       const uint8_t* data, size_t length)
{
    if (length == 0) {
        return 0;
    }

    if (length > 0x7FFFFFFF) {
        return -1;
    }

    ShmRingHeader* resp_ring = getResponseRing(ctx);
    uint8_t* resp_data = getResponseData(ctx);

    if (!resp_ring || !resp_data) {
        OMNI_LOG_ERROR(LOG_TAG, "serverSend() missing response ring for client[%u]", client_id);
        return -1;
    }

    uint32_t total_needed = static_cast<uint32_t>(sizeof(uint32_t) + length);
    bool was_empty = (ringAvailableRead(resp_ring) == 0);
    uint32_t avail = ringAvailableWrite(resp_ring);

    if (avail < total_needed) {
        OMNI_LOG_DEBUG(LOG_TAG, "serverSend() response ring full for client[%u]: need %u, avail %u",
                         client_id, total_needed, avail);
        return 0;
    }

    uint32_t len32 = static_cast<uint32_t>(length);
    ringWrite(resp_ring, resp_data, reinterpret_cast<const uint8_t*>(&len32), sizeof(uint32_t));
    ringWrite(resp_ring, resp_data, data, static_cast<uint32_t>(length));

    if (was_empty && eventfd_enabled_ && ctx.resp_eventfd >= 0) {
        platform::eventFdNotify(ctx.resp_eventfd);
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Server sent %zu bytes to client[%u] on shm '%s'",
                     length, client_id, ctx.shm_name.c_str());
    return static_cast<int>(length);
}

int ShmTransport::serverBroadcast(const uint8_t* data, size_t length)
{
    if (!is_server_ || state_ != ConnectionState::CONNECTED) {
        return -1;
    }

    int count = 0;
    for (std::map<uint32_t, ClientShmContext>::iterator it = client_contexts_.begin();
         it != client_contexts_.end(); ++it) {
        int ret = serverSendToContext(it->second, it->first, data, length);
        if (ret > 0) {
            ++count;
        }
    }

    return count;
}

bool ShmTransport::waitReady(uint32_t timeout_ms)
{
    (void)timeout_ms;

    if (is_server_) {
        return state_ == ConnectionState::CONNECTED;
    }

    // Client is ready after initClient completes (UDS handshake done)
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

uint32_t ShmTransport::responseSlotsInUse() const
{
    return clientCount();
}

size_t ShmTransport::totalResponseArenaSize() const
{
    if (!is_server_) {
        return 0;
    }

    size_t total = 0;
    for (std::map<uint32_t, ClientShmContext>::const_iterator it = client_contexts_.begin();
         it != client_contexts_.end(); ++it) {
        if (it->second.ctrl) {
            total += responseBlockSize(it->second.ctrl);
        }
    }
    return total;
}

size_t ShmTransport::activeResponseArenaSize() const
{
    return totalResponseArenaSize();
}

uint32_t ShmTransport::maxClients() const
{
    return clientCount();
}

// ============================================================
// UDS eventfd + SHM name exchange (server side)
// ============================================================

void ShmTransport::onUdsClientConnect()
{
    int client_fd = platform::udsAccept(uds_listen_fd_);
    if (client_fd < 0) {
        return;
    }

    // Step 1: Receive SHM name from the client (no fds expected;
    //         udsRecvFds returns false when no SCM_RIGHTS, but data is still received)
    int client_fds[1] = {-1};
    char name_buf[256] = {0};
    size_t data_len = 0;

    bool has_fd = platform::udsRecvFds(client_fd, client_fds, 1,
                                        name_buf, sizeof(name_buf) - 1, &data_len);

    std::string client_shm_name(name_buf, data_len);

    // If client sent an fd (legacy or misbehaving), close it — we don't need it
    if (has_fd && client_fds[0] >= 0) {
        platform::closeEventFd(client_fds[0]);
    }

    if (client_shm_name.empty()) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: empty SHM name from client");
        platform::udsClose(client_fd);
        return;
    }

    // Open the client's SHM
    size_t try_size = sizeof(ShmControlBlock) + sizeof(ShmRingHeader) * 2
                     + SHM_DEFAULT_REQ_RING_CAPACITY + SHM_DEFAULT_RESP_RING_CAPACITY;
    void* addr = platform::shmCreate(client_shm_name, try_size, false);
    if (addr == NULL) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: failed to open client SHM '%s'",
                        client_shm_name.c_str());
        platform::udsClose(client_fd);
        return;
    }

    ShmControlBlock* ctrl = reinterpret_cast<ShmControlBlock*>(addr);

    if (ctrl->magic != SHM_MAGIC) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: invalid SHM magic 0x%08X for '%s'",
                        ctrl->magic, client_shm_name.c_str());
        platform::shmDetach(addr, try_size);
        platform::udsClose(client_fd);
        return;
    }

    // Calculate exact size and re-map if needed
    size_t exact_size = calculateShmSize(ctrl->req_ring_capacity, ctrl->resp_ring_capacity, 1);
    if (exact_size > try_size) {
        platform::shmDetach(addr, try_size);
        addr = platform::shmCreate(client_shm_name, exact_size, false);
        if (addr == NULL) {
            OMNI_LOG_WARN(LOG_TAG, "UDS: failed to re-open client SHM '%s' with exact size",
                            client_shm_name.c_str());
            platform::udsClose(client_fd);
            return;
        }
        ctrl = reinterpret_cast<ShmControlBlock*>(addr);
    }

    // Step 2: Create resp_eventfd for this client
    int resp_efd = platform::createEventFd();
    if (resp_efd < 0) {
        OMNI_LOG_WARN(LOG_TAG, "UDS: failed to create resp_eventfd for client '%s'",
                        client_shm_name.c_str());
        platform::shmDetach(addr, exact_size);
        platform::udsClose(client_fd);
        return;
    }

    // Step 3: Send eventfd(s) back to client via platform-specific mechanism.
    //         resp_eventfd → client epoll wakes when server writes response
    //         Linux: platform sends shared master_eventfd
    //         Windows: platform creates per-client pipe and returns it via out_new_fd
    {
        int new_server_fd = -1;
        if (!platform::udsSendServerResponse(client_fd, resp_efd, req_eventfd_,
                                              &new_server_fd)) {
            OMNI_LOG_WARN(LOG_TAG, "UDS: failed to send eventfds to client '%s'",
                            client_shm_name.c_str());
            platform::shmDetach(addr, exact_size);
            platform::closeEventFd(resp_efd);
            platform::udsClose(client_fd);
            return;
        }
        if (new_server_fd >= 0 && on_new_client_fd_cb_) {
            on_new_client_fd_cb_(new_server_fd);
        }
    }

    platform::udsClose(client_fd);

    // Assign client ID and store context
    uint32_t assigned_id = next_client_id_++;

    ClientShmContext ctx;
    ctx.client_id = assigned_id;
    ctx.shm_name = client_shm_name;
    ctx.shm_addr = addr;
    ctx.shm_size = exact_size;
    ctx.ctrl = ctrl;
    ctx.resp_eventfd = resp_efd;

    client_contexts_[assigned_id] = ctx;

    // Notify the master eventfd to wake up the event loop (data from new client
    // may need servicing if server uses polling/scanning callback)
    if (req_eventfd_ >= 0) {
        platform::eventFdNotify(req_eventfd_);
    }

    OMNI_LOG_INFO(LOG_TAG, "UDS: client[%u] connected, shm='%s', resp_efd=%d",
                    assigned_id, client_shm_name.c_str(), resp_efd);
}

// ============================================================
// getShmUdsPath
// ============================================================

std::string ShmTransport::getShmUdsPath(const std::string& shm_name)
{
    std::string name = shm_name;
    if (!name.empty() && name[0] == '/') {
        name = name.substr(1);
    }
    return "/tmp/" + name + ".sock";
}

} // namespace omnibinder
