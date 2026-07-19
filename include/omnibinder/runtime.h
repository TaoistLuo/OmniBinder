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
typedef std::function<void(const Buffer& data)> DiagEventCallback;

class OmniRuntime {
public:
    OmniRuntime();
    ~OmniRuntime();

    // ============================================================
    // 生命周期
    // ============================================================

    /*
     * @brief  初始化运行时并连接到 ServiceManager
     * @param[in]  sm_host ServiceManager 所在主机地址（IP 或域名）
     * @param[in]  sm_port ServiceManager 监听端口（默认 9900）
     * @return 0 成功，<0 失败（网络错误或连接超时）
     * @note   必须在其他 API 调用之前完成初始化
     */
    int  init(const std::string& sm_host, uint16_t sm_port = 9900);

    /*
     * @brief  启动 owner event-loop（阻塞直到 stop() 被调用）
     * @note   可从内部通过 stop() 结束，也可从其它线程调用 stop()
     */
    void run();

    /*
     * @brief  停止 event-loop（线程安全，可从任意线程调用）
     * @note   仅标记停止；实际退出在下一轮事件循环检查时生效
     */
    void stop();

    /*
     * @brief  查询 event-loop 是否正在运行
     * @return true 正在运行，false 已停止
     */
    bool isRunning() const;

    /*
     * @brief  驱动事件循环执行一轮（非阻塞）
     * @param[in]  timeout_ms 等待 I/O 事件的超时时间（毫秒，0 表示立即返回）
     * @note   适合需要手动控制循环节奏的场景（如嵌入式主循环）
     */
    void pollOnce(int timeout_ms = 0);

    // ============================================================
    // 控制面 — 与 ServiceManager 交互
    // ============================================================

    // 服务注册/发现

    /*
     * @brief  向 ServiceManager 注册本地服务
     * @param[in]  service 服务实例指针（由 IDL 生成的 Stub 子类）
     * @return 0 成功，<0 失败（服务名重复或 SM 通信错误）
     * @note   Service 生命周期由调用方管理，注销前不可释放
     */
    int registerService(Service* service);

    /*
     * @brief  从 ServiceManager 注销本地服务
     * @param[in]  service 之前注册的服务实例指针
     * @return 0 成功，<0 失败（未注册或 SM 通信错误）
     * @note   注销后所有已连接的 Proxy 将收到死亡通知
     */
    int unregisterService(Service* service);

    /*
     * @brief  按名称查找远程服务
     * @param[in]  name 服务名称
     * @param[out] info 服务信息（地址、端口、接口列表等）
     * @return 0 成功，<0 失败（不存在或 SM 通信错误）
     */
    int lookupService(const std::string& name, ServiceInfo& info);

    /*
     * @brief  列出所有已注册服务
     * @param[out] services 服务信息列表
     * @return 0 成功，<0 SM 通信错误
     */
    int listServices(std::vector<ServiceInfo>& services);

    /*
     * @brief  查询指定服务的接口定义列表
     * @param[in]  name   服务名称
     * @param[out] ifaces 接口信息列表
     * @return 0 成功，<0 失败
     */
    int queryInterfaces(const std::string& name, std::vector<InterfaceInfo>& ifaces);

    /*
     * @brief  查询指定服务已发布的 topic 列表
     * @param[in]  name   服务名称
     * @param[out] topics topic 名称列表
     * @return 0 成功，<0 失败
     */
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics);

    /*
     * @brief  查询指定服务已发布的 topic 列表（带超时）
     * @param[in]  name       服务名称
     * @param[out] topics     topic 名称列表
     * @param[in]  timeout_ms 超时时间（毫秒，0 使用默认值）
     * @return 0 成功，<0 失败（含超时）
     */
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics,
                             uint32_t timeout_ms);

    // 话题声明（向 SM 注册发布/订阅关系）

    /*
     * @brief  向 ServiceManager 声明 topic 发布
     * @param[in]  topic_name topic 名称
     * @return 0 成功，<0 失败
     * @note   声明后其他服务可通过 queryPublishedTopics 发现此 topic
     */
    int publishTopic(const std::string& topic_name);

    /*
     * @brief  订阅指定 topic（控制面注册 + 数据面接收）
     * @param[in]  topic_name topic 名称
     * @param[in]  on_msg    消息回调（在 owner event-loop 线程执行）
     * @param[in]  on_err    错误回调（可选，topic 不存在或发布者离线时触发）
     * @return 0 成功，<0 失败
     * @note   回调在 owner event-loop 线程执行，应避免长时间阻塞
     */
    int subscribeTopic(const std::string& topic_name, const TopicCallback& on_msg,
                       const TopicErrorCallback& on_err);

    /*
     * @brief  取消 topic 订阅
     * @param[in]  topic_name topic 名称
     * @return 0 成功，<0 失败
     */
    int unsubscribeTopic(const std::string& topic_name);

    // 死亡通知订阅

    /*
     * @brief  订阅指定服务的死亡通知
     * @param[in]  name 服务名称
     * @param[in]  cb   死亡回调（服务离线时在 owner event-loop 线程执行）
     * @return 0 成功，<0 失败
     * @note   服务因崩溃/注销离线时触发；可用于触发重连或降级逻辑
     */
    int subscribeServiceDeath(const std::string& name, const DeathCallback& cb);

    /*
     * @brief  取消服务死亡通知订阅
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败
     */
    int unsubscribeServiceDeath(const std::string& name);

    // ============================================================
    // 数据面 — 服务间直连通信
    // ============================================================

    // 连接管理

    /*
     * @brief  建立到指定服务的数据面直连
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败（服务不存在或网络错误）
     * @note   同机自动使用 SHM，跨机走 TCP
     */
    int  connectService(const std::string& name);

    /*
     * @brief  断开到指定服务的数据面直连
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败
     */
    int  disconnectService(const std::string& name);

    /*
     * @brief  查询与指定服务的数据面连接状态
     * @param[in]  name 服务名称
     * @return true 已连接，false 未连接
     */
    bool isServiceConnected(const std::string& name) const;

    /*
     * @brief  启用/禁用自动重连
     * @param[in]  name   服务名称
     * @param[in]  enable true 启用，false 禁用（默认启用）
     * @note   启用后连接断开时自动尝试重连（使用指数退避策略）
     */
    void enableAutoReconnect(const std::string& name, bool enable = true);

    /*
     * @brief  设置重连间隔
     * @param[in]  name        服务名称
     * @param[in]  interval_ms 重连间隔（毫秒）
     */
    void setReconnectInterval(const std::string& name, uint32_t interval_ms);

    // 心跳

    /*
     * @brief  启动到指定服务的心跳检测
     * @param[in]  name        服务名称
     * @param[in]  interval_ms 心跳发送间隔（毫秒，默认 5000）
     * @param[in]  timeout_ms  心跳超时阈值（毫秒，默认 10000）
     * @note   超时后自动触发重连（如果启用了自动重连）
     */
    void startHeartbeat(const std::string& name, uint32_t interval_ms = 5000,
                        uint32_t timeout_ms = 10000);

    /*
     * @brief  停止到指定服务的心跳检测
     * @param[in]  name 服务名称
     */
    void stopHeartbeat(const std::string& name);

    // RPC 调用

    /*
     * @brief  同步 RPC 调用（阻塞等待响应）
     * @param[in]  name       服务名称
     * @param[in]  iface_id   接口 ID（由 IDL 生成）
     * @param[in]  method_id  方法 ID（由 IDL 生成）
     * @param[in]  idl_hash   IDL 哈希（用于接口版本校验）
     * @param[in]  req        请求参数（Buffer 序列化）
     * @param[out] resp       响应数据
     * @param[in]  timeout_ms 超时时间（毫秒，0 使用默认值）
     * @return 0 成功，<0 失败（超时/序列化错误/网络错误）
     * @note   线程安全；内部通过 callSerialized 投递到 owner event-loop
     */
    int invoke(const std::string& name, uint32_t iface_id, uint32_t method_id,
               uint32_t idl_hash, const Buffer& req, Buffer& resp, uint32_t timeout_ms = 0);

    /*
     * @brief  单向 RPC 调用（不等待响应）
     * @param[in]  name      服务名称
     * @param[in]  iface_id  接口 ID
     * @param[in]  method_id 方法 ID
     * @param[in]  idl_hash  IDL 哈希
     * @param[in]  req       请求参数
     * @return 0 发送成功，<0 失败
     * @note   无响应确认；适用于日志上报、通知等场景
     */
    int invokeOneWay(const std::string& name, uint32_t iface_id, uint32_t method_id,
                     uint32_t idl_hash, const Buffer& req);

    // 话题广播（数据面直连，不经过 SM）

    /*
     * @brief  广播消息到指定 topic 的所有订阅者
     * @param[in]  topic_id topic ID（由 fnv1a_32(topic_name) 生成）
     * @param[in]  data     广播数据
     * @return 0 成功，<0 失败
     * @note   仅在已 publishTopic 后才能广播
     */
    int broadcast(uint32_t topic_id, const Buffer& data);

    // ============================================================
    // 诊断 — 运行时可观测性
    // ============================================================

    /*
     * @brief  启用对指定服务的诊断监控
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败
     */
    int  enableDiagnostic(const std::string& name);

    /*
     * @brief  禁用对指定服务的诊断监控
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败
     */
    int  disableDiagnostic(const std::string& name);

    /*
     * @brief  设置指定进程的日志级别
     * @param[in]  pid   目标进程 ID
     * @param[in]  level 日志级别
     * @return 0 成功，<0 失败
     */
    int  setLogLevelByPid(uint32_t pid, uint32_t level);

    /*
     * @brief  列出所有已连接的运行时实例
     * @param[out] runtimes 运行时信息列表
     * @return 0 成功，<0 失败
     */
    int  listRuntimes(std::vector<RuntimeInfo>& runtimes);

    /*
     * @brief  订阅指定进程的诊断事件流
     * @param[in]  pid 目标进程 ID
     * @param[in]  cb  事件回调
     * @return 0 成功，<0 失败
     */
    int  watchPid(uint32_t pid, const DiagEventCallback& cb);

    /*
     * @brief  取消对指定进程的诊断事件订阅
     * @param[in]  pid 目标进程 ID
     * @return 0 成功，<0 失败
     */
    int  unwatchPid(uint32_t pid);

    /*
     * @brief  获取运行时统计信息
     * @param[out] stats 统计数据结构
     * @return 0 成功，<0 失败
     */
    int  getStats(RuntimeStats& stats);

    /*
     * @brief  重置运行时统计信息
     * @return 0 成功，<0 失败
     */
    int  resetStats();

    /*
     * @brief  清除本地服务发现缓存
     */
    void clearServiceCache();

    /*
     * @brief  关闭所有数据面连接
     */
    void closeAllConnections();

    // ============================================================
    // 配置
    // ============================================================

    /*
     * @brief  设置服务注册时的对外通告地址
     * @param[in]  host 通告地址（IP 或域名）
     * @note   跨机部署时，此地址将写入 ServiceInfo 供其他机器直连使用
     */
    void setRegisterHost(const std::string& host);

    /*
     * @brief  获取服务注册时的对外通告地址
     * @return 通告地址字符串
     */
    const std::string& getRegisterHost() const;

    /*
     * @brief  设置全局心跳间隔
     * @param[in]  ms 心跳间隔（毫秒）
     */
    void setHeartbeatInterval(uint32_t ms);

    /*
     * @brief  设置全局默认 RPC 超时
     * @param[in]  ms 超时时间（毫秒）
     */
    void setDefaultTimeout(uint32_t ms);

    /*
     * @brief  获取本机标识符（用于判断同机通信）
     * @return 本机标识字符串
     */
    const std::string& hostId() const;

private:
    OmniRuntime(const OmniRuntime&);
    OmniRuntime& operator=(const OmniRuntime&);

    class Impl;
    Impl* impl_;
};

} // namespace omnibinder

#endif
