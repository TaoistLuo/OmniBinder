/**************************************************************************************************
 * @file        service_registry.h
 * @brief       服务注册表
 * @details     ServiceManager 的核心数据结构，存储所有已注册服务的元信息
 *              （ServiceInfo、ServiceHandle、控制连接 fd）。支持按名称、句柄、
 *              fd 进行增删查操作，内部维护多重索引以实现 O(1) 查找。
 *              线程安全，所有方法均可从任意线程调用。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef OMNIBINDER_SERVICE_REGISTRY_H
#define OMNIBINDER_SERVICE_REGISTRY_H

#include "omnibinder/types.h"
#include <string>
#include <map>
#include <vector>
#include <mutex>

namespace omnibinder {

// ============================================================
// ServiceEntry - Internal storage for a registered service
// ============================================================
struct ServiceEntry {
    ServiceInfo info;
    ServiceHandle handle;
    int control_fd;  // TCP fd for the service's control connection

    ServiceEntry() : handle(INVALID_HANDLE), control_fd(-1) {}
};

// ============================================================
// ServiceRegistry - Stores and manages registered services
//
// Thread-safe: all methods can be called from any thread.
// ============================================================
class ServiceRegistry {
public:
    ServiceRegistry();
    ~ServiceRegistry();

    // Disable copy
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    // Add a new service to the registry.
    // Returns the assigned ServiceHandle, or INVALID_HANDLE if the service
    // name is already registered.
    ServiceHandle addService(const ServiceInfo& info, int control_fd);

    // Remove a service by name.
    // Returns true if the service was found and removed.
    bool removeService(const std::string& name);

    // Remove a service by its handle.
    // Returns true if the service was found and removed.
    bool removeServiceByHandle(ServiceHandle handle);

    // Remove all services associated with a given control fd.
    // Returns the list of service names that were removed.
    std::vector<std::string> removeByFd(int fd);

    // Find a service by name.
    // Returns true if found, and fills in the entry.
    bool findService(const std::string& name, ServiceEntry& entry) const;

    // Find a service by handle.
    // Returns true if found, and fills in the entry.
    bool findServiceByHandle(ServiceHandle handle, ServiceEntry& entry) const;

    // List all registered services.
    std::vector<ServiceInfo> listServices() const;

    // Get the number of registered services.
    size_t count() const;

    // Check if a service exists by name.
    bool exists(const std::string& name) const;

    // Get the control fd for a service by name.
    // Returns -1 if not found.
    int getControlFd(const std::string& name) const;

    // Check whether the given fd owns the specified service.
    bool ownsService(int fd, const std::string& name) const;

private:
    // Generate a unique handle for a new service
    ServiceHandle generateHandle();

    mutable std::mutex mutex_;
    std::map<std::string, ServiceEntry> services_by_name_;
    std::map<ServiceHandle, std::string> handle_to_name_;
    std::map<int, std::vector<std::string>> fd_to_services_;
    ServiceHandle next_handle_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SERVICE_REGISTRY_H
