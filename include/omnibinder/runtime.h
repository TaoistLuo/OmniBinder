/**************************************************************************************************
 * @file        runtime.h
 * @brief       进程级运行时入口 — 控制面 + 数据面 + 诊断的统一 API
 *
 * 线程模型：
 *   - 对外提供线程安全公共 API
 *   - 内部由 owner event-loop 串行驱动
 *   - 回调在 owner event-loop 线程执行
 *   - `stop()` 可从任意线程安全调用
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-02-11
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
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
typedef std::function<void(const Buffer& data)> DiagEventCallback;

class OmniRuntime {
public:
    OmniRuntime();
    ~OmniRuntime();

    // ============================================================
    // 生命周期
    // ============================================================
    int  init(const std::string& sm_host, uint16_t sm_port = 9900);
    void run();
    void stop();
    bool isRunning() const;
    void pollOnce(int timeout_ms = 0);

    // ============================================================
    // 控制面 — 与 ServiceManager 交互
    // ============================================================

    // 服务注册/发现
    int registerService(Service* service);
    int unregisterService(Service* service);
    int lookupService(const std::string& name, ServiceInfo& info);
    int listServices(std::vector<ServiceInfo>& services);
    int queryInterfaces(const std::string& name, std::vector<InterfaceInfo>& ifaces);
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics);
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics,
                             uint32_t timeout_ms);

    // 话题声明（向 SM 注册发布/订阅关系）
    int publishTopic(const std::string& topic_name);
    int subscribeTopic(const std::string& topic_name, const TopicCallback& on_msg,
                       const TopicErrorCallback& on_err);
    int unsubscribeTopic(const std::string& topic_name);

    // 死亡通知订阅
    int subscribeServiceDeath(const std::string& name, const DeathCallback& cb);
    int unsubscribeServiceDeath(const std::string& name);

    // ============================================================
    // 数据面 — 服务间直连通信
    // ============================================================

    // 连接管理
    int  connectService(const std::string& name);
    int  disconnectService(const std::string& name);
    bool isServiceConnected(const std::string& name) const;
    void enableAutoReconnect(const std::string& name, bool enable = true);
    void setReconnectInterval(const std::string& name, uint32_t interval_ms);

    // 心跳
    void startHeartbeat(const std::string& name, uint32_t interval_ms = 5000,
                        uint32_t timeout_ms = 10000);
    void stopHeartbeat(const std::string& name);

    // RPC 调用
    int invoke(const std::string& name, uint32_t iface_id, uint32_t method_id,
               uint32_t idl_hash, const Buffer& req, Buffer& resp, uint32_t timeout_ms = 0);
    int invokeOneWay(const std::string& name, uint32_t iface_id, uint32_t method_id,
                     uint32_t idl_hash, const Buffer& req);

    // 话题广播（数据面直连，不经过 SM）
    int broadcast(uint32_t topic_id, const Buffer& data);

    // ============================================================
    // 诊断 — 运行时可观测性
    // ============================================================
    int  enableDiagnostic(const std::string& name);
    int  disableDiagnostic(const std::string& name);
    int  setLogLevelByPid(uint32_t pid, uint32_t level);
    int  listRuntimes(std::vector<RuntimeInfo>& runtimes);
    int  watchPid(uint32_t pid, const DiagEventCallback& cb);
    int  unwatchPid(uint32_t pid);
    int  getStats(RuntimeStats& stats);
    int  resetStats();
    void clearServiceCache();
    void closeAllConnections();

    // ============================================================
    // 配置
    // ============================================================
    void setRegisterHost(const std::string& host);
    const std::string& getRegisterHost() const;
    void setHeartbeatInterval(uint32_t ms);
    void setDefaultTimeout(uint32_t ms);
    const std::string& hostId() const;

private:
    OmniRuntime(const OmniRuntime&);
    OmniRuntime& operator=(const OmniRuntime&);

    class Impl;
    Impl* impl_;
};

} // namespace omnibinder

#endif
