#include "transport/shm_server_connection.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"
#include "platform/platform.h"

#include <string.h>

#define LOG_TAG "ShmServerConnection"

namespace omnibinder {

ShmRingHeader* ShmServerConnection::requestRing() const
{
    if (!shm_addr_) return NULL;
    return shmRequestRingFromBase(static_cast<uint8_t*>(shm_addr_));
}

uint8_t* ShmServerConnection::requestData() const
{
    if (!shm_addr_) return NULL;
    return shmRequestDataFromBase(static_cast<uint8_t*>(shm_addr_));
}

ShmRingHeader* ShmServerConnection::responseRing() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return shmResponseRingFromBase(static_cast<uint8_t*>(shm_addr_), req_ring_capacity_);
}

uint8_t* ShmServerConnection::responseData() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return shmResponseDataFromBase(static_cast<uint8_t*>(shm_addr_), req_ring_capacity_);
}

int ShmServerConnection::peekFrameSize(size_t& out_length)
{
    out_length = 0;
    if (state_ != ConnectionState::CONNECTED) return -1;
    int ret = shmInspectFrame(requestRing(), requestData(), req_ring_capacity_, out_length);
    if (ret < 0) {
        state_ = ConnectionState::ERROR;
    }
    return ret;
}

int ShmServerConnection::recv(uint8_t* buf, size_t buf_size)
{
    if (state_ != ConnectionState::CONNECTED) return -1;
    if (buf_size == 0) return 0;

    ShmRingHeader* req_ring = requestRing();
    const uint8_t* req_data = requestData();
    if (!req_ring || !req_data) {
        OMNI_LOG_ERROR(LOG_TAG, "recv() missing request ring for client[%d]", client_id_);
        return -1;
    }

    size_t msg_len = 0;
    int ret = shmRingRecvFrame(req_ring, req_data, req_ring_capacity_,
                               buf, buf_size, msg_len);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "recv() malformed request ring from client[%d]", client_id_);
        state_ = ConnectionState::ERROR;
        return -1;
    }
    if (ret == 0) {
        return 0;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Received %zu bytes from client[%d] on shm '%s'",
                   msg_len, client_id_, shm_name_.c_str());
    return static_cast<int>(msg_len);
}

int ShmServerConnection::send(const uint8_t* data, size_t length)
{
    if (state_ != ConnectionState::CONNECTED) return -1;
    if (length == 0) return 0;
    if (length > MAX_MESSAGE_SIZE || length > 0x7FFFFFFF) {
        OMNI_LOG_ERROR(LOG_TAG, "send() message too large: %zu bytes", length);
        return -1;
    }

    ShmRingHeader* resp_ring = responseRing();
    uint8_t* resp_data = responseData();
    if (!resp_ring || !resp_data) {
        OMNI_LOG_ERROR(LOG_TAG, "send() missing response ring for client[%d]", client_id_);
        return -1;
    }

    uint32_t written = shmRingSendFrame(resp_ring, resp_data, resp_ring_capacity_,
                                        data, static_cast<uint32_t>(length),
                                        resp_eventfd_);
    if (written == 0) {
        OMNI_LOG_DEBUG(LOG_TAG, "send() response ring full for client[%d]", client_id_);
        return 0;
    }

    OMNI_LOG_DEBUG(LOG_TAG, "Sent %zu bytes to client[%d] on shm '%s'",
                   length, client_id_, shm_name_.c_str());
    return static_cast<int>(length);
}

int ShmServerConnection::sendAll(const uint8_t* data, size_t length,
                                       uint32_t timeout_ms, uint32_t* elapsed_ms)
{
    int64_t start_ms = platform::currentTimeMs();
    if (elapsed_ms) {
        *elapsed_ms = 0;
    }
    if (state_ != ConnectionState::CONNECTED) return -1;
    if (length == 0) return 0;
    if (length > MAX_MESSAGE_SIZE || length > 0x7FFFFFFF) {
        OMNI_LOG_ERROR(LOG_TAG, "sendAll() message too large: %zu bytes", length);
        return -1;
    }

    ShmRingHeader* resp_ring = responseRing();
    uint8_t* resp_data = responseData();
    if (!resp_ring || !resp_data) {
        OMNI_LOG_ERROR(LOG_TAG, "sendAll() missing response ring for client[%d]", client_id_);
        return -1;
    }

    int rc = shmRingSendFrameWithinTimeout(resp_ring, resp_data, resp_ring_capacity_,
                                           data, static_cast<uint32_t>(length),
                                           resp_eventfd_, timeout_ms);
    if (elapsed_ms) {
        *elapsed_ms = static_cast<uint32_t>(platform::currentTimeMs() - start_ms);
    }
    if (rc == -1) {
        OMNI_LOG_ERROR(LOG_TAG,
                       "sendAll() frame can never fit for client[%d]: need %zu, ring capacity %u",
                       client_id_, length + sizeof(uint32_t), resp_ring_capacity_);
        return -1;
    }
    if (rc != 0) {
        OMNI_LOG_WARN(LOG_TAG,
                      "sendAll() response ring full timeout for client[%d]: need=%zu timeout_ms=%u",
                      client_id_, length + sizeof(uint32_t), timeout_ms);
        return -1;
    }
    return 0;
}

void ShmServerConnection::cleanup()
{
    if (shm_addr_ != NULL) {
        platform::shmDetach(shm_addr_, shm_size_);
        shm_addr_ = NULL;
    }
    // 客户端已断开时，服务端负责清理 SHM 对象避免 /dev/shm 残留
    if (!shm_name_.empty()) {
        platform::shmUnlink(shm_name_);
    }
    if (resp_eventfd_ >= 0) {
        platform::closeEventFd(resp_eventfd_);
        resp_eventfd_ = -1;
    }
    if (request_notify_fd_ >= 0) {
        platform::closeEventFd(request_notify_fd_);
        request_notify_fd_ = -1;
    }
    if (liveness_channel_) {
        platform::handshakeClose(liveness_channel_);
        liveness_channel_ = NULL;
    }
    ctrl_ = NULL;
    shm_size_ = 0;
    state_ = ConnectionState::DISCONNECTED;
}

} // namespace omnibinder
