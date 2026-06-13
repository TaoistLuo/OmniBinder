/**************************************************************************************************
 * @file        runtime.h
 * @brief       进程级运行时入口
 * @details     OmniRuntime 是 OmniBinder 框架的统一入口类，提供服务注册/注销、
 *              服务发现（lookup/list/queryInterfaces）、RPC 调用（invoke/invokeOneWay）、
 *              话题发布/订阅（publishTopic/broadcast/subscribeTopic）以及服务死亡通知
 *              等完整功能。内部通过 Pimpl 模式隐藏实现细节。
 *
 *              线程模型目标：
 *              - 对外提供线程安全公共 API
 *              - 对内由 owner event-loop 串行驱动核心状态
 *              - 回调默认在 owner event-loop 线程执行
 *              - `run()` / `pollOnce()` 不能由多个线程并发驱动同一实例
 *              - `stop()` 可以从任意线程安全调用
 *
 *              注意：详细语义见 docs/threading-model.md。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-02-11
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
#ifndef OMNIBINDER_RUNTIME_H
#define OMNIBINDER_RUNTIME_H

#include "omnibinder/types.h"
#include "omnibinder/buffer.h"
#include "omnibinder/error.h"
#include <string>
#include <vector>
#include <functional>

namespace omnibinder {

class Service;

typedef std::function<void(const std::string& service_name)> DeathCallback;
typedef std::function<void(uint32_t topic_id, const Buffer& data)> TopicCallback;
typedef std::function<void(uint32_t topic_id, ErrorCode error, const Buffer& raw_data)> TopicErrorCallback;

class OmniRuntime {
public:
    OmniRuntime();
    ~OmniRuntime();

    int init(const std::string& sm_host, uint16_t sm_port = 9900);
    void run();
    void stop();
    bool isRunning() const;
    void pollOnce(int timeout_ms = 0);

    int registerService(Service* service);
    int unregisterService(Service* service);

    int lookupService(const std::string& service_name, ServiceInfo& info);
    int listServices(std::vector<ServiceInfo>& services);
    int queryInterfaces(const std::string& service_name, std::vector<InterfaceInfo>& interfaces);

    int connectService(const std::string& service_name);
    int disconnectService(const std::string& service_name);
    bool isServiceConnected(const std::string& service_name) const;
    void enableAutoReconnect(const std::string& service_name, bool enable = true);
    void setReconnectInterval(const std::string& service_name, uint32_t interval_ms);
    void startHeartbeat(const std::string& service_name, uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000);
    void stopHeartbeat(const std::string& service_name);

    int invoke(const std::string& service_name, uint32_t interface_id, uint32_t method_id,
               uint32_t idl_hash, const Buffer& request, Buffer& response,
               uint32_t timeout_ms = 0);
    int invokeOneWay(const std::string& service_name, uint32_t interface_id, uint32_t method_id,
                     uint32_t idl_hash, const Buffer& request);

    int subscribeServiceDeath(const std::string& service_name, const DeathCallback& callback);
    int unsubscribeServiceDeath(const std::string& service_name);

    int publishTopic(const std::string& topic_name);
    int broadcast(uint32_t topic_id, const Buffer& data);
    int subscribeTopic(const std::string& topic_name, const TopicCallback& on_message,
                       const TopicErrorCallback& on_error);
    int unsubscribeTopic(const std::string& topic_name);

    void setRegisterHost(const std::string& host);
    const std::string& getRegisterHost() const;

    void setHeartbeatInterval(uint32_t interval_ms);
    void setDefaultTimeout(uint32_t timeout_ms);
    const std::string& hostId() const;
    int getStats(RuntimeStats& stats);
    int resetStats();
    void clearServiceCache();
    void closeAllConnections();

    int enableDiagnostic(const std::string& service_name);
    int disableDiagnostic(const std::string& service_name);

private:
    OmniRuntime(const OmniRuntime&);
    OmniRuntime& operator=(const OmniRuntime&);

    class Impl;
    Impl* impl_;
};


} // namespace omnibinder

#endif
