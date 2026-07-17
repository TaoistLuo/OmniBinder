#include "service_manager_app.h"
#include "omnibinder/log.h"
#include "platform/platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <algorithm>

#define TAG "ServiceManager"

 {
        std::string name;
        if (!tryReadExactStringArg(msg, name, MAX_SERVICE_NAME_LENGTH)) {
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

void ServiceManagerApp::sendQueryPublishedTopicsReply(ClientConnection* conn, uint32_t seq, bool found, {
        Message reply(MessageType::MSG_QUERY_PUBLISHED_TOPICS_REPLY, seq);
        if (!serializePublishedTopicsReply(found, topics, reply.payload)) {
            OMNI_LOG_ERROR(TAG, "Failed to serialize published topics reply for seq=%u", seq);
            return;
        }
        sendMessage(conn, reply);
}

 {
        Buffer payload(msg.payload.data(), msg.payload.size());
        std::string topic;
        if (!payload.tryReadString(topic)
            || topic.empty() || topic.size() > MAX_TOPIC_NAME_LENGTH) {
            OMNI_LOG_WARN(TAG, "Reject malformed publish topic request from fd=%d", conn->fd);
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }
        ServiceInfo pub_info;
        if (!deserializeServiceInfo(payload, pub_info)) {
            OMNI_LOG_ERROR(TAG, "Failed to deserialize publisher info for topic %s", topic.c_str());
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }
        uint32_t idl_hash = 0;
        if (payload.remaining() != 0
            && (!payload.tryReadUint32(idl_hash) || payload.remaining() != 0)) {
            OMNI_LOG_WARN(TAG, "Reject publish topic request with malformed tail from fd=%d",
                          conn->fd);
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }

        if (pub_info.name.empty() || pub_info.name.size() > MAX_SERVICE_NAME_LENGTH) {
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }

        if (!registry_.ownsService(conn->fd, pub_info.name)) {
            OMNI_LOG_WARN(TAG, "Reject publish topic %s from fd=%d for non-owned service %s",
                           topic.c_str(), conn->fd, pub_info.name.c_str());
            sendPublishTopicReply(conn, msg.header.sequence, false);
            return;
        }

        bool success = topic_manager_.registerPublisher(topic, pub_info, conn->fd,
                                                        idl_hash);
        sendPublishTopicReply(conn, msg.header.sequence, success);

        if (success) {
            // Notify existing subscribers about the new publisher
            std::vector<int> subscribers = topic_manager_.getSubscribers(topic);
            for (size_t i = 0; i < subscribers.size(); ++i) {
                sendTopicPublisherNotify(subscribers[i], topic, pub_info);
            }
        }
}

 {
        Message reply(MessageType::MSG_PUBLISH_TOPIC_REPLY, seq);
        reply.payload.writeBool(success);
        sendMessage(conn, reply);
}

 {
        std::string topic;
        if (!tryReadExactTopicArg(msg, topic)) {
            OMNI_LOG_WARN(TAG, "Drop malformed unpublish topic request from fd=%d", conn->fd);
            return;
        }

        if (!topic_manager_.removePublisher(topic, conn->fd)) {
            OMNI_LOG_WARN(TAG, "Reject unpublish topic %s from non-owner fd=%d",
                           topic.c_str(), conn->fd);
        }
        // No reply needed
}

 {
        std::string topic;
        if (!tryReadExactTopicArg(msg, topic)) {
            OMNI_LOG_WARN(TAG, "Reject malformed subscribe topic request from fd=%d", conn->fd);
            sendSubscribeTopicReply(conn, msg.header.sequence, false);
            return;
        }

        bool added = topic_manager_.addSubscriber(topic, conn->fd);
        uint32_t idl_hash = 0;
        topic_manager_.getIdlHash(topic, idl_hash);
        sendSubscribeTopicReply(conn, msg.header.sequence, added, idl_hash);

        // If there's already a publisher, notify the subscriber
        ServiceInfo pub_info;
        if (topic_manager_.getPublisher(topic, pub_info)) {
            sendTopicPublisherNotify(conn->fd, topic, pub_info);
        }
}

 {
        Message reply(MessageType::MSG_SUBSCRIBE_TOPIC_REPLY, seq);
        reply.payload.writeBool(success);
        if (success) {
            reply.payload.writeUint32(idl_hash);
        }
        sendMessage(conn, reply);
}

 {
        std::string topic;
        if (!tryReadExactTopicArg(msg, topic)) {
            OMNI_LOG_WARN(TAG, "Drop malformed unsubscribe topic request from fd=%d", conn->fd);
            return;
        }

        topic_manager_.removeSubscriber(topic, conn->fd);
        // No reply needed
}

void ServiceManagerApp::sendTopicPublisherNotify(int subscriber_fd, const std::string& topic, {
        auto it = clients_.find(subscriber_fd);
        if (it == clients_.end()) {
            return;
        }

        Message notify(MessageType::MSG_TOPIC_PUBLISHER_NOTIFY, nextSequenceNumber());
        notify.payload.writeString(topic);
        serializeServiceInfo(pub_info, notify.payload);
        sendMessage(it->second, notify);
}

