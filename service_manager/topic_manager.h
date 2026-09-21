/**************************************************************************************************
 * @file        topic_manager.h
 * @brief       话题发布/订阅管理器
 * @details     管理话题的发布者和订阅者关系。话题身份为 (发布者服务名, 主题名)：
 *              不同服务可使用同名主题而互不影响，同一服务的同名主题允许存在多个
 *              发布者实例（按 host_id + instance_id 区分）。订阅支持三种粒度：
 *              仅按主题名（兼容旧行为，匹配任意服务）、服务级（指定服务的全部实例）、
 *              实例级（指定服务的目标实例）。ServiceManager 通过此模块将发布者的
 *              连接信息转发给匹配的订阅者，使订阅者可直连发布者接收广播数据。
 *              支持按 fd 批量清理断开连接的发布者和订阅者。
 *              仅 ServiceManager owner 线程访问（SM 为单线程事件循环）。
 *
 * @author      taoist.luo
 * @version     1.1.0
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
// TopicSubscriptionFilter - 订阅粒度选择器（SM 侧）
//
// @brief  描述订阅的粒度：仅主题名 / 服务级 / 实例级
// @details TOPIC_ONLY 兼容历史"按主题名订阅"语义（匹配任意服务发布的该主题）；
//          SERVICE 匹配 (publisher_service, topic) 的全部发布者实例；
//          INSTANCE 再叠加 (host_id, instance_id) 精确匹配单个发布者实例。
// ============================================================
enum class TopicSubscribeMode : uint8_t {
    TOPIC_ONLY = 0,
    SERVICE    = 1,
    INSTANCE   = 2
};

struct TopicSubscriptionFilter {
    TopicSubscribeMode mode;
    std::string        publisher_service;  // SERVICE / INSTANCE 有效
    std::string        host_id;            // INSTANCE 有效
    std::string        instance_id;        // INSTANCE 有效

    TopicSubscriptionFilter() : mode(TopicSubscribeMode::TOPIC_ONLY) {}

    /* @brief 构造服务级选择器（该服务发布的 topic 全部实例） */
    static TopicSubscriptionFilter service(const std::string& publisher_service) {
        TopicSubscriptionFilter selector;
        selector.mode = TopicSubscribeMode::SERVICE;
        selector.publisher_service = publisher_service;
        return selector;
    }

    /* @brief 构造实例级选择器（仅该服务的目标实例） */
    static TopicSubscriptionFilter instance(const std::string& publisher_service,
                                              const std::string& host_id,
                                              const std::string& instance_id) {
        TopicSubscriptionFilter selector;
        selector.mode = TopicSubscribeMode::INSTANCE;
        selector.publisher_service = publisher_service;
        selector.host_id = host_id;
        selector.instance_id = instance_id;
        return selector;
    }

    /* @brief 选择器唯一键（用于同一 fd 上的订阅去重） */
    std::string key() const;

    /* @brief 该选择器是否匹配给定发布者实例 */
    bool matches(const ServiceInfo& publisher) const;
};

// ============================================================
// TopicEntry - 话题发布的内部存储
//
// @brief  (发布者服务名, 主题名) 下的全部发布者实例与订阅者
// ============================================================
struct PublisherSlot {
    int         fd;
    ServiceInfo info;      // 发布者实例信息（含 host_id / instance_id）
    uint32_t    idl_hash;
    PublisherSlot() : fd(-1), idl_hash(0) {}
};

struct TopicEntry {
    std::string service_name;  // 发布者服务名；TOPIC_ONLY 订阅条目为空串
    std::string topic_name;
    // 发布者实例：instance_key (host_id|instance_id) -> slot
    std::map<std::string, PublisherSlot> publishers;
    // 订阅者：fd -> (选择器 key -> 选择器)
    std::map<int, std::map<std::string, TopicSubscriptionFilter> > subscribers;

    TopicEntry() {}
};

// ============================================================
// TopicPublisherView - 查询结果中的发布者视图（信息 + IDL hash）
// ============================================================
struct TopicPublisherView {
    ServiceInfo info;
    uint32_t    idl_hash;
    TopicPublisherView() : idl_hash(0) {}
};

// ============================================================
// TopicManager - 管理话题发布者与订阅者
//
// @brief  管理话题发布者与订阅者
// @details 话题身份 = (发布者服务名, 主题名)，其下允许多个发布者实例并存；
//          订阅按选择器粒度匹配后下发发布者连接信息。
// ============================================================
class TopicManager {
public:
    TopicManager();
    ~TopicManager();

    // 禁止拷贝
    TopicManager(const TopicManager&) = delete;
    TopicManager& operator=(const TopicManager&) = delete;

    /*
     * @brief  为话题注册发布者实例
     * @param[in]  topic          话题名称
     * @param[in]  publisher_info 发布者服务信息（含 host_id / instance_id）
     * @param[in]  publisher_fd   发布者的控制连接 fd
     * @param[in]  idl_hash       发布者在该话题上的 IDL hash
     * @return true 表示注册成功（相同发布者实例重复注册为幂等更新）；false 表示超限/非法
     */
    bool registerPublisher(const std::string& topic, const ServiceInfo& publisher_info,
                           int publisher_fd, uint32_t idl_hash = 0);

    /*
     * @brief  移除该主题名下全部服务的发布者（保留订阅关系）
     * @param[in]  topic 话题名称
     * @return true 表示至少移除一个发布者
     */
    bool removePublisher(const std::string& topic);

    /*
     * @brief  移除该主题名下归属指定 fd 的发布者
     * @param[in]  topic        话题名称
     * @param[in]  publisher_fd 发布者的控制连接 fd
     * @return true 表示至少移除一个匹配发布者
     */
    bool removePublisher(const std::string& topic, int publisher_fd);

    /*
     * @brief  检查指定 fd 是否为该话题任一发布者实例的归属连接
     * @param[in]  topic        话题名称
     * @param[in]  publisher_fd 控制连接 fd
     * @return true 表示该 fd 归属此话题的某个发布者角色
     */
    bool isPublisherOwner(const std::string& topic, int publisher_fd) const;

    /*
     * @brief  按主题名订阅（兼容旧行为，匹配任意服务发布的该主题）
     * @param[in]  topic         话题名称
     * @param[in]  subscriber_fd 订阅者的控制连接 fd
     * @return true 表示订阅者已添加；已订阅时返回 false
     */
    bool addSubscriber(const std::string& topic, int subscriber_fd);

    /*
     * @brief  按选择器粒度订阅
     * @param[in]  topic         话题名称
     * @param[in]  selector      订阅选择器
     * @param[in]  subscriber_fd 订阅者的控制连接 fd
     * @return true 表示订阅者已添加；同一 fd 上重复添加同一选择器返回 false
     */
    bool addSubscriber(const std::string& topic, const TopicSubscriptionFilter& selector,
                       int subscriber_fd);

    /*
     * @brief  移除该主题名下指定 fd 的全部订阅
     * @param[in]  topic         话题名称
     * @param[in]  subscriber_fd 订阅者的控制连接 fd
     * @return true 表示至少移除一条订阅
     */
    bool removeSubscriber(const std::string& topic, int subscriber_fd);

    /*
     * @brief  移除指定 fd 上匹配选择器的一条订阅
     * @param[in]  topic         话题名称
     * @param[in]  selector      订阅选择器
     * @param[in]  subscriber_fd 订阅者的控制连接 fd
     * @return true 表示该订阅存在并已移除
     */
    bool removeSubscriber(const std::string& topic, const TopicSubscriptionFilter& selector,
                          int subscriber_fd);

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
     * @brief  获取话题的全部订阅者（所有服务的并集）
     * @param[in]  topic 话题名称
     * @return 订阅者 fd 列表（去重）
     */
    std::vector<int> getSubscribers(const std::string& topic) const;

    /*
     * @brief  获取匹配给定发布者实例的订阅者 fd
     * @param[in]  topic     话题名称
     * @param[in]  publisher 发布者实例信息
     * @return 匹配的订阅者 fd 列表（去重）
     * @note   TOPIC_ONLY 订阅匹配任意发布者；SERVICE / INSTANCE 按选择器匹配。
     *         用于发布者上线时只通知匹配的订阅者。
     */
    std::vector<int> getMatchingSubscribers(const std::string& topic,
                                            const ServiceInfo& publisher) const;

    /*
     * @brief  获取匹配选择器的已存在发布者实例
     * @param[in]  topic    话题名称
     * @param[in]  selector 订阅选择器
     * @return 匹配的发布者视图列表（按 (服务, 实例) 确定顺序）
     * @note   用于订阅请求到达时立即向该订阅者下发已在线发布者。
     */
    std::vector<TopicPublisherView> getMatchingPublishers(
        const std::string& topic, const TopicSubscriptionFilter& selector) const;

    /*
     * @brief  获取话题的第一个发布者信息（兼容旧接口）
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
     * @brief  获取话题第一个发布者的 IDL hash（兼容旧接口）
     * @param[in]   topic    话题名称
     * @param[out]  idl_hash 有发布者时填充 IDL hash
     * @return true 表示该话题有发布者
     */
    bool getIdlHash(const std::string& topic, uint32_t& idl_hash) const;

    /*
     * @brief  获取匹配选择器的第一个发布者 IDL hash
     * @param[in]   topic    话题名称
     * @param[in]   selector 订阅选择器
     * @param[out]  idl_hash 有匹配发布者时填充 IDL hash
     * @return true 表示存在匹配的发布者
     */
    bool getIdlHash(const std::string& topic, const TopicSubscriptionFilter& selector,
                    uint32_t& idl_hash) const;

    /*
     * @brief  设置话题第一个发布者的 IDL hash（兼容旧接口）
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
     * @brief  获取全部话题名（含仅订阅的话题）
     * @return 话题名列表（去重）
     */
    std::vector<std::string> listTopics() const;

private:
    std::string entryKey(const std::string& service_name,
                         const std::string& topic_name) const;
    void releaseTopicNameRef(const std::string& topic_name);
    std::map<std::string, TopicEntry>::iterator eraseEntryIfUnused(
        std::map<std::string, TopicEntry>::iterator it);
    bool removePublisherSlots(const std::string& topic_name, int publisher_fd,
                              bool filter_fd);

    // 话题条目：composite key -> entry（TOPIC_ONLY 订阅条目的 service_name 为空）
    std::map<std::string, TopicEntry> topics_;

    // 话题名引用计数：仅订阅的话题也占一个名额（全局话题上限）
    std::map<std::string, size_t> topic_name_refs_;

    // 反向索引：fd -> 该 fd 作为订阅者的条目 key 集合
    std::map<int, std::set<std::string> > fd_subscriptions_;

    // 反向索引：fd -> 该 fd 作为发布者的条目 key 集合
    std::map<int, std::set<std::string> > fd_publications_;

    // 每连接订阅条数（同一 fd 在同一话题上的不同选择器分别计数）
    std::map<int, size_t> fd_subscription_counts_;

    size_t total_subscriptions_;
};

} // namespace omnibinder

#endif // OMNIBINDER_TOPIC_MANAGER_H
