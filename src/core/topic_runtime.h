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

    void rememberSubscription(const std::string& topic_name, const TopicCallback& callback);
    void forgetSubscription(const std::string& topic_name);
    void setErrorCallback(const std::string& topic_name, const TopicErrorCallback& cb);
    void notifyError(uint32_t topic_id, ErrorCode error);
    void rememberPublishedTopic(const std::string& topic_name, uint32_t topic_id,
                                const std::string& owner_service);
    void forgetPublishedTopic(const std::string& topic_name);
    void forgetPublishedTopicsByIds(const std::vector<uint32_t>& topic_ids);
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

    bool isTopicPublished(const std::string& name) const;
    uint32_t getTopicId(const std::string& name) const;

private:
    std::map<std::string, TopicCallback> callbacks_;
    std::map<std::string, uint32_t> topic_name_to_id_;
    std::map<uint32_t, std::vector<TopicCallback> > callbacks_by_id_;
    std::map<uint32_t, std::vector<int> > tcp_subscribers_;
    std::map<uint32_t, std::vector<ShmSubscriber> > shm_subscribers_;
    std::map<std::string, std::string> published_topic_owners_;
    std::map<uint32_t, TopicErrorCallback> error_callbacks_;
    std::map<std::string, uint32_t> published_topics_;
};

} // namespace omnibinder

#endif
