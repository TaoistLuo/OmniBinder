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

/* @brief 名字级死亡通知回调
 * @param[in] service_name 已离线的服务名称（该名字下最后一个存活实例消失时触发） */
typedef std::function<void(const std::string& service_name)> DeathCallback;

/* @brief 实例级死亡通知回调
 * @details 目标实例死亡时回传其完整身份 (service_name, host_id, instance_id)，
 *          订阅方据此定位是哪个实例消失；与名字级 DeathCallback 并存。 */
typedef std::function<void(const std::string& service_name, const std::string& host_id,
                           const std::string& instance_id)> DeathSourceCallback;
/* @brief 话题消息回调（仅携带 topic_id 与数据，不区分发布者实例） */
typedef std::function<void(uint32_t topic_id, const Buffer& data)> TopicCallback;
/* @brief 话题错误回调（topic 不存在 / 发布者离线 / IDL 不匹配时触发） */
typedef std::function<void(uint32_t topic_id, ErrorCode error, const Buffer& raw_data)> TopicErrorCallback;
/* @brief 诊断事件回调（watchPid 订阅的运行时事件流） */
typedef std::function<void(const Buffer& data)> DiagEventCallback;

/* @brief 话题消息回调（携带来源实例身份，决策 D5）
 * @details 服务级与实例级订阅共用本回调类型：收到消息时回传发布者的 host_id 与
 *          instance_id，订阅方据此区分数据来自哪个实例，无需改动消息 payload。 */
typedef std::function<void(const std::string& host_id, const std::string& instance_id,
                           uint32_t topic_id, const Buffer& data)> TopicSourceCallback;

/* @brief 话题订阅选择器：服务级（指定服务的全部实例）/ 实例级（指定实例）
 * @note  与旧 subscribeTopic(topic, ...) 的"仅按主题名（任意服务）"语义并存。
 *        服务级/实例级订阅与 TopicSourceCallback 搭配；旧接口与 TopicCallback 搭配。 */
struct TopicSubscriptionSelector {
    std::string publisher_service;  // 发布者服务名
    std::string host_id;            // 实例级：目标 host_id
    std::string instance_id;        // 实例级：目标实例标识
    bool        instance_level;     // true 实例级；false 服务级

    TopicSubscriptionSelector() : instance_level(false) {}

    /* @brief 构造服务级选择器（该服务发布的 topic 全部实例） */
    static TopicSubscriptionSelector service(const std::string& publisher_service) {
        TopicSubscriptionSelector selector;
        selector.publisher_service = publisher_service;
        return selector;
    }

    /* @brief 构造实例级选择器（仅该服务的目标实例） */
    static TopicSubscriptionSelector instance(const std::string& publisher_service,
                                              const std::string& host_id,
                                              const std::string& instance_id) {
        TopicSubscriptionSelector selector;
        selector.publisher_service = publisher_service;
        selector.host_id = host_id;
        selector.instance_id = instance_id;
        selector.instance_level = true;
        return selector;
    }
};

/* @brief 生成数据面实例连接键
 * @param[in]  name        服务名称
 * @param[in]  host_id     目标板级主机身份
 * @param[in]  instance_id 目标实例标识
 * @return 用于 invoke/heartbeat/重连/断开的连接键
 * @note   默认实例（host_id 与 instance_id 均为空）的键即服务名本身，与服务级
 *         connectService 使用同一键；显式实例键在服务名后追加
 *         makeServiceInstanceKey(host_id, instance_id) 无歧义编码，
 *         因此同一服务名下不同实例互不覆盖。
 *         与 connectServiceInstance() 配套：绑定实例后，invoke/心跳/重连/断开
 *         均以本键寻址该实例的数据面连接。 */
std::string makeServiceConnectionKey(const std::string& name,
                                     const std::string& host_id,
                                     const std::string& instance_id);

/* @brief 进程级运行时入口：控制面（注册/发现/心跳/死亡通知）+ 数据面（直连 RPC/广播）+ 诊断 */
class OmniRuntime {
public:
    OmniRuntime();
    ~OmniRuntime();

    // ============================================================
    // 生命周期
    // ============================================================

    /* @brief 初始化运行时并连接到 ServiceManager
     * @param[in]  sm_host ServiceManager 所在主机地址（IP 或域名）
     * @param[in]  sm_port ServiceManager 监听端口（默认 9900）
     * @return 0 成功，<0 失败（网络错误或连接超时）
     * @note   必须在其他 API 调用之前完成初始化 */
    int  init(const std::string& sm_host, uint16_t sm_port = 9900);

    /* @brief 启动 owner event-loop（阻塞直到 stop() 被调用）
     * @note   可从内部通过 stop() 结束，也可从其它线程调用 stop() */
    void run();

    /* @brief 停止 event-loop（线程安全，可从任意线程调用）
     * @note   仅标记停止；实际退出在下一轮事件循环检查时生效 */
    void stop();

    /* @brief 查询 event-loop 是否正在运行
     * @return true 正在运行，false 已停止 */
    bool isRunning() const;

    /* @brief 驱动事件循环执行一轮（非阻塞）
     * @param[in]  timeout_ms 等待 I/O 事件的超时时间（毫秒，0 表示立即返回）
     * @note   适合需要手动控制循环节奏的场景（如嵌入式主循环） */
    void pollOnce(int timeout_ms = 0);

    // ============================================================
    // 控制面 — 与 ServiceManager 交互
    // ============================================================

    // 服务注册/发现

    /* @brief 向 ServiceManager 注册本地服务
     * @param[in]  service 服务实例指针（由 IDL 生成的 Stub 子类）
     * @return 0 成功，<0 失败（服务名重复或 SM 通信错误）
     * @note   Service 生命周期由调用方管理，注销前不可释放 */
    int registerService(Service* service);

    /* @brief 从 ServiceManager 注销本地服务
     * @param[in]  service 之前注册的服务实例指针
     * @return 0 成功，<0 失败（未注册或 SM 通信错误）
     * @note   注销后所有已连接的 Proxy 将收到死亡通知 */
    int unregisterService(Service* service);

    /* @brief 按名称查找远程服务
     * @param[in]  name 服务名称
     * @param[out] info 服务信息（地址、端口、接口列表等）
     * @return 0 成功，<0 失败（不存在或 SM 通信错误） */
    int lookupService(const std::string& name, ServiceInfo& info);

    /* @brief 列出所有已注册服务
     * @param[out] services 服务信息列表
     * @return 0 成功，<0 SM 通信错误 */
    int listServices(std::vector<ServiceInfo>& services);

    /* @brief 列出指定服务名下的全部实例（跨主机与实例身份）
     * @param[in]  name 服务名称
     * @param[out] out  服务实例信息列表；每项含 name、instance_id、host_id、host、port
     * @return 0 成功；ERR_SERVICE_NOT_FOUND 该名称下无任何实例；ERR_DESERIALIZE 应答畸形
     * @note   结果顺序为 SM 注册表实例键字典序（确定性，与 lookupService 选取的第一实例一致）；
     *         线程安全，内部经 callSerialized 投递到 owner event-loop */
    int listServiceInstances(const std::string& name, std::vector<ServiceInfo>& out);

    /* @brief 查询指定服务的接口定义列表
     * @param[in]  name   服务名称
     * @param[out] ifaces 接口信息列表
     * @return 0 成功，<0 失败 */
    int queryInterfaces(const std::string& name, std::vector<InterfaceInfo>& ifaces);

    /* @brief 查询指定服务已发布的 topic 列表
     * @param[in]  name       服务名称
     * @param[out] topics     topic 名称列表
     * @param[in]  timeout_ms 超时时间（毫秒，0 使用默认值）
     * @return 0 成功，<0 失败（含超时） */
    int queryPublishedTopics(const std::string& name, std::vector<std::string>& topics,
                             uint32_t timeout_ms = 0);

    // 话题声明（向 SM 注册发布/订阅关系）

    /* @brief 向 ServiceManager 声明 topic 发布
     * @param[in]  topic_name topic 名称
     * @param[in]  idl_hash   发布者 topic IDL 哈希（由 omni-idlc 生成；0 表示不声明）
     * @return 0 成功，<0 失败
     * @note   声明后其他服务可通过 queryPublishedTopics 发现此 topic；
     *         订阅者声明了期望哈希时，ServiceManager 会将本哈希转发给订阅者，
     *         哈希不一致会导致订阅失败（ERR_IDL_MISMATCH）且不投递数据 */
    int publishTopic(const std::string& topic_name, uint32_t idl_hash = 0);

    /* @brief 订阅指定 topic（控制面注册 + 数据面接收）
     * @param[in]  topic_name topic 名称
     * @param[in]  on_msg    消息回调（在 owner event-loop 线程执行）
     * @param[in]  on_err    错误回调（可选，topic 不存在或发布者离线时触发）
     * @return 0 成功，<0 失败
     * @note   回调在 owner event-loop 线程执行，应避免长时间阻塞 */
    int subscribeTopic(const std::string& topic_name, const TopicCallback& on_msg,
                       const TopicErrorCallback& on_err);

    /* @brief 订阅指定 topic（携带期望 IDL 哈希，控制面注册 + 数据面接收）
     * @param[in]  topic_name         topic 名称
     * @param[in]  expected_idl_hash  订阅者期望的 topic IDL 哈希（由 omni-idlc 生成；0 表示不校验）
     * @param[in]  on_msg             消息回调（在 owner event-loop 线程执行）
     * @param[in]  on_err             错误回调（发布者哈希不匹配时收到 ERR_IDL_MISMATCH）
     * @return 0 成功，<0 失败；发布者哈希与期望不一致时返回 ERR_IDL_MISMATCH
     * @note   发布者尚未上线时订阅成功，待发布者通知到达后校验；不匹配则经 on_err 通知且不投递数据 */
    int subscribeTopic(const std::string& topic_name, uint32_t expected_idl_hash,
                       const TopicCallback& on_msg, const TopicErrorCallback& on_err);

    /* @brief 取消 topic 订阅
     * @param[in]  topic_name topic 名称
     * @return 0 成功，<0 失败 */
    int unsubscribeTopic(const std::string& topic_name);

    /* @brief 按选择器订阅 topic（服务级 / 实例级，回调携带来源实例身份）
     * @param[in]  selector          订阅选择器（服务级或实例级）
     * @param[in]  topic_name        topic 名称
     * @param[in]  expected_idl_hash 订阅者期望的 topic IDL 哈希（0 表示不校验）
     * @param[in]  on_msg            消息回调（携带发布者 host_id + instance_id）
     * @param[in]  on_err            错误回调（可选）
     * @return 0 成功，<0 失败；发布者哈希与期望不一致时返回 ERR_IDL_MISMATCH
     * @note   服务级订阅该发布者服务的全部实例：每个实例使用独立数据面连接，
     *         回调据 host_id / instance_id 区分来源；实例级仅订阅指定实例。
     *         同一运行时对同一 (选择器, topic) 重复订阅为追加，可分别订阅多个实例。 */
    int subscribeTopic(const TopicSubscriptionSelector& selector,
                       const std::string& topic_name, uint32_t expected_idl_hash,
                       const TopicSourceCallback& on_msg, const TopicErrorCallback& on_err);

    /* @brief 按选择器取消 topic 订阅（与 subscribeTopic 选择器一一对应）
     * @param[in]  selector   订阅选择器
     * @param[in]  topic_name topic 名称
     * @return 0 成功，<0 失败
     * @note   仅移除该选择器对应的订阅与数据面绑定，不影响同话题的其它选择器订阅 */
    int unsubscribeTopic(const TopicSubscriptionSelector& selector,
                         const std::string& topic_name);

    // 死亡通知订阅

    /* @brief 订阅指定服务的死亡通知
     * @param[in]  name 服务名称
     * @param[in]  cb   死亡回调（服务离线时在 owner event-loop 线程执行）
     * @return 0 成功，<0 失败
     * @note   服务因崩溃/注销离线时触发；可用于触发重连或降级逻辑 */
    int subscribeServiceDeath(const std::string& name, const DeathCallback& cb);

    /* @brief 取消服务死亡通知订阅
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败 */
    int unsubscribeServiceDeath(const std::string& name);

    /* @brief 订阅指定服务实例的死亡通知
     * @param[in]  name        服务名称
     * @param[in]  host_id     目标板级主机身份
     * @param[in]  instance_id 目标实例标识
     * @param[in]  cb          死亡回调，在 owner event-loop 线程执行并携带实例身份
     * @return 0 成功，<0 失败
     * @note   仅当该 (name, host_id, instance_id) 实例死亡时触发，同名的其它实例
     *         死亡不影响本订阅；同一目标已订阅时返回 ERR_REGISTER_FAILED（需先
     *         unsubscribeServiceInstanceDeath 再重订阅以更换回调）。
     *         名字级 subscribeServiceDeath 语义不变：仅当该服务名已无任何存活实例
     *         （最后一个实例消失）时才触发。 */
    int subscribeServiceInstanceDeath(const std::string& name,
                                      const std::string& host_id,
                                      const std::string& instance_id,
                                      const DeathSourceCallback& cb);

    /* @brief 取消指定服务实例的死亡通知订阅
     * @param[in]  name        服务名称
     * @param[in]  host_id     目标板级主机身份
     * @param[in]  instance_id 目标实例标识
     * @return 0 成功，<0 失败
     * @note   仅移除该实例的订阅，不影响名字级订阅或同名其它实例订阅 */
    int unsubscribeServiceInstanceDeath(const std::string& name,
                                        const std::string& host_id,
                                        const std::string& instance_id);

    // ============================================================
    // 数据面 — 服务间直连通信
    // ============================================================

    // 连接管理

    /* @brief 建立到指定服务的数据面直连
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败（服务不存在或网络错误）
     * @note   同机自动使用 SHM，跨机走 TCP */
    int  connectService(const std::string& name);

    /* @brief 建立到指定服务实例 (host_id, instance_id) 的数据面直连
     * @param[in]  name        服务名称
     * @param[in]  host_id     目标板级主机身份（ServiceInfo.host_id）
     * @param[in]  instance_id 目标实例标识（ServiceInfo.instance_id）
     * @return 0 成功；ERR_SERVICE_NOT_FOUND 该名称下无匹配实例；网络/协议错误时返回对应错误码
     * @note   通过 listServiceInstances 解析目标实例后建立直连，不新增线协议。
     *         连接以 makeServiceConnectionKey(name, host_id, instance_id) 为键，
     *         之后对该实例的 invoke / 心跳 / 自动重连 / 断开均使用同一连接键。
     *         不指定实例的 connectService(name) 行为保持不变 */
    int  connectServiceInstance(const std::string& name,
                                const std::string& host_id,
                                const std::string& instance_id);

    /* @brief 断开到指定服务的数据面直连
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败 */
    int  disconnectService(const std::string& name);

    /* @brief 断开到指定服务实例的数据面直连
     * @param[in]  name        服务名称
     * @param[in]  host_id     目标板级主机身份
     * @param[in]  instance_id 目标实例标识
     * @return 0 成功，<0 失败
     * @note   与 connectServiceInstance 使用同一连接键，断开该实例的连接与
     *         重连/心跳状态；对同名的其它实例无影响 */
    int  disconnectServiceInstance(const std::string& name,
                                   const std::string& host_id,
                                   const std::string& instance_id);

    /* @brief 查询与指定服务的数据面连接状态
     * @param[in]  name 服务名称
     * @return true 已连接，false 未连接 */
    bool isServiceConnected(const std::string& name) const;

    /* @brief 查询与指定服务实例的数据面连接状态
     * @param[in]  name        服务名称
     * @param[in]  host_id     目标板级主机身份
     * @param[in]  instance_id 目标实例标识
     * @return true 已连接，false 未连接
     * @note   连接以 makeServiceConnectionKey(name, host_id, instance_id) 为键，
     *         与 connectServiceInstance 配套。按实例建立的连接不能用按服务名的
     *         isServiceConnected(name) 检测（后者只命中以服务名为键的连接）。 */
    bool isServiceInstanceConnected(const std::string& name, const std::string& host_id,
                                    const std::string& instance_id) const;

    // 自动重连 / 重连间隔 / 心跳的 name 参数是"数据面连接键"：服务级连接即服务名；
    // 实例级连接为 makeServiceConnectionKey(name, host_id, instance_id)。生成的 Proxy
    // 绑定实例后即以实例连接键调用本组 API，无需额外的实例版接口。

    /* @brief 启用/禁用自动重连
     * @param[in]  name   服务名称（或实例连接键 makeServiceConnectionKey(...)）
     * @param[in]  enable true 启用，false 禁用（默认启用）
     * @note   启用后连接断开时自动尝试重连（使用指数退避策略） */
    void enableAutoReconnect(const std::string& name, bool enable = true);

    /* @brief 设置重连间隔
     * @param[in]  name        服务名称（或实例连接键 makeServiceConnectionKey(...)）
     * @param[in]  interval_ms 重连间隔（毫秒） */
    void setReconnectInterval(const std::string& name, uint32_t interval_ms);

    // 心跳
    //
    // 两类心跳互相独立，不可混淆：
    // 1) 控制面注册心跳：runtime 对每个本地已注册服务（registerService）自动周期
    //    发送给 ServiceManager，生命周期与注册绑定，本组接口无法启停它。停止向 SM
    //    宣告服务存活的唯一方式是 unregisterService()。
    // 2) 数据面连接保活：本组 startHeartbeat/stopHeartbeat 接口，调用方作为客户端
    //    对某个远端服务名的直连做心跳检测，超时后触发数据面自动重连。stopHeartbeat
    //    只停止该检测，不注销服务、不影响控制面注册心跳。

    /* @brief 启动到指定服务的数据面连接心跳检测
     * @param[in]  name        服务名称（或实例连接键 makeServiceConnectionKey(...)）
     * @param[in]  interval_ms 心跳发送间隔（毫秒，默认 5000）
     * @param[in]  timeout_ms  心跳超时阈值（毫秒，默认 10000）
     * @note   超时后自动触发数据面重连（如果启用了自动重连）。
     *         不涉及本地服务向 SM 的控制面注册心跳 */
    void startHeartbeat(const std::string& name, uint32_t interval_ms = 5000,
                        uint32_t timeout_ms = 10000);

    /* @brief 停止到指定服务的数据面连接心跳检测
     * @param[in]  name 服务名称（或实例连接键 makeServiceConnectionKey(...)）
     * @note   只停止检测与该服务的超时重连调度；不注销服务、不影响控制面注册心跳 */
    void stopHeartbeat(const std::string& name);

    // RPC 调用

    /* @brief 同步 RPC 调用（阻塞等待响应）
     * @param[in]  name       服务名称
     * @param[in]  iface_id   接口 ID（由 IDL 生成）
     * @param[in]  method_id  方法 ID（由 IDL 生成）
     * @param[in]  idl_hash   IDL 哈希（用于接口版本校验）
     * @param[in]  req        请求参数（Buffer 序列化）
     * @param[out] resp       响应数据
     * @param[in]  timeout_ms 超时时间（毫秒，0 使用默认值）
     * @return 0 成功，<0 失败（超时/序列化错误/网络错误）
     * @note   线程安全；内部通过 callSerialized 投递到 owner event-loop */
    int invoke(const std::string& name, uint32_t iface_id, uint32_t method_id,
               uint32_t idl_hash, const Buffer& req, Buffer& resp, uint32_t timeout_ms = 0);

    /* @brief 单向 RPC 调用（不等待响应）
     * @param[in]  name      服务名称
     * @param[in]  iface_id  接口 ID
     * @param[in]  method_id 方法 ID
     * @param[in]  idl_hash  IDL 哈希
     * @param[in]  req       请求参数
     * @return 0 发送成功，<0 失败
     * @note   无响应确认；适用于日志上报、通知等场景 */
    int invokeOneWay(const std::string& name, uint32_t iface_id, uint32_t method_id,
                     uint32_t idl_hash, const Buffer& req);

    // 话题广播（数据面直连，不经过 SM）

    /* @brief 广播消息到指定 topic 的所有订阅者
     * @param[in]  topic_id topic ID（由 fnv1a_32(topic_name) 生成）
     * @param[in]  data     广播数据
     * @return 0 成功，<0 失败
     * @note   仅在已 publishTopic 后才能广播 */
    int broadcast(uint32_t topic_id, const Buffer& data);

    // ============================================================
    // 诊断 — 运行时可观测性
    // ============================================================

    /* @brief 启用对指定服务的诊断监控
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败 */
    int  enableDiagnostic(const std::string& name);

    /* @brief 禁用对指定服务的诊断监控
     * @param[in]  name 服务名称
     * @return 0 成功，<0 失败 */
    int  disableDiagnostic(const std::string& name);

    /* @brief 设置指定进程的日志级别
     * @param[in]  pid   目标进程 ID
     * @param[in]  level 日志级别
     * @return 0 成功，<0 失败 */
    int  setLogLevelByPid(uint32_t pid, uint32_t level);

    /* @brief 列出所有已连接的运行时实例
     * @param[out] runtimes 运行时信息列表
     * @return 0 成功，<0 失败 */
    int  listRuntimes(std::vector<RuntimeInfo>& runtimes);

    /* @brief 订阅指定进程的诊断事件流
     * @param[in]  pid 目标进程 ID
     * @param[in]  cb  事件回调
     * @return 0 成功，<0 失败 */
    int  watchPid(uint32_t pid, const DiagEventCallback& cb);

    /* @brief 取消对指定进程的诊断事件订阅
     * @param[in]  pid 目标进程 ID
     * @return 0 成功，<0 失败 */
    int  unwatchPid(uint32_t pid);

    /* @brief 获取运行时统计信息
     * @param[out] stats 统计数据结构
     * @return 0 成功，<0 失败 */
    int  getStats(RuntimeStats& stats);

    /* @brief 重置运行时统计信息
     * @return 0 成功，<0 失败 */
    int  resetStats();

    /* @brief 清除本地服务发现缓存 */
    void clearServiceCache();

    /* @brief 关闭所有数据面连接 */
    void closeAllConnections();

    // ============================================================
    // 配置
    // ============================================================

    /* @brief 设置服务注册时的对外通告地址
     * @param[in]  host 通告地址（IP 或域名）
     * @note   跨机部署时，此地址将写入 ServiceInfo 供其他机器直连使用 */
    void setRegisterHost(const std::string& host);

    /* @brief 获取服务注册时的对外通告地址
     * @return 通告地址字符串 */
    std::string getRegisterHost() const;

    /* @brief 设置全局心跳间隔
     * @param[in]  ms 心跳间隔（毫秒） */
    void setHeartbeatInterval(uint32_t ms);

    /* @brief 设置全局默认 RPC 超时
     * @param[in]  ms 超时时间（毫秒） */
    void setDefaultTimeout(uint32_t ms);

    /* @brief 设置服务端 RPC 回复发送的专用超时预算
     * @param[in]  ms 发送超时（毫秒），0 表示仅尝试一次（不等待）
     * @note   该预算独立于 setDefaultTimeout。服务端回复需要同步写回客户端，
     *         使用有界预算避免慢客户端长时间占住 owner event-loop。
     *         默认值为 DEFAULT_REPLY_SEND_TIMEOUT（1000ms）。 */
    void setReplySendTimeout(uint32_t ms);

    /* @brief 覆盖本机标识符 host_id（用于判断同机通信与实例身份）
     * @param[in]  host_id 指定的主机/设备标识；空串表示使用平台默认机器标识
     * @note   必须在 init() 之前调用；init() 之后调用不再生效（返回后忽略）。
     *         默认取自 platform::getMachineId()（Linux 为 /etc/machine-id）。
     *         量产镜像可能被克隆导致多板 machine-id 相同，此时应在各设备显式设置
     *         本设备唯一标识（MAC / 芯片 ID / 序列号）——否则多板会被误判为同一
     *         身份而互相顶替，且 SHM 误判为同机（设计决策 D8）。
     *         host_id 写入服务注册信息，并在数据面据其判断同机/跨机传输选择。 */
    void setHostId(const std::string& host_id);

    /* @brief 获取本机标识符（用于判断同机通信）
     * @return 本机标识字符串；未设置时返回平台机器标识 */
    std::string hostId() const;

private:
    OmniRuntime(const OmniRuntime&);
    OmniRuntime& operator=(const OmniRuntime&);

    class Impl;
    Impl* impl_;
};

} // namespace omnibinder

#endif
