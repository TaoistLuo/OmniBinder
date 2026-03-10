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
    // Handle 0 is INVALID_HANDLE, so we start from 1 and wrap around
    ServiceHandle h = next_handle_++;
    if (next_handle_ == INVALID_HANDLE) {
        next_handle_ = 1;
    }
    return h;
}

ServiceHandle ServiceRegistry::addService(const ServiceInfo& info, int control_fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (info.name.empty()) {
        OMNI_LOG_ERROR(TAG, "Cannot register service with empty name");
        return INVALID_HANDLE;
    }

    if (info.name.length() > MAX_SERVICE_NAME_LENGTH) {
        OMNI_LOG_ERROR(TAG, "Service name too long: %s", info.name.c_str());
        return INVALID_HANDLE;
    }

    // Check for duplicate name
    if (services_by_name_.find(info.name) != services_by_name_.end()) {
        OMNI_LOG_WARN(TAG, "Service already registered: %s", info.name.c_str());
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

bool ServiceRegistry::removeService(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = services_by_name_.find(name);
    if (it == services_by_name_.end()) {
        return false;
    }

    ServiceEntry& entry = it->second;
    ServiceHandle handle = entry.handle;
    int fd = entry.control_fd;

    // Remove from handle map
    handle_to_name_.erase(handle);

    // Remove from fd map
    auto fd_it = fd_to_services_.find(fd);
    if (fd_it != fd_to_services_.end()) {
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

    // Remove from name map
    services_by_name_.erase(it);

    OMNI_LOG_INFO(TAG, "Unregistered service: %s (handle=%u)", name.c_str(), handle);
    return true;
}

bool ServiceRegistry::removeServiceByHandle(ServiceHandle handle)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto hit = handle_to_name_.find(handle);
    if (hit == handle_to_name_.end()) {
        return false;
    }

    std::string name = hit->second;
    handle_to_name_.erase(hit);

    auto it = services_by_name_.find(name);
    if (it != services_by_name_.end()) {
        int fd = it->second.control_fd;

        // Remove from fd map
        auto fd_it = fd_to_services_.find(fd);
        if (fd_it != fd_to_services_.end()) {
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

        services_by_name_.erase(it);
    }

    OMNI_LOG_INFO(TAG, "Unregistered service by handle: %s (handle=%u)", name.c_str(), handle);
    return true;
}

std::vector<std::string> ServiceRegistry::removeByFd(int fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> removed;

    auto fd_it = fd_to_services_.find(fd);
    if (fd_it == fd_to_services_.end()) {
        return removed;
    }

    // Copy the list since we'll be modifying the map
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
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = services_by_name_.find(name);
    if (it == services_by_name_.end()) {
        return false;
    }

    entry = it->second;
    return true;
}

bool ServiceRegistry::findServiceByHandle(ServiceHandle handle, ServiceEntry& entry) const
{
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ServiceInfo> result;
    result.reserve(services_by_name_.size());

    for (auto it = services_by_name_.begin(); it != services_by_name_.end(); ++it) {
        result.push_back(it->second.info);
    }

    return result;
}

size_t ServiceRegistry::count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return services_by_name_.size();
}

bool ServiceRegistry::exists(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return services_by_name_.find(name) != services_by_name_.end();
}

int ServiceRegistry::getControlFd(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = services_by_name_.find(name);
    if (it == services_by_name_.end()) {
        return -1;
    }

    return it->second.control_fd;
}

bool ServiceRegistry::ownsService(int fd, const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, ServiceEntry>::const_iterator it = services_by_name_.find(name);
    return it != services_by_name_.end() && it->second.control_fd == fd;
}

} // namespace omnibinder
