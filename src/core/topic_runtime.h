/**************************************************************************************************
 * @file        topic_runtime.h
 * @brief       话题广播运行时
 * @details     管理话题发布/订阅关系。每个话题可存在多条订阅，按 (发布者服务,
 *              实例身份) 选择器区分服务级与实例级；发布/订阅者映射与广播消息分发。
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
#ifndef OMNIBINDER_CORE_TOPIC_RUNTIME_H
#define OMNIBINDER_CORE_TOPIC_RUNTIME_H

#include "omnibinder/runtime.h"
#include "omnibinder/buffer.h"
#include <map>
#include <string>
#include <vector>

namespace omnibinder {

class TopicRuntime {
public:
    struct ShmSubscriber {
        std::string service_name;
        uint32_t client_id;
    };

    /*
     * @brief 订阅来源选择器
     * @details publisher_service 为空表示不限定发布者服务（匹配任意服务发布的该主题）；
     *          instance_level 为 true 时再按 (host_id, instance_id) 精确匹配。
     */
    struct SubscriptionSelector {
        std::string publisher_service;
        std::string host_id;
        std::string instance_id;
        bool        instance_level;

        SubscriptionSelector() : instance_level(false) {}

        /* @brief 选择器唯一键（用于订阅去重与定位） */
        std::string key() const;

        /* @brief 是否匹配给定发布者实例身份 */
        bool matches(const std::string& service, const std::string& host_id,
                     const std::string& instance_id) const;
    };

    /*
     * @brief 控制面恢复用的订阅记录（含选择器与期望哈希）
     */
    struct SubscriptionRecord {
        std::string topic_name;
        SubscriptionSelector selector;
        uint32_t expected_idl_hash;
    };

    /*
     * @brief 发布者通知匹配结果
     */
    struct MatchedSubscription {
        std::string key;
        SubscriptionSelector selector;
        uint32_t expected_idl_hash;
    };

    /* @brief 登记一条订阅（选择器统一承载普通订阅：默认空 selector 匹配任意发布者） */
    void rememberSubscription(const std::string& topic_name,
                              const SubscriptionSelector& selector,
                              const TopicSourceCallback& callback,
                              uint32_t expected_idl_hash);
    /* @brief 仅移除匹配该选择器的订阅（普通/选择器订阅互不影响） */
    void forgetSubscription(const std::string& topic_name,
                            const SubscriptionSelector& selector);
    /* @brief 登记某选择器对应订阅的错误回调 */
    void setErrorCallback(const std::string& topic_name,
                          const SubscriptionSelector& selector,
                          const TopicErrorCallback& cb);
    void notifyError(uint32_t topic_id, ErrorCode error);
    /* @brief 仅通知匹配该发布者实例来源的订阅错误回调（空身份匹配全部） */
    void notifyError(uint32_t topic_id, const std::string& publisher_service,
                     const std::string& host_id, const std::string& instance_id,
                     ErrorCode error);
    void notifySubscriptionError(const std::string& topic_name,
                                 const std::string& subscription_key, ErrorCode error);
    void rememberPublishedTopic(const std::string& topic_name, uint32_t topic_id,
                                const std::string& owner_service, uint32_t idl_hash);
    void forgetPublishedTopic(const std::string& topic_name);
    void forgetPublishedTopicsByOwner(const std::string& owner_service);
    void addTcpSubscriber(uint32_t topic_id, int client_fd);
    void removeTcpSubscriberFd(int client_fd);
    void addShmSubscriberService(uint32_t topic_id, const std::string& service_name,
                                 uint32_t client_id);
    void removeShmSubscriberService(const std::string& service_name, uint32_t client_id);

    const std::vector<int>& tcpSubscribers(uint32_t topic_id) const;
    const std::vector<ShmSubscriber>& shmSubscribers(uint32_t topic_id) const;
    void removeTcpSubscriber(uint32_t topic_id, int client_fd);

    /* @brief 按来源实例身份匹配选择器后分发（服务级/实例级过滤） */
    void dispatchFrom(const std::string& publisher_service, const std::string& host_id,
                      const std::string& instance_id, uint32_t topic_id,
                      const Buffer& data) const;

    std::vector<SubscriptionRecord> subscriptionRecords() const;
    std::vector<MatchedSubscription> matchSubscriptions(const std::string& topic_name,
                                                        const std::string& publisher_service,
                                                        const std::string& host_id,
                                                        const std::string& instance_id) const;
    std::map<std::string, std::string> publishedTopicOwners() const;
    std::map<std::string, uint32_t> publishedTopicHashes() const;
    void setExpectedSubscriptionHash(const std::string& topic_name,
                                     const SubscriptionSelector& selector,
                                     uint32_t idl_hash);

    uint32_t getTopicId(const std::string& name) const;

private:
    /*
     * @brief 单条订阅状态（选择器 + 回调 + 期望哈希）
     */
    struct Subscription {
        SubscriptionSelector selector;
        std::string          key;
        TopicSourceCallback  source_callback;
        bool                 has_source_callback;
        TopicErrorCallback   error_callback;
        bool                 has_error_callback;
        uint32_t             expected_idl_hash;

        Subscription()
            : has_source_callback(false), has_error_callback(false),
              expected_idl_hash(0) {}
    };

    /*
     * @brief  单个话题的全部状态（订阅/发布/订阅者/哈希），取代此前散落的 9 张 map
     */
    struct TopicState {
        uint32_t             id;
        std::string          name;
        std::vector<Subscription> subscriptions;
        std::vector<int>     tcp_subscribers;
        std::vector<ShmSubscriber> shm_subscribers;
        bool                 published;
        std::string          owner_service;
        uint32_t             published_hash;

        TopicState()
            : id(0), published(false), published_hash(0) {}
    };

    TopicState& ensureTopic(uint32_t id, const std::string& name);
    Subscription& ensureSubscription(TopicState& state, const SubscriptionSelector& selector);
    Subscription* findSubscription(TopicState& state, const std::string& key);
    void dropNameIfUnused(const std::string& name, uint32_t id);
    bool isTopicUnused(const TopicState& state) const;
    std::map<uint32_t, TopicState>::iterator eraseTopicIfUnused(
        std::map<uint32_t, TopicState>::iterator it);

    std::map<uint32_t, TopicState> topics_;
    std::map<std::string, uint32_t> name_to_id_;
};

} // namespace omnibinder

#endif
