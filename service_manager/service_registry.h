/**************************************************************************************************
 * @file        service_registry.h
 * @brief       服务注册表
 * @details     ServiceManager 的核心数据结构，存储所有已注册服务的元信息
 *              （ServiceInfo、ServiceHandle、控制连接 fd）。支持按名称、句柄、
 *              fd 进行增删查操作，内部维护多重索引以实现 O(1) 查找。
 *              仅 ServiceManager owner 线程访问（SM 为单线程事件循环）。
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
#include <stddef.h>

namespace omnibinder {

// ============================================================
// ServiceEntry - 已注册服务的内部存储
//
// @brief  已注册服务的内部存储条目
// ============================================================
struct ServiceEntry {
    ServiceInfo info;
    ServiceHandle handle;
    int control_fd;  // 服务的控制连接 TCP fd

    ServiceEntry() : handle(INVALID_HANDLE), control_fd(-1) {}
};

// ============================================================
// ServiceRegistry - 存储并管理已注册的服务
//
// @brief  存储并管理已注册的服务
// @details 仅 owner 线程访问：ServiceManager 由单个 event-loop 线程驱动。
// ============================================================
class ServiceRegistry {
public:
    ServiceRegistry();
    ~ServiceRegistry();

    // 禁止拷贝
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    /*
     * @brief  向注册表添加新服务（同名后来者顶替先前者）
     * @param[in]   info         服务信息
     * @param[in]   control_fd   服务的控制连接 TCP fd
     * @param[out]  displaced_fd 非空时输出被顶替的旧控制连接 fd；未被顶替或与
     *                           control_fd 相同时置 -1
     * @return 服务 handle；名称为空或超长时返回 INVALID_HANDLE
     * @note   服务名全局唯一，同名重复注册一律视为后来者顶替（崩溃重启场景），
     *         刷新 ServiceInfo 与控制 fd 并保留原 handle。旧连接上残留的话题
     *         归属与连接回收由调用方依据 displaced_fd 完成。
     */
    ServiceHandle addService(const ServiceInfo& info, int control_fd,
                             int* displaced_fd = NULL);

    /*
     * @brief  按名称移除服务
     * @param[in]  name 服务名称
     * @return true 表示服务已找到并移除
     */
    bool removeService(const std::string& name);

    /*
     * @brief  按 handle 移除服务
     * @param[in]  handle 服务 handle
     * @return true 表示服务已找到并移除
     */
    bool removeServiceByHandle(ServiceHandle handle);

    /*
     * @brief  移除指定控制 fd 关联的全部服务
     * @param[in]  fd 控制连接 fd
     * @return 被移除的服务名列表
     */
    std::vector<std::string> removeByFd(int fd);

    /*
     * @brief  按名称查找服务
     * @param[in]   name  服务名称
     * @param[out]  entry 找到时填充的服务条目
     * @return true 表示找到
     */
    bool findService(const std::string& name, ServiceEntry& entry) const;

    /*
     * @brief  按 handle 查找服务
     * @param[in]   handle 服务 handle
     * @param[out]  entry  找到时填充的服务条目
     * @return true 表示找到
     */
    bool findServiceByHandle(ServiceHandle handle, ServiceEntry& entry) const;

    /*
     * @brief  列出全部已注册服务
     * @return 服务信息列表
     */
    std::vector<ServiceInfo> listServices() const;

    /*
     * @brief  获取已注册服务数量
     * @return 已注册服务数量
     */
    size_t count() const;

    /*
     * @brief  检查指定名称的服务是否存在
     * @param[in]  name 服务名称
     * @return true 表示存在
     */
    bool exists(const std::string& name) const;

    /*
     * @brief  按名称获取服务的控制连接 fd
     * @param[in]  name 服务名称
     * @return 控制连接 fd；未找到时返回 -1
     */
    int getControlFd(const std::string& name) const;

    /*
     * @brief  获取某个控制连接 fd 当前拥有的全部服务名
     * @param[in]  fd 控制连接 fd
     * @return 该 fd 名下的服务名列表；无归属时返回空列表
     * @note   fd 索引是"服务归属"的唯一权威来源，连接断开清理应以此为据
     */
    std::vector<std::string> listServiceNamesByFd(int fd) const;

    /*
     * @brief  检查指定 fd 是否为该服务的归属连接
     * @param[in]  fd   控制连接 fd
     * @param[in]  name 服务名称
     * @return true 表示该 fd 归属此服务
     */
    bool ownsService(int fd, const std::string& name) const;

private:
    /*
     * @brief  为新服务生成唯一 handle
     * @return 新的 ServiceHandle
     */
    ServiceHandle generateHandle();

    void removeFromFdIndex(int fd, const std::string& name);

    std::map<std::string, ServiceEntry> services_by_name_;
    std::map<ServiceHandle, std::string> handle_to_name_;
    std::map<int, std::vector<std::string>> fd_to_services_;
    ServiceHandle next_handle_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SERVICE_REGISTRY_H
