#include "transport/transport_selector.h"
#include "transport/tcp_transport.h"
#include "transport/shm_transport.h"
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

ITransport* selectTransport(const std::string& service_name,
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
        ShmTransport* shm = new ShmTransport(generateShmName(service_name), false,
                                             req_cap, resp_cap);
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

    TcpTransport* tcp = new TcpTransport();
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

} // namespace omnibinder
