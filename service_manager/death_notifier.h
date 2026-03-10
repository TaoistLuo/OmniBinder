/**************************************************************************************************
 * @file        death_notifier.h
 * @brief       服务死亡通知管理器
 * @details     管理服务死亡通知的订阅关系。客户端可订阅目标服务的死亡事件，
 *              当目标服务断开或心跳超时时，DeathNotifier 返回所有需要通知的
 *              订阅者 fd 列表。内部维护双向映射（服务→订阅者、订阅者→服务），
 *              支持按 fd 批量清理断开的订阅者。线程安全。
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
#ifndef OMNIBINDER_DEATH_NOTIFIER_H
#define OMNIBINDER_DEATH_NOTIFIER_H

#include <string>
#include <map>
#include <set>
#include <vector>
#include <mutex>

namespace omnibinder {

// ============================================================
// DeathNotifier - Manages death notification subscriptions
//
// Services can subscribe to be notified when a target service
// dies (disconnects or times out). When a service dies, all
// subscribers are notified via their control connection fd.
// ============================================================
class DeathNotifier {
public:
    DeathNotifier();
    ~DeathNotifier();

    // Disable copy
    DeathNotifier(const DeathNotifier&) = delete;
    DeathNotifier& operator=(const DeathNotifier&) = delete;

    // Subscribe: subscriber_fd wants to be notified when target_service dies.
    // Returns true if the subscription was added (false if already subscribed).
    bool subscribe(int subscriber_fd, const std::string& target_service);

    // Unsubscribe: subscriber_fd no longer wants death notifications for target_service.
    // Returns true if the subscription was found and removed.
    bool unsubscribe(int subscriber_fd, const std::string& target_service);

    // Notify: a service has died. Returns the list of subscriber fds that
    // should be notified. Also removes all subscriptions for the dead service.
    std::vector<int> notify(const std::string& dead_service_name);

    // Remove all subscriptions where subscriber_fd is the subscriber.
    // Called when a subscriber disconnects.
    void removeSubscriber(int subscriber_fd);

    // Get the list of services that a subscriber is watching.
    std::vector<std::string> getWatchedServices(int subscriber_fd) const;

    // Get the number of subscribers for a given service.
    size_t subscriberCount(const std::string& service_name) const;

private:
    mutable std::mutex mutex_;

    // target_service -> set of subscriber fds
    std::map<std::string, std::set<int>> service_to_subscribers_;

    // subscriber_fd -> set of target services
    std::map<int, std::set<std::string>> subscriber_to_services_;
};

} // namespace omnibinder

#endif // OMNIBINDER_DEATH_NOTIFIER_H
