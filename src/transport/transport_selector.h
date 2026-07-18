#ifndef OMNIBINDER_TRANSPORT_SELECTOR_H
#define OMNIBINDER_TRANSPORT_SELECTOR_H

#include "omnibinder/transport.h"
#include "omnibinder/types.h"
#include <string>
#include <stdint.h>

namespace omnibinder {

class ITransport;

enum class TransportSelectionPolicy {
    PREFER_SHM,
    USE_TCP
};

TransportSelectionPolicy chooseTransportPolicy(
    const std::string& local_host_id,
    const std::string& remote_host_id);

// 传输选择 — 扩展点。
// 当前: 同机优先 SHM，失败/跨机使用 TCP。
// 如需添加新传输 (如 I2C/UDP/RDMA)，在此函数中扩展。
ITransport* selectTransport(const std::string& service_name,
                            const std::string& host, uint16_t port,
                            const std::string& local_host_id,
                            const std::string& remote_host_id,
                            const ShmConfig& shm_config);

} // namespace omnibinder

#endif
