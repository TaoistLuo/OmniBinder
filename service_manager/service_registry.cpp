#include "service_registry.h"
#include "omnibinder/log.h"

#include <algorithm>

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
    // handle 仅作为注册应答里的非零 sentinel（客户端只校验 != 0），不参与查找
    ServiceHandle h = next_handle_++;
    if (h == INVALID_HANDLE) {
        h = next_handle_++;
    }
    return h;
}

const ServiceEntry* ServiceRegistry::findEntry(const std::string& name,
                                               const std::string& instance_key) const
{
    std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator nit =
        services_by_name_.find(name);
    if (nit == services_by_name_.end()) {
        return NULL;
    }
    std::map<std::string, ServiceEntry>::const_iterator eit = nit->second.find(instance_key);
    if (eit == nit->second.end()) {
        return NULL;
    }
    return &eit->second;
}

void ServiceRegistry::removeFromFdIndex(int fd, const std::string& name,
                                        const std::string& instance_key)
{
    std::map<int, std::vector<ServiceIdentityRef> >::iterator fd_it = fd_to_services_.find(fd);
    if (fd_it == fd_to_services_.end()) {
        return;
    }
    std::vector<ServiceIdentityRef>& refs = fd_it->second;
    for (std::vector<ServiceIdentityRef>::iterator it = refs.begin(); it != refs.end(); ++it) {
        if (it->name == name && it->instance_key == instance_key) {
            refs.erase(it);
            break;
        }
    }
    if (refs.empty()) {
        fd_to_services_.erase(fd_it);
    }
}

bool ServiceRegistry::removeEntryByName(const std::string& name,
                                        const std::string& instance_key)
{
    std::map<std::string, std::map<std::string, ServiceEntry> >::iterator nit =
        services_by_name_.find(name);
    if (nit == services_by_name_.end()) {
        return false;
    }
    std::map<std::string, ServiceEntry>::iterator eit = nit->second.find(instance_key);
    if (eit == nit->second.end()) {
        return false;
    }

    ServiceEntry& entry = eit->second;
    removeFromFdIndex(entry.control_fd, name, instance_key);
    nit->second.erase(eit);
    if (nit->second.empty()) {
        services_by_name_.erase(nit);
    }
    return true;
}

ServiceHandle ServiceRegistry::addService(const ServiceInfo& info, int control_fd,
                                          int* displaced_fd)
{
    if (displaced_fd) {
        *displaced_fd = -1;
    }

    if (info.name.empty()) {
        OMNI_LOG_ERROR(TAG, "Cannot register service with empty name");
        return INVALID_HANDLE;
    }

    if (info.name.length() > MAX_SERVICE_NAME_LENGTH) {
        OMNI_LOG_ERROR(TAG, "Service name too long: %s", info.name.c_str());
        return INVALID_HANDLE;
    }

    if (info.host_id.length() > MAX_SERVICE_NAME_LENGTH
        || info.instance_id.length() > MAX_SERVICE_NAME_LENGTH) {
        OMNI_LOG_ERROR(TAG, "Service identity too long: %s (host_id_len=%zu, instance_id_len=%zu)",
                       info.name.c_str(), info.host_id.size(), info.instance_id.size());
        return INVALID_HANDLE;
    }

    const std::string instance_key = makeServiceInstanceKey(info.host_id, info.instance_id);

    // 仅完全相同的身份 (name, host_id, instance_id) 才顶替：崩溃重启的服务接替原身份。
    // 同机不同 instance_id、或不同 host_id 的同名服务是合法多实例，新增并存。
    std::map<std::string, std::map<std::string, ServiceEntry> >::iterator nit =
        services_by_name_.find(info.name);
    if (nit != services_by_name_.end()) {
        std::map<std::string, ServiceEntry>::iterator eit = nit->second.find(instance_key);
        if (eit != nit->second.end()) {
            int old_fd = eit->second.control_fd;
            eit->second.info = info;
            eit->second.control_fd = control_fd;

            // 控制连接变化时（例如重连/重启后）保持 fd -> instances 索引一致，
            // 并回传旧 fd 供调用方回收其上残留的话题/连接状态
            if (old_fd != control_fd) {
                removeFromFdIndex(old_fd, info.name, instance_key);
                fd_to_services_[control_fd].push_back(
                    ServiceIdentityRef(info.name, instance_key));
                if (displaced_fd) {
                    *displaced_fd = old_fd;
                }
            }

            OMNI_LOG_INFO(TAG, "Service instance taken over by newer registration: %s "
                               "(host_id=%s, instance_id=%s, handle=%u, fd=%d replaces fd=%d)",
                          info.name.c_str(), info.host_id.c_str(), info.instance_id.c_str(),
                          eit->second.handle, control_fd, old_fd);
            return eit->second.handle;
        }
    }

    ServiceHandle handle = generateHandle();

    ServiceEntry entry;
    entry.info = info;
    entry.handle = handle;
    entry.control_fd = control_fd;

    services_by_name_[info.name][instance_key] = entry;
    fd_to_services_[control_fd].push_back(ServiceIdentityRef(info.name, instance_key));

    OMNI_LOG_INFO(TAG, "Registered service: %s (handle=%u, fd=%d, host=%s, port=%u, "
                       "host_id=%s, instance_id=%s)",
                  info.name.c_str(), handle, control_fd, info.host.c_str(), info.port,
                  info.host_id.c_str(), info.instance_id.c_str());

    return handle;
}

bool ServiceRegistry::removeService(const std::string& name)
{
    std::map<std::string, std::map<std::string, ServiceEntry> >::iterator nit =
        services_by_name_.find(name);
    if (nit == services_by_name_.end() || nit->second.empty()) {
        return false;
    }

    const std::string instance_key = nit->second.begin()->first;
    ServiceHandle handle = nit->second.begin()->second.handle;
    if (!removeEntryByName(name, instance_key)) {
        return false;
    }

    OMNI_LOG_INFO(TAG, "Unregistered service: %s (handle=%u)", name.c_str(), handle);
    return true;
}

bool ServiceRegistry::removeServiceByIdentity(const ServiceIdentity& identity)
{
    const std::string instance_key =
        makeServiceInstanceKey(identity.host_id, identity.instance_id);
    const ServiceEntry* entry = findEntry(identity.name, instance_key);
    ServiceHandle handle = entry ? entry->handle : INVALID_HANDLE;

    if (!removeEntryByName(identity.name, instance_key)) {
        return false;
    }

    OMNI_LOG_INFO(TAG, "Unregistered service instance: %s (host_id=%s, instance_id=%s, handle=%u)",
                  identity.name.c_str(), identity.host_id.c_str(), identity.instance_id.c_str(),
                  handle);
    return true;
}

std::vector<ServiceIdentity> ServiceRegistry::removeByFd(int fd)
{
    std::vector<ServiceIdentity> removed;

    std::map<int, std::vector<ServiceIdentityRef> >::iterator fd_it = fd_to_services_.find(fd);
    if (fd_it == fd_to_services_.end()) {
        return removed;
    }

    // 先拷贝列表，后续会修改 map
    std::vector<ServiceIdentityRef> refs = fd_it->second;
    fd_to_services_.erase(fd_it);

    for (size_t i = 0; i < refs.size(); ++i) {
        std::map<std::string, std::map<std::string, ServiceEntry> >::iterator nit =
            services_by_name_.find(refs[i].name);
        if (nit == services_by_name_.end()) {
            continue;
        }
        std::map<std::string, ServiceEntry>::iterator eit =
            nit->second.find(refs[i].instance_key);
        if (eit == nit->second.end()) {
            continue;
        }

        ServiceInfo info = eit->second.info;
        nit->second.erase(eit);
        if (nit->second.empty()) {
            services_by_name_.erase(nit);
        }

        removed.push_back(ServiceIdentity(info.name, info.host_id, info.instance_id));
        OMNI_LOG_INFO(TAG, "Removed service instance (fd closed): %s (host_id=%s, "
                           "instance_id=%s, fd=%d)",
                      info.name.c_str(), info.host_id.c_str(), info.instance_id.c_str(), fd);
    }

    return removed;
}

bool ServiceRegistry::findService(const std::string& name, ServiceEntry& entry) const
{
    std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator nit =
        services_by_name_.find(name);
    if (nit == services_by_name_.end() || nit->second.empty()) {
        return false;
    }

    entry = nit->second.begin()->second;
    return true;
}

bool ServiceRegistry::findServiceByIdentity(const ServiceIdentity& identity,
                                            ServiceEntry& entry) const
{
    const ServiceEntry* found = findEntry(
        identity.name, makeServiceInstanceKey(identity.host_id, identity.instance_id));
    if (!found) {
        return false;
    }
    entry = *found;
    return true;
}

bool ServiceRegistry::findServiceByFdAndName(int fd, const std::string& name,
                                             ServiceEntry& entry) const
{
    std::map<int, std::vector<ServiceIdentityRef> >::const_iterator fd_it =
        fd_to_services_.find(fd);
    if (fd_it == fd_to_services_.end()) {
        return false;
    }

    const std::vector<ServiceIdentityRef>& refs = fd_it->second;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].name != name) {
            continue;
        }
        const ServiceEntry* found = findEntry(refs[i].name, refs[i].instance_key);
        if (found) {
            entry = *found;
            return true;
        }
    }
    return false;
}

std::vector<ServiceInfo> ServiceRegistry::listServices() const
{
    std::vector<ServiceInfo> result;
    result.reserve(count());

    for (std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator nit =
             services_by_name_.begin(); nit != services_by_name_.end(); ++nit) {
        for (std::map<std::string, ServiceEntry>::const_iterator eit = nit->second.begin();
             eit != nit->second.end(); ++eit) {
            result.push_back(eit->second.info);
        }
    }

    return result;
}

std::vector<ServiceInfo> ServiceRegistry::listServiceInstances(const std::string& name) const
{
    std::vector<ServiceInfo> result;

    std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator nit =
        services_by_name_.find(name);
    if (nit == services_by_name_.end()) {
        return result;
    }

    // 内层 map 以实例键为序，遍历即得确定性字典序
    result.reserve(nit->second.size());
    for (std::map<std::string, ServiceEntry>::const_iterator eit = nit->second.begin();
         eit != nit->second.end(); ++eit) {
        result.push_back(eit->second.info);
    }

    return result;
}

size_t ServiceRegistry::count() const
{
    size_t total = 0;
    for (std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator it =
             services_by_name_.begin(); it != services_by_name_.end(); ++it) {
        total += it->second.size();
    }
    return total;
}

bool ServiceRegistry::exists(const std::string& name) const
{
    std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator it =
        services_by_name_.find(name);
    return it != services_by_name_.end() && !it->second.empty();
}

int ServiceRegistry::getControlFd(const std::string& name) const
{
    std::map<std::string, std::map<std::string, ServiceEntry> >::const_iterator it =
        services_by_name_.find(name);
    if (it == services_by_name_.end() || it->second.empty()) {
        return -1;
    }

    return it->second.begin()->second.control_fd;
}

std::vector<std::string> ServiceRegistry::listServiceNamesByFd(int fd) const
{
    std::vector<std::string> names;

    std::map<int, std::vector<ServiceIdentityRef> >::const_iterator it =
        fd_to_services_.find(fd);
    if (it == fd_to_services_.end()) {
        return names;
    }

    const std::vector<ServiceIdentityRef>& refs = it->second;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (std::find(names.begin(), names.end(), refs[i].name) == names.end()) {
            names.push_back(refs[i].name);
        }
    }
    return names;
}

bool ServiceRegistry::ownsService(int fd, const std::string& name) const
{
    std::map<int, std::vector<ServiceIdentityRef> >::const_iterator it =
        fd_to_services_.find(fd);
    if (it == fd_to_services_.end()) {
        return false;
    }

    const std::vector<ServiceIdentityRef>& refs = it->second;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].name == name) {
            return true;
        }
    }
    return false;
}

} // namespace omnibinder
