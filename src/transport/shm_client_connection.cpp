#include "transport/shm_client_connection.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"
#include "platform/platform.h"

#include <atomic>
#include <stdio.h>

#define LOG_TAG "ShmClientConnection"

namespace omnibinder {

ShmClientConnection::ShmClientConnection(const std::string& service_name,
                                       size_t req_ring_capacity,
                                       size_t resp_ring_capacity)
    : service_name_(service_name)
    , state_(ConnectionState::DISCONNECTED)
    , requested_req_ring_capacity_(shmNormalizeRingCapacity(req_ring_capacity))
    , requested_resp_ring_capacity_(shmNormalizeRingCapacity(resp_ring_capacity))
    , shm_addr_(NULL)
    , shm_size_(0)
    , ctrl_(NULL)
    , event_fd_(-1)
    , peer_notify_fd_(-1)
    , handshake_channel_(NULL)
{
}

ShmClientConnection::~ShmClientConnection()
{
    close();
}

// ============================================================
// 连接：创建自己的 SHM + 握手交换通知句柄
// ============================================================

bool ShmClientConnection::initClient()
{
    std::string server_shm_name = generateShmName(service_name_);

    // 进程内原子计数 + pid 生成唯一客户端 SHM 名，避免同进程多实例冲突
    {
        static std::atomic<uint64_t> s_instance_counter(0);
        uint64_t unique_id = s_instance_counter.fetch_add(1, std::memory_order_relaxed);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s_cli_%d_%lu",
                 server_shm_name.c_str(), platform::getPid(),
                 static_cast<unsigned long>(unique_id));
        shm_name_ = std::string(buf);
    }

    size_t req_cap = requested_req_ring_capacity_;
    size_t resp_cap = requested_resp_ring_capacity_;
    if (!shmIsValidRingCapacity(req_cap) || !shmIsValidRingCapacity(resp_cap)) {
        OMNI_LOG_ERROR(LOG_TAG, "invalid SHM ring capacity req=%zu resp=%zu", req_cap, resp_cap);
        return false;
    }
    shm_size_ = calculateShmSize(req_cap, resp_cap);
    if (shm_size_ == 0) {
        OMNI_LOG_ERROR(LOG_TAG, "invalid SHM size for req=%zu resp=%zu", req_cap, resp_cap);
        return false;
    }

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

    // 初始化控制块 + 两侧 ring 头，并发布 ready_flag
    shmInitLayout(shm_addr_, shm_size_,
                  static_cast<uint32_t>(req_cap), static_cast<uint32_t>(resp_cap));
    ctrl_ = reinterpret_cast<ShmControlBlock*>(shm_addr_);

    // 握手：连接服务端并发送 SHM 名
    std::string path = shmHandshakePath(server_shm_name);
    platform::handshake_channel* ch = platform::handshakeConnect(path);
    if (!ch) {
        OMNI_LOG_WARN(LOG_TAG, "handshake connect failed for '%s', falling back from SHM",
                      service_name_.c_str());
        cleanup();
        return false;
    }

    // 步骤 1：发送 SHM 名（不带 fd）
    if (!platform::handshakeSend(ch, shm_name_.data(), shm_name_.size(), NULL, 0)) {
        OMNI_LOG_WARN(LOG_TAG, "handshake send name failed for client '%s', falling back",
                      shm_name_.c_str());
        platform::handshakeClose(ch);
        cleanup();
        return false;
    }

    // 步骤 2：接收 [resp_eventfd, master_eventfd]
    {
        int recv_fds[2] = {-1, -1};
        size_t response_len = 0;
        int recv_fd_count = 0;
        if (!platform::handshakeRecv(ch, NULL, 0, &response_len,
                                     recv_fds, 2, &recv_fd_count)
            || response_len != 0 || recv_fd_count != 2) {
            OMNI_LOG_WARN(LOG_TAG, "handshake recv fds failed for client '%s', falling back",
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

    OMNI_LOG_INFO(LOG_TAG, "Client connected: shm='%s', event_fd=%d, notify_fd=%d",
                  shm_name_.c_str(), event_fd_, peer_notify_fd_);
    return true;
}

void ShmClientConnection::cleanup()
{
    if (handshake_channel_) {
        platform::handshakeClose(handshake_channel_);
        handshake_channel_ = NULL;
    }
    if (peer_notify_fd_ >= 0) {
        platform::closeEventFd(peer_notify_fd_);
        peer_notify_fd_ = -1;
    }
    if (event_fd_ >= 0) {
        platform::closeEventFd(event_fd_);
        event_fd_ = -1;
    }
    if (shm_addr_ != NULL) {
        platform::shmDetach(shm_addr_, shm_size_);
        shm_addr_ = NULL;
    }
    if (!shm_name_.empty()) {
        platform::shmUnlink(shm_name_);
    }
    ctrl_ = NULL;
    shm_size_ = 0;
}

// ============================================================
// IMessageConnection
// ============================================================

int ShmClientConnection::connect(const std::string& host, uint16_t port)
{
    (void)host;
    (void)port;

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

int ShmClientConnection::send(const uint8_t* data, size_t length)
{
    if (state_ != ConnectionState::CONNECTED) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (length > MAX_MESSAGE_SIZE || length > 0x7FFFFFFF) {
        OMNI_LOG_ERROR(LOG_TAG, "send() message too large: %zu bytes", length);
        return -1;
    }

    ShmRingHeader* req_ring = requestRing();
    uint8_t* req_data = requestData();
    if (!req_ring || !req_data) {
        OMNI_LOG_ERROR(LOG_TAG, "send() missing request ring");
        return -1;
    }

    uint32_t req_capacity = static_cast<uint32_t>(requested_req_ring_capacity_);
    uint32_t written = shmRingSendFrame(req_ring, req_data, req_capacity,
                                        data, static_cast<uint32_t>(length),
                                        peer_notify_fd_);
    if (written == 0) {
        OMNI_LOG_DEBUG(LOG_TAG, "send() request ring full: need %zu",
                       length + sizeof(uint32_t));
        return 0;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Client sent %zu bytes on shm '%s'", length, shm_name_.c_str());
    return static_cast<int>(length);
}

int ShmClientConnection::sendAll(const uint8_t* data, size_t length,
                                uint32_t timeout_ms, uint32_t* elapsed_ms)
{
    int64_t start_ms = platform::currentTimeMs();
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }
    if (state_ != ConnectionState::CONNECTED) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (length > MAX_MESSAGE_SIZE || length > 0x7FFFFFFF) {
        OMNI_LOG_ERROR(LOG_TAG, "sendAll() message too large: %zu bytes", length);
        return -1;
    }

    ShmRingHeader* req_ring = requestRing();
    uint8_t* req_data = requestData();
    if (!req_ring || !req_data) {
        OMNI_LOG_ERROR(LOG_TAG, "sendAll() missing request ring");
        return -1;
    }

    const uint32_t req_capacity = static_cast<uint32_t>(requested_req_ring_capacity_);
    int rc = shmRingSendFrameWithinTimeout(req_ring, req_data, req_capacity,
                                           data, static_cast<uint32_t>(length),
                                           peer_notify_fd_, timeout_ms);
    if (elapsed_ms) {
        *elapsed_ms = static_cast<uint32_t>(platform::currentTimeMs() - start_ms);
    }
    if (rc == -1) {
        OMNI_LOG_ERROR(LOG_TAG, "sendAll() frame can never fit: need %zu, ring capacity %u",
                       length + sizeof(uint32_t), req_capacity);
        return -1;
    }
    if (rc != 0) {
        OMNI_LOG_WARN(LOG_TAG, "sendAll() request ring full timeout: need=%zu timeout_ms=%u",
                      length + sizeof(uint32_t), timeout_ms);
        return -1;
    }
    return 0;
}

void ShmClientConnection::consumeReadiness()
{
    if (event_fd_ >= 0) {
        platform::eventFdConsume(event_fd_);
    }
}

bool ShmClientConnection::isFramed() const
{
    return true;
}

int ShmClientConnection::recv(uint8_t* buf, size_t buf_size)
{
    if (state_ != ConnectionState::CONNECTED) {
        return -1;
    }
    if (buf_size == 0) {
        return 0;
    }

    ShmRingHeader* resp_ring = responseRing();
    const uint8_t* resp_data = responseData();
    if (!resp_ring || !resp_data) {
        OMNI_LOG_ERROR(LOG_TAG, "recv() missing response ring");
        return -1;
    }

    size_t msg_len = 0;
    int ret = shmRingRecvFrame(resp_ring, resp_data,
                               static_cast<uint32_t>(requested_resp_ring_capacity_),
                               buf, buf_size, msg_len);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "recv() malformed response ring");
        state_ = ConnectionState::ERROR;
        return -1;
    }
    if (ret == 0) {
        return 0;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Client received %zu bytes on shm '%s'",
                   msg_len, shm_name_.c_str());
    return static_cast<int>(msg_len);
}

int ShmClientConnection::peekFrameSize(size_t& out_length)
{
    out_length = 0;
    if (state_ != ConnectionState::CONNECTED || !ctrl_) return -1;
    int ret = shmInspectFrame(responseRing(), responseData(),
                              static_cast<uint32_t>(requested_resp_ring_capacity_),
                              out_length);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "peekFrameSize() malformed response ring");
        state_ = ConnectionState::ERROR;
    }
    return ret;
}

void ShmClientConnection::close()
{
    if (state_ == ConnectionState::DISCONNECTED && shm_addr_ == NULL) {
        return;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Closing shm client transport '%s'", shm_name_.c_str());
    cleanup();
    state_ = ConnectionState::DISCONNECTED;
}

ConnectionState ShmClientConnection::state() const
{
    return state_;
}

int ShmClientConnection::fd() const
{
    return event_fd_;
}

TransportType ShmClientConnection::type() const
{
    return TransportType::SHM;
}

// ============================================================
// SHM 指针导航
// ============================================================

ShmRingHeader* ShmClientConnection::requestRing() const
{
    if (!shm_addr_) return NULL;
    return shmRequestRingFromBase(static_cast<uint8_t*>(shm_addr_));
}

uint8_t* ShmClientConnection::requestData() const
{
    if (!shm_addr_) return NULL;
    return shmRequestDataFromBase(static_cast<uint8_t*>(shm_addr_));
}

ShmRingHeader* ShmClientConnection::responseRing() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return shmResponseRingFromBase(static_cast<uint8_t*>(shm_addr_),
                                   static_cast<uint32_t>(requested_req_ring_capacity_));
}

uint8_t* ShmClientConnection::responseData() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return shmResponseDataFromBase(static_cast<uint8_t*>(shm_addr_),
                                   static_cast<uint32_t>(requested_req_ring_capacity_));
}

} // namespace omnibinder
