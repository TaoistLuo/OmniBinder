#include "transport/transport_factory.h"
#include "transport/tcp_transport.h"
#include "transport/shm_transport.h"
#include "omnibinder/log.h"

#include <string>
#include <cstdio>

#define LOG_TAG "TransportFactory"

namespace omnibinder {

// ============================================================
// TransportFactory implementation
// ============================================================

TransportFactory::TransportFactory()
{
}

TransportFactory::~TransportFactory()
{
}

TransportFactory& TransportFactory::instance()
{
    static TransportFactory inst;
    return inst;
}

bool TransportFactory::isSameMachine(const std::string& local_host_id,
                                     const std::string& remote_host_id)
{
    return !local_host_id.empty()
        && !remote_host_id.empty()
        && local_host_id == remote_host_id;
}

TransportType TransportFactory::selectPreferredTransport(const std::string& local_host_id,
                                                         const std::string& remote_host_id,
                                                         const std::string& shm_name)
{
    (void)shm_name;
    if (isSameMachine(local_host_id, remote_host_id)) {
        return TransportType::SHM;
    }
    return TransportType::TCP;
}

ITransport* TransportFactory::createTransport(const std::string& local_host_id,
                                              const std::string& remote_host_id)
{
    if (selectPreferredTransport(local_host_id, remote_host_id, std::string()) == TransportType::SHM) {
        OMNI_LOG_INFO(LOG_TAG,
                        "Same machine detected (host_id='%s'), creating SHM transport",
                        local_host_id.c_str());
        return createTransport(TransportType::SHM);
    }

    OMNI_LOG_INFO(LOG_TAG,
                    "Different machines (local='%s', remote='%s'), creating TCP transport",
                    local_host_id.c_str(), remote_host_id.c_str());
    return createTransport(TransportType::TCP);
}

ITransportServer* TransportFactory::createServer(TransportType type)
{
    switch (type) {
    case TransportType::TCP: {
        OMNI_LOG_DEBUG(LOG_TAG, "Creating TcpTransportServer");
        return new TcpTransportServer();
    }
    case TransportType::SHM: {
        // In the new multi-client model, SHM server is created directly
        // via ShmTransport(name, true) in registerService, not through
        // the factory. Return NULL here.
        OMNI_LOG_WARN(LOG_TAG, "SHM server should be created directly, not via factory");
        return NULL;
    }
    default:
        OMNI_LOG_ERROR(LOG_TAG, "Unknown transport type: %d", static_cast<int>(type));
        return NULL;
    }
}

ITransport* TransportFactory::createTransport(TransportType type)
{
    switch (type) {
    case TransportType::TCP: {
        OMNI_LOG_DEBUG(LOG_TAG, "Creating TcpTransport");
        return new TcpTransport();
    }
    case TransportType::SHM: {
        // For client-side SHM transport, generate a temporary name.
        // In the new multi-client model, the actual shm name is provided
        // by the service via ServiceInfo.shm_name. This factory path is
        // only used as a fallback.
        char name_buf[256];
        snprintf(name_buf, sizeof(name_buf), "/binder_tmp_%u",
                 static_cast<uint32_t>(platform::currentTimeMs() & 0xFFFFFFFFu));
        std::string shm_name(name_buf);
        OMNI_LOG_DEBUG(LOG_TAG, "Creating ShmTransport (client, name='%s')",
                         shm_name.c_str());
        return new ShmTransport(shm_name, false);
    }
    default:
        OMNI_LOG_ERROR(LOG_TAG, "Unknown transport type: %d", static_cast<int>(type));
        return NULL;
    }
}

} // namespace omnibinder
