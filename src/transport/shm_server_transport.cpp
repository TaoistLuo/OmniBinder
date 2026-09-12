#include "transport/shm_server_transport.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"
#include "platform/platform.h"

#include <atomic>
#include <string.h>

#define LOG_TAG "ShmServerTransport"

namespace omnibinder {

// ============================================================
// ShmServerClientConnection — 服务端持有的单客户端连接
// ============================================================

class ShmServerClientConnection : public IClientTransport {
public:
    ShmServerClientConnection(int client_id, const std::string& shm_name,
                              void* shm_addr, size_t shm_size, ShmControlBlock* ctrl,
                              uint32_t req_ring_capacity, uint32_t resp_ring_capacity,
                              int resp_eventfd, int request_notify_fd,
                              platform::handshake_channel* liveness_channel)
        : client_id_(client_id)
        , shm_name_(shm_name)
        , shm_addr_(shm_addr)
        , shm_size_(shm_size)
        , ctrl_(ctrl)
        , req_ring_capacity_(req_ring_capacity)
        , resp_ring_capacity_(resp_ring_capacity)
        , resp_eventfd_(resp_eventfd)
        , request_notify_fd_(request_notify_fd)
        , liveness_channel_(liveness_channel)
        , state_(ConnectionState::CONNECTED)
    {
    }

    virtual ~ShmServerClientConnection()
    {
        cleanup();
    }

    // 服务端不主动发起连接
    virtual int connect(const std::string&, uint16_t) { return -1; }

    virtual int send(const uint8_t* data, size_t length);
    virtual int sendAll(const uint8_t* data, size_t length,
                        uint32_t timeout_ms, uint32_t* elapsed_ms);
    virtual int recv(uint8_t* buf, size_t buf_size);
    int peekFrameSize(size_t& out_length) override;

    // 端点 onPollEvent 已消费 master eventfd，故连接级无需再消费
    virtual void consumeReadiness() {}
    virtual bool isFramed() const { return true; }

    // 最小化 close：真正释放由 ShmServerTransport::removeClient 触发（析构 cleanup）
    virtual void close() { state_ = ConnectionState::DISCONNECTED; }
    virtual ConnectionState state() const { return state_; }

    // 返回 liveness channel fd（死亡检测）
    virtual int fd() const { return livenessFd(); }
    virtual TransportType type() const { return TransportType::SHM; }

    int clientId() const { return client_id_; }
    int requestNotifyFd() const { return request_notify_fd_; }

    int livenessFd() const
    {
        return liveness_channel_ ? platform::handshakeGetFd(liveness_channel_) : -1;
    }

    // 释放 SHM 映射（服务端负责 unlink）/ eventfd / liveness channel，幂等
    void cleanup();

private:
    ShmRingHeader* requestRing() const;
    uint8_t*       requestData() const;
    ShmRingHeader* responseRing() const;
    uint8_t*       responseData() const;

    int             client_id_;
    std::string     shm_name_;
    void*           shm_addr_;
    size_t          shm_size_;
    ShmControlBlock* ctrl_;
    uint32_t        req_ring_capacity_;
    uint32_t        resp_ring_capacity_;
    int             resp_eventfd_;
    int             request_notify_fd_;
    platform::handshake_channel* liveness_channel_;
    ConnectionState state_;
};

ShmRingHeader* ShmServerClientConnection::requestRing() const
{
    if (!shm_addr_) return NULL;
    return shmRequestRingFromBase(static_cast<uint8_t*>(shm_addr_));
}

uint8_t* ShmServerClientConnection::requestData() const
{
    if (!shm_addr_) return NULL;
    return shmRequestDataFromBase(static_cast<uint8_t*>(shm_addr_));
}

ShmRingHeader* ShmServerClientConnection::responseRing() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return shmResponseRingFromBase(static_cast<uint8_t*>(shm_addr_), req_ring_capacity_);
}

uint8_t* ShmServerClientConnection::responseData() const
{
    if (!shm_addr_ || !ctrl_) return NULL;
    return shmResponseDataFromBase(static_cast<uint8_t*>(shm_addr_), req_ring_capacity_);
}

int ShmServerClientConnection::peekFrameSize(size_t& out_length)
{
    out_length = 0;
    if (state_ != ConnectionState::CONNECTED) return -1;
    int ret = shmInspectFrame(requestRing(), requestData(), req_ring_capacity_, out_length);
    if (ret < 0) {
        state_ = ConnectionState::ERROR;
    }
    return ret;
}

int ShmServerClientConnection::recv(uint8_t* buf, size_t buf_size)
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

int ShmServerClientConnection::send(const uint8_t* data, size_t length)
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

int ShmServerClientConnection::sendAll(const uint8_t* data, size_t length,
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

void ShmServerClientConnection::cleanup()
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

// ============================================================
// ShmServerTransport
// ============================================================

ShmServerTransport::ShmServerTransport(const std::string& service_name,
                                       size_t req_ring_capacity,
                                       size_t resp_ring_capacity)
    : service_name_(service_name)
    , req_ring_capacity_(shmNormalizeRingCapacity(req_ring_capacity))
    , resp_ring_capacity_(shmNormalizeRingCapacity(resp_ring_capacity))
    , handshake_listener_(NULL)
    , master_eventfd_(-1)
{
}

ShmServerTransport::~ShmServerTransport()
{
    close();
}

int ShmServerTransport::start(const std::string& host, uint16_t port,
                              const TransportConfig& config)
{
    (void)host;
    (void)port;
    if (handshake_listener_ != NULL) {
        OMNI_LOG_WARN(LOG_TAG, "start() on an already started endpoint '%s'",
                      service_name_.c_str());
        return 0;
    }

    if (config.req_capacity > 0 && shmIsValidRingCapacity(config.req_capacity)) {
        req_ring_capacity_ = shmNormalizeRingCapacity(config.req_capacity);
    }
    if (config.resp_capacity > 0 && shmIsValidRingCapacity(config.resp_capacity)) {
        resp_ring_capacity_ = shmNormalizeRingCapacity(config.resp_capacity);
    }

    handshake_path_ = shmHandshakePath(generateShmName(service_name_));
    handshake_listener_ = platform::handshakeListen(handshake_path_);
    if (!handshake_listener_) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create handshake listen for '%s'",
                       service_name_.c_str());
        handshake_path_.clear();
        return -1;
    }

    master_eventfd_ = platform::createEventFd();
    if (master_eventfd_ < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "Failed to create master req_eventfd for '%s'",
                       service_name_.c_str());
        platform::handshakeCloseListener(handshake_listener_);
        handshake_listener_ = NULL;
        handshake_path_.clear();
        return -1;
    }

    OMNI_LOG_INFO(LOG_TAG, "Server listening on handshake '%s' (fd=%d, req_eventfd=%d)",
                  handshake_path_.c_str(), handshakeListenFd(), master_eventfd_);
    return 0;
}

void ShmServerTransport::close()
{
    for (std::map<int, ShmServerClientConnection*>::iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        delete it->second;
    }
    clients_.clear();

    if (master_eventfd_ >= 0) {
        platform::closeEventFd(master_eventfd_);
        master_eventfd_ = -1;
    }
    if (handshake_listener_) {
        platform::handshakeCloseListener(handshake_listener_);
        handshake_listener_ = NULL;
    }
    handshake_path_.clear();
}

TransportType ShmServerTransport::type() const
{
    return TransportType::SHM;
}

int ShmServerTransport::handshakeListenFd() const
{
    return handshake_listener_ ? platform::handshakeGetListenerFd(handshake_listener_) : -1;
}

void ShmServerTransport::pollFds(std::vector<int>& fds) const
{
    int listen_fd = handshakeListenFd();
    if (listen_fd >= 0) {
        fds.push_back(listen_fd);
    }
    if (master_eventfd_ >= 0) {
        fds.push_back(master_eventfd_);
    }
    for (std::map<int, ShmServerClientConnection*>::const_iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        int liveness_fd = it->second->livenessFd();
        if (liveness_fd >= 0) {
            fds.push_back(liveness_fd);
        }
        // Windows 平台：per-client 本地请求通知 fd
        int notify_fd = it->second->requestNotifyFd();
        if (notify_fd >= 0) {
            fds.push_back(notify_fd);
        }
    }
}

void ShmServerTransport::onPollEvent(int fd, uint32_t events)
{
    (void)events;

    if (fd >= 0 && fd == handshakeListenFd()) {
        acceptHandshakeClients();
        return;
    }

    if (fd == master_eventfd_) {
        platform::eventFdConsume(fd);
        scanClientsForReadable();
        return;
    }

    int client_id = -1;
    bool is_request_notify = false;
    for (std::map<int, ShmServerClientConnection*>::iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        if (it->second->livenessFd() == fd) {
            client_id = it->first;
            break;
        }
        if (it->second->requestNotifyFd() == fd) {
            is_request_notify = true;
            break;
        }
    }

    if (is_request_notify) {
        platform::eventFdConsume(fd);
        scanClientsForReadable();
        return;
    }
    if (client_id < 0) {
        return;
    }

    // 客户端死亡：报告断开，不在 transport 内删除。
    // core 的 disconnect 回调负责先摘 fd，再调用 removeClient。
    OMNI_LOG_INFO(LOG_TAG, "client[%d] liveness fd %d signaled, reporting disconnect",
                  client_id, fd);
    DisconnectCallback cb = disconnect_cb_;
    if (cb) cb(client_id);
}

void ShmServerTransport::setAcceptCallback(const AcceptCallback& cb)
{
    accept_cb_ = cb;
}

void ShmServerTransport::setReadableCallback(const ReadableCallback& cb)
{
    readable_cb_ = cb;
}

void ShmServerTransport::setDisconnectCallback(const DisconnectCallback& cb)
{
    disconnect_cb_ = cb;
}

int ShmServerTransport::allocClientId()
{
    // 进程内单调递增，与 fd 号命名空间隔离
    static std::atomic<int> s_next(SHM_CLIENT_ID_BASE);
    int id = s_next.fetch_add(1, std::memory_order_relaxed);
    if (id < SHM_CLIENT_ID_BASE) {
        s_next.store(SHM_CLIENT_ID_BASE + 1, std::memory_order_relaxed);
        id = SHM_CLIENT_ID_BASE;
    }
    return id;
}

// 完成一次握手：接收 SHM 名 → 打开并校验 → 回传通知句柄 → 建立私有连接
ShmServerClientConnection* ShmServerTransport::createClientFromHandshake(
    platform::handshake_channel* ch)
{
    // 步骤 1：接收 SHM 名（不允许携带 fd）
    char name_buf[256] = {0};
    size_t data_len = 0;
    int fd_count = 0;
    if (!platform::handshakeRecv(ch, name_buf, sizeof(name_buf) - 1, &data_len,
                                 NULL, 0, &fd_count)
        || fd_count != 0 || data_len == 0) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: invalid client request");
        platform::handshakeClose(ch);
        return NULL;
    }

    std::string client_shm_name(name_buf, data_len);
    if (client_shm_name.empty()) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: empty SHM name from client");
        platform::handshakeClose(ch);
        return NULL;
    }

    // 打开客户端 SHM
    size_t mapped_size = 0;
    void* addr = platform::shmCreate(client_shm_name, sizeof(ShmControlBlock), false,
                                     &mapped_size);
    if (addr == NULL) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: failed to open client SHM '%s'",
                      client_shm_name.c_str());
        platform::handshakeClose(ch);
        return NULL;
    }

    ShmControlBlock* ctrl = NULL;
    uint32_t req_capacity = 0;
    uint32_t resp_capacity = 0;
    if (!shmValidateMappedLayout(addr, mapped_size, ctrl, req_capacity, resp_capacity)) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: SHM object does not cover a valid layout for '%s'",
                      client_shm_name.c_str());
        platform::shmDetach(addr, mapped_size);
        // 布局无效（典型场景：客户端在 ready_flag 发布前崩溃），
        // 按 SHM 生命周期契约「服务端负责 shmUnlink」兜底清理，避免 /dev/shm 残留。
        platform::shmUnlink(client_shm_name);
        platform::handshakeClose(ch);
        return NULL;
    }

    // 步骤 2：为该客户端创建响应通知 eventfd
    int resp_efd = platform::createEventFd();
    if (resp_efd < 0) {
        OMNI_LOG_WARN(LOG_TAG, "handshake: failed to create resp_eventfd for client '%s'",
                      client_shm_name.c_str());
        platform::shmDetach(addr, mapped_size);
        platform::shmUnlink(client_shm_name);
        platform::handshakeClose(ch);
        return NULL;
    }

    // 步骤 3：回传 [响应通知, 请求通知]。
    //   响应通知 → 服务端写响应时唤醒客户端 epoll
    //   请求通知 → Linux 为共享 master_eventfd；Windows 由平台生成 per-client fd
    {
        int response_fds[2] = {resp_efd, master_eventfd_};
        if (!platform::handshakeSend(ch, NULL, 0, response_fds, 2)) {
            OMNI_LOG_WARN(LOG_TAG, "handshake: failed to send eventfds to client '%s'",
                          client_shm_name.c_str());
            platform::shmDetach(addr, mapped_size);
            platform::shmUnlink(client_shm_name);
            platform::closeEventFd(resp_efd);
            platform::handshakeClose(ch);
            return NULL;
        }
    }

    int local_notify_fd = platform::handshakeTakeLocalNotifyFd(ch);

    int assigned_id = allocClientId();
    ShmServerClientConnection* conn = new ShmServerClientConnection(
        assigned_id, client_shm_name, addr, mapped_size, ctrl,
        req_capacity, resp_capacity, resp_efd, local_notify_fd, ch);

    OMNI_LOG_INFO(LOG_TAG, "handshake: client[%d] connected, shm='%s', resp_efd=%d",
                  assigned_id, client_shm_name.c_str(), resp_efd);
    return conn;
}

void ShmServerTransport::acceptHandshakeClients()
{
    std::vector<std::pair<int, IClientTransport*> > accepted;
    while (true) {
        platform::handshake_channel* ch = platform::handshakeAccept(handshake_listener_);
        if (!ch) {
            break;
        }
        ShmServerClientConnection* conn = createClientFromHandshake(ch);
        if (!conn) {
            continue;
        }
        clients_[conn->clientId()] = conn;
        accepted.push_back(std::make_pair(conn->clientId(),
                                          static_cast<IClientTransport*>(conn)));
        // 新客户端的首批请求可能已就绪，通知主控 eventfd 触发扫描
        if (master_eventfd_ >= 0) {
            platform::eventFdNotify(master_eventfd_);
        }
    }

    // 回调前拷贝：用户回调（core 回调内可能注销服务）可能在第一次回调后
    // 删除本 transport，后续不得再访问 this
    AcceptCallback cb = accept_cb_;
    for (size_t i = 0; i < accepted.size() && cb; ++i) {
        cb(accepted[i].first, accepted[i].second);
    }
}

void ShmServerTransport::scanClientsForReadable()
{
    std::vector<int> malformed;
    std::vector<int> readable;

    for (std::map<int, ShmServerClientConnection*>::iterator it = clients_.begin();
         it != clients_.end(); ++it) {
        size_t frame_size = 0;
        int ret = it->second->peekFrameSize(frame_size);
        if (ret < 0) {
            malformed.push_back(it->first);
        } else if (ret > 0) {
            readable.push_back(it->first);
        }
    }

    // 回调前拷贝：回调可能删除客户端乃至本 transport（服务注销）
    DisconnectCallback disconnect = disconnect_cb_;
    ReadableCallback readable_cb = readable_cb_;

    // 畸形 ring 按断开上报，不在这里删除（core 统一清理，removeClient 最后调用）
    for (size_t i = 0; i < malformed.size(); ++i) {
        OMNI_LOG_ERROR(LOG_TAG, "malformed request ring from client[%d], reporting disconnect",
                       malformed[i]);
        if (disconnect) disconnect(malformed[i]);
    }
    for (size_t i = 0; i < readable.size(); ++i) {
        if (readable_cb) readable_cb(readable[i]);
    }
}

void ShmServerTransport::removeClient(int client_id)
{
    std::map<int, ShmServerClientConnection*>::iterator it = clients_.find(client_id);
    if (it == clients_.end()) {
        return;
    }
    ShmServerClientConnection* conn = it->second;
    clients_.erase(it);
    delete conn;
    OMNI_LOG_INFO(LOG_TAG, "client[%d] disconnected from shm service '%s'",
                  client_id, service_name_.c_str());
}

} // namespace omnibinder
