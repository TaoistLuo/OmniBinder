/**************************************************************************************************
 * @file        topic_manager.h
 * @brief       话题发布/订阅管理器
 * @details     管理话题的发布者和订阅者关系。每个话题最多一个发布者、零或多个订阅者。
 *              ServiceManager 通过此模块将发布者的连接信息转发给订阅者，使订阅者
 *              可直连发布者接收广播数据。支持按 fd 批量清理断开连接的发布者和订阅者。
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
#ifndef OMNIBINDER_TOPIC_MANAGER_H
#define OMNIBINDER_TOPIC_MANAGER_H

#include "omnibinder/types.h"
#include <string>
#include <map>
#include <set>
#include <vector>

namespace omnibinder {

const size_t MAX_TOPICS_GLOBAL = 16384u;
const size_t MAX_TOPIC_PUBLICATIONS_PER_CONNECTION = 4096u;
const size_t MAX_TOPIC_SUBSCRIPTIONS_PER_CONNECTION = 4096u;
const size_t MAX_SUBSCRIBERS_PER_TOPIC = 4096u;
const size_t MAX_TOPIC_SUBSCRIPTIONS_GLOBAL = 16384u;

// ============================================================
// TopicEntry - 话题发布者信息的内部存储
//
// @brief  话题发布者信息的内部存储条目
// ============================================================
struct TopicEntry {
    std::string topic_name;
    ServiceInfo publisher_info;  // 发布者的连接信息（host:port）
    int publisher_fd;            // 发布者的控制连接 fd
    std::set<int> subscriber_fds;
    uint32_t idl_hash;           // 发布者在该话题上的 IDL hash

    TopicEntry() : publisher_fd(-1), idl_hash(0) {}
};

// ============================================================
// TopicManager - 管理话题发布者与订阅者
//
// @brief  管理话题发布者与订阅者
// @details 每个话题最多一个发布者、零或多个订阅者。ServiceManager 通过它把
//          话题发布者信息转发给订阅者，使订阅者能直连发布者。
// ============================================================
class TopicManager {
public:
    TopicManager();
    ~TopicManager();

    // 禁止拷贝
    TopicManager(const TopicManager&) = delete;
    TopicManager& operator=(const TopicManager&) = delete;

    /*
     * @brief  为话题注册发布者
     * @param[in]  topic          话题名称
     * @param[in]  publisher_info 发布者服务信息
     * @param[in]  publisher_fd   发布者的控制连接 fd
     * @param[in]  idl_hash       发布者在该话题上的 IDL hash
     * @return true 表示注册成功；false 表示该话题已有发布者
     */
    bool registerPublisher(const std::string& topic, const ServiceInfo& publisher_info,
                           int publisher_fd, uint32_t idl_hash = 0);

    /*
     * @brief  移除话题的发布者
     * @param[in]  topic 话题名称
     * @return true 表示发布者已找到并移除
     */
    bool removePublisher(const std::string& topic);

    /*
     * @brief  移除话题的发布者（仅当归属指定 fd 时）
     * @param[in]  topic        话题名称
     * @param[in]  publisher_fd 发布者的控制连接 fd
     * @return 仅当话题存在且该 fd 为归属者时返回 true
     */
    bool removePublisher(const std::string& topic, int publisher_fd);

    /*
     * @brief  检查指定 fd 是否为该话题发布者的归属连接
     * @param[in]  topic        话题名称
     * @param[in]  publisher_fd 控制连接 fd
     * @return true 表示该 fd 归属此话题的发布者角色
     */
    bool isPublisherOwner(const std::string& topic, int publisher_fd) const;

    /*
     * @brief  为话题添加订阅者
     * @param[in]  topic         话题名称
     * @param[in]  subscriber_fd 订阅者的控制连接 fd
     * @return true 表示订阅者已添加；已订阅时返回 false
     */
    bool addSubscriber(const std::string& topic, int subscriber_fd);

    /*
     * @brief  移除话题的订阅者
     * @param[in]  topic         话题名称
     * @param[in]  subscriber_fd 订阅者的控制连接 fd
     * @return true 表示订阅者已找到并移除
     */
    bool removeSubscriber(const std::string& topic, int subscriber_fd);

    /*
     * @brief  移除指定 fd 关联的全部话题与订阅关系
     * @param[in]  fd 控制连接 fd
     * @return 该 fd 作为发布者的话题名列表
     * @note   同时处理发布者和订阅者两种角色
     */
    std::vector<std::string> removeByFd(int fd);

    /*
     * @brief  移除指定服务在预期控制 fd 上的全部发布关系
     * @param[in]  service_name 服务名称
     * @param[in]  publisher_fd 预期控制连接 fd
     * @return 被移除的发布话题名列表
     * @note   保留订阅关系及共享该 fd 的同级服务
     */
    std::vector<std::string> removePublishersByService(
        const std::string& service_name, int publisher_fd);

    /*
     * @brief  获取话题的全部订阅者
     * @param[in]  topic 话题名称
     * @return 订阅者 fd 列表
     */
    std::vector<int> getSubscribers(const std::string& topic) const;

    /*
     * @brief  获取话题的发布者信息
     * @param[in]   topic          话题名称
     * @param[out]  publisher_info 有发布者时填充其服务信息
     * @return true 表示该话题有发布者
     */
    bool getPublisher(const std::string& topic, ServiceInfo& publisher_info) const;

    /*
     * @brief  获取指定服务当前发布的话题（按确定顺序）
     * @param[in]  service_name 服务名称
     * @return 话题名列表
     */
    std::vector<std::string> getPublishedTopics(const std::string& service_name) const;

    /*
     * @brief  获取话题发布者的 IDL hash
     * @param[in]   topic    话题名称
     * @param[out]  idl_hash 有发布者时填充 IDL hash
     * @return true 表示该话题有发布者
     */
    bool getIdlHash(const std::string& topic, uint32_t& idl_hash) const;

    /*
     * @brief  设置话题发布者的 IDL hash
     * @param[in]  topic    话题名称
     * @param[in]  idl_hash IDL 哈希
     * @return true 表示该话题存在且有发布者
     */
    bool setIdlHash(const std::string& topic, uint32_t idl_hash);

    /*
     * @brief  检查话题是否有发布者
     * @param[in]  topic 话题名称
     * @return true 表示有发布者
     */
    bool hasPublisher(const std::string& topic) const;

    /*
     * @brief  获取全部话题名
     * @return 话题名列表
     */
    std::vector<std::string> listTopics() const;

private:
    bool removePublisherLocked(const std::string& topic, int publisher_fd);

    std::map<std::string, TopicEntry> topics_;

    // 反向索引：fd -> 该 fd 作为订阅者的话题集合
    std::map<int, std::set<std::string>> fd_subscriptions_;

    // 反向索引：fd -> 该 fd 作为发布者的话题集合
    std::map<int, std::set<std::string>> fd_publications_;
    size_t total_subscriptions_;
};

} // namespace omnibinder

#endif // OMNIBINDER_TOPIC_MANAGER_H
