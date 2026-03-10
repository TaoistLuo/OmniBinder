#ifndef OMNIBINDER_CORE_TOPIC_RUNTIME_H
#define OMNIBINDER_CORE_TOPIC_RUNTIME_H

#include "omnibinder/runtime.h"
#include "omnibinder/buffer.h"
#include <map>
#include <string>
#include <vector>

namespace omnibinder {

class TopicRuntime {
public:
    void rememberSubscription(const std::string& topic_name, const TopicCallback& callback);
    void forgetSubscription(const std::string& topic_name);
    void rememberPublishedTopic(const std::string& topic_name, uint32_t topic_id,
                                const std::string& owner_service);
    void forgetPublishedTopic(const std::string& topic_name);
    void forgetPublishedTopicsByIds(const std::vector<uint32_t>& topic_ids);
    void forgetPublishedTopicsByOwner(const std::string& owner_service);
    void addTcpSubscriber(uint32_t topic_id, int client_fd);
    void removeTcpSubscriberFd(int client_fd);
    void addShmSubscriberService(uint32_t topic_id, const std::string& service_name);
    std::vector<int>* tcpSubscribers(uint32_t topic_id);
    std::vector<std::string>* shmSubscriberServices(uint32_t topic_id);
    bool dispatch(uint32_t topic_id, const Buffer& data) const;
    std::map<std::string, TopicCallback> subscriptions() const;
    std::map<std::string, std::string> publishedTopicOwners() const;

    std::map<std::string, uint32_t> published_topics;

private:
    std::map<std::string, TopicCallback> callbacks_;
    std::map<std::string, uint32_t> topic_name_to_id_;
    std::map<uint32_t, std::vector<TopicCallback> > callbacks_by_id_;
    std::map<uint32_t, std::vector<int> > tcp_subscribers_;
    std::map<uint32_t, std::vector<std::string> > shm_subscriber_services_;
    std::map<std::string, std::string> published_topic_owners_;
};

} // namespace omnibinder

#endif
