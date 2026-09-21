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

bool DeathNotifier::subscribeInstance(int subscriber_fd, const ServiceIdentity& identity)
{
    const std::string instance_key = makeServiceIdentityKey(identity);
    std::set<int>& subs = instance_to_subscribers_[instance_key];
    if (subs.find(subscriber_fd) != subs.end()) {
        OMNI_LOG_DEBUG(TAG, "fd=%d already subscribed to instance %s",
                        subscriber_fd, instance_key.c_str());
        return false;
    }

    subs.insert(subscriber_fd);
    subscriber_to_instances_[subscriber_fd].insert(instance_key);

    OMNI_LOG_INFO(TAG, "fd=%d subscribed to death of instance %s (host_id=%s, instance_id=%s)",
                    subscriber_fd, identity.name.c_str(), identity.host_id.c_str(),
                    identity.instance_id.c_str());
    return true;
}

bool DeathNotifier::unsubscribeInstance(int subscriber_fd, const ServiceIdentity& identity)
{
    const std::string instance_key = makeServiceIdentityKey(identity);
    auto iit = instance_to_subscribers_.find(instance_key);
    if (iit == instance_to_subscribers_.end()) {
        return false;
    }

    auto fd_it = iit->second.find(subscriber_fd);
    if (fd_it == iit->second.end()) {
        return false;
    }

    iit->second.erase(fd_it);
    if (iit->second.empty()) {
        instance_to_subscribers_.erase(iit);
    }
    eraseInstanceSubscription(subscriber_fd, instance_key);

    OMNI_LOG_INFO(TAG, "fd=%d unsubscribed from death of instance %s",
                    subscriber_fd, instance_key.c_str());
    return true;
}

std::vector<int> DeathNotifier::notifyInstanceDeath(const ServiceIdentity& identity,
                                                    bool name_gone)
{
    std::set<int> notified;
    const std::string instance_key = makeServiceIdentityKey(identity);

    auto iit = instance_to_subscribers_.find(instance_key);
    if (iit != instance_to_subscribers_.end()) {
        notified.insert(iit->second.begin(), iit->second.end());
        instance_to_subscribers_.erase(iit);
        for (std::set<int>::iterator it = notified.begin(); it != notified.end(); ++it) {
            eraseInstanceSubscription(*it, instance_key);
        }
    }

    if (name_gone) {
        auto sit = service_to_subscribers_.find(identity.name);
        if (sit != service_to_subscribers_.end()) {
            notified.insert(sit->second.begin(), sit->second.end());
            service_to_subscribers_.erase(sit);
            for (std::set<int>::iterator it = notified.begin(); it != notified.end(); ++it) {
                eraseNameSubscription(*it, identity.name);
            }
        }
    }

    std::vector<int> result(notified.begin(), notified.end());
    OMNI_LOG_INFO(TAG, "Instance %s (host_id=%s, instance_id=%s) died, name_gone=%d, notifying %zu subscribers",
                    identity.name.c_str(), identity.host_id.c_str(), identity.instance_id.c_str(),
                    name_gone ? 1 : 0, result.size());
    return result;
}

void DeathNotifier::eraseInstanceSubscription(int fd, const std::string& instance_key)
{
    auto sub_it = subscriber_to_instances_.find(fd);
    if (sub_it == subscriber_to_instances_.end()) {
        return;
    }
    sub_it->second.erase(instance_key);
    if (sub_it->second.empty()) {
        subscriber_to_instances_.erase(sub_it);
    }
}

void DeathNotifier::eraseNameSubscription(int fd, const std::string& service_name)
{
    auto sub_it = subscriber_to_services_.find(fd);
    if (sub_it == subscriber_to_services_.end()) {
        return;
    }
    sub_it->second.erase(service_name);
    if (sub_it->second.empty()) {
        subscriber_to_services_.erase(sub_it);
    }
}

void DeathNotifier::removeSubscriber(int subscriber_fd)
{
    auto sub_it = subscriber_to_services_.find(subscriber_fd);
    if (sub_it != subscriber_to_services_.end()) {
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
    }

    auto inst_it = subscriber_to_instances_.find(subscriber_fd);
    if (inst_it != subscriber_to_instances_.end()) {
        const std::set<std::string>& keys = inst_it->second;
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            auto iit = instance_to_subscribers_.find(*it);
            if (iit != instance_to_subscribers_.end()) {
                iit->second.erase(subscriber_fd);
                if (iit->second.empty()) {
                    instance_to_subscribers_.erase(iit);
                }
            }
        }
        subscriber_to_instances_.erase(inst_it);
    }

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
