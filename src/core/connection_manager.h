/**************************************************************************************************
 * @file        connection_manager.h
 * @brief       服务连接管理器
 * @details     管理 OmniRuntime 与远程服务之间的直连（TCP/SHM）。负责按需创建连接、
 *              缓存已建立的连接、接收并解析消息、检测断开事件。SHM 连接通过
 *              eventfd 事件通知集成到 EventLoop，与 TCP 连接统一的事件驱动模型。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef OMNIBINDER_CONNECTION_MANAGER_H
#define OMNIBINDER_CONNECTION_MANAGER_H

#include "omnibinder/types.h"
#include "omnibinder/buffer.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include "platform/platform.h"
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <atomic>

namespace omnibinder {

class EventLoop;

// ============================================================
// 单个服务连接的上下文
// ============================================================
struct ServiceConnection {
    std::string     service_name;
    std::string     host;
    uint16_t        port;
    std::string     host_id;
    ITransport*     transport;
    Buffer          recv_buffer;
    bool            connected;

    ServiceConnection()
        : port(0), transport(NULL), connected(false) {}

    ~ServiceConnection() {
        if (transport) {
            transport->close();
            delete transport;
            transport = NULL;
        }
    }

private:
    ServiceConnection(const ServiceConnection&);
    ServiceConnection& operator=(const ServiceConnection&);
};

// ============================================================
// ConnectionManager - 管理与其他服务的直连
// ============================================================
class ConnectionManager {
public:
    typedef std::function<void(const std::string& service_name,
                               const Message& msg)> MessageCallback;
    typedef std::function<void(const std::string& service_name)> DisconnectCallback;

    ConnectionManager(EventLoop& loop, const std::string& local_host_id);
    ~ConnectionManager();

    // 获取或创建到指定服务的连接
    ServiceConnection* getOrCreateConnection(
        const std::string& service_name,
        const std::string& host,
        uint16_t port,
        const std::string& host_id,
        const std::string& shm_name = "",
        const ShmConfig& shm_config = ShmConfig());

    // 获取已有连接
    ServiceConnection* getConnection(const std::string& service_name);

    // 移除连接
    void removeConnection(const std::string& service_name);

    // 通过连接发送消息
    bool sendMessage(const std::string& service_name, const Message& msg);

    // 通过连接发送原始数据
    bool sendRaw(ServiceConnection* conn, const uint8_t* data, size_t length);

    // 设置消息回调
    void setMessageCallback(const MessageCallback& cb);

    // 设置断开回调
    void setDisconnectCallback(const DisconnectCallback& cb);

    // 关闭所有连接
    void closeAll();

    // 获取所有连接的服务名
    std::vector<std::string> connectedServices() const;
    uint32_t activeConnectionCount() const;
    uint32_t tcpConnectionCount() const;
    uint32_t shmConnectionCount() const;

private:
    ConnectionManager(const ConnectionManager&);
    ConnectionManager& operator=(const ConnectionManager&);

    void onConnectionData(const std::string& service_name, int fd);
    void processMessages(ServiceConnection* conn);

    EventLoop&      loop_;
    std::string     local_host_id_;
    std::map<std::string, ServiceConnection*> connections_;
    MessageCallback     message_cb_;
    DisconnectCallback  disconnect_cb_;
};

} // namespace omnibinder

#endif // OMNIBINDER_CONNECTION_MANAGER_H
