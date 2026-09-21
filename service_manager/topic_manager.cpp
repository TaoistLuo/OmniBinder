#include "topic_manager.h"
#include "omnibinder/log.h"

#include <algorithm>
#include <sstream>

#define TAG "TopicManager"

namespace omnibinder {

std::string TopicSubscriptionFilter::key() const
{
    std::ostringstream os;
    if (mode == TopicSubscribeMode::INSTANCE) {
        os << 'I' << '|' << publisher_service.size() << '|' << publisher_service
           << host_id.size() << '|' << host_id << instance_id;
    } else if (mode == TopicSubscribeMode::SERVICE) {
        os << 'S' << '|' << publisher_service.size() << '|' << publisher_service;
    } else {
        os << 'T';
    }
    return os.str();
}

bool TopicSubscriptionFilter::matches(const ServiceInfo& publisher) const
{
    if (mode == TopicSubscribeMode::TOPIC_ONLY) {
        return true;
    }
    if (publisher.name != publisher_service) {
        return false;
    }
    if (mode == TopicSubscribeMode::INSTANCE) {
        return publisher.host_id == host_id && publisher.instance_id == instance_id;
    }
    return true;
}

TopicManager::TopicManager()
    : total_subscriptions_(0)
{
}

TopicManager::~TopicManager()
{
}

std::string TopicManager::entryKey(const std::string& service_name,
                                   const std::string& topic_name) const
{
    // 以服务名字节长度做前缀，保证 (service_name, topic_name) 的编码单射：
    // 服务名/主题名即使包含任意字节（含分隔符）也不会跨对碰撞。
    // 空服务名（TOPIC_ONLY 订阅条目）编码为 "0:" + 主题名。
    std::ostringstream os;
    os << service_name.size() << ':' << service_name << topic_name;
    return os.str();
}

void TopicManager::releaseTopicNameRef(const std::string& topic_name)
{
    std::map<std::string, size_t>::iterator it = topic_name_refs_.find(topic_name);
    if (it == topic_name_refs_.end()) {
        return;
    }
    if (it->second > 1) {
        --it->second;
    } else {
        topic_name_refs_.erase(it);
    }
}

std::map<std::string, TopicEntry>::iterator
TopicManager::eraseEntryIfUnused(std::map<std::string, TopicEntry>::iterator it)
{
    if (it->second.publishers.empty() && it->second.subscribers.empty()) {
        releaseTopicNameRef(it->second.topic_name);
        return topics_.erase(it);
    }
    return ++it;
}

bool TopicManager::removePublisherSlots(const std::string& topic_name, int publisher_fd,
                                        bool filter_fd)
{
    bool any_removed = false;
    for (std::map<std::string, TopicEntry>::iterator it = topics_.begin();
         it != topics_.end();) {
        if (it->second.topic_name != topic_name) {
            ++it;
            continue;
        }

        for (std::map<std::string, PublisherSlot>::iterator pit =
                 it->second.publishers.begin();
             pit != it->second.publishers.end();) {
            if (filter_fd && pit->second.fd != publisher_fd) {
                ++pit;
                continue;
            }
            std::map<int, std::set<std::string> >::iterator fpit =
                fd_publications_.find(pit->second.fd);
            if (fpit != fd_publications_.end()) {
                fpit->second.erase(it->first);
                if (fpit->second.empty()) {
                    fd_publications_.erase(fpit);
                }
            }
            pit = it->second.publishers.erase(pit);
            any_removed = true;
        }
        it = eraseEntryIfUnused(it);
    }
    return any_removed;
}

bool TopicManager::registerPublisher(const std::string& topic,
                                     const ServiceInfo& publisher_info,
                                     int publisher_fd,
                                     uint32_t idl_hash)
{
    if (topic.empty()) {
        OMNI_LOG_ERROR(TAG, "Cannot register publisher with empty topic name");
        return false;
    }
    if (topic.length() > MAX_TOPIC_NAME_LENGTH) {
        OMNI_LOG_ERROR(TAG, "Topic name too long: %s", topic.c_str());
        return false;
    }
    if (publisher_info.name.empty()
        || publisher_info.name.length() > MAX_SERVICE_NAME_LENGTH) {
        return false;
    }

    const std::string instance_key =
        makeServiceInstanceKey(publisher_info.host_id, publisher_info.instance_id);
    const std::string ekey = entryKey(publisher_info.name, topic);

    std::map<std::string, TopicEntry>::iterator it = topics_.find(ekey);
    const bool new_entry = (it == topics_.end());

    if (new_entry && topic_name_refs_.find(topic) == topic_name_refs_.end()
        && topic_name_refs_.size() >= MAX_TOPICS_GLOBAL) {
        return false;
    }

    // 每连接发布数上限：新增条目 key 才计数
    std::map<int, std::set<std::string> >::iterator fpit = fd_publications_.find(publisher_fd);
    const bool new_publication_for_fd =
        (fpit == fd_publications_.end() || fpit->second.find(ekey) == fpit->second.end());
    if (new_publication_for_fd && fpit != fd_publications_.end()
        && fpit->second.size() >= MAX_TOPIC_PUBLICATIONS_PER_CONNECTION) {
        return false;
    }

    // 每服务发布话题数/字节上限：当前话题计一次，其余该服务已发布话题累加
    size_t service_publications = 1u;
    size_t service_topic_bytes = topic.size();
    for (std::map<std::string, TopicEntry>::iterator t = topics_.begin();
         t != topics_.end(); ++t) {
        if (t == it || t->second.service_name != publisher_info.name
            || t->second.publishers.empty()) {
            continue;
        }
        ++service_publications;
        service_topic_bytes += t->second.topic_name.size();
    }
    if (service_publications > MAX_PUBLISHED_TOPICS
        || service_topic_bytes > MAX_PUBLISHED_TOPICS_BYTES) {
        return false;
    }

    if (new_entry) {
        TopicEntry entry;
        entry.service_name = publisher_info.name;
        entry.topic_name = topic;
        it = topics_.insert(std::make_pair(ekey, entry)).first;
        topic_name_refs_[topic]++;
    }

    PublisherSlot slot;
    slot.fd = publisher_fd;
    slot.info = publisher_info;
    slot.idl_hash = idl_hash;
    it->second.publishers[instance_key] = slot;

    fd_publications_[publisher_fd].insert(ekey);

    OMNI_LOG_INFO(TAG,
                  "Publisher registered for service=%s topic=%s (fd=%d, host=%s, port=%u, "
                  "instance=%s)",
                  publisher_info.name.c_str(), topic.c_str(), publisher_fd,
                  publisher_info.host.c_str(), publisher_info.port,
                  publisher_info.instance_id.c_str());
    return true;
}

bool TopicManager::removePublisher(const std::string& topic)
{
    if (topic.empty() || topic.length() > MAX_TOPIC_NAME_LENGTH) {
        return false;
    }
    return removePublisherSlots(topic, -1, false);
}

bool TopicManager::removePublisher(const std::string& topic, int publisher_fd)
{
    if (topic.empty() || topic.length() > MAX_TOPIC_NAME_LENGTH) {
        return false;
    }
    return removePublisherSlots(topic, publisher_fd, true);
}

bool TopicManager::isPublisherOwner(const std::string& topic, int publisher_fd) const
{
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic) {
            continue;
        }
        for (std::map<std::string, PublisherSlot>::const_iterator pit =
                 it->second.publishers.begin();
             pit != it->second.publishers.end(); ++pit) {
            if (pit->second.fd == publisher_fd) {
                return true;
            }
        }
    }
    return false;
}

bool TopicManager::addSubscriber(const std::string& topic, int subscriber_fd)
{
    return addSubscriber(topic, TopicSubscriptionFilter(), subscriber_fd);
}

bool TopicManager::addSubscriber(const std::string& topic,
                                 const TopicSubscriptionFilter& selector,
                                 int subscriber_fd)
{
    if (topic.empty() || topic.length() > MAX_TOPIC_NAME_LENGTH) {
        return false;
    }
    if (selector.publisher_service.length() > MAX_SERVICE_NAME_LENGTH) {
        return false;
    }

    std::map<int, size_t>::iterator cit = fd_subscription_counts_.find(subscriber_fd);
    const size_t fd_count = (cit == fd_subscription_counts_.end()) ? 0u : cit->second;
    if (fd_count >= MAX_TOPIC_SUBSCRIPTIONS_PER_CONNECTION) {
        return false;
    }
    if (total_subscriptions_ >= MAX_TOPIC_SUBSCRIPTIONS_GLOBAL) {
        return false;
    }

    const std::string ekey = entryKey(selector.publisher_service, topic);
    std::map<std::string, TopicEntry>::iterator it = topics_.find(ekey);
    if (it == topics_.end()
        && topic_name_refs_.find(topic) == topic_name_refs_.end()
        && topic_name_refs_.size() >= MAX_TOPICS_GLOBAL) {
        return false;
    }

    if (it == topics_.end()) {
        TopicEntry entry;
        entry.service_name = selector.publisher_service;
        entry.topic_name = topic;
        it = topics_.insert(std::make_pair(ekey, entry)).first;
        topic_name_refs_[topic]++;
    }

    const std::string selkey = selector.key();
    std::map<int, std::map<std::string, TopicSubscriptionFilter> >::iterator subit =
        it->second.subscribers.find(subscriber_fd);
    if (subit != it->second.subscribers.end()
        && subit->second.find(selkey) != subit->second.end()) {
        return false;
    }
    if (subit == it->second.subscribers.end()
        && it->second.subscribers.size() >= MAX_SUBSCRIBERS_PER_TOPIC) {
        return false;
    }

    it->second.subscribers[subscriber_fd][selkey] = selector;
    fd_subscriptions_[subscriber_fd].insert(ekey);
    fd_subscription_counts_[subscriber_fd] = fd_count + 1;
    ++total_subscriptions_;

    OMNI_LOG_INFO(TAG, "Subscriber fd=%d added to service=%s topic=%s (mode=%u)",
                  subscriber_fd, selector.publisher_service.c_str(), topic.c_str(),
                  static_cast<unsigned>(selector.mode));
    return true;
}

bool TopicManager::removeSubscriber(const std::string& topic, int subscriber_fd)
{
    if (topic.empty() || topic.length() > MAX_TOPIC_NAME_LENGTH) {
        return false;
    }

    bool any_removed = false;
    for (std::map<std::string, TopicEntry>::iterator it = topics_.begin();
         it != topics_.end();) {
        if (it->second.topic_name != topic) {
            ++it;
            continue;
        }
        std::map<int, std::map<std::string, TopicSubscriptionFilter> >::iterator subit =
            it->second.subscribers.find(subscriber_fd);
        if (subit == it->second.subscribers.end()) {
            ++it;
            continue;
        }

        const size_t removed = subit->second.size();
        total_subscriptions_ -= removed;
        it->second.subscribers.erase(subit);
        any_removed = true;

        std::map<int, std::set<std::string> >::iterator fsit =
            fd_subscriptions_.find(subscriber_fd);
        if (fsit != fd_subscriptions_.end()) {
            fsit->second.erase(it->first);
            if (fsit->second.empty()) {
                fd_subscriptions_.erase(fsit);
            }
        }
        std::map<int, size_t>::iterator cit = fd_subscription_counts_.find(subscriber_fd);
        if (cit != fd_subscription_counts_.end()) {
            if (cit->second > removed) {
                cit->second -= removed;
            } else {
                fd_subscription_counts_.erase(cit);
            }
        }

        it = eraseEntryIfUnused(it);
    }
    return any_removed;
}

bool TopicManager::removeSubscriber(const std::string& topic,
                                    const TopicSubscriptionFilter& selector,
                                    int subscriber_fd)
{
    if (topic.empty() || topic.length() > MAX_TOPIC_NAME_LENGTH) {
        return false;
    }

    const std::string ekey = entryKey(selector.publisher_service, topic);
    std::map<std::string, TopicEntry>::iterator it = topics_.find(ekey);
    if (it == topics_.end()) {
        return false;
    }

    std::map<int, std::map<std::string, TopicSubscriptionFilter> >::iterator subit =
        it->second.subscribers.find(subscriber_fd);
    if (subit == it->second.subscribers.end()) {
        return false;
    }
    std::map<std::string, TopicSubscriptionFilter>::iterator kit =
        subit->second.find(selector.key());
    if (kit == subit->second.end()) {
        return false;
    }

    subit->second.erase(kit);
    --total_subscriptions_;
    if (subit->second.empty()) {
        it->second.subscribers.erase(subit);
    }

    std::map<int, size_t>::iterator cit = fd_subscription_counts_.find(subscriber_fd);
    if (cit != fd_subscription_counts_.end()) {
        if (cit->second > 1) {
            --cit->second;
        } else {
            fd_subscription_counts_.erase(cit);
        }
    }
    if (it->second.subscribers.find(subscriber_fd) == it->second.subscribers.end()) {
        std::map<int, std::set<std::string> >::iterator fsit =
            fd_subscriptions_.find(subscriber_fd);
        if (fsit != fd_subscriptions_.end()) {
            fsit->second.erase(ekey);
            if (fsit->second.empty()) {
                fd_subscriptions_.erase(fsit);
            }
        }
    }

    eraseEntryIfUnused(it);
    return true;
}

std::vector<std::string> TopicManager::removeByFd(int fd)
{
    std::vector<std::string> removed_publications;

    std::map<int, std::set<std::string> >::iterator fpit = fd_publications_.find(fd);
    if (fpit != fd_publications_.end()) {
        std::set<std::string> pub_keys = fpit->second;
        fd_publications_.erase(fpit);
        for (std::set<std::string>::iterator kit = pub_keys.begin();
             kit != pub_keys.end(); ++kit) {
            std::map<std::string, TopicEntry>::iterator it = topics_.find(*kit);
            if (it == topics_.end()) {
                continue;
            }
            bool removed_here = false;
            for (std::map<std::string, PublisherSlot>::iterator pit =
                     it->second.publishers.begin();
                 pit != it->second.publishers.end();) {
                if (pit->second.fd == fd) {
                    pit = it->second.publishers.erase(pit);
                    removed_here = true;
                } else {
                    ++pit;
                }
            }
            if (removed_here) {
                removed_publications.push_back(it->second.topic_name);
                OMNI_LOG_INFO(TAG, "Publisher fd=%d removed from topic: %s (fd closed)",
                              fd, it->second.topic_name.c_str());
            }
            eraseEntryIfUnused(it);
        }
    }

    std::map<int, std::set<std::string> >::iterator fsit = fd_subscriptions_.find(fd);
    if (fsit != fd_subscriptions_.end()) {
        std::set<std::string> sub_keys = fsit->second;
        fd_subscriptions_.erase(fsit);
        for (std::set<std::string>::iterator kit = sub_keys.begin();
             kit != sub_keys.end(); ++kit) {
            std::map<std::string, TopicEntry>::iterator it = topics_.find(*kit);
            if (it == topics_.end()) {
                continue;
            }
            std::map<int, std::map<std::string, TopicSubscriptionFilter> >::iterator subit =
                it->second.subscribers.find(fd);
            if (subit != it->second.subscribers.end()) {
                total_subscriptions_ -= subit->second.size();
                it->second.subscribers.erase(subit);
                OMNI_LOG_DEBUG(TAG, "Subscriber fd=%d removed from topic: %s (fd closed)",
                               fd, it->second.topic_name.c_str());
            }
            eraseEntryIfUnused(it);
        }
    }
    fd_subscription_counts_.erase(fd);

    return removed_publications;
}

std::vector<std::string> TopicManager::removePublishersByService(
    const std::string& service_name, int publisher_fd)
{
    std::vector<std::string> removed_publications;
    std::set<std::string> seen_topics;
    for (std::map<std::string, TopicEntry>::iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.service_name != service_name) {
            continue;
        }
        for (std::map<std::string, PublisherSlot>::iterator pit =
                 it->second.publishers.begin();
             pit != it->second.publishers.end();) {
            if (pit->second.fd != publisher_fd) {
                ++pit;
                continue;
            }
            std::map<int, std::set<std::string> >::iterator fpit =
                fd_publications_.find(pit->second.fd);
            if (fpit != fd_publications_.end()) {
                fpit->second.erase(it->first);
                if (fpit->second.empty()) {
                    fd_publications_.erase(fpit);
                }
            }
            pit = it->second.publishers.erase(pit);
            if (seen_topics.insert(it->second.topic_name).second) {
                removed_publications.push_back(it->second.topic_name);
            }
        }
    }
    for (std::map<std::string, TopicEntry>::iterator it = topics_.begin();
         it != topics_.end();) {
        if (it->second.service_name == service_name) {
            it = eraseEntryIfUnused(it);
        } else {
            ++it;
        }
    }
    return removed_publications;
}

std::vector<int> TopicManager::getSubscribers(const std::string& topic) const
{
    std::set<int> fds;
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic) {
            continue;
        }
        for (std::map<int, std::map<std::string, TopicSubscriptionFilter> >::const_iterator
                 sit = it->second.subscribers.begin();
             sit != it->second.subscribers.end(); ++sit) {
            fds.insert(sit->first);
        }
    }
    return std::vector<int>(fds.begin(), fds.end());
}

std::vector<int> TopicManager::getMatchingSubscribers(const std::string& topic,
                                                      const ServiceInfo& publisher) const
{
    std::set<int> fds;
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic) {
            continue;
        }
        for (std::map<int, std::map<std::string, TopicSubscriptionFilter> >::const_iterator
                 sit = it->second.subscribers.begin();
             sit != it->second.subscribers.end(); ++sit) {
            for (std::map<std::string, TopicSubscriptionFilter>::const_iterator kit =
                     sit->second.begin();
                 kit != sit->second.end(); ++kit) {
                if (kit->second.matches(publisher)) {
                    fds.insert(sit->first);
                    break;
                }
            }
        }
    }
    return std::vector<int>(fds.begin(), fds.end());
}

std::vector<TopicPublisherView> TopicManager::getMatchingPublishers(
    const std::string& topic, const TopicSubscriptionFilter& selector) const
{
    std::vector<TopicPublisherView> result;
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic) {
            continue;
        }
        if (selector.mode != TopicSubscribeMode::TOPIC_ONLY
            && it->second.service_name != selector.publisher_service) {
            continue;
        }
        for (std::map<std::string, PublisherSlot>::const_iterator pit =
                 it->second.publishers.begin();
             pit != it->second.publishers.end(); ++pit) {
            if (selector.mode == TopicSubscribeMode::INSTANCE
                && !selector.matches(pit->second.info)) {
                continue;
            }
            TopicPublisherView view;
            view.info = pit->second.info;
            view.idl_hash = pit->second.idl_hash;
            result.push_back(view);
        }
    }
    return result;
}

bool TopicManager::getPublisher(const std::string& topic, ServiceInfo& publisher_info) const
{
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic || it->second.publishers.empty()) {
            continue;
        }
        publisher_info = it->second.publishers.begin()->second.info;
        return true;
    }
    return false;
}

std::vector<std::string> TopicManager::getPublishedTopics(
    const std::string& service_name) const
{
    std::vector<std::string> result;
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.service_name == service_name && !it->second.publishers.empty()) {
            result.push_back(it->second.topic_name);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool TopicManager::hasPublisher(const std::string& topic) const
{
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name == topic && !it->second.publishers.empty()) {
            return true;
        }
    }
    return false;
}

bool TopicManager::getIdlHash(const std::string& topic, uint32_t& idl_hash) const
{
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic || it->second.publishers.empty()) {
            continue;
        }
        idl_hash = it->second.publishers.begin()->second.idl_hash;
        return true;
    }
    return false;
}

bool TopicManager::getIdlHash(const std::string& topic,
                              const TopicSubscriptionFilter& selector,
                              uint32_t& idl_hash) const
{
    for (std::map<std::string, TopicEntry>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic) {
            continue;
        }
        if (selector.mode != TopicSubscribeMode::TOPIC_ONLY
            && it->second.service_name != selector.publisher_service) {
            continue;
        }
        for (std::map<std::string, PublisherSlot>::const_iterator pit =
                 it->second.publishers.begin();
             pit != it->second.publishers.end(); ++pit) {
            if (selector.mode == TopicSubscribeMode::INSTANCE
                && !selector.matches(pit->second.info)) {
                continue;
            }
            idl_hash = pit->second.idl_hash;
            return true;
        }
    }
    return false;
}

bool TopicManager::setIdlHash(const std::string& topic, uint32_t idl_hash)
{
    for (std::map<std::string, TopicEntry>::iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.topic_name != topic || it->second.publishers.empty()) {
            continue;
        }
        it->second.publishers.begin()->second.idl_hash = idl_hash;
        return true;
    }
    return false;
}

std::vector<std::string> TopicManager::listTopics() const
{
    std::vector<std::string> result;
    result.reserve(topic_name_refs_.size());
    for (std::map<std::string, size_t>::const_iterator it = topic_name_refs_.begin();
         it != topic_name_refs_.end(); ++it) {
        result.push_back(it->first);
    }
    return result;
}

} // namespace omnibinder
