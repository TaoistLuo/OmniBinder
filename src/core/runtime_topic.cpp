#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/log.h"

#include <algorithm>
#include <utility>

#define LOG_TAG "OmniRuntimeTopic"

namespace omnibinder {

namespace {

TopicRuntime::SubscriptionSelector toRuntimeSelector(
    const TopicSubscriptionSelector& selector) {
    TopicRuntime::SubscriptionSelector runtime_selector;
    runtime_selector.publisher_service = selector.publisher_service;
    runtime_selector.host_id = selector.host_id;
    runtime_selector.instance_id = selector.instance_id;
    runtime_selector.instance_level = selector.instance_level;
    return runtime_selector;
}

uint8_t selectorWireMode(const TopicSubscriptionSelector& selector) {
    return selector.instance_level ? TOPIC_SUB_MODE_INSTANCE : TOPIC_SUB_MODE_SERVICE;
}

bool watchesOverlap(const ServiceWatchRequest& a, const ServiceWatchRequest& b) {
    if (a.service_name != b.service_name) return false;
    if (!a.host_id.empty() && !b.host_id.empty() && a.host_id != b.host_id) return false;
    if (!a.instance_id.empty() && !b.instance_id.empty()
        && a.instance_id != b.instance_id) return false;
    return true;
}

bool validWatchRequests(const std::vector<ServiceWatchRequest>& requests) {
    if (requests.empty() || requests.size() > 64) return false;
    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].service_name.empty()
            || requests[i].service_name.size() > MAX_SERVICE_NAME_LENGTH
            || requests[i].host_id.size() > MAX_SERVICE_NAME_LENGTH
            || requests[i].instance_id.size() > MAX_SERVICE_NAME_LENGTH) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (watchesOverlap(requests[i], requests[j])) return false;
        }
    }
    return true;
}

} // namespace

// ============================================================
// 服务状态 watch
// ============================================================

int OmniRuntime::Impl::watchServices(const std::vector<ServiceWatchRequest>& requests,
                                     const ServiceWatchCallback& callback,
                                     uint64_t& watch_id) {
    return callSerialized([this, &requests, &callback, &watch_id]() -> int {
        return watchServicesInternal(requests, callback, watch_id);
    });
}

int OmniRuntime::Impl::watchServicesInternal(
    const std::vector<ServiceWatchRequest>& requests,
    const ServiceWatchCallback& callback, uint64_t& watch_id) {
    if (!validWatchRequests(requests) || !callback) {
        return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    }

    uint64_t id = next_watch_id_++;
    if (id == 0) id = next_watch_id_++;
    while (service_watches_.find(id) != service_watches_.end()) {
        id = next_watch_id_++;
        if (id == 0) id = next_watch_id_++;
    }

    ServiceWatchSubscription pending_sub;
    pending_sub.watch_id = id;
    pending_sub.requests = requests;
    pending_sub.callback = callback;
    pending_sub.active = false;
    service_watches_[id] = pending_sub;

    // SM 尚未就绪时先保留本地登记，等 SM 连接成功后由
    // restoreControlPlaneState() 重放；否则启动早于 SM 的客户端会永久丢失 watch。
    if (!ready_ || !sm_channel_.isConnected()) {
        watch_id = id;
        OMNI_LOG_INFO(LOG_TAG,
                      "watchServices deferred until SM ready watch_id=%llu",
                      (unsigned long long)id);
        return 0;
    }

    Message msg(MessageType::MSG_SUBSCRIBE_SERVICE_WATCH, allocSequence());
    if (!msg.payload.writeUint64(id) || !msg.payload.writeUint32(
            static_cast<uint32_t>(requests.size()))) {
        service_watches_.erase(id);
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    for (size_t i = 0; i < requests.size(); ++i) {
        if (!msg.payload.writeString(requests[i].service_name)
            || !msg.payload.writeString(requests[i].host_id)
            || !msg.payload.writeString(requests[i].instance_id)) {
            service_watches_.erase(id);
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
    }

    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) {
        service_watches_.erase(id);
        return ret;
    }
    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        service_watches_.erase(id);
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        service_watches_.erase(id);
        return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    }

    std::map<uint64_t, ServiceWatchSubscription>::iterator it = service_watches_.find(id);
    if (it == service_watches_.end()) {
        // 初始通知回调里已经 unwatch 本组
        watch_id = id;
        return 0;
    }
    it->second.active = true;
    std::vector<ServiceWatchEvent> pending;
    pending.swap(it->second.pending);
    ServiceWatchCallback cb = it->second.callback;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (service_watches_.find(id) == service_watches_.end()) break;
        cb(id, pending[i].identity, pending[i].online);
    }
    watch_id = id;
    return 0;
}

int OmniRuntime::Impl::unwatchServices(uint64_t watch_id) {
    return callSerialized([this, watch_id]() -> int {
        return unwatchServicesInternal(watch_id);
    });
}

int OmniRuntime::Impl::unwatchServicesInternal(uint64_t watch_id) {
    if (watch_id == 0) {
        return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    }
    Message msg(MessageType::MSG_UNSUBSCRIBE_SERVICE_WATCH, allocSequence());
    if (!msg.payload.writeUint64(watch_id)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    sendToSM(msg);
    service_watches_.erase(watch_id);
    return 0;
}

int OmniRuntime::Impl::restoreServiceWatch(uint64_t watch_id) {
    std::map<uint64_t, ServiceWatchSubscription>::iterator it =
        service_watches_.find(watch_id);
    if (it == service_watches_.end()) {
        return 0;
    }
    it->second.active = false;
    it->second.pending.clear();
    const ServiceWatchSubscription sub = it->second;
    Message msg(MessageType::MSG_SUBSCRIBE_SERVICE_WATCH, allocSequence());
    if (!msg.payload.writeUint64(sub.watch_id)
        || !msg.payload.writeUint32(static_cast<uint32_t>(sub.requests.size()))) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    for (size_t i = 0; i < sub.requests.size(); ++i) {
        if (!msg.payload.writeString(sub.requests[i].service_name)
            || !msg.payload.writeString(sub.requests[i].host_id)
            || !msg.payload.writeString(sub.requests[i].instance_id)) {
            return static_cast<int>(ErrorCode::ERR_SERIALIZE);
        }
    }
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;
    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        OMNI_LOG_WARN(LOG_TAG, "restore service watch malformed reply watch_id=%llu",
                      (unsigned long long)watch_id);
        return 0;
    }
    if (!accepted) {
        OMNI_LOG_WARN(LOG_TAG, "restore service watch failed for watch_id=%llu",
                      (unsigned long long)watch_id);
        return 0;
    }

    it = service_watches_.find(watch_id);
    if (it == service_watches_.end()) {
        return 0;
    }
    it->second.active = true;
    std::vector<ServiceWatchEvent> pending;
    pending.swap(it->second.pending);
    ServiceWatchCallback callback = it->second.callback;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (service_watches_.find(watch_id) == service_watches_.end()) break;
        callback(watch_id, pending[i].identity, pending[i].online);
    }
    return 0;
}

// ============================================================
// 话题
// ============================================================

int OmniRuntime::Impl::publishTopic(const std::string& topic_name, uint32_t idl_hash) {
    return callSerialized([this, &topic_name, idl_hash]() -> int {
        return publishTopicInternal(topic_name, idl_hash);
    });
}

int OmniRuntime::Impl::publishTopicForService(LocalServiceEntry* entry,
                                               const std::string& service_name,
                                               const std::string& topic_name,
                                               uint32_t idl_hash,
                                               bool allow_pending) {
    if (!entry || !entry->service) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    if (!sm_channel_.isConnected()) {
        if (!allow_pending) {
            return static_cast<int>(ErrorCode::ERR_SM_UNREACHABLE);
        }
        topic_runtime_.rememberPublishedTopic(topic_name, topic_id, service_name, idl_hash);
        return 0;
    }

    Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);

    ServiceInfo pub_info;
    pub_info.name = service_name;
    pub_info.host = resolveRegisterHost(entry->service, listenerAdvertiseHost());
    pub_info.port = entry->port;
    pub_info.host_id = host_id_;
    pub_info.instance_id = entry->service->instanceId();
    pub_info.shm_config = entry->service->shmConfig();
    if (!serializeServiceInfo(pub_info, msg.payload)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    if (!msg.payload.writeUint32(idl_hash)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) {
        if (allow_pending && isTransientSmError(ret)) {
            topic_runtime_.rememberPublishedTopic(topic_name, topic_id, service_name, idl_hash);
            return 0;
        }
        return ret;
    }

    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    topic_runtime_.rememberPublishedTopic(topic_name, topic_id, service_name, idl_hash);
    return 0;
}

int OmniRuntime::Impl::publishDeclaredTopicsForService(const std::string& service_name,
                                                       LocalServiceEntry* entry,
                                                       Service* service,
                                                       bool allow_pending) {
    if (!service || !entry) {
        return 0;
    }
    const std::vector<PublishedTopicInfo>& topics = service->publishedTopics();
    for (size_t i = 0; i < topics.size(); ++i) {
        int ret = publishTopicForService(entry, service_name, topics[i].name,
                                        topics[i].idl_hash, allow_pending);
        if (ret != 0) {
            OMNI_LOG_WARN(LOG_TAG, "publish topic %s for service %s failed err=%d",
                          topics[i].name.c_str(), service_name.c_str(), ret);
        }
    }
    return 0;
}

int OmniRuntime::Impl::publishTopicInternal(const std::string& topic_name, uint32_t idl_hash) {
    if (!initialized_) {
        return static_cast<int>(ErrorCode::ERR_NOT_INITIALIZED);
    }

    Message msg(MessageType::MSG_PUBLISH_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    
    // 构建 publisher ServiceInfo 供 SM 告知订阅者连接地址。
    // 使用第一个已注册本地服务的监听端口；若没有本地服务则返回 ERR_SERVICE_NOT_FOUND
    // （与 initDiagDataService 的差异路径不同：它会在 publish 前先注册一个专用诊断服务，保证存在本地服务）。
    ServiceInfo pub_info;
    pub_info.host_id = host_id_;
    
    if (!local_services_.empty()) {
        LocalServiceEntry* entry = local_services_.begin()->second;
        pub_info.name = local_services_.begin()->first;
        // 与注册路径一致地解析对外通告地址：跨机场景 setRegisterHost 设置的真实地址
        // 必须用于话题发布者信息，否则订阅者拿到 127.0.0.1（listenerAdvertiseHost 的
        // 回退值）而连到本机（约束：控制面地址一致性）
        pub_info.host = resolveRegisterHost(entry->service, listenerAdvertiseHost());
        pub_info.port = entry->port;
        if (entry->service) {
            pub_info.instance_id = entry->service->instanceId();
            pub_info.shm_config = entry->service->shmConfig();
        }
    } else {
        OMNI_LOG_WARN(LOG_TAG, "publishTopic requires a registered local service to advertise publisher endpoint");
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    if (!serializeServiceInfo(pub_info, msg.payload)) {
        OMNI_LOG_ERROR(LOG_TAG, "serialize_publish_topic_payload_failed topic=%s",
                       topic_name.c_str());
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    if (!msg.payload.writeUint32(idl_hash)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;

    bool accepted = false;
    if (!decodeBoolReplyPayload(reply, accepted)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }

    uint32_t topic_id = fnv1a_32(topic_name);
    topic_runtime_.rememberPublishedTopic(topic_name, topic_id, pub_info.name, idl_hash);
    return 0;
}

int OmniRuntime::Impl::broadcast(uint32_t topic_id, const Buffer& data) {
    return callSerialized([this, topic_id, &data]() -> int {
        return broadcastInternal(topic_id, data);
    });
}

int OmniRuntime::Impl::broadcastInternal(uint32_t topic_id, const Buffer& data) {
    Message msg(MessageType::MSG_BROADCAST, 0);
    msg.payload.writeUint32(topic_id);
    msg.payload.writeUint32(static_cast<uint32_t>(data.size()));
    if (data.size() > 0) {
        msg.payload.writeRaw(data.data(), data.size());
    }

    // 诊断 BROADCAST 钩子（diag topic 跳过，防止递归）
    if (diag_.active_count > 0 && !isDiagDataTopic(topic_id)) {
        // 先拷贝服务名再遍历：emitDiagHook 内部会广播，广播的 TCP 部分写失败路径
        // 会执行断连用户回调并可能注销服务，持 local_services_ 迭代器会失效（约束 2）
        std::vector<std::string> diag_service_names;
        bool is_diag_topic = false;
        for (std::map<std::string, LocalServiceEntry*>::iterator kv = local_services_.begin();
             kv != local_services_.end(); ++kv) {
            if (kv->second->diag_topic_id == topic_id) {
                is_diag_topic = true;
                break;
            }
            diag_service_names.push_back(kv->first);
        }
        if (!is_diag_topic) {
            for (size_t i = 0; i < diag_service_names.size(); ++i) {
                std::map<std::string, LocalServiceEntry*>::iterator eit =
                    local_services_.find(diag_service_names[i]);
                if (eit == local_services_.end()) continue;
                emitDiagHook(eit->second, DIAG_EVENT_BROADCAST, msg);
            }
        }
    }
    if (!isDiagDataTopic(topic_id)) {
        emitDiagEvent(DIAG_EVENT_BROADCAST, msg);
    }

    Buffer send_buf;
    if (!msg.serialize(send_buf)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

    // 遍历订阅列表期间不得执行用户代码：onServiceClientDisconnected 会移除订阅
    // 条目并可能经 onClientDisconnected 注销服务（约束 1/2）。TCP 部分写/错误
    // 先收集，两个订阅列表遍历完再统一走正常断开路径。
    std::vector<std::pair<std::string, int> > broken_tcp_clients;

    const std::vector<int>& tcp_subscribers = topic_runtime_.tcpSubscribers(topic_id);
    if (!tcp_subscribers.empty()) {
        OMNI_LOG_DEBUG(LOG_TAG, "Broadcast topic 0x%08x over TCP to %zu subscribers",
                        topic_id, tcp_subscribers.size());
        // 逆序遍历，便于安全删除陈旧条目
        for (int i = static_cast<int>(tcp_subscribers.size()) - 1; i >= 0; --i) {
            int client_id = tcp_subscribers[i];
            std::map<int, std::string>::iterator sit = client_id_to_service_.find(client_id);
            if (sit == client_id_to_service_.end()) {
                topic_runtime_.removeTcpSubscriber(topic_id, client_id);
                continue;
            }
            const std::string service_name = sit->second;
            std::map<std::string, LocalServiceEntry*>::iterator eit =
                local_services_.find(service_name);
            if (eit == local_services_.end()) {
                topic_runtime_.removeTcpSubscriber(topic_id, client_id);
                continue;
            }
            std::map<int, IMessageConnection*>::iterator tit =
                eit->second->clients.find(client_id);
            if (tit == eit->second->clients.end() || !tit->second) {
                topic_runtime_.removeTcpSubscriber(topic_id, client_id);
                continue;
            }
            // TCP 为字节流：send 的部分写字节已进入流中，会让订阅端帧错位。
            // timeout=0 的 sendAll 只尝试一次（不等待、不阻塞 owner loop），
            // 失败即视为该连接损坏，由断开路径回收；订阅端重连后重放订阅
            if (tit->second->sendAll(send_buf.data(), send_buf.size(), 0, NULL) != 0) {
                OMNI_LOG_WARN(LOG_TAG,
                              "broadcast_tcp_send_incomplete client=%d service=%s disconnect",
                              client_id, service_name.c_str());
                broken_tcp_clients.push_back(std::make_pair(service_name, client_id));
            }
        }
    }

    // SHM 订阅者：send 要么整帧写入、要么返回 0（ring 满）；ring 满只丢帧，不断开
    {
        const std::vector<TopicRuntime::ShmSubscriber>& shm_subscribers = topic_runtime_.shmSubscribers(topic_id);
        for (size_t i = 0; i < shm_subscribers.size(); ++i) {
            const std::string service_name = shm_subscribers[i].service_name;
            std::map<std::string, LocalServiceEntry*>::iterator eit = local_services_.find(service_name);
            if (eit == local_services_.end()) {
                continue;
            }
            std::map<int, IMessageConnection*>::iterator tit =
                eit->second->clients.find(static_cast<int>(shm_subscribers[i].client_id));
            if (tit == eit->second->clients.end() || !tit->second) {
                continue;
            }
            int sent = tit->second->send(send_buf.data(), send_buf.size());
            if (sent != static_cast<int>(send_buf.size())) {
                OMNI_LOG_DEBUG(LOG_TAG, "broadcast dropped for shm client=%u (ring full)",
                               shm_subscribers[i].client_id);
            }
        }
    }

    for (size_t i = 0; i < broken_tcp_clients.size(); ++i) {
        onServiceClientDisconnected(broken_tcp_clients[i].first, broken_tcp_clients[i].second);
    }

    return 0;
}

int OmniRuntime::Impl::subscribeTopic(const std::string& topic_name, uint32_t expected_idl_hash,
                                       const TopicCallback& on_message,
                                       const TopicErrorCallback& on_error) {
    return callSerialized([this, &topic_name, expected_idl_hash, &on_message, &on_error]() -> int {
        // 普通订阅 = 默认空选择器（匹配任意发布者）；回调适配为携带来源身份的签名
        const TopicSubscriptionSelector selector;
        // 按值捕获 on_message：该回调会被长期存入 topic_runtime_，
        // 按引用捕获会在 subscribeTopic 返回后悬垂（约束 1）
        const TopicSourceCallback source_cb =
            [on_message](const std::string&, const std::string&, uint32_t topic_id,
                         const Buffer& data) {
                if (on_message) {
                    on_message(topic_id, data);
                }
            };
        return subscribeTopicInternal(selector, topic_name, expected_idl_hash, source_cb,
                                      on_error);
    });
}

int OmniRuntime::Impl::unsubscribeTopic(const std::string& topic_name) {
    return callSerialized([this, &topic_name]() -> int {
        return unsubscribeTopicInternal(TopicSubscriptionSelector(), topic_name);
    });
}

// ============================================================
// 话题订阅（普通订阅 = 默认空选择器；选择器订阅 = 服务级 / 实例级）
// ============================================================

int OmniRuntime::Impl::subscribeTopic(const TopicSubscriptionSelector& selector,
                                      const std::string& topic_name,
                                      uint32_t expected_idl_hash,
                                      const TopicSourceCallback& on_message,
                                      const TopicErrorCallback& on_error) {
    return callSerialized(
        [this, &selector, &topic_name, expected_idl_hash, &on_message, &on_error]() -> int {
            return subscribeTopicInternal(selector, topic_name, expected_idl_hash,
                                          on_message, on_error);
        });
}

int OmniRuntime::Impl::subscribeTopicInternal(
    const TopicSubscriptionSelector& selector, const std::string& topic_name,
    uint32_t expected_idl_hash, const TopicSourceCallback& callback,
    const TopicErrorCallback& on_error) {
    if (selector.publisher_service.empty() && selector.instance_level) {
        return static_cast<int>(ErrorCode::ERR_INVALID_PARAM);
    }
    const TopicRuntime::SubscriptionSelector runtime_selector =
        toRuntimeSelector(selector);

    // 期望哈希必须在发起 SM 请求前登记：SM 可能在应答之后紧接推送
    // MSG_TOPIC_PUBLISHER_NOTIFY，且两者可能在同一次 socket 读取中被连续分发，
    // 通知处理需要能查到期望哈希才能完成严格校验
    topic_runtime_.setExpectedSubscriptionHash(topic_name, runtime_selector,
                                               expected_idl_hash);

    Message msg(MessageType::MSG_SUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    // 空选择器（普通订阅）不写选择器字段，保持既有线协议
    if (!selector.publisher_service.empty()
        && !writeTopicSubscriptionSelector(msg.payload, selectorWireMode(selector),
                                           selector.publisher_service, selector.host_id,
                                           selector.instance_id)) {
        topic_runtime_.forgetSubscription(topic_name, runtime_selector);
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }

    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) {
        topic_runtime_.forgetSubscription(topic_name, runtime_selector);
        return ret;
    }

    Buffer reply_payload(reply.payload.data(), reply.payload.size());
    bool accepted = false;
    if (!reply_payload.tryReadBool(accepted)) {
        topic_runtime_.forgetSubscription(topic_name, runtime_selector);
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!accepted) {
        topic_runtime_.forgetSubscription(topic_name, runtime_selector);
        return static_cast<int>(ErrorCode::ERR_REGISTER_FAILED);
    }
    uint32_t publisher_idl_hash = 0;
    if (reply_payload.remaining() >= sizeof(uint32_t)
        && !reply_payload.tryReadUint32(publisher_idl_hash)) {
        topic_runtime_.forgetSubscription(topic_name, runtime_selector);
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }

    // 严格 topic 哈希校验：发布者已上线且声明了哈希时，必须与订阅者期望一致。
    // 发布者尚未上线（SM 返回 0）时延迟到 MSG_TOPIC_PUBLISHER_NOTIFY 再校验。
    if (expected_idl_hash != 0 && publisher_idl_hash != 0
        && publisher_idl_hash != expected_idl_hash) {
        OMNI_LOG_WARN(LOG_TAG,
                      "topic_idl_mismatch topic=%s expected_hash=0x%08x publisher_hash=0x%08x",
                      topic_name.c_str(), expected_idl_hash, publisher_idl_hash);
        unsubscribeTopicInternal(selector, topic_name);
        return static_cast<int>(ErrorCode::ERR_IDL_MISMATCH);
    }

    topic_runtime_.rememberSubscription(topic_name, runtime_selector, callback,
                                        expected_idl_hash);
    topic_runtime_.setErrorCallback(topic_name, runtime_selector, on_error);
    return 0;
}

int OmniRuntime::Impl::unsubscribeTopic(const TopicSubscriptionSelector& selector,
                                        const std::string& topic_name) {
    return callSerialized([this, &selector, &topic_name]() -> int {
        return unsubscribeTopicInternal(selector, topic_name);
    });
}

int OmniRuntime::Impl::unsubscribeTopicInternal(
    const TopicSubscriptionSelector& selector, const std::string& topic_name) {
    const TopicRuntime::SubscriptionSelector runtime_selector =
        toRuntimeSelector(selector);

    Message msg(MessageType::MSG_UNSUBSCRIBE_TOPIC, allocSequence());
    msg.payload.writeString(topic_name);
    if (!selector.publisher_service.empty()
        && !writeTopicSubscriptionSelector(msg.payload, selectorWireMode(selector),
                                           selector.publisher_service, selector.host_id,
                                           selector.instance_id)) {
        return static_cast<int>(ErrorCode::ERR_SERIALIZE);
    }
    sendToSM(msg);
    topic_runtime_.forgetSubscription(topic_name, runtime_selector);
    // 同步移除连接上的订阅绑定：否则直连重连会重放已取消的订阅
    removeTopicSubscriptionBindingForSelector(runtime_selector, topic_name);
    return 0;
}

} // namespace omnibinder
