#include "core/topic_runtime.h"

#include "omnibinder/message.h"

#include <algorithm>
#include <sstream>

namespace omnibinder {

std::string TopicRuntime::SubscriptionSelector::key() const
{
    std::ostringstream os;
    if (instance_level) {
        os << 'I' << publisher_service.size() << '|' << publisher_service
           << host_id.size() << '|' << host_id << instance_id;
    } else if (!publisher_service.empty()) {
        os << 'S' << publisher_service.size() << '|' << publisher_service;
    } else {
        os << 'L';
    }
    return os.str();
}

bool TopicRuntime::SubscriptionSelector::matches(const std::string& service,
                                                 const std::string& host_id,
                                                 const std::string& instance_id) const
{
    if (publisher_service.empty()) {
        return true;
    }
    if (publisher_service != service) {
        return false;
    }
    if (instance_level) {
        return this->host_id == host_id && this->instance_id == instance_id;
    }
    return true;
}

TopicRuntime::TopicState& TopicRuntime::ensureTopic(uint32_t id, const std::string& name) {
    TopicState& state = topics_[id];
    state.id = id;
    if (!name.empty()) {
        state.name = name;
        name_to_id_[name] = id;
    }
    return state;
}

TopicRuntime::Subscription& TopicRuntime::ensureSubscription(
    TopicState& state, const SubscriptionSelector& selector) {
    const std::string sub_key = selector.key();
    for (size_t i = 0; i < state.subscriptions.size(); ++i) {
        if (state.subscriptions[i].key == sub_key) {
            return state.subscriptions[i];
        }
    }
    Subscription sub;
    sub.selector = selector;
    sub.key = sub_key;
    state.subscriptions.push_back(sub);
    return state.subscriptions.back();
}

TopicRuntime::Subscription* TopicRuntime::findSubscription(TopicState& state,
                                                           const std::string& key) {
    for (size_t i = 0; i < state.subscriptions.size(); ++i) {
        if (state.subscriptions[i].key == key) {
            return &state.subscriptions[i];
        }
    }
    return NULL;
}

void TopicRuntime::dropNameIfUnused(const std::string& name, uint32_t id) {
    std::map<uint32_t, TopicState>::iterator it = topics_.find(id);
    if (it == topics_.end()) {
        name_to_id_.erase(name);
        return;
    }
    if (it->second.subscriptions.empty() && !it->second.published) {
        name_to_id_.erase(name);
    }
    eraseTopicIfUnused(it);
}

bool TopicRuntime::isTopicUnused(const TopicState& state) const {
    return state.subscriptions.empty() && !state.published
        && state.tcp_subscribers.empty() && state.shm_subscribers.empty();
}

std::map<uint32_t, TopicRuntime::TopicState>::iterator
TopicRuntime::eraseTopicIfUnused(std::map<uint32_t, TopicState>::iterator it) {
    if (!isTopicUnused(it->second)) {
        return ++it;
    }
    // 订阅/发布/回调/订阅者全部清空后擦除节点，避免 topics_ 随动态话题无界增长
    if (!it->second.name.empty()) {
        name_to_id_.erase(it->second.name);
    }
    return topics_.erase(it);
}

void TopicRuntime::rememberSubscription(const std::string& topic_name,
                                        const SubscriptionSelector& selector,
                                        const TopicSourceCallback& callback,
                                        uint32_t expected_idl_hash) {
    TopicState& state = ensureTopic(fnv1a_32(topic_name), topic_name);
    Subscription& sub = ensureSubscription(state, selector);
    sub.source_callback = callback;
    sub.has_source_callback = true;
    sub.expected_idl_hash = expected_idl_hash;
}

void TopicRuntime::forgetSubscription(const std::string& topic_name,
                                      const SubscriptionSelector& selector) {
    std::map<std::string, uint32_t>::iterator id_it = name_to_id_.find(topic_name);
    if (id_it == name_to_id_.end()) {
        return;
    }
    uint32_t id = id_it->second;
    std::map<uint32_t, TopicState>::iterator it = topics_.find(id);
    if (it == topics_.end()) {
        return;
    }

    const std::string sub_key = selector.key();
    std::vector<Subscription>& subs = it->second.subscriptions;
    for (std::vector<Subscription>::iterator sit = subs.begin(); sit != subs.end(); ++sit) {
        if (sit->key == sub_key) {
            subs.erase(sit);
            break;
        }
    }
    dropNameIfUnused(topic_name, id);
}

void TopicRuntime::setErrorCallback(const std::string& topic_name,
                                    const SubscriptionSelector& selector,
                                    const TopicErrorCallback& cb) {
    TopicState& state = ensureTopic(fnv1a_32(topic_name), topic_name);
    Subscription& sub = ensureSubscription(state, selector);
    sub.error_callback = cb;
    sub.has_error_callback = true;
}

void TopicRuntime::notifyError(uint32_t topic_id, ErrorCode error) {
    std::map<uint32_t, TopicState>::iterator it = topics_.find(topic_id);
    if (it == topics_.end()) {
        return;
    }

    // 先拷贝回调对象再调用：回调内可能 unsubscribeTopic → forgetSubscription 清除回调（约束 2）
    std::vector<TopicErrorCallback> callbacks;
    for (size_t i = 0; i < it->second.subscriptions.size(); ++i) {
        if (it->second.subscriptions[i].has_error_callback
            && it->second.subscriptions[i].error_callback) {
            callbacks.push_back(it->second.subscriptions[i].error_callback);
        }
    }
    Buffer empty;
    for (size_t i = 0; i < callbacks.size(); ++i) {
        callbacks[i](topic_id, error, empty);
    }
}

void TopicRuntime::notifyError(uint32_t topic_id, const std::string& publisher_service,
                               const std::string& host_id, const std::string& instance_id,
                               ErrorCode error) {
    std::map<uint32_t, TopicState>::iterator it = topics_.find(topic_id);
    if (it == topics_.end()) {
        return;
    }

    // 先拷贝回调对象再调用：回调内可能 unsubscribeTopic → forgetSubscription 清除回调（约束 2）
    std::vector<TopicErrorCallback> callbacks;
    for (size_t i = 0; i < it->second.subscriptions.size(); ++i) {
        const Subscription& sub = it->second.subscriptions[i];
        if (!sub.selector.matches(publisher_service, host_id, instance_id)) {
            continue;
        }
        if (sub.has_error_callback && sub.error_callback) {
            callbacks.push_back(sub.error_callback);
        }
    }
    Buffer empty;
    for (size_t i = 0; i < callbacks.size(); ++i) {
        callbacks[i](topic_id, error, empty);
    }
}

void TopicRuntime::notifySubscriptionError(const std::string& topic_name,
                                           const std::string& subscription_key,
                                           ErrorCode error) {
    std::map<std::string, uint32_t>::iterator id_it = name_to_id_.find(topic_name);
    if (id_it == name_to_id_.end()) {
        return;
    }
    std::map<uint32_t, TopicState>::iterator it = topics_.find(id_it->second);
    if (it == topics_.end()) {
        return;
    }
    Subscription* sub = findSubscription(it->second, subscription_key);
    if (!sub || !sub->has_error_callback || !sub->error_callback) {
        return;
    }
    TopicErrorCallback callback = sub->error_callback;
    Buffer empty;
    callback(it->second.id, error, empty);
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
         it != topics_.end();) {
        std::vector<int>& fds = it->second.tcp_subscribers;
        fds.erase(std::remove(fds.begin(), fds.end(), client_fd), fds.end());
        it = eraseTopicIfUnused(it);
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
         it != topics_.end();) {
        std::vector<ShmSubscriber>& subscribers = it->second.shm_subscribers;
        subscribers.erase(
            std::remove_if(subscribers.begin(), subscribers.end(),
                           [&service_name, client_id](const ShmSubscriber& subscriber) {
                               return subscriber.service_name == service_name
                                   && subscriber.client_id == client_id;
                           }),
            subscribers.end());
        it = eraseTopicIfUnused(it);
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

void TopicRuntime::dispatchFrom(const std::string& publisher_service,
                                const std::string& host_id,
                                const std::string& instance_id,
                                uint32_t topic_id, const Buffer& data) const {
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(topic_id);
    if (it == topics_.end()) {
        return;
    }

    // 先拷贝匹配回调再调用：回调内可能 unsubscribeTopic → forgetSubscription（约束 2）
    std::vector<TopicSourceCallback> source_callbacks;
    for (size_t i = 0; i < it->second.subscriptions.size(); ++i) {
        const Subscription& sub = it->second.subscriptions[i];
        if (!sub.selector.matches(publisher_service, host_id, instance_id)) {
            continue;
        }
        if (sub.has_source_callback && sub.source_callback) {
            source_callbacks.push_back(sub.source_callback);
        }
    }
    for (size_t i = 0; i < source_callbacks.size(); ++i) {
        source_callbacks[i](host_id, instance_id, topic_id, data);
    }
}

std::vector<TopicRuntime::SubscriptionRecord> TopicRuntime::subscriptionRecords() const {
    std::vector<SubscriptionRecord> result;
    for (std::map<uint32_t, TopicState>::const_iterator it = topics_.begin();
         it != topics_.end(); ++it) {
        if (it->second.name.empty()) {
            continue;
        }
        for (size_t i = 0; i < it->second.subscriptions.size(); ++i) {
            const Subscription& sub = it->second.subscriptions[i];
            if (!sub.has_source_callback) {
                continue;
            }
            SubscriptionRecord record;
            record.topic_name = it->second.name;
            record.selector = sub.selector;
            record.expected_idl_hash = sub.expected_idl_hash;
            result.push_back(record);
        }
    }
    return result;
}

std::vector<TopicRuntime::MatchedSubscription> TopicRuntime::matchSubscriptions(
    const std::string& topic_name, const std::string& publisher_service,
    const std::string& host_id, const std::string& instance_id) const {
    std::vector<MatchedSubscription> result;
    std::map<std::string, uint32_t>::const_iterator id_it = name_to_id_.find(topic_name);
    if (id_it == name_to_id_.end()) {
        return result;
    }
    std::map<uint32_t, TopicState>::const_iterator it = topics_.find(id_it->second);
    if (it == topics_.end()) {
        return result;
    }
    for (size_t i = 0; i < it->second.subscriptions.size(); ++i) {
        const Subscription& sub = it->second.subscriptions[i];
        if (!sub.selector.matches(publisher_service, host_id, instance_id)) {
            continue;
        }
        MatchedSubscription matched;
        matched.key = sub.key;
        matched.selector = sub.selector;
        matched.expected_idl_hash = sub.expected_idl_hash;
        result.push_back(matched);
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

void TopicRuntime::setExpectedSubscriptionHash(const std::string& topic_name,
                                               const SubscriptionSelector& selector,
                                               uint32_t idl_hash) {
    TopicState& state = ensureTopic(fnv1a_32(topic_name), topic_name);
    Subscription& sub = ensureSubscription(state, selector);
    sub.expected_idl_hash = idl_hash;
}

} // namespace omnibinder
