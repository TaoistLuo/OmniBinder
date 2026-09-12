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

/*
 * @brief  单个服务连接的上下文
 */
struct ServiceConnection {
    std::string     service_name;
    IClientTransport*     transport;
    Buffer          recv_buffer;
    bool            connected;

    ServiceConnection()
        : transport(NULL), connected(false) {}

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

/*
 * @brief  ConnectionManager - 管理与其他服务的直连
 */
class ConnectionManager {
public:
    typedef std::function<void(const std::string& service_name,
                               Message& msg)> MessageCallback;
    typedef std::function<void(std::string service_name)> DisconnectCallback;

    ConnectionManager(EventLoop& loop, const std::string& local_host_id);
    ~ConnectionManager();

    /*
     * @brief  获取或创建到指定服务的连接
     */
    ServiceConnection* getOrCreateConnection(
        const std::string& service_name,
        const std::string& host,
        uint16_t port,
        const std::string& host_id,
        const ShmConfig& shm_config = ShmConfig());

    /*
     * @brief  获取已有连接
     */
    ServiceConnection* getConnection(const std::string& service_name);

    /*
     * @brief  移除连接
     */
    void removeConnection(const std::string& service_name);

    /*
     * @brief  通过连接发送消息（原地序列化：成功后 msg.payload 即完整线上帧）
     */
    bool sendMessage(const std::string& service_name, Message& msg);

    /*
     * @brief  通过连接发送原始数据
     */
    bool sendRaw(ServiceConnection* conn, const uint8_t* data, size_t length);

    /*
     * @brief  在超时内将完整消息同步发送出去
     */
    bool sendMessageWithinTimeout(const std::string& service_name, Message& msg,
                                  uint32_t timeout_ms, uint32_t* elapsed_ms = NULL);

    /*
     * @brief  在超时内将完整原始数据同步发送出去
     */
    bool sendRawWithinTimeout(ServiceConnection* conn, const uint8_t* data, size_t length,
                              uint32_t timeout_ms, uint32_t* elapsed_ms = NULL);

    /*
     * @brief  在 deadline 时间戳前发送数据，0 表示无 deadline
     */
    bool sendRawWithDeadline(ServiceConnection* conn, const uint8_t* data, size_t length,
                             uint32_t deadline_ms);

    /*
     * @brief  设置消息回调
     */
    void setMessageCallback(const MessageCallback& cb);

    /*
     * @brief  设置断开回调
     */
    void setDisconnectCallback(const DisconnectCallback& cb);

    /*
     * @brief  关闭所有连接
     */
    void closeAll();

    void connectionCounts(uint32_t& active, uint32_t& tcp, uint32_t& shm) const;

private:
    ConnectionManager(const ConnectionManager&);
    ConnectionManager& operator=(const ConnectionManager&);

    /*
     * @brief  连接失败统一处理：connected=false → removeFd → disconnect_cb_ → 判活
     * @return false 表示回调链已删除该连接，调用方须立即停止使用 conn（约束 1/3）
     */
    bool failConnection(ServiceConnection* conn);

    void onConnectionData(const std::string& service_name, int fd);

    EventLoop&      loop_;
    std::string     local_host_id_;
    std::map<std::string, ServiceConnection*> connections_;
    MessageCallback     message_cb_;
    DisconnectCallback  disconnect_cb_;
};

} // namespace omnibinder

#endif // OMNIBINDER_CONNECTION_MANAGER_H
