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

 {
        BufferView buf(msg.payload.data(), msg.payload.size());
        RuntimeInfo info;
        if (!deserializeRuntimeInfo(buf, info) || info.pid == 0) {
            sendBoolReply(conn, MessageType::MSG_RUNTIME_HELLO_REPLY, msg.header.sequence, false);
            return;
        }
        if (conn->runtime_registered && conn->pid != info.pid) {
            removePidFd(conn->pid, conn->fd);
        }
        conn->pid = info.pid;
        conn->process_name = info.process_name;
        conn->log_level = info.log_level;
        conn->runtime_registered = true;
        std::vector<int>& fds = pid_to_fds_[info.pid];
        if (std::find(fds.begin(), fds.end(), conn->fd) == fds.end()) {
            fds.push_back(conn->fd);
        }
        OMNI_LOG_INFO(TAG, "Runtime hello: pid=%u process=%s fd=%d log=%u",
                      info.pid, info.process_name.c_str(), conn->fd, info.log_level);
        sendBoolReply(conn, MessageType::MSG_RUNTIME_HELLO_REPLY, msg.header.sequence, true);
}

 {
        Message reply(MessageType::MSG_RUNTIME_LIST_REPLY, msg.header.sequence);
        uint32_t count = 0;
        for (std::map<int, ClientConnection*>::const_iterator it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->second && it->second->runtime_registered) {
                count++;
            }
        }
        reply.payload.writeUint32(count);
        for (std::map<int, ClientConnection*>::const_iterator it = clients_.begin(); it != clients_.end(); ++it) {
            ClientConnection* runtime_conn = it->second;
            if (!runtime_conn || !runtime_conn->runtime_registered) {
                continue;
            }
            RuntimeInfo info;
            info.pid = runtime_conn->pid;
            info.process_name = runtime_conn->process_name;
            for (size_t si = 0; si < runtime_conn->registered_services.size(); ++si) {
                const std::string& service = runtime_conn->registered_services[si];
                if (service.find("__diag_pid_") == 0) {
                    continue;
                }
                info.services.push_back(service);
            }
            info.role = info.services.empty() ? "client" : "service";
            info.log_level = runtime_conn->log_level;
            info.diag_capabilities = RUNTIME_DIAG_CAP_WATCH;
            serializeRuntimeInfo(info, reply.payload);
        }
        sendMessage(conn, reply);
}

 {
        BufferView buf(msg.payload.data(), msg.payload.size());
        uint32_t pid = 0;
        uint32_t level = 0;
        if (!buf.tryReadUint32(pid) || !buf.tryReadUint32(level) || level > static_cast<uint32_t>(OMNI_LOG_OFF)) {
            sendBoolReply(conn, MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY, msg.header.sequence, false);
            return;
        }
        std::map<uint32_t, std::vector<int> >::iterator pit = pid_to_fds_.find(pid);
        if (pit == pid_to_fds_.end()) {
            sendBoolReply(conn, MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY, msg.header.sequence, false);
            return;
        }
        bool sent = false;
        for (size_t i = 0; i < pit->second.size(); ++i) {
            std::map<int, ClientConnection*>::iterator cit = clients_.find(pit->second[i]);
            if (cit == clients_.end() || cit->second == conn) {
                continue;
            }
            Message ctrl(MessageType::MSG_DIAG_SET_LOG_LEVEL, nextSequenceNumber());
            ctrl.payload.writeUint32(level);
            sendMessage(cit->second, ctrl);
            cit->second->log_level = level;
            sent = true;
        }
        sendBoolReply(conn, MessageType::MSG_DIAG_SET_LOG_LEVEL_REPLY, msg.header.sequence, sent);
}

 {
        BufferView buf(msg.payload.data(), msg.payload.size());
        uint32_t pid = 0;
        if (!buf.tryReadUint32(pid)) {
            sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_START_REPLY, msg.header.sequence, false);
            return;
        }
        std::map<uint32_t, std::vector<int> >::iterator pit = pid_to_fds_.find(pid);
        if (pit == pid_to_fds_.end()) {
            sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_START_REPLY, msg.header.sequence, false);
            return;
        }
        bool sent = false;
        for (size_t i = 0; i < pit->second.size(); ++i) {
            std::map<int, ClientConnection*>::iterator cit = clients_.find(pit->second[i]);
            if (cit == clients_.end() || cit->second == conn) {
                continue;
            }
            Message ctrl(MessageType::MSG_DIAG_WATCH_START, nextSequenceNumber());
            sendMessage(cit->second, ctrl);
            sent = true;
        }
        if (sent) {
            watcher_to_pid_[conn->fd] = pid;
            std::vector<int>& watchers = pid_watchers_[pid];
            if (std::find(watchers.begin(), watchers.end(), conn->fd) == watchers.end()) {
                watchers.push_back(conn->fd);
            }
        }
        sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_START_REPLY, msg.header.sequence, sent);
}

 {
        std::map<uint32_t, std::vector<int> >::iterator pit = pid_to_fds_.find(pid);
        if (pit == pid_to_fds_.end()) {
            return;
        }
        for (size_t i = 0; i < pit->second.size(); ++i) {
            std::map<int, ClientConnection*>::iterator cit = clients_.find(pit->second[i]);
            if (cit == clients_.end() || cit->second->fd == except_fd) {
                continue;
            }
            Message ctrl(MessageType::MSG_DIAG_WATCH_STOP, nextSequenceNumber());
            sendMessage(cit->second, ctrl);
        }
}

 {
        std::map<int, uint32_t>::iterator watch_it = watcher_to_pid_.find(watcher_fd);
        if (watch_it == watcher_to_pid_.end()) {
            return;
        }
        uint32_t watched_pid = watch_it->second;
        bool should_stop = false;
        std::map<uint32_t, std::vector<int> >::iterator pit = pid_watchers_.find(watched_pid);
        if (pit != pid_watchers_.end()) {
            std::vector<int>& watchers = pit->second;
            watchers.erase(std::remove(watchers.begin(), watchers.end(), watcher_fd), watchers.end());
            if (watchers.empty()) {
                pid_watchers_.erase(pit);
                should_stop = true;
            }
        } else {
            should_stop = true;
        }
        watcher_to_pid_.erase(watch_it);
        if (should_stop) {
            sendDiagWatchStopToPid(watched_pid, watcher_fd);
        }
}

 {
        BufferView buf(msg.payload.data(), msg.payload.size());
        uint32_t pid = 0;
        if (!buf.tryReadUint32(pid)) {
            sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_STOP_REPLY, msg.header.sequence, false);
            return;
        }
        (void)pid;
        removeWatcherAndMaybeStopTarget(conn->fd);
        sendBoolReply(conn, MessageType::MSG_DIAG_WATCH_STOP_REPLY, msg.header.sequence, true);
}

