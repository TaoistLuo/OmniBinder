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
        error_callbacks_.erase(id_it->second);
    }
    callbacks_.erase(topic_name);
    topic_name_to_id_.erase(topic_name);
}

void TopicRuntime::setErrorCallback(const std::string& topic_name, const TopicErrorCallback& cb) {
    uint32_t topic_id = fnv1a_32(topic_name);
    error_callbacks_[topic_id] = cb;
}

void TopicRuntime::notifyError(uint32_t topic_id, ErrorCode error) {
    Buffer empty;
    std::map<uint32_t, TopicErrorCallback>::iterator it = error_callbacks_.find(topic_id);
    if (it != error_callbacks_.end()) {
        it->second(topic_id, error, empty);
    }
}

void TopicRuntime::rememberPublishedTopic(const std::string& topic_name, uint32_t topic_id,
                                          const std::string& owner_service) {
    published_topics_[topic_name] = topic_id;
    published_topic_owners_[topic_name] = owner_service;
}

void TopicRuntime::forgetPublishedTopic(const std::string& topic_name) {
    std::map<std::string, uint32_t>::iterator it = published_topics_.find(topic_name);
    if (it == published_topics_.end()) {
        return;
    }
    tcp_subscribers_.erase(it->second);
    shm_subscribers_.erase(it->second);
    published_topics_.erase(it);
    published_topic_owners_.erase(topic_name);
}

void TopicRuntime::forgetPublishedTopicsByIds(const std::vector<uint32_t>& topic_ids) {
    for (size_t i = 0; i < topic_ids.size(); ++i) {
        uint32_t topic_id = topic_ids[i];
        tcp_subscribers_.erase(topic_id);
        shm_subscribers_.erase(topic_id);
        for (std::map<std::string, uint32_t>::iterator it = published_topics_.begin();
             it != published_topics_.end();) {
            if (it->second == topic_id) {
                published_topic_owners_.erase(it->first);
                it = published_topics_.erase(it);
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

void TopicRuntime::addShmSubscriberService(uint32_t topic_id, const std::string& service_name,
                                           uint32_t client_id) {
    std::vector<ShmSubscriber>& subscribers = shm_subscribers_[topic_id];
    for (size_t i = 0; i < subscribers.size(); ++i) {
        if (subscribers[i].service_name == service_name && subscribers[i].client_id == client_id) {
            return;
        }
    }
    ShmSubscriber subscriber;
    subscriber.service_name = service_name;
    subscriber.client_id = client_id;
    subscribers.push_back(subscriber);
}

void TopicRuntime::removeShmSubscriberService(const std::string& service_name,
                                               uint32_t client_id) {
    for (std::map<uint32_t, std::vector<ShmSubscriber> >::iterator it = shm_subscribers_.begin();
         it != shm_subscribers_.end();) {
        std::vector<ShmSubscriber>& subscribers = it->second;
        for (std::vector<ShmSubscriber>::iterator subscriber = subscribers.begin();
             subscriber != subscribers.end();) {
            if (subscriber->service_name == service_name && subscriber->client_id == client_id) {
                subscriber = subscribers.erase(subscriber);
            } else {
                ++subscriber;
            }
        }
        if (subscribers.empty()) {
            it = shm_subscribers_.erase(it);
        } else {
            ++it;
        }
    }
}

const std::vector<int>& TopicRuntime::tcpSubscribers(uint32_t topic_id) const {
    static const std::vector<int> empty;
    std::map<uint32_t, std::vector<int> >::const_iterator it = tcp_subscribers_.find(topic_id);
    return it == tcp_subscribers_.end() ? empty : it->second;
}

const std::vector<TopicRuntime::ShmSubscriber>& TopicRuntime::shmSubscribers(uint32_t topic_id) const {
    static const std::vector<ShmSubscriber> empty;
    std::map<uint32_t, std::vector<ShmSubscriber> >::const_iterator it = shm_subscribers_.find(topic_id);
    return it == shm_subscribers_.end() ? empty : it->second;
}

void TopicRuntime::removeTcpSubscriber(uint32_t topic_id, int client_fd) {
    std::map<uint32_t, std::vector<int> >::iterator it = tcp_subscribers_.find(topic_id);
    if (it == tcp_subscribers_.end()) return;
    std::vector<int>& fds = it->second;
    for (std::vector<int>::iterator fd_it = fds.begin(); fd_it != fds.end(); ++fd_it) {
        if (*fd_it == client_fd) {
            fds.erase(fd_it);
            return;
        }
    }
}

bool TopicRuntime::isTopicPublished(const std::string& name) const {
    return published_topics_.find(name) != published_topics_.end();
}

uint32_t TopicRuntime::getTopicId(const std::string& name) const {
    std::map<std::string, uint32_t>::const_iterator it = published_topics_.find(name);
    return it == published_topics_.end() ? 0 : it->second;
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
