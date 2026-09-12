#include "service_registry.h"
#include "omnibinder/log.h"

#define TAG "ServiceRegistry"

namespace omnibinder {
ServiceRegistry::ServiceRegistry()
    : next_handle_(1)
{
}

ServiceRegistry::~ServiceRegistry()
{
}

ServiceHandle ServiceRegistry::generateHandle()
{
    // Handle 0 是 INVALID_HANDLE。跳过它以及仍在使用的 handle，
    // 避免 32 位计数器回绕后与存活注册的 handle 冲突。
    for (;;) {
        ServiceHandle h = next_handle_++;
        if (next_handle_ == INVALID_HANDLE) {
            next_handle_ = 1;
        }
        if (h != INVALID_HANDLE && handle_to_name_.find(h) == handle_to_name_.end()) {
            return h;
        }
    }
}

ServiceHandle ServiceRegistry::addService(const ServiceInfo& info, int control_fd)
{
    if (info.name.empty()) {
        OMNI_LOG_ERROR(TAG, "Cannot register service with empty name");
        return INVALID_HANDLE;
    }

    if (info.name.length() > MAX_SERVICE_NAME_LENGTH) {
        OMNI_LOG_ERROR(TAG, "Service name too long: %s", info.name.c_str());
        return INVALID_HANDLE;
    }

    // 检查重名。同一 runtime（相同且非空 host_id）重复注册视为幂等更新：
    // 刷新 ServiceInfo 和控制 fd，保留原 handle 并返回成功。这样客户端在
    // 部分失败后可重试完整注册流程而不被拒绝。不同 host_id 使用相同名称
    // 仍是硬冲突。
    std::map<std::string, ServiceEntry>::iterator existing = services_by_name_.find(info.name);
    if (existing != services_by_name_.end()) {
        if (!existing->second.info.host_id.empty()
            && existing->second.info.host_id == info.host_id) {
            int old_fd = existing->second.control_fd;
            existing->second.info = info;
            existing->second.control_fd = control_fd;

            // 控制连接变化时（例如重连后）保持 fd -> services 索引一致
            if (old_fd != control_fd) {
                removeFromFdIndex(old_fd, info.name);
                fd_to_services_[control_fd].push_back(info.name);
            }

            OMNI_LOG_INFO(TAG, "Re-registered service (idempotent update): %s (handle=%u, fd=%d)",
                          info.name.c_str(), existing->second.handle, control_fd);
            return existing->second.handle;
        }

        OMNI_LOG_WARN(TAG, "Service already registered by a different host_id: %s", info.name.c_str());
        return INVALID_HANDLE;
    }

    ServiceHandle handle = generateHandle();

    ServiceEntry entry;
    entry.info = info;
    entry.handle = handle;
    entry.control_fd = control_fd;

    services_by_name_[info.name] = entry;
    handle_to_name_[handle] = info.name;
    fd_to_services_[control_fd].push_back(info.name);

    OMNI_LOG_INFO(TAG, "Registered service: %s (handle=%u, fd=%d, host=%s, port=%u)",
                    info.name.c_str(), handle, control_fd,
                    info.host.c_str(), info.port);

    return handle;
}

void ServiceRegistry::removeFromFdIndex(int fd, const std::string& name)
{
    auto fd_it = fd_to_services_.find(fd);
    if (fd_it == fd_to_services_.end()) {
        return;
    }
    std::vector<std::string>& names = fd_it->second;
    for (auto nit = names.begin(); nit != names.end(); ++nit) {
        if (*nit == name) {
            names.erase(nit);
            break;
        }
    }
    if (names.empty()) {
        fd_to_services_.erase(fd_it);
    }
}

bool ServiceRegistry::removeService(const std::string& name)
{
    auto it = services_by_name_.find(name);
    if (it == services_by_name_.end()) {
        return false;
    }

    ServiceEntry& entry = it->second;
    ServiceHandle handle = entry.handle;
    int fd = entry.control_fd;

    // 从 handle 映射中移除
    handle_to_name_.erase(handle);

    removeFromFdIndex(fd, name);

    // 从名称映射中移除
    services_by_name_.erase(it);

    OMNI_LOG_INFO(TAG, "Unregistered service: %s (handle=%u)", name.c_str(), handle);
    return true;
}

bool ServiceRegistry::removeServiceByHandle(ServiceHandle handle)
{
    auto hit = handle_to_name_.find(handle);
    if (hit == handle_to_name_.end()) {
        return false;
    }

    std::string name = hit->second;
    handle_to_name_.erase(hit);

    auto it = services_by_name_.find(name);
    if (it != services_by_name_.end()) {
        removeFromFdIndex(it->second.control_fd, name);
        services_by_name_.erase(it);
    }

    OMNI_LOG_INFO(TAG, "Unregistered service by handle: %s (handle=%u)", name.c_str(), handle);
    return true;
}

std::vector<std::string> ServiceRegistry::removeByFd(int fd)
{
    std::vector<std::string> removed;

    auto fd_it = fd_to_services_.find(fd);
    if (fd_it == fd_to_services_.end()) {
        return removed;
    }

    // 先拷贝列表，后续会修改 map
    removed = fd_it->second;
    fd_to_services_.erase(fd_it);

    for (size_t i = 0; i < removed.size(); ++i) {
        const std::string& name = removed[i];
        auto it = services_by_name_.find(name);
        if (it != services_by_name_.end()) {
            handle_to_name_.erase(it->second.handle);
            services_by_name_.erase(it);
            OMNI_LOG_INFO(TAG, "Removed service (fd closed): %s (fd=%d)", name.c_str(), fd);
        }
    }

    return removed;
}

bool ServiceRegistry::findService(const std::string& name, ServiceEntry& entry) const
{
    auto it = services_by_name_.find(name);
    if (it == services_by_name_.end()) {
        return false;
    }

    entry = it->second;
    return true;
}

bool ServiceRegistry::findServiceByHandle(ServiceHandle handle, ServiceEntry& entry) const
{
    auto hit = handle_to_name_.find(handle);
    if (hit == handle_to_name_.end()) {
        return false;
    }

    auto it = services_by_name_.find(hit->second);
    if (it == services_by_name_.end()) {
        return false;
    }

    entry = it->second;
    return true;
}

std::vector<ServiceInfo> ServiceRegistry::listServices() const
{
    std::vector<ServiceInfo> result;
    result.reserve(services_by_name_.size());

    for (auto it = services_by_name_.begin(); it != services_by_name_.end(); ++it) {
        result.push_back(it->second.info);
    }

    return result;
}

size_t ServiceRegistry::count() const
{
    return services_by_name_.size();
}

bool ServiceRegistry::exists(const std::string& name) const
{
    return services_by_name_.find(name) != services_by_name_.end();
}

int ServiceRegistry::getControlFd(const std::string& name) const
{
    auto it = services_by_name_.find(name);
    if (it == services_by_name_.end()) {
        return -1;
    }

    return it->second.control_fd;
}

std::vector<std::string> ServiceRegistry::listServiceNamesByFd(int fd) const
{
    std::map<int, std::vector<std::string> >::const_iterator it = fd_to_services_.find(fd);
    if (it == fd_to_services_.end()) {
        return std::vector<std::string>();
    }

    return it->second;
}

bool ServiceRegistry::ownsService(int fd, const std::string& name) const
{
    std::map<std::string, ServiceEntry>::const_iterator it = services_by_name_.find(name);
    return it != services_by_name_.end() && it->second.control_fd == fd;
}

} // namespace omnibinder
