#include "death_notifier.h"
#include "omnibinder/log.h"

#define TAG "DeathNotifier"

namespace omnibinder {
DeathNotifier::DeathNotifier()
{
}

DeathNotifier::~DeathNotifier()
{
}

bool DeathNotifier::subscribe(int subscriber_fd, const std::string& target_service)
{
    // 检查是否已订阅
    auto& subs = service_to_subscribers_[target_service];
    if (subs.find(subscriber_fd) != subs.end()) {
        OMNI_LOG_DEBUG(TAG, "fd=%d already subscribed to %s",
                        subscriber_fd, target_service.c_str());
        return false;
    }

    subs.insert(subscriber_fd);
    subscriber_to_services_[subscriber_fd].insert(target_service);

    OMNI_LOG_INFO(TAG, "fd=%d subscribed to death of: %s",
                    subscriber_fd, target_service.c_str());
    return true;
}

bool DeathNotifier::unsubscribe(int subscriber_fd, const std::string& target_service)
{
    // 从 service -> subscribers 映射中移除
    auto sit = service_to_subscribers_.find(target_service);
    if (sit == service_to_subscribers_.end()) {
        return false;
    }

    auto fd_it = sit->second.find(subscriber_fd);
    if (fd_it == sit->second.end()) {
        return false;
    }

    sit->second.erase(fd_it);
    if (sit->second.empty()) {
        service_to_subscribers_.erase(sit);
    }

    // 从 subscriber -> services 映射中移除
    auto sub_it = subscriber_to_services_.find(subscriber_fd);
    if (sub_it != subscriber_to_services_.end()) {
        sub_it->second.erase(target_service);
        if (sub_it->second.empty()) {
            subscriber_to_services_.erase(sub_it);
        }
    }

    OMNI_LOG_INFO(TAG, "fd=%d unsubscribed from death of: %s",
                    subscriber_fd, target_service.c_str());
    return true;
}

std::vector<int> DeathNotifier::notify(const std::string& dead_service_name)
{
    std::vector<int> result;

    auto sit = service_to_subscribers_.find(dead_service_name);
    if (sit == service_to_subscribers_.end()) {
        return result;
    }

    // 收集全部订阅者 fd
    for (auto it = sit->second.begin(); it != sit->second.end(); ++it) {
        result.push_back(*it);
    }

    // 清理：从所有 subscriber -> services 映射中移除该死亡服务
    for (size_t i = 0; i < result.size(); ++i) {
        int fd = result[i];
        auto sub_it = subscriber_to_services_.find(fd);
        if (sub_it != subscriber_to_services_.end()) {
            sub_it->second.erase(dead_service_name);
            if (sub_it->second.empty()) {
                subscriber_to_services_.erase(sub_it);
            }
        }
    }

    // 移除死亡服务的条目
    service_to_subscribers_.erase(sit);

    OMNI_LOG_INFO(TAG, "Service %s died, notifying %zu subscribers",
                    dead_service_name.c_str(), result.size());

    return result;
}

void DeathNotifier::removeSubscriber(int subscriber_fd)
{
    auto sub_it = subscriber_to_services_.find(subscriber_fd);
    if (sub_it == subscriber_to_services_.end()) {
        return;
    }

    // 从所有 service -> subscribers 映射中移除此订阅者
    const std::set<std::string>& services = sub_it->second;
    for (auto it = services.begin(); it != services.end(); ++it) {
        auto sit = service_to_subscribers_.find(*it);
        if (sit != service_to_subscribers_.end()) {
            sit->second.erase(subscriber_fd);
            if (sit->second.empty()) {
                service_to_subscribers_.erase(sit);
            }
        }
    }

    subscriber_to_services_.erase(sub_it);

    OMNI_LOG_DEBUG(TAG, "Removed all subscriptions for fd=%d", subscriber_fd);
}

std::vector<std::string> DeathNotifier::getWatchedServices(int subscriber_fd) const
{
    std::vector<std::string> result;

    auto sub_it = subscriber_to_services_.find(subscriber_fd);
    if (sub_it != subscriber_to_services_.end()) {
        for (auto it = sub_it->second.begin(); it != sub_it->second.end(); ++it) {
            result.push_back(*it);
        }
    }

    return result;
}

size_t DeathNotifier::subscriberCount(const std::string& service_name) const
{
    auto sit = service_to_subscribers_.find(service_name);
    if (sit == service_to_subscribers_.end()) {
        return 0;
    }

    return sit->second.size();
}

} // namespace omnibinder
