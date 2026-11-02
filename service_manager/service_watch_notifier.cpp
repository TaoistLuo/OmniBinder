#include "service_watch_notifier.h"

#include "omnibinder/log.h"

#define TAG "ServiceWatchNotifier"

namespace omnibinder {

ServiceWatchNotifier::ServiceWatchNotifier() {}

ServiceWatchNotifier::~ServiceWatchNotifier() {}

bool ServiceWatchNotifier::subscribe(int subscriber_fd, uint64_t watch_id,
                                     const std::vector<ServiceWatchFilter>& filters)
{
    if (subscriber_fd < 0 || watch_id == 0 || filters.empty()) {
        return false;
    }
    const WatchKey key(subscriber_fd, watch_id);
    if (groups_.find(key) != groups_.end()) {
        return false;
    }

    ServiceWatchGroup group;
    group.subscriber_fd = subscriber_fd;
    group.watch_id = watch_id;
    group.filters = filters;
    groups_[key] = group;
    subscriber_index_[subscriber_fd].insert(watch_id);

    for (size_t i = 0; i < filters.size(); ++i) {
        service_index_[filters[i].service_name].insert(key);
    }
    OMNI_LOG_INFO(TAG, "fd=%d subscribed watch_id=%llu filters=%zu",
                  subscriber_fd, (unsigned long long)watch_id, filters.size());
    return true;
}

bool ServiceWatchNotifier::unsubscribe(int subscriber_fd, uint64_t watch_id)
{
    std::map<WatchKey, ServiceWatchGroup>::iterator it =
        groups_.find(WatchKey(subscriber_fd, watch_id));
    if (it == groups_.end()) {
        return false;
    }
    eraseGroup(it);
    OMNI_LOG_INFO(TAG, "fd=%d unsubscribed watch_id=%llu",
                  subscriber_fd, (unsigned long long)watch_id);
    return true;
}

void ServiceWatchNotifier::removeSubscriber(int subscriber_fd)
{
    std::map<int, std::set<uint64_t> >::iterator sit = subscriber_index_.find(subscriber_fd);
    if (sit == subscriber_index_.end()) {
        return;
    }
    const std::set<uint64_t> ids = sit->second;
    for (std::set<uint64_t>::const_iterator it = ids.begin(); it != ids.end(); ++it) {
        std::map<WatchKey, ServiceWatchGroup>::iterator git =
            groups_.find(WatchKey(subscriber_fd, *it));
        if (git != groups_.end()) {
            eraseGroup(git);
        }
    }
    OMNI_LOG_DEBUG(TAG, "Removed all watch subscriptions for fd=%d", subscriber_fd);
}

std::vector<ServiceWatchGroup> ServiceWatchNotifier::matching(
    const ServiceIdentity& identity) const
{
    std::vector<ServiceWatchGroup> result;
    std::map<std::string, std::set<WatchKey> >::const_iterator sit =
        service_index_.find(identity.name);
    if (sit == service_index_.end()) {
        return result;
    }

    for (std::set<WatchKey>::const_iterator it = sit->second.begin();
         it != sit->second.end(); ++it) {
        std::map<WatchKey, ServiceWatchGroup>::const_iterator git = groups_.find(*it);
        if (git == groups_.end()) {
            continue;
        }
        const ServiceWatchGroup& group = git->second;
        for (size_t i = 0; i < group.filters.size(); ++i) {
            if (matches(group.filters[i], identity)) {
                result.push_back(group);
                break;
            }
        }
    }
    return result;
}

bool ServiceWatchNotifier::matches(const ServiceWatchFilter& filter,
                                   const ServiceIdentity& identity)
{
    if (filter.service_name != identity.name) {
        return false;
    }
    // 逻辑身份 = (service_name, instance_id)；host_id 只是部署位置元数据。
    if (!filter.instance_id.empty() && filter.instance_id != identity.instance_id) {
        return false;
    }
    return true;
}

bool ServiceWatchNotifier::overlaps(const ServiceWatchFilter& a,
                                    const ServiceWatchFilter& b)
{
    if (a.service_name != b.service_name) {
        return false;
    }
    // host_id 不参与逻辑匹配；仅 instance_id 约束可能互斥。
    if (!a.instance_id.empty() && !b.instance_id.empty()
        && a.instance_id != b.instance_id) {
        return false;
    }
    return true;
}

void ServiceWatchNotifier::eraseGroup(
    std::map<WatchKey, ServiceWatchGroup>::iterator it)
{
    const WatchKey key = it->first;
    const ServiceWatchGroup group = it->second;
    for (size_t i = 0; i < group.filters.size(); ++i) {
        std::map<std::string, std::set<WatchKey> >::iterator sit =
            service_index_.find(group.filters[i].service_name);
        if (sit != service_index_.end()) {
            sit->second.erase(key);
            if (sit->second.empty()) {
                service_index_.erase(sit);
            }
        }
    }

    std::map<int, std::set<uint64_t> >::iterator sub_it =
        subscriber_index_.find(group.subscriber_fd);
    if (sub_it != subscriber_index_.end()) {
        sub_it->second.erase(group.watch_id);
        if (sub_it->second.empty()) {
            subscriber_index_.erase(sub_it);
        }
    }
    groups_.erase(it);
}

} // namespace omnibinder
