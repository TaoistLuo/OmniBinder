/**************************************************************************************************
 * @file        topic_runtime.h
 * @brief       话题广播运行时
 * @details     管理话题发布/订阅关系。维护 topic_id 到 subscriber 回调的映射，
 *              支持广播消息分发和死亡通知触发。
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

    void rememberSubscription(const std::string& topic_name, const TopicCallback& callback,
                              uint32_t expected_idl_hash);
    void forgetSubscription(const std::string& topic_name);
    void setErrorCallback(const std::string& topic_name, const TopicErrorCallback& cb);
    void notifyError(uint32_t topic_id, ErrorCode error);
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

    bool dispatch(uint32_t topic_id, const Buffer& data) const;
    std::map<std::string, TopicCallback> subscriptions() const;
    std::map<std::string, std::string> publishedTopicOwners() const;
    std::map<std::string, uint32_t> publishedTopicHashes() const;
    uint32_t expectedSubscriptionHash(const std::string& topic_name) const;
    void setExpectedSubscriptionHash(const std::string& topic_name, uint32_t idl_hash);

    uint32_t getTopicId(const std::string& name) const;

private:
    /*
     * @brief  单个话题的全部状态（订阅/发布/订阅者/哈希），取代此前散落的 9 张 map
     */
    struct TopicState {
        uint32_t             id;
        std::string          name;
        TopicCallback        callback;
        bool                 has_callback;
        TopicErrorCallback   error_callback;
        bool                 has_error_callback;
        std::vector<int>     tcp_subscribers;
        std::vector<ShmSubscriber> shm_subscribers;
        bool                 published;
        std::string          owner_service;
        uint32_t             published_hash;
        uint32_t             expected_subscription_hash;

        TopicState()
            : id(0), has_callback(false), has_error_callback(false),
              published(false), published_hash(0), expected_subscription_hash(0) {}
    };

    TopicState& ensureTopic(uint32_t id, const std::string& name);
    void dropNameIfUnused(const std::string& name, uint32_t id);

    std::map<uint32_t, TopicState> topics_;
    std::map<std::string, uint32_t> name_to_id_;
};

} // namespace omnibinder

#endif
