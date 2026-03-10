#include "core/topic_runtime.h"

#include "omnibinder/message.h"

#include <algorithm>

namespace omnibinder {

void TopicRuntime::rememberSubscription(const std::string& topic_name,
                                        const TopicCallback& callback) {
    callbacks_[topic_name] = callback;
    uint32_t topic_id = fnv1a_32(topic_name);
    topic_name_to_id_[topic_name] = topic_id;

    std::vector<TopicCallback>& list = callbacks_by_id_[topic_id];
    list.clear();
    list.push_back(callback);
}

void TopicRuntime::forgetSubscription(const std::string& topic_name) {
    std::map<std::string, uint32_t>::iterator id_it = topic_name_to_id_.find(topic_name);
    if (id_it != topic_name_to_id_.end()) {
        callbacks_by_id_.erase(id_it->second);
    }
    callbacks_.erase(topic_name);
    topic_name_to_id_.erase(topic_name);
}

void TopicRuntime::rememberPublishedTopic(const std::string& topic_name, uint32_t topic_id,
                                          const std::string& owner_service) {
    published_topics[topic_name] = topic_id;
    published_topic_owners_[topic_name] = owner_service;
}

void TopicRuntime::forgetPublishedTopic(const std::string& topic_name) {
    std::map<std::string, uint32_t>::iterator it = published_topics.find(topic_name);
    if (it == published_topics.end()) {
        return;
    }
    tcp_subscribers_.erase(it->second);
    shm_subscriber_services_.erase(it->second);
    published_topics.erase(it);
    published_topic_owners_.erase(topic_name);
}

void TopicRuntime::forgetPublishedTopicsByIds(const std::vector<uint32_t>& topic_ids) {
    for (size_t i = 0; i < topic_ids.size(); ++i) {
        uint32_t topic_id = topic_ids[i];
        tcp_subscribers_.erase(topic_id);
        shm_subscriber_services_.erase(topic_id);
        for (std::map<std::string, uint32_t>::iterator it = published_topics.begin();
             it != published_topics.end();) {
            if (it->second == topic_id) {
                published_topic_owners_.erase(it->first);
                it = published_topics.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void TopicRuntime::forgetPublishedTopicsByOwner(const std::string& owner_service) {
    std::vector<std::string> topic_names;
    for (std::map<std::string, std::string>::iterator it = published_topic_owners_.begin();
         it != published_topic_owners_.end(); ++it) {
        if (it->second == owner_service) {
            topic_names.push_back(it->first);
        }
    }
    for (size_t i = 0; i < topic_names.size(); ++i) {
        forgetPublishedTopic(topic_names[i]);
    }
}

void TopicRuntime::addTcpSubscriber(uint32_t topic_id, int client_fd) {
    std::vector<int>& fds = tcp_subscribers_[topic_id];
    if (std::find(fds.begin(), fds.end(), client_fd) == fds.end()) {
        fds.push_back(client_fd);
    }
}

void TopicRuntime::removeTcpSubscriberFd(int client_fd) {
    for (std::map<uint32_t, std::vector<int> >::iterator it = tcp_subscribers_.begin();
         it != tcp_subscribers_.end(); ++it) {
        std::vector<int>& fds = it->second;
        fds.erase(std::remove(fds.begin(), fds.end(), client_fd), fds.end());
    }
}

void TopicRuntime::addShmSubscriberService(uint32_t topic_id, const std::string& service_name) {
    std::vector<std::string>& services = shm_subscriber_services_[topic_id];
    if (std::find(services.begin(), services.end(), service_name) == services.end()) {
        services.push_back(service_name);
    }
}

std::vector<int>* TopicRuntime::tcpSubscribers(uint32_t topic_id) {
    std::map<uint32_t, std::vector<int> >::iterator it = tcp_subscribers_.find(topic_id);
    return it == tcp_subscribers_.end() ? NULL : &it->second;
}

std::vector<std::string>* TopicRuntime::shmSubscriberServices(uint32_t topic_id) {
    std::map<uint32_t, std::vector<std::string> >::iterator it = shm_subscriber_services_.find(topic_id);
    return it == shm_subscriber_services_.end() ? NULL : &it->second;
}

bool TopicRuntime::dispatch(uint32_t topic_id, const Buffer& data) const {
    std::map<uint32_t, std::vector<TopicCallback> >::const_iterator it = callbacks_by_id_.find(topic_id);
    if (it == callbacks_by_id_.end()) {
        return false;
    }

    bool dispatched = false;
    for (size_t i = 0; i < it->second.size(); ++i) {
        if (it->second[i]) {
            it->second[i](topic_id, data);
            dispatched = true;
        }
    }
    return dispatched;
}

std::map<std::string, TopicCallback> TopicRuntime::subscriptions() const {
    return callbacks_;
}

std::map<std::string, std::string> TopicRuntime::publishedTopicOwners() const {
    return published_topic_owners_;
}

} // namespace omnibinder
