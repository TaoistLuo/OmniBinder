#include "core/omni_runtime.h"
#include "core/runtime_helpers.h"
#include "omnibinder/buffer_view.h"
#include "omnibinder/log.h"

#include <sstream>

#define LOG_TAG "OmniRuntimeDiag"

namespace omnibinder {
namespace {

/*
 * @brief  诊断专用本地服务：被 SM 的 watch 流程作为独立发布者注册
 */
class RuntimeDiagService : public Service {
public:
    explicit RuntimeDiagService(const std::string& name) : Service(name) {
        iface_.interface_id = OMNI_DIAG_IFACE_ID;
        iface_.name = name;
    }

    virtual const char* serviceName() const { return name().c_str(); }
    virtual const InterfaceInfo& interfaceInfo() const { return iface_; }

protected:
    virtual int onInvoke(uint32_t, const Buffer&, Buffer&) {
        return static_cast<int>(ErrorCode::ERR_NOT_SUPPORTED);
    }

private:
    InterfaceInfo iface_;
};

} // namespace

// ============================================================
// 诊断辅助
// ============================================================

std::string OmniRuntime::Impl::runtimeProcessName() const {
    std::string process_name = platform::getProcessName();
    if (!process_name.empty()) {
        return process_name;
    }

    std::ostringstream os;
    os << "pid-" << platform::getPid();
    return os.str();
}

std::string OmniRuntime::Impl::diagDataServiceName(uint32_t pid) const {
    std::ostringstream os;
    os << DIAG_SERVICE_NAME_PREFIX << pid;
    return os.str();
}

bool OmniRuntime::Impl::initDiagDataService() {
    if (diag_watch_topic_id_ != 0) {
        return true;
    }
    std::string name = diagDataServiceName(pid_);
    RuntimeDiagService* service = NULL;
    if (local_services_.empty()) {
        service = new RuntimeDiagService(name);
        service->setShmConfig(ShmConfig());
        int register_ret = registerServiceInternal(service);
        if (register_ret != 0) {
            delete service;
            return false;
        }
        diag_data_service_ = service;
    }
    int ret = publishTopicInternal(name, 0);
    if (ret != 0) {
        if (service) {
            unregisterServiceInternal(service);
            delete service;
            diag_data_service_ = NULL;
        }
        return false;
    }
    diag_watch_topic_id_ = fnv1a_32(name);
    return true;
}

void OmniRuntime::Impl::destroyDiagDataService() {
    std::string name = diagDataServiceName(pid_);
    if (diag_watch_topic_id_ != 0) {
        Message unpublish(MessageType::MSG_UNPUBLISH_TOPIC, allocSequence());
        unpublish.payload.writeString(name);
        sendToSM(unpublish);
        topic_runtime_.forgetPublishedTopic(name);
        diag_watch_topic_id_ = 0;
    }

    if (diag_data_service_) {
        Service* service = diag_data_service_;
        diag_data_service_ = NULL;
        unregisterServiceInternal(service);
        delete service;
    }
}

bool OmniRuntime::Impl::isDiagDataTopic(uint32_t topic_id) const {
    return diag_watch_topic_id_ != 0 && topic_id == diag_watch_topic_id_;
}

void OmniRuntime::Impl::emitDiagEvent(uint8_t direction, const Message& msg) {
    if (!diag_watch_active_) {
        return;
    }
    Buffer event_payload;
    diag_serialize_event(event_payload, direction, msg);
    if (diag_watch_topic_id_ != 0) {
        broadcastInternal(diag_watch_topic_id_, event_payload);
    }
}

// ============================================================
// 诊断 API
// ============================================================

int OmniRuntime::Impl::enableDiagnostic(const std::string& service_name) {
    Buffer req, resp;
    req.writeUint8(1);
    int ret = invoke(service_name, OMNI_DIAG_IFACE_ID, 1, 0, req, resp, 3000);
    if (ret != 0) return ret;
    if (resp.size() >= 1) return resp.data()[0];
    return -1;
}

int OmniRuntime::Impl::disableDiagnostic(const std::string& service_name) {
    Buffer req, resp;
    req.writeUint8(0);
    int ret = invoke(service_name, OMNI_DIAG_IFACE_ID, 1, 0, req, resp, 3000);
    if (ret != 0) return ret;
    if (resp.size() >= 1) return resp.data()[0];
    return -1;
}

int OmniRuntime::Impl::setLogLevelByPid(uint32_t pid, uint32_t level) {
    return callSerialized([this, pid, level]() -> int {
        Message msg(MessageType::MSG_DIAG_SET_LOG_LEVEL, allocSequence());
        msg.payload.writeUint32(pid);
        msg.payload.writeUint32(level);
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        bool ok = false;
        if (!decodeBoolReplyPayload(reply, ok)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        return ok ? 0 : static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    });
}

int OmniRuntime::Impl::listRuntimes(std::vector<RuntimeInfo>& runtimes) {
    return callSerialized([this, &runtimes]() -> int {
        Message msg(MessageType::MSG_RUNTIME_LIST, allocSequence());
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        BufferView buf(reply.payload.data(), reply.payload.size());
        uint32_t count = 0;
        if (!buf.tryReadUint32(count)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        runtimes.clear();
        for (uint32_t i = 0; i < count; ++i) {
            RuntimeInfo info;
            if (!deserializeRuntimeInfo(buf, info)) {
                return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
            }
            runtimes.push_back(info);
        }
        return 0;
    });
}

int OmniRuntime::Impl::requestDiagWatch(uint32_t pid) {
    Message msg(MessageType::MSG_DIAG_WATCH_START, allocSequence());
    msg.payload.writeUint32(pid);
    Message reply;
    int ret = sendSMRequestAndWaitReply(msg, reply);
    if (ret != 0) return ret;
    bool ok = false;
    if (!decodeBoolReplyPayload(reply, ok)) {
        return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
    }
    if (!ok) {
        return static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    }
    return 0;
}

int OmniRuntime::Impl::watchPid(uint32_t pid, const DiagEventCallback& callback) {
    return callSerialized([this, pid, &callback]() -> int {
        int ret = requestDiagWatch(pid);
        if (ret != 0) return ret;
        std::string topic_name = diagDataServiceName(pid);
        ret = subscribeTopicInternal(topic_name, 0,
            [callback](uint32_t, const Buffer& data) {
                if (callback) {
                    callback(data);
                }
            });
        if (ret != 0) {
            Message stop_msg(MessageType::MSG_DIAG_WATCH_STOP, allocSequence());
            stop_msg.payload.writeUint32(pid);
            Message stop_reply;
            sendSMRequestAndWaitReply(stop_msg, stop_reply);
            return ret;
        }
        // 本地登记 watcher：SM 重连后据此重放 WATCH_START（约束 6）
        DiagWatcherEntry& entry = diag_watchers_[pid];
        entry.callback = callback;
        entry.topic_name = topic_name;
        entry.active = true;
        return 0;
    });
}

int OmniRuntime::Impl::restoreDiagWatchers() {
    if (diag_watchers_.empty()) {
        return 0;
    }

    // 先拷贝待重放 pid 再遍历：requestDiagWatch 的 waitForReply 期间用户回调
    // 可能 watchPid/unwatchPid 修改 diag_watchers_（约束 2）
    std::vector<uint32_t> pids;
    pids.reserve(diag_watchers_.size());
    for (std::map<uint32_t, DiagWatcherEntry>::iterator it = diag_watchers_.begin();
         it != diag_watchers_.end(); ++it) {
        if (!it->second.active) {
            pids.push_back(it->first);
        }
    }

    for (size_t i = 0; i < pids.size(); ++i) {
        // 重放期间用户可能已 unwatch 移除登记（约束 1），逐项重查
        std::map<uint32_t, DiagWatcherEntry>::iterator it = diag_watchers_.find(pids[i]);
        if (it == diag_watchers_.end() || it->second.active) {
            continue;
        }
        int ret = requestDiagWatch(pids[i]);
        it = diag_watchers_.find(pids[i]);
        if (it == diag_watchers_.end()) {
            continue;
        }
        if (ret == 0) {
            it->second.active = true;
        } else {
            // 目标可能晚于 watcher 重连；不阻断恢复流程，心跳周期兜底重试
            OMNI_LOG_WARN(LOG_TAG, "diag_watch_restore_failed pid=%u err=%d", pids[i], ret);
        }
    }
    return 0;
}

void OmniRuntime::Impl::retryInactiveDiagWatchers() {
    if (diag_watchers_.empty()) {
        return;
    }
    restoreDiagWatchers();
}

int OmniRuntime::Impl::unwatchPid(uint32_t pid) {
    return callSerialized([this, pid]() -> int {
        diag_watchers_.erase(pid);
        unsubscribeTopicInternal(diagDataServiceName(pid));
        Message msg(MessageType::MSG_DIAG_WATCH_STOP, allocSequence());
        msg.payload.writeUint32(pid);
        Message reply;
        int ret = sendSMRequestAndWaitReply(msg, reply);
        if (ret != 0) return ret;
        bool ok = false;
        if (!decodeBoolReplyPayload(reply, ok)) {
            return static_cast<int>(ErrorCode::ERR_DESERIALIZE);
        }
        return ok ? 0 : static_cast<int>(ErrorCode::ERR_SERVICE_NOT_FOUND);
    });
}

} // namespace omnibinder
