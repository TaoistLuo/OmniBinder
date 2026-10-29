#include "service_manager_app.h"
#include "omnibinder/log.h"
#include "platform/platform.h"
#include <algorithm>

#define TAG "ServiceManager"

namespace omnibinder {

void ServiceManagerApp::removePidFd(uint32_t pid, int fd) {
    std::map<uint32_t, std::vector<int> >::iterator it = pid_to_fds_.find(pid);
    if (it == pid_to_fds_.end()) {
        return;
    }
    std::vector<int>& fds = it->second;
    fds.erase(std::remove(fds.begin(), fds.end(), fd), fds.end());
    if (fds.empty()) {
        pid_to_fds_.erase(it);
    }
}

bool ServiceManagerApp::forEachPidConn(
    uint32_t pid, int except_fd,
    const std::function<bool(int fd, ClientConnection* conn)>& visitor) {
    std::map<uint32_t, std::vector<int> >::iterator pit = pid_to_fds_.find(pid);
    if (pit == pid_to_fds_.end()) {
        return false;
    }
    // 先拷贝 fd 列表再遍历：visitor 内 sendMessage 失败可能 closeClient ->
    // removePidFd 修改甚至删除 pid_to_fds_ 条目，直接迭代属于未定义行为（约束 2）
    std::vector<int> fds = pit->second;
    bool handled = false;
    for (size_t i = 0; i < fds.size(); ++i) {
        int fd = fds[i];
        std::map<int, ClientConnection*>::iterator cit = clients_.find(fd);
        if (cit == clients_.end() || fd == except_fd) {
            continue;
        }
        if (visitor(fd, cit->second)) {
            handled = true;
        }
    }
    return handled;
}

void ServiceManagerApp::handleRuntimeHello(ClientConnection* conn, const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    RuntimeInfo info;
    SessionHelloAck ack;
    ack.peer_incarnation_id = sm_incarnation_id_;

    if (!deserializeRuntimeInfo(buf, info) || info.pid == 0
        || info.session_version != SESSION_PROTOCOL_VERSION) {
        ack.status = SESSION_STATUS_REJECTED;
        Message reply(MessageType::MSG_RUNTIME_HELLO_REPLY, msg.header.sequence);
        serializeSessionHelloAck(ack, reply.payload);
        sendMessage(conn, reply);
        return;
    }

    const uint32_t requested_interval = info.heartbeat_interval_ms;
    const uint32_t interval = clampHeartbeatInterval(requested_interval);
    // requested==0 表示"由服务端默认值裁定"：此时服务端默认值可直接生效，
    // 不再抬到 interval*3（运维用小 timeout 做快速故障演练/测试）。
    uint32_t effective_timeout = 0;
    if (info.requested_timeout_ms == 0) {
        effective_timeout = default_session_timeout_ms_;
        if (max_session_timeout_ms_ != 0 && effective_timeout > max_session_timeout_ms_) {
            effective_timeout = max_session_timeout_ms_;
        }
        if (effective_timeout == 0) {
            effective_timeout = negotiateSessionTimeout(interval, 0, max_session_timeout_ms_);
        }
    } else {
        effective_timeout = negotiateSessionTimeout(
            interval, info.requested_timeout_ms, max_session_timeout_ms_);
    }

    std::string runtime_id = info.runtime_id.empty() ? info.process_name : info.runtime_id;
    if (runtime_id.empty()) {
        runtime_id = "runtime-" + std::to_string(info.pid);
    }
    if (runtime_id.size() > MAX_SERVICE_NAME_LENGTH
        || info.host_id.size() > MAX_SERVICE_NAME_LENGTH) {
        OMNI_LOG_WARN(TAG, "Reject runtime hello with oversized identity fd=%d", conn->fd);
        ack.status = SESSION_STATUS_REJECTED;
        Message reply(MessageType::MSG_RUNTIME_HELLO_REPLY, msg.header.sequence);
        serializeSessionHelloAck(ack, reply.payload);
        sendMessage(conn, reply);
        return;
    }
    const std::string session_key = makeRuntimeSessionKey(info.host_id, runtime_id);

    // 同一 (host_id, runtime_id) 的新会话顶替旧会话。incarnation 相同说明是同一进程
    // 重连（不触发死亡通知）；不同说明进程重启（保留死亡通知语义）。
    std::map<std::string, int>::iterator sit = runtime_sessions_.find(session_key);
    if (sit != runtime_sessions_.end() && sit->second != conn->fd) {
        int old_fd = sit->second;
        std::map<int, ClientConnection*>::iterator oit = clients_.find(old_fd);
        bool same_incarnation = false;
        if (oit != clients_.end() && oit->second) {
            same_incarnation = (oit->second->incarnation_id == info.incarnation_id);
        }
        OMNI_LOG_INFO(TAG, "Session superseded runtime_id=%s old_fd=%d new_fd=%d same_inc=%d",
                      runtime_id.c_str(), old_fd, conn->fd, same_incarnation ? 1 : 0);
        closeClient(old_fd, !same_incarnation);
        // 死亡通知回调可能重入并关闭当前连接（约束 1）：重新判活后再继续
        if (clients_.find(conn->fd) == clients_.end()) {
            return;
        }
    }
    runtime_sessions_[session_key] = conn->fd;

    if (conn->runtime_registered && conn->pid != info.pid) {
        removePidFd(conn->pid, conn->fd);
    }
    conn->pid = info.pid;
    conn->process_name = info.process_name;
    conn->log_level = info.log_level;
    conn->runtime_registered = true;
    conn->runtime_id = runtime_id;
    conn->host_id = info.host_id;
    conn->session_key = session_key;
    conn->incarnation_id = info.incarnation_id;
    conn->session_interval_ms = interval;
    conn->session_timeout_ms = effective_timeout;
    conn->last_heartbeat_ms = platform::currentTimeMs();
    conn->hello_received = true;
    if (conn->hello_timer_id > 0) {
        loop_.cancelTimer(conn->hello_timer_id);
        conn->hello_timer_id = 0;
    }
    armSessionDeadline(conn);

    std::vector<int>& fds = pid_to_fds_[info.pid];
    if (std::find(fds.begin(), fds.end(), conn->fd) == fds.end()) {
        fds.push_back(conn->fd);
    }

    ack.status = (interval == requested_interval
                  && (info.requested_timeout_ms == 0
                      || effective_timeout == info.requested_timeout_ms))
        ? SESSION_STATUS_OK : SESSION_STATUS_CLAMPED;
    ack.accepted_interval_ms = interval;
    ack.effective_timeout_ms = effective_timeout;

    Message reply(MessageType::MSG_RUNTIME_HELLO_REPLY, msg.header.sequence);
    if (!serializeSessionHelloAck(ack, reply.payload)) {
        closeClient(conn->fd);
        return;
    }
    sendMessage(conn, reply);

    OMNI_LOG_INFO(TAG, "Runtime hello: pid=%u process=%s runtime_id=%s fd=%d "
                  "incarnation=%u interval=%u timeout=%u status=%u",
                  info.pid, info.process_name.c_str(), runtime_id.c_str(), conn->fd,
                  info.incarnation_id, interval, effective_timeout, ack.status);
}

void ServiceManagerApp::handleRuntimeList(ClientConnection* conn, const Message& msg) {
    Message reply(MessageType::MSG_RUNTIME_LIST_REPLY, msg.header.sequence);
    // 先按 pid 聚合：同一进程可能有多条控制连接（同进程多 runtime，或重连后旧连接
    // 尚未回收），runtime list 以进程为行，合并各连接名下服务的并集，避免同 pid
    // 重复列出多行。服务归属唯一来源是 registry fd 索引，不再依赖 conn 上的镜像列表。
    std::map<uint32_t, RuntimeInfo> by_pid;
    std::vector<uint32_t> pid_order;
    for (std::map<int, ClientConnection*>::const_iterator it = clients_.begin(); it != clients_.end(); ++it) {
        ClientConnection* runtime_conn = it->second;
        if (!runtime_conn || !runtime_conn->runtime_registered) {
            continue;
        }
        std::map<uint32_t, RuntimeInfo>::iterator pit = by_pid.find(runtime_conn->pid);
        if (pit == by_pid.end()) {
            RuntimeInfo info;
            info.pid = runtime_conn->pid;
            info.process_name = runtime_conn->process_name;
            info.log_level = runtime_conn->log_level;
            info.diag_capabilities = RUNTIME_DIAG_CAP_WATCH;
            info.session_version = SESSION_PROTOCOL_VERSION;
            info.runtime_id = runtime_conn->runtime_id;
            info.incarnation_id = runtime_conn->incarnation_id;
            info.heartbeat_interval_ms = runtime_conn->session_interval_ms;
            info.requested_timeout_ms = runtime_conn->session_timeout_ms;
            info.host_id = runtime_conn->host_id;
            pit = by_pid.insert(std::make_pair(runtime_conn->pid, info)).first;
            pid_order.push_back(runtime_conn->pid);
        }
        RuntimeInfo& info = pit->second;
        const std::vector<std::string> services = registry_.listServiceNamesByFd(it->first);
        for (size_t si = 0; si < services.size(); ++si) {
            const std::string& service = services[si];
            if (service.find(DIAG_SERVICE_NAME_PREFIX) == 0) {
                continue;
            }
            if (std::find(info.services.begin(), info.services.end(), service)
                == info.services.end()) {
                info.services.push_back(service);
            }
        }
    }

    // 先完整序列化每个条目到独立缓冲，任一失败则跳过该条目并回退写游标，
    // 避免向调用方发送"数量与实际条目不一致"的截断帧
    Buffer entries;
    uint32_t entry_count = 0;
    for (size_t i = 0; i < pid_order.size(); ++i) {
        RuntimeInfo& info = by_pid[pid_order[i]];
        info.role = info.services.empty() ? "client" : "service";
        const size_t before = entries.writePosition();
        if (!serializeRuntimeInfo(info, entries)) {
            entries.setWritePosition(before);
            OMNI_LOG_ERROR(TAG, "Failed to serialize runtime list entry pid=%u, skipped", info.pid);
            continue;
        }
        entry_count++;
    }
    reply.payload.writeUint32(entry_count);
    if (entries.writePosition() > 0) {
        reply.payload.writeRaw(entries.data(), entries.writePosition());
    }
    sendMessage(conn, reply);
}

void ServiceManagerApp::handleDiagSetLogLevel(ClientConnection* conn, const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t pid = 0;
    uint32_t level = 0;
    if (!buf.tryReadUint32(pid) || !buf.tryReadUint32(level) || level > static_cast<uint32_t>(OMNI_LOG_OFF)) {
        sendBoolReply(conn, MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY, msg.header.sequence, false);
        return;
    }
    const int requester_fd = conn->fd;
    bool sent = forEachPidConn(pid, requester_fd,
        [this, level](int fd, ClientConnection* target) -> bool {
            Message ctrl(MessageType::MSG_DIAG_SET_LOG_LEVEL, nextSMProactiveSequence());
            ctrl.payload.writeUint32(level);
            sendMessage(target, ctrl);
            // sendMessage 失败时可能 closeClient(fd)（删除 conn 并 erase clients_），
            // 需按 fd 重新查询确认存活后再访问，避免 UAF
            std::map<int, ClientConnection*>::iterator cit = clients_.find(fd);
            if (cit == clients_.end()) {
                return false;
            }
            cit->second->log_level = level;
            return true;
        });
    // forEachPidConn 内发送失败可级联重入 closeClient(requester_fd)（约束 1），回复前判活
    std::map<int, ClientConnection*>::iterator requester_it = clients_.find(requester_fd);
    if (requester_it == clients_.end()) {
        return;
    }
    sendBoolReply(requester_it->second, MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY,
                  msg.header.sequence, sent);
}

void ServiceManagerApp::handleDiagWatchStart(ClientConnection* conn, const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t pid = 0;
    if (!buf.tryReadUint32(pid)) {
        sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_START_REPLY, msg.header.sequence, false);
        return;
    }
    const int watcher_fd = conn->fd;
    bool sent = forEachPidConn(pid, watcher_fd,
        [this](int fd, ClientConnection* target) -> bool {
            Message ctrl(MessageType::MSG_DIAG_WATCH_START, nextSMProactiveSequence());
            sendMessage(target, ctrl);
            // sendMessage 失败会 closeClient(target)：按 fd 重查，目标已失效视为未成功
            return clients_.find(fd) != clients_.end();
        });
    // forEachPidConn 内 sendMessage 失败可重入 closeClient(watcher_fd)（约束 1）
    if (clients_.find(watcher_fd) == clients_.end()) {
        return;
    }
    if (sent) {
        watcher_to_pids_[watcher_fd].insert(pid);
        std::vector<int>& watchers = pid_watchers_[pid];
        if (std::find(watchers.begin(), watchers.end(), watcher_fd) == watchers.end()) {
            watchers.push_back(watcher_fd);
        }
    }
    sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_START_REPLY, msg.header.sequence, sent);
}

void ServiceManagerApp::sendDiagWatchStopToPid(uint32_t pid, int except_fd) {
    forEachPidConn(pid, except_fd,
        [this](int, ClientConnection* target) -> bool {
            Message ctrl(MessageType::MSG_DIAG_WATCH_STOP, nextSMProactiveSequence());
            sendMessage(target, ctrl);
            return true;
        });
}

void ServiceManagerApp::stopTargetIfUnwatched(uint32_t pid, int watcher_fd) {
    std::map<uint32_t, std::vector<int> >::iterator pit = pid_watchers_.find(pid);
    if (pit != pid_watchers_.end()) {
        std::vector<int>& watchers = pit->second;
        watchers.erase(std::remove(watchers.begin(), watchers.end(), watcher_fd), watchers.end());
        if (!watchers.empty()) {
            return;
        }
        pid_watchers_.erase(pit);
    }
    sendDiagWatchStopToPid(pid, watcher_fd);
}

bool ServiceManagerApp::removeWatcherPair(int watcher_fd, uint32_t pid) {
    std::map<int, std::set<uint32_t> >::iterator wit = watcher_to_pids_.find(watcher_fd);
    if (wit == watcher_to_pids_.end() || wit->second.erase(pid) == 0) {
        return false;
    }
    if (wit->second.empty()) {
        watcher_to_pids_.erase(wit);
    }
    stopTargetIfUnwatched(pid, watcher_fd);
    return true;
}

void ServiceManagerApp::removeWatcherAndMaybeStopTargets(int watcher_fd) {
    std::map<int, std::set<uint32_t> >::iterator wit = watcher_to_pids_.find(watcher_fd);
    if (wit == watcher_to_pids_.end()) {
        return;
    }
    // 先拷贝 pid 集合并摘除映射，再下发 WATCH_STOP：下发过程可能重入 closeClient（约束 2）
    std::set<uint32_t> pids = wit->second;
    watcher_to_pids_.erase(wit);
    for (std::set<uint32_t>::const_iterator it = pids.begin(); it != pids.end(); ++it) {
        stopTargetIfUnwatched(*it, watcher_fd);
    }
}

void ServiceManagerApp::removeTargetWatchers(uint32_t pid) {
    std::map<uint32_t, std::vector<int> >::iterator pit = pid_watchers_.find(pid);
    if (pit == pid_watchers_.end()) {
        return;
    }
    // 先拷贝 watcher 列表再摘除反向索引，避免删除过程中迭代失效
    std::vector<int> watchers = pit->second;
    pid_watchers_.erase(pit);
    for (size_t i = 0; i < watchers.size(); ++i) {
        std::map<int, std::set<uint32_t> >::iterator wit = watcher_to_pids_.find(watchers[i]);
        if (wit == watcher_to_pids_.end()) {
            continue;
        }
        wit->second.erase(pid);
        if (wit->second.empty()) {
            watcher_to_pids_.erase(wit);
        }
    }
}

void ServiceManagerApp::handleDiagWatchStop(ClientConnection* conn, const Message& msg) {
    BufferView buf(msg.payload.data(), msg.payload.size());
    uint32_t pid = 0;
    if (!buf.tryReadUint32(pid)) {
        sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_STOP_REPLY, msg.header.sequence, false);
        return;
    }
    const int watcher_fd = conn->fd;
    // 只解除 (watcher_fd, pid) 这一条关系；该 pid 若无其它 watcher 才停止目标连接
    removeWatcherPair(watcher_fd, pid);
    // stopTargetIfUnwatched 内 sendMessage 失败可重入 closeClient(watcher_fd)（约束 1）
    if (clients_.find(watcher_fd) == clients_.end()) {
        return;
    }
    sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_STOP_REPLY, msg.header.sequence, true);
}

} // namespace omnibinder
