#include "transport/shm_server_endpoint.h"
#include "transport/shm_server_connection.h"
#include "omnibinder/log.h"
#include "omnibinder/types.h"
#include "platform/platform.h"

#include <atomic>
#include <string.h>

#define LOG_TAG "ShmServerEndpoint"

namespace omnibinder {

// ============================================================
// ShmServerEndpoint
// ============================================================

ShmServerEndpoint::ShmServerEndpoint(const std::string& service_name,
                                       size_t req_ring_capacity,
                                       size_t resp_ring_capacity)
    : service_name_(service_name)
    , req_ring_capacity_(shmNormalizeRingCapacity(req_ring_capacity))
    , resp_ring_capacity_(shmNormalizeRingCapacity(resp_ring_capacity))
    , handshake_listener_(NULL)
    , master_eventfd_(-1)
{
}

ShmServerEndpoint::~ShmServerEndpoint()
{
    close();
}

int ShmServerEndpoint::start(const std::string& host, uint16_t port,
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

void ShmServerEndpoint::close()
{
    for (std::map<int, ShmServerConnection*>::iterator it = clients_.begin();
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

TransportType ShmServerEndpoint::type() const
{
    return TransportType::SHM;
}

int ShmServerEndpoint::handshakeListenFd() const
{
    return handshake_listener_ ? platform::handshakeGetListenerFd(handshake_listener_) : -1;
}

void ShmServerEndpoint::pollFds(std::vector<int>& fds) const
{
    int listen_fd = handshakeListenFd();
    if (listen_fd >= 0) {
        fds.push_back(listen_fd);
    }
    if (master_eventfd_ >= 0) {
        fds.push_back(master_eventfd_);
    }
    for (std::map<int, ShmServerConnection*>::const_iterator it = clients_.begin();
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

void ShmServerEndpoint::onPollEvent(int fd, uint32_t events)
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
    for (std::map<int, ShmServerConnection*>::iterator it = clients_.begin();
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

void ShmServerEndpoint::setAcceptCallback(const AcceptCallback& cb)
{
    accept_cb_ = cb;
}

void ShmServerEndpoint::setReadableCallback(const ReadableCallback& cb)
{
    readable_cb_ = cb;
}

void ShmServerEndpoint::setDisconnectCallback(const DisconnectCallback& cb)
{
    disconnect_cb_ = cb;
}

int ShmServerEndpoint::allocClientId()
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
ShmServerConnection* ShmServerEndpoint::createClientFromHandshake(
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
    ShmServerConnection* conn = new ShmServerConnection(
        assigned_id, client_shm_name, addr, mapped_size, ctrl,
        req_capacity, resp_capacity, resp_efd, local_notify_fd, ch);

    OMNI_LOG_INFO(LOG_TAG, "handshake: client[%d] connected, shm='%s', resp_efd=%d",
                  assigned_id, client_shm_name.c_str(), resp_efd);
    return conn;
}

void ShmServerEndpoint::acceptHandshakeClients()
{
    std::vector<std::pair<int, IMessageConnection*> > accepted;
    while (true) {
        platform::handshake_channel* ch = platform::handshakeAccept(handshake_listener_);
        if (!ch) {
            break;
        }
        ShmServerConnection* conn = createClientFromHandshake(ch);
        if (!conn) {
            continue;
        }
        clients_[conn->clientId()] = conn;
        accepted.push_back(std::make_pair(conn->clientId(),
                                          static_cast<IMessageConnection*>(conn)));
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

void ShmServerEndpoint::scanClientsForReadable()
{
    std::vector<int> malformed;
    std::vector<int> readable;

    for (std::map<int, ShmServerConnection*>::iterator it = clients_.begin();
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

void ShmServerEndpoint::removeClient(int client_id)
{
    std::map<int, ShmServerConnection*>::iterator it = clients_.find(client_id);
    if (it == clients_.end()) {
        return;
    }
    ShmServerConnection* conn = it->second;
    clients_.erase(it);
    delete conn;
    OMNI_LOG_INFO(LOG_TAG, "client[%d] disconnected from shm service '%s'",
                  client_id, service_name_.c_str());
}

} // namespace omnibinder
