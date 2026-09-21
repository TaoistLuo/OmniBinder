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
// ServiceIdentityRef - 指向某个 (name, host_id, instance_id) 条目的轻量索引
//
// @brief  注册表内部索引使用的实例键引用；instance_key 由 makeServiceInstanceKey 生成
// ============================================================
struct ServiceIdentityRef {
    std::string name;
    std::string instance_key;

    ServiceIdentityRef() {}
    ServiceIdentityRef(const std::string& n, const std::string& k)
        : name(n), instance_key(k) {}
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
     * @brief  向注册表添加服务，身份键为 (服务名, host_id, instance_id)
     * @param[in]   info         服务信息
     * @param[in]   control_fd   服务的控制连接 TCP fd
     * @param[out]  displaced_fd 非空时输出被顶替的旧控制连接 fd；未被顶替或与
     *                           control_fd 相同时置 -1
     * @return 服务 handle；名称为空或超长时返回 INVALID_HANDLE
     * @note   同身份（name + host_id + instance_id 全同）重复注册视为后来者顶替
     *         （崩溃重启场景），刷新 ServiceInfo 与控制 fd 并保留原 handle；
     *         异身份（同机不同 instance_id / 不同 host_id）新增条目并存，不顶替。
     *         旧连接上残留的话题归属与连接回收由调用方依据 displaced_fd 完成。
     */
    ServiceHandle addService(const ServiceInfo& info, int control_fd,
                             int* displaced_fd = NULL);

    /*
     * @brief  按名称移除服务（同名多实例时移除确定性的第一个实例）
     * @param[in]  name 服务名称
     * @return true 表示服务已找到并移除
     * @note   同名可能存在多个实例；需要精确移除时使用 removeServiceByIdentity
     */
    bool removeService(const std::string& name);

    /*
     * @brief  按完整实例身份精确移除服务
     * @param[in]  identity 服务实例身份
     * @return true 表示该实例已找到并移除
     */
    bool removeServiceByIdentity(const ServiceIdentity& identity);

    /*
     * @brief  移除指定控制 fd 关联的全部服务实例
     * @param[in]  fd 控制连接 fd
     * @return 被移除实例的身份列表
     */
    std::vector<ServiceIdentity> removeByFd(int fd);

    /*
     * @brief  按名称查找服务（同名多实例时返回确定性的第一个实例）
     * @param[in]   name  服务名称
     * @param[out]  entry 找到时填充的服务条目
     * @return true 表示找到
     * @note   确定性顺序为实例键（host_id, instance_id 的无歧义编码）字典序；
     *         同名多实例的完整枚举由 listServiceInstances 提供
     */
    bool findService(const std::string& name, ServiceEntry& entry) const;

    /*
     * @brief  按完整实例身份查找服务
     * @param[in]   identity 服务实例身份
     * @param[out]  entry    找到时填充的服务条目
     * @return true 表示找到
     */
    bool findServiceByIdentity(const ServiceIdentity& identity, ServiceEntry& entry) const;

    /*
     * @brief  在指定控制 fd 名下按服务名查找实例
     * @param[in]   fd    控制连接 fd
     * @param[in]   name  服务名称
     * @param[out]  entry 找到时填充的服务条目
     * @return true 表示找到
     * @note   同一 fd 与同一名称至多对应一个实例，用于解析心跳/注销消息的实例身份
     */
    bool findServiceByFdAndName(int fd, const std::string& name, ServiceEntry& entry) const;

    /*
     * @brief  列出全部已注册服务实例（含同名多实例）
     * @return 服务信息列表
     */
    std::vector<ServiceInfo> listServices() const;

    /*
     * @brief  列出指定服务名下的全部实例（跨 host_id / instance_id）
     * @param[in]  name 服务名称
     * @return 该名称下的服务信息列表；名称不存在时返回空列表
     * @note   顺序为实例键 (host_id, instance_id 的无歧义编码) 字典序，
     *         与 findService 选取的"确定性第一实例"一致
     */
    std::vector<ServiceInfo> listServiceInstances(const std::string& name) const;

    /*
     * @brief  获取已注册服务实例总数（同名多实例按实例计数）
     * @return 已注册实例数
     */
    size_t count() const;

    /*
     * @brief  检查指定名称是否存在任一实例
     * @param[in]  name 服务名称
     * @return true 表示存在
     */
    bool exists(const std::string& name) const;

    /*
     * @brief  按名称获取服务实例的控制连接 fd（同名多实例时返回确定性第一个实例）
     * @param[in]  name 服务名称
     * @return 控制连接 fd；未找到时返回 -1
     */
    int getControlFd(const std::string& name) const;

    /*
     * @brief  获取某个控制连接 fd 当前拥有的全部服务名（去重）
     * @param[in]  fd 控制连接 fd
     * @return 该 fd 名下的服务名列表；无归属时返回空列表
     * @note   fd 索引是"服务归属"的唯一权威来源，连接断开清理应以此为据
     */
    std::vector<std::string> listServiceNamesByFd(int fd) const;

    /*
     * @brief  检查指定 fd 是否归属该名称的任一实例
     * @param[in]  fd   控制连接 fd
     * @param[in]  name 服务名称
     * @return true 表示该 fd 归属此名称
     */
    bool ownsService(int fd, const std::string& name) const;

private:
    /*
     * @brief  为新服务生成唯一 handle
     * @return 新的 ServiceHandle
     */
    ServiceHandle generateHandle();

    /*
     * @brief  按 (名称, 实例键) 移除条目并同步全部索引
     * @param[in]  name         服务名称
     * @param[in]  instance_key makeServiceInstanceKey 生成的实例键
     * @return true 表示条目存在并已移除
     */
    bool removeEntryByName(const std::string& name, const std::string& instance_key);

    void removeFromFdIndex(int fd, const std::string& name,
                           const std::string& instance_key);

    const ServiceEntry* findEntry(const std::string& name,
                                  const std::string& instance_key) const;

    // 外层 key: 服务名；内层 key: makeServiceInstanceKey(host_id, instance_id)
    std::map<std::string, std::map<std::string, ServiceEntry> > services_by_name_;
    // 控制 fd -> 其拥有的实例引用列表
    std::map<int, std::vector<ServiceIdentityRef> > fd_to_services_;
    ServiceHandle next_handle_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SERVICE_REGISTRY_H
