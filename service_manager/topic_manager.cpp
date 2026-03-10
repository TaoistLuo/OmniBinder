#include "topic_manager.h"
#include "omnibinder/log.h"

#define TAG "TopicManager"

namespace omnibinder {

TopicManager::TopicManager()
{
}

TopicManager::~TopicManager()
{
}

bool TopicManager::registerPublisher(const std::string& topic,
                                     const ServiceInfo& publisher_info,
                                     int publisher_fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (topic.empty()) {
        OMNI_LOG_ERROR(TAG, "Cannot register publisher with empty topic name");
        return false;
    }

    if (topic.length() > MAX_TOPIC_NAME_LENGTH) {
        OMNI_LOG_ERROR(TAG, "Topic name too long: %s", topic.c_str());
        return false;
    }

    auto it = topics_.find(topic);
    if (it != topics_.end() && it->second.publisher_fd >= 0) {
        OMNI_LOG_WARN(TAG, "Topic %s already has a publisher (fd=%d)",
                       topic.c_str(), it->second.publisher_fd);
        return false;
    }

    if (it == topics_.end()) {
        TopicEntry entry;
        entry.topic_name = topic;
        entry.publisher_info = publisher_info;
        entry.publisher_fd = publisher_fd;
        topics_[topic] = entry;
    } else {
        it->second.publisher_info = publisher_info;
        it->second.publisher_fd = publisher_fd;
    }

    fd_publications_[publisher_fd].insert(topic);

    OMNI_LOG_INFO(TAG, "Publisher registered for topic: %s (fd=%d, host=%s, port=%u)",
                    topic.c_str(), publisher_fd,
                    publisher_info.host.c_str(), publisher_info.port);
    return true;
}

bool TopicManager::removePublisher(const std::string& topic)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = topics_.find(topic);
    if (it == topics_.end()) {
        return false;
    }

    int pub_fd = it->second.publisher_fd;
    if (pub_fd < 0) {
        return false;
    }

    // Remove from fd_publications_ reverse index
    auto fp_it = fd_publications_.find(pub_fd);
    if (fp_it != fd_publications_.end()) {
        fp_it->second.erase(topic);
        if (fp_it->second.empty()) {
            fd_publications_.erase(fp_it);
        }
    }

    // If there are no subscribers either, remove the entire topic entry
    if (it->second.subscriber_fds.empty()) {
        topics_.erase(it);
    } else {
        // Keep the entry but clear the publisher
        it->second.publisher_fd = -1;
        it->second.publisher_info = ServiceInfo();
    }

    OMNI_LOG_INFO(TAG, "Publisher removed for topic: %s", topic.c_str());
    return true;
}

bool TopicManager::removePublisher(const std::string& topic, int publisher_fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = topics_.find(topic);
    if (it == topics_.end() || it->second.publisher_fd != publisher_fd) {
        return false;
    }

    int pub_fd = it->second.publisher_fd;
    auto fp_it = fd_publications_.find(pub_fd);
    if (fp_it != fd_publications_.end()) {
        fp_it->second.erase(topic);
        if (fp_it->second.empty()) {
            fd_publications_.erase(fp_it);
        }
    }

    if (it->second.subscriber_fds.empty()) {
        topics_.erase(it);
    } else {
        it->second.publisher_fd = -1;
        it->second.publisher_info = ServiceInfo();
    }

    OMNI_LOG_INFO(TAG, "Publisher removed for topic: %s (fd=%d)", topic.c_str(), publisher_fd);
    return true;
}

bool TopicManager::isPublisherOwner(const std::string& topic, int publisher_fd) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = topics_.find(topic);
    return it != topics_.end() && it->second.publisher_fd == publisher_fd;
}

bool TopicManager::addSubscriber(const std::string& topic, int subscriber_fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Create topic entry if it doesn't exist
    TopicEntry& entry = topics_[topic];
    if (entry.topic_name.empty()) {
        entry.topic_name = topic;
        entry.publisher_fd = -1;
    }

    if (entry.subscriber_fds.find(subscriber_fd) != entry.subscriber_fds.end()) {
        OMNI_LOG_DEBUG(TAG, "fd=%d already subscribed to topic %s",
                        subscriber_fd, topic.c_str());
        return false;
    }

    entry.subscriber_fds.insert(subscriber_fd);
    fd_subscriptions_[subscriber_fd].insert(topic);

    OMNI_LOG_INFO(TAG, "Subscriber fd=%d added to topic: %s", subscriber_fd, topic.c_str());
    return true;
}

bool TopicManager::removeSubscriber(const std::string& topic, int subscriber_fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = topics_.find(topic);
    if (it == topics_.end()) {
        return false;
    }

    auto fd_it = it->second.subscriber_fds.find(subscriber_fd);
    if (fd_it == it->second.subscriber_fds.end()) {
        return false;
    }

    it->second.subscriber_fds.erase(fd_it);

    // Remove from reverse index
    auto fs_it = fd_subscriptions_.find(subscriber_fd);
    if (fs_it != fd_subscriptions_.end()) {
        fs_it->second.erase(topic);
        if (fs_it->second.empty()) {
            fd_subscriptions_.erase(fs_it);
        }
    }

    // If no publisher and no subscribers, remove the topic entirely
    if (it->second.publisher_fd < 0 && it->second.subscriber_fds.empty()) {
        topics_.erase(it);
    }

    OMNI_LOG_INFO(TAG, "Subscriber fd=%d removed from topic: %s", subscriber_fd, topic.c_str());
    return true;
}

std::vector<std::string> TopicManager::removeByFd(int fd)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> removed_publications;

    // Remove as publisher
    auto fp_it = fd_publications_.find(fd);
    if (fp_it != fd_publications_.end()) {
        // Copy the set since we'll modify topics_ during iteration
        std::set<std::string> pub_topics = fp_it->second;
        fd_publications_.erase(fp_it);

        for (auto it = pub_topics.begin(); it != pub_topics.end(); ++it) {
            const std::string& topic = *it;
            removed_publications.push_back(topic);

            auto tit = topics_.find(topic);
            if (tit != topics_.end()) {
                if (tit->second.subscriber_fds.empty()) {
                    topics_.erase(tit);
                } else {
                    tit->second.publisher_fd = -1;
                    tit->second.publisher_info = ServiceInfo();
                }
            }

            OMNI_LOG_INFO(TAG, "Publisher fd=%d removed from topic: %s (fd closed)",
                           fd, topic.c_str());
        }
    }

    // Remove as subscriber
    auto fs_it = fd_subscriptions_.find(fd);
    if (fs_it != fd_subscriptions_.end()) {
        std::set<std::string> sub_topics = fs_it->second;
        fd_subscriptions_.erase(fs_it);

        for (auto it = sub_topics.begin(); it != sub_topics.end(); ++it) {
            const std::string& topic = *it;
            auto tit = topics_.find(topic);
            if (tit != topics_.end()) {
                tit->second.subscriber_fds.erase(fd);

                // Clean up empty topic entries
                if (tit->second.publisher_fd < 0 && tit->second.subscriber_fds.empty()) {
                    topics_.erase(tit);
                }
            }

            OMNI_LOG_DEBUG(TAG, "Subscriber fd=%d removed from topic: %s (fd closed)",
                            fd, topic.c_str());
        }
    }

    return removed_publications;
}

std::vector<int> TopicManager::getSubscribers(const std::string& topic) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<int> result;

    auto it = topics_.find(topic);
    if (it != topics_.end()) {
        for (auto sit = it->second.subscriber_fds.begin();
             sit != it->second.subscriber_fds.end(); ++sit) {
            result.push_back(*sit);
        }
    }

    return result;
}

bool TopicManager::getPublisher(const std::string& topic, ServiceInfo& publisher_info) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = topics_.find(topic);
    if (it == topics_.end() || it->second.publisher_fd < 0) {
        return false;
    }

    publisher_info = it->second.publisher_info;
    return true;
}

bool TopicManager::hasPublisher(const std::string& topic) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = topics_.find(topic);
    return it != topics_.end() && it->second.publisher_fd >= 0;
}

std::vector<std::string> TopicManager::listTopics() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> result;
    result.reserve(topics_.size());

    for (auto it = topics_.begin(); it != topics_.end(); ++it) {
        result.push_back(it->first);
    }

    return result;
}

} // namespace omnibinder
