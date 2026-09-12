#include "transport/transport_selector.h"
#include "transport/tcp_transport.h"
#include "transport/shm_client_transport.h"
#include "transport/shm_server_transport.h"
#include "platform/platform.h"
#include "omnibinder/error.h"
#include "omnibinder/log.h"

#define LOG_TAG "TransportSelector"

namespace omnibinder {

TransportSelectionPolicy chooseTransportPolicy(
    const std::string& local_host_id,
    const std::string& remote_host_id)
{
    return !local_host_id.empty()
        && !remote_host_id.empty()
        && local_host_id == remote_host_id
        ? TransportSelectionPolicy::PREFER_SHM
        : TransportSelectionPolicy::USE_TCP;
}

IClientTransport* createClientTransport(const std::string& service_name,
                            const std::string& host, uint16_t port,
                            const std::string& local_host_id,
                            const std::string& remote_host_id,
                            const ShmConfig& shm_config)
{
    if (chooseTransportPolicy(local_host_id, remote_host_id)
        == TransportSelectionPolicy::PREFER_SHM) {
        size_t req_cap = shm_config.req_ring_capacity > 0
            ? shm_config.req_ring_capacity : SHM_DEFAULT_REQ_RING_CAPACITY;
        size_t resp_cap = shm_config.resp_ring_capacity > 0
            ? shm_config.resp_ring_capacity : SHM_DEFAULT_RESP_RING_CAPACITY;
        ShmClientTransport* shm = new ShmClientTransport(service_name, req_cap, resp_cap);
        int ret = shm->connect("", 0);
        if (ret == 0 && shm->state() == ConnectionState::CONNECTED) {
            OMNI_LOG_INFO(LOG_TAG, "Connected to %s via SHM (same machine)", service_name.c_str());
            return shm;
        }
        OMNI_LOG_WARN(LOG_TAG,
                      "data_connect_fallback service=%s transport=SHM fallback=TCP reason=connect_failed",
                      service_name.c_str());
        delete shm;
    }

    TcpClientTransport* tcp = new TcpClientTransport();
    int ret = tcp->connect(host, port);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG,
                       "data_connect_failed service=%s transport=TCP host=%s port=%u err=%d",
                       service_name.c_str(), host.c_str(), port,
                       static_cast<int>(ErrorCode::ERR_CONNECT_FAILED));
        delete tcp;
        return NULL;
    }
    if (ret == 1) {
        platform::waitSocketWritable(tcp->fd(), 1000);
        tcp->checkConnectComplete();
        if (tcp->state() != ConnectionState::CONNECTED) {
            OMNI_LOG_ERROR(LOG_TAG,
                           "data_connect_timeout service=%s transport=TCP host=%s port=%u err=%d",
                           service_name.c_str(), host.c_str(), port,
                           static_cast<int>(ErrorCode::ERR_TIMEOUT));
            delete tcp;
            return NULL;
        }
    }
    OMNI_LOG_INFO(LOG_TAG, "Connected to %s via TCP at %s:%u (fd=%d)",
                    service_name.c_str(), host.c_str(), port, tcp->fd());
    return tcp;
}

IServerTransport* createServerTransport(const std::string& service_name,
                                        TransportType type,
                                        const TransportConfig& config)
{
    if (type == TransportType::SHM) {
        // 容量 0 表示使用 SHM 传输默认值
        size_t req_cap = config.req_capacity > 0
            ? config.req_capacity : SHM_DEFAULT_REQ_RING_CAPACITY;
        size_t resp_cap = config.resp_capacity > 0
            ? config.resp_capacity : SHM_DEFAULT_RESP_RING_CAPACITY;
        return new ShmServerTransport(service_name, req_cap, resp_cap);
    }
    (void)service_name;
    (void)config;
    return new TcpServerTransport();
}

IClientTransport* createControlTransport(const std::string& host, uint16_t port,
                                         int& out_err)
{
    out_err = 0;
    TcpClientTransport* tcp = new TcpClientTransport();
    int ret = tcp->connect(host, port);
    if (ret < 0) {
        OMNI_LOG_ERROR(LOG_TAG, "sm_connect_failed host=%s port=%u err=%d",
                       host.c_str(), port, static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE));
        out_err = static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
        delete tcp;
        return NULL;
    }
    if (ret == 1) {
        platform::waitSocketWritable(tcp->fd(), 1000);
        tcp->checkConnectComplete();
        if (tcp->state() != ConnectionState::CONNECTED) {
            OMNI_LOG_ERROR(LOG_TAG, "sm_connect_timeout host=%s port=%u timeout_ms=%u err=%d",
                           host.c_str(), port, 1000u, static_cast<int>(ErrorCode::ERR_TIMEOUT));
            out_err = static_cast<int>(ErrorCode::ERR_TIMEOUT);
            delete tcp;
            return NULL;
        }
    }
    return tcp;
}

} // namespace omnibinder
