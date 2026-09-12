#include "core/topic_runtime.h"

#include "omnibinder/message.h"

#include <algorithm>

namespace omnibinder {

TopicRuntime::TopicState& TopicRuntime::ensureTopic(uint32_t id, const std::string& name) {
    TopicState& state = topics_[id];
    state.id = id;
    if (!name.empty()) {
        state.name = name;
        name_to_id_[name] = id;
    }
    return state;
}

void TopicRuntime::dropNameIfUnused(const std::string& name, uint32_t id) {
    std::map<uint32_t, TopicState>::iterator it = topics_.find(id);
    if (it == topics_.end() || (!it->second.has_callback && !it->second.published)) {
        name_to_id_.erase(name);
    }
}

void TopicRuntime::rememberSubscription(const std::string& topic_name,
                                        const TopicCallback& callback,
                                        uint32_t expected_idl_hash) {
    TopicState& state = ensureTopic(fnv1a_32(topic_name), topic_name);
    state.callback = callback;
    state.has_callback = true;
    state.expected_subscription_hash = expected_idl_hash;
}

void TopicRuntime::forgetSubscription(const std::string& topic_name) {
    std::map<std::string, uint32_t>::iterator id_it = name_to_id_.find(topic_name);
    if (id_it == name_to_id_.end()) {
        return;
    }
    uint32_t id = id_it->second;

    std::map<uint32_t, TopicState>::iterator it = topics_.find(id);
    if (it != topics_.end()) {
        it->second.callback = TopicCallback();
        it->second.has_callback = false;
        it->second.error_callback = TopicErrorCallback();
        it->second.has_error_callback = false;
        it->second.expected_subscription_hash = 0;
    }
    dropNameIfUnused(topic_name, id);
}

void TopicRuntime::setErrorCallback(const std::string& topic_name, const TopicErrorCallback& cb) {
    TopicState& state = ensureTopic(fnv1a_32(topic_name), topic_name);
    state.error_callback = cb;
    state.has_error_callback = true;
}

void TopicRuntime::notifyError(uint32_t topic_id, ErrorCode error) {
    std::map<uint32_t, TopicState>::iterator it = topics_.find(topic_id);
    if (it == topics_.end() || !it->second.has_error_callback || !it->second.error_callback) {
        return;
    }

    // 拷贝回调对象再调用：回调内可能 unsubscribeTopic → forgetSubscription 清除本回调，
    // 若持迭代器/引用调用则回调后已失效（约束 2）
    TopicErrorCallback callback = it->second.error_callback;
    Buffer empty;
    callback(topic_id, error, empty);
}

void TopicRuntime::rememberPublishedTopic(const std::string& topic_name, uint32_t topic_id,
                                          const std::string& owner_service, uint32_t idl_hash) {
    TopicState& state = ensureTopic(topic_id, topic_name);
    state.published = true;
    state.owner_service = owner_service;
    state.published_hash = idl_hash;
}

void TopicRuntime::forgetPublishedTopic(const std::string& topic_name) {
    std::map<std::string, uint32_t>::iterator id_it = name_to_id_.find(topic_name);
    if (id_it == name_to_id_.end()) {
        return;
    }
    uint32_t id = id_it->second;

    std::map<uint32_t, TopicState>::iterator it = topics_.find(id);
    if (it == topics_.end() || !it->second.published) {
        return;
    }
    it->second.published = false;
    it->second.owner_service.clear();
    it->second.published_hash = 0;
    it->second.tcp_subscribers.clear();
    it->second.shm_subscribers.clear();
    dropNameIfUnused(topic_name, id);
}

void TopicRuntime::forgetPublishedTopicsByOwner(const std::string& owner_service) {
    std::vector<std::string> topic_names;
    for (std::map<uint32_t, TopicState>::iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.published && it->second.owner_service == owner_service
            && !it->second.name.empty()) {
            topic_names.push_back(it->second.name);
        }
    }
    for (size_t i = 0; i < topic_names.size(); ++i) {
        forgetPublishedTopic(topic_names[i]);
    }
}

void TopicRuntime::addTcpSubscriber(uint32_t topic_id, int client_fd) {
    std::vector<int>& fds = ensureTopic(topic_id, std::string()).tcp_subscribers;
    if (std::find(fds.begin(), fds.end(), client_fd) == fds.end()) {
        fds.push_back(client_fd);
    }
}

void TopicRuntime::removeTcpSubscriberFd(int client_fd) {
    for (std::map<uint32_t, TopicState>::iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        std::vector<int>& fds = it->second.tcp_subscribers;
        fds.erase(std::remove(fds.begin(), fds.end(), client_fd), fds.end());
    }
}

void TopicRuntime::addShmSubscriberService(uint32_t topic_id, const std::string& service_name,
                                           uint32_t client_id) {
    std::vector<ShmSubscriber>& subscribers = ensureTopic(topic_id, std::string()).shm_subscribers;
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
    for (std::map<uint32_t, TopicState>::iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        std::vector<ShmSubscriber>& subscribers = it->second.shm_subscribers;
        subscribers.erase(
            std::remove_if(subscribers.begin(), subscribers.end(),
                           [&service_name, client_id](const ShmSubscriber& subscriber) {
                               return subscriber.service_name == service_name
                                   && subscriber.client_id == client_id;
                           }),
            subscribers.end());
    }
}

const std::vector<int>& TopicRuntime::tcpSubscribers(uint32_t topic_id) const {
    static const std::vector<int> empty;
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(topic_id);
    return it == topics_.end() ? empty : it->second.tcp_subscribers;
}

const std::vector<TopicRuntime::ShmSubscriber>& TopicRuntime::shmSubscribers(uint32_t topic_id) const {
    static const std::vector<ShmSubscriber> empty;
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(topic_id);
    return it == topics_.end() ? empty : it->second.shm_subscribers;
}

void TopicRuntime::removeTcpSubscriber(uint32_t topic_id, int client_fd) {
    std::map<uint32_t, TopicState>::iterator it = topics_.find(topic_id);
    if (it == topics_.end()) {
        return;
    }
    std::vector<int>& fds = it->second.tcp_subscribers;
    fds.erase(std::remove(fds.begin(), fds.end(), client_fd), fds.end());
}

uint32_t TopicRuntime::getTopicId(const std::string& name) const {
    std::map<std::string, uint32_t>::const_iterator id_it = name_to_id_.find(name);
    if (id_it == name_to_id_.end()) {
        return 0;
    }
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(id_it->second);
    if (it == topics_.end() || !it->second.published) {
        return 0;
    }
    return it->second.id;
}

bool TopicRuntime::dispatch(uint32_t topic_id, const Buffer& data) const {
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(topic_id);
    if (it == topics_.end() || !it->second.has_callback || !it->second.callback) {
        return false;
    }

    // 先拷贝回调再调用：回调内可能 unsubscribeTopic → forgetSubscription 清除本回调（约束 2）
    TopicCallback callback = it->second.callback;
    callback(topic_id, data);
    return true;
}

std::map<std::string, TopicCallback> TopicRuntime::subscriptions() const {
    std::map<std::string, TopicCallback> result;
    for (std::map<uint32_t, TopicState>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.has_callback && it->second.callback && !it->second.name.empty()) {
            result[it->second.name] = it->second.callback;
        }
    }
    return result;
}

std::map<std::string, std::string> TopicRuntime::publishedTopicOwners() const {
    std::map<std::string, std::string> result;
    for (std::map<uint32_t, TopicState>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.published && !it->second.name.empty()) {
            result[it->second.name] = it->second.owner_service;
        }
    }
    return result;
}

std::map<std::string, uint32_t> TopicRuntime::publishedTopicHashes() const {
    std::map<std::string, uint32_t> result;
    for (std::map<uint32_t, TopicState>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.published && !it->second.name.empty()) {
            result[it->second.name] = it->second.published_hash;
        }
    }
    return result;
}

uint32_t TopicRuntime::expectedSubscriptionHash(const std::string& topic_name) const {
    std::map<std::string, uint32_t>::const_iterator id_it = name_to_id_.find(topic_name);
    if (id_it == name_to_id_.end()) {
        return 0;
    }
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(id_it->second);
    return it == topics_.end() ? 0 : it->second.expected_subscription_hash;
}

void TopicRuntime::setExpectedSubscriptionHash(const std::string& topic_name, uint32_t idl_hash) {
    ensureTopic(fnv1a_32(topic_name), topic_name).expected_subscription_hash = idl_hash;
}

} // namespace omnibinder
