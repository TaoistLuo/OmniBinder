#include "service_manager_app.h"
#include "sm_parse_helpers.h"
#include "omnibinder/log.h"

#define TAG "ServiceManager"

namespace omnibinder {

namespace {
// host_id / instance_id 均为实例身份辅助字段，对齐服务名长度上限即可防止畸形输入
const size_t MAX_INSTANCE_FIELD_LENGTH = MAX_SERVICE_NAME_LENGTH;

bool parseSubscribeTopicRequest(const Message& msg, std::string& topic,
                                TopicSubscriptionFilter& selector)
{
    Buffer payload(msg.payload.data(), msg.payload.size());
    if (!payload.tryReadString(topic) || topic.empty()
        || topic.size() > MAX_TOPIC_NAME_LENGTH) {
        return false;
    }
    if (payload.remaining() == 0) {
        selector = TopicSubscriptionFilter();
        return true;
    }

    uint8_t mode = 0;
    std::string publisher_service;
    std::string host_id;
    std::string instance_id;
    if (!readTopicSubscriptionSelector(payload, mode, publisher_service, host_id, instance_id)
        || payload.remaining() != 0) {
        return false;
    }
    if (mode >= TOPIC_SUB_MODE_SERVICE
        && (publisher_service.empty() || publisher_service.size() > MAX_SERVICE_NAME_LENGTH)) {
        return false;
    }
    if (mode == TOPIC_SUB_MODE_INSTANCE
        && (host_id.size() > MAX_INSTANCE_FIELD_LENGTH
            || instance_id.size() > MAX_INSTANCE_FIELD_LENGTH)) {
        return false;
    }

    selector.mode = static_cast<TopicSubscribeMode>(mode);
    selector.publisher_service = publisher_service;
    selector.host_id = host_id;
    selector.instance_id = instance_id;
    return true;
}

} // namespace

void ServiceManagerApp::handleQueryPublishedTopics(ClientConnection* conn,
                                               const Message& msg) {
    std::string name;
    if (!sm_internal::tryReadExactStringArg(msg, name, MAX_SERVICE_NAME_LENGTH)) {
        OMNI_LOG_WARN(TAG, "Reject malformed query published topics request from fd=%d",
                      conn->fd);
        sendQueryPublishedTopicsReply(conn, msg.header.sequence, false,
                                      std::vector<std::string>());
        return;
    }

    ServiceEntry entry;
    const bool found = registry_.findService(name, entry);
    const std::vector<std::string> topics = found
        ? topic_manager_.getPublishedTopics(name)
        : std::vector<std::string>();
    sendQueryPublishedTopicsReply(conn, msg.header.sequence, found, topics);
}

void ServiceManagerApp::sendQueryPublishedTopicsReply(
    ClientConnection* conn, uint32_t seq, bool found,
    const std::vector<std::string>& topics) {
    Message reply(MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY, seq);
    if (!serializePublishedTopicsReply(found, topics, reply.payload)) {
        OMNI_LOG_ERROR(TAG, "Failed to serialize published topics reply for seq=%u", seq);
        return;
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handlePublishTopic(ClientConnection* conn, const Message& msg) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    std::string topic;
    if (!payload.tryReadString(topic)
        || topic.empty() || topic.size() > MAX_TOPIC_NAME_LENGTH) {
        OMNI_LOG_WARN(TAG, "Reject malformed publish topic request from fd=%d", conn->fd);
        sendBoolReply(conn, MessageType::MSG_PUBLISH_TOPIC_REPLY, msg.header.sequence, false);
        return;
    }
    ServiceInfo pub_info;
    if (!deserializeServiceInfo(payload, pub_info)) {
        OMNI_LOG_ERROR(TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
        sendBoolReply(conn, MessageType::MSG_PUBLISH_TOPIC_REPLY, msg.header.sequence, false);
        return;
    }
    uint32_t idl_hash = 0;
    if (payload.remaining() != 0
        && (!payload.tryReadUint32(idl_hash) || payload.remaining() != 0)) {
        OMNI_LOG_WARN(TAG, "Reject publish topic request with malformed tail from fd=%d",
                      conn->fd);
        sendBoolReply(conn, MessageType::MSG_PUBLISH_TOPIC_REPLY, msg.header.sequence, false);
        return;
    }

    if (pub_info.name.empty() || pub_info.name.size() > MAX_SERVICE_NAME_LENGTH) {
        sendBoolReply(conn, MessageType::MSG_PUBLISH_TOPIC_REPLY, msg.header.sequence, false);
        return;
    }

    // 发布身份以注册表为准：仅校验 fd 拥有该服务名不足以阻止发布者自报
    // host_id/instance_id 冒充同名其它实例（实例级订阅会据此下发连接信息）。
    // 这里用该 fd 名下已注册实例的权威身份覆盖客户端自报字段。
    ServiceEntry owner_entry;
    if (!registry_.findServiceByFdAndName(conn->fd, pub_info.name, owner_entry)) {
        OMNI_LOG_WARN(TAG, "Reject publish topic %s from fd=%d for non-owned service %s",
                       topic.c_str(), conn->fd, pub_info.name.c_str());
        sendBoolReply(conn, MessageType::MSG_PUBLISH_TOPIC_REPLY, msg.header.sequence, false);
        return;
    }
    pub_info.host_id = owner_entry.info.host_id;
    pub_info.instance_id = owner_entry.info.instance_id;

    bool success = topic_manager_.registerPublisher(topic, pub_info, conn->fd,
                                                    idl_hash);
    sendBoolReply(conn, MessageType::MSG_PUBLISH_TOPIC_REPLY, msg.header.sequence, success);

    if (success) {
        // 只通知选择器匹配该发布者实例的订阅者
        std::vector<int> subscribers = topic_manager_.getMatchingSubscribers(topic, pub_info);
        for (size_t i = 0; i < subscribers.size(); ++i) {
            sendTopicPublisherNotify(subscribers[i], topic, pub_info, idl_hash);
        }
    }
}

void ServiceManagerApp::handleUnpublishTopic(ClientConnection* conn, const Message& msg) {
    std::string topic;
    if (!sm_internal::tryReadExactTopicArg(msg, topic)) {
        OMNI_LOG_WARN(TAG, "Drop malformed unpublish topic request from fd=%d", conn->fd);
        return;
    }

    if (!topic_manager_.removePublisher(topic, conn->fd)) {
        OMNI_LOG_WARN(TAG, "Reject unpublish topic %s from non-owner fd=%d",
                       topic.c_str(), conn->fd);
    }
    // 无需回复
}

void ServiceManagerApp::handleSubscribeTopic(ClientConnection* conn, const Message& msg) {
    std::string topic;
    TopicSubscriptionFilter selector;
    if (!parseSubscribeTopicRequest(msg, topic, selector)) {
        OMNI_LOG_WARN(TAG, "Reject malformed subscribe topic request from fd=%d", conn->fd);
        sendSubscribeTopicReply(conn, msg.header.sequence, false);
        return;
    }

    bool added = topic_manager_.addSubscriber(topic, selector, conn->fd);
    uint32_t idl_hash = 0;
    topic_manager_.getIdlHash(topic, selector, idl_hash);
    // sendSubscribeTopicReply 发送失败时可能 closeClient 删除 conn，
    // 先保存 fd 副本，后续只用 fd 值，避免对已释放 conn 解引用。
    int fd = conn->fd;
    sendSubscribeTopicReply(conn, msg.header.sequence, added, idl_hash);

    // 立即通知该订阅者所有匹配选择器的在线发布者实例
    std::vector<TopicPublisherView> publishers =
        topic_manager_.getMatchingPublishers(topic, selector);
    for (size_t i = 0; i < publishers.size(); ++i) {
        sendTopicPublisherNotify(fd, topic, publishers[i].info, publishers[i].idl_hash);
    }
}

void ServiceManagerApp::sendSubscribeTopicReply(ClientConnection* conn, uint32_t seq,
                                            bool success, uint32_t idl_hash) {
    Message reply(MessageType::MSG_SUBSCRIBE_TOPIC_REPLY, seq);
    reply.payload.writeBool(success);
    if (success) {
        reply.payload.writeUint32(idl_hash);
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleUnsubscribeTopic(ClientConnection* conn, const Message& msg) {
    std::string topic;
    TopicSubscriptionFilter selector;
    if (!parseSubscribeTopicRequest(msg, topic, selector)) {
        OMNI_LOG_WARN(TAG, "Drop malformed unsubscribe topic request from fd=%d", conn->fd);
        return;
    }

    topic_manager_.removeSubscriber(topic, selector, conn->fd);
    // 无需回复
}

void ServiceManagerApp::sendTopicPublisherNotify(int subscriber_fd,
                                             const std::string& topic,
                                             const ServiceInfo& pub_info,
                                             uint32_t idl_hash) {
    auto it = clients_.find(subscriber_fd);
    if (it == clients_.end()) {
        return;
    }

    Message notify(MessageType::MSG_TOPIC_PUBLISHER_NOTIFY, nextSMProactiveSequence());
    notify.payload.writeString(topic);
    if (!serializeServiceInfo(pub_info, notify.payload)) {
        OMNI_LOG_ERROR(TAG, "Failed to serialize publisher notify for topic %s", topic.c_str());
        return;
    }
    if (!notify.payload.writeUint32(idl_hash)) {
        return;
    }
    sendMessage(it->second, notify);
}

} // namespace omnibinder
