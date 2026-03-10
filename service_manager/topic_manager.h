/**************************************************************************************************
 * @file        topic_manager.h
 * @brief       话题发布/订阅管理器
 * @details     管理话题的发布者和订阅者关系。每个话题最多一个发布者、零或多个订阅者。
 *              ServiceManager 通过此模块将发布者的连接信息转发给订阅者，使订阅者
 *              可直连发布者接收广播数据。支持按 fd 批量清理断开连接的发布者和订阅者。
 *              线程安全。
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
#include <mutex>

namespace omnibinder {

// ============================================================
// TopicEntry - Internal storage for a topic's publisher info
// ============================================================
struct TopicEntry {
    std::string topic_name;
    ServiceInfo publisher_info;  // Publisher's connection info (host:port)
    int publisher_fd;            // Publisher's control connection fd
    std::set<int> subscriber_fds;

    TopicEntry() : publisher_fd(-1) {}
};

// ============================================================
// TopicManager - Manages topic publishers and subscribers
//
// Each topic has at most one publisher and zero or more subscribers.
// The ServiceManager uses this to route topic publisher info
// to subscribers so they can connect directly.
// ============================================================
class TopicManager {
public:
    TopicManager();
    ~TopicManager();

    // Disable copy
    TopicManager(const TopicManager&) = delete;
    TopicManager& operator=(const TopicManager&) = delete;

    // Register a publisher for a topic.
    // Returns true if successful, false if the topic already has a publisher.
    bool registerPublisher(const std::string& topic, const ServiceInfo& publisher_info, int publisher_fd);

    // Remove a publisher for a topic.
    // Returns true if the publisher was found and removed.
    bool removePublisher(const std::string& topic);

    // Remove a publisher for a topic, but only if owned by the given fd.
    // Returns true only when the topic exists and the given fd is the owner.
    bool removePublisher(const std::string& topic, int publisher_fd);

    // Check whether the given fd owns the publisher role for the topic.
    bool isPublisherOwner(const std::string& topic, int publisher_fd) const;

    // Add a subscriber to a topic.
    // Returns true if the subscriber was added (false if already subscribed).
    bool addSubscriber(const std::string& topic, int subscriber_fd);

    // Remove a subscriber from a topic.
    // Returns true if the subscriber was found and removed.
    bool removeSubscriber(const std::string& topic, int subscriber_fd);

    // Remove all topics and subscriptions associated with a given fd.
    // This handles both publishers and subscribers.
    // Returns the list of topic names where this fd was the publisher.
    std::vector<std::string> removeByFd(int fd);

    // Get all subscriber fds for a topic.
    std::vector<int> getSubscribers(const std::string& topic) const;

    // Get the publisher info for a topic.
    // Returns true if the topic has a publisher.
    bool getPublisher(const std::string& topic, ServiceInfo& publisher_info) const;

    // Check if a topic has a publisher.
    bool hasPublisher(const std::string& topic) const;

    // Get all topic names.
    std::vector<std::string> listTopics() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, TopicEntry> topics_;

    // Reverse index: fd -> set of topics where this fd is a subscriber
    std::map<int, std::set<std::string>> fd_subscriptions_;

    // Reverse index: fd -> set of topics where this fd is the publisher
    std::map<int, std::set<std::string>> fd_publications_;
};

} // namespace omnibinder

#endif // OMNIBINDER_TOPIC_MANAGER_H
