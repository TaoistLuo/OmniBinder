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
 *              注意：当前发布态仍在向该模型演进，详细语义见 docs/threading-model.md。
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

    int invoke(const std::string& service_name, uint32_t interface_id, uint32_t method_id,
               const Buffer& request, Buffer& response, uint32_t timeout_ms = 0);
    int invokeOneWay(const std::string& service_name, uint32_t interface_id, uint32_t method_id,
                     const Buffer& request);

    int subscribeServiceDeath(const std::string& service_name, const DeathCallback& callback);
    int unsubscribeServiceDeath(const std::string& service_name);

    int publishTopic(const std::string& topic_name);
    int broadcast(uint32_t topic_id, const Buffer& data);
    int subscribeTopic(const std::string& topic_name, const TopicCallback& callback);
    int unsubscribeTopic(const std::string& topic_name);

    void setHeartbeatInterval(uint32_t interval_ms);
    void setDefaultTimeout(uint32_t timeout_ms);
    const std::string& hostId() const;
    int getStats(RuntimeStats& stats);
    void resetStats();

private:
    OmniRuntime(const OmniRuntime&);
    OmniRuntime& operator=(const OmniRuntime&);

    class Impl;
    Impl* impl_;
};


} // namespace omnibinder

#endif
