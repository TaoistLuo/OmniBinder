/**************************************************************************************************
 * @file        proxy_base.h
 * @brief       Proxy 基类
 * @details     定义由 omni-idlc 生成的所有 Proxy 类的公共基类。封装 OmniRuntime 引用、连接状态管理、RPC 超时等横切关注点。
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
#ifndef OMNIBINDER_PROXY_BASE_H
#define OMNIBINDER_PROXY_BASE_H

#include "omnibinder/runtime.h"
#include <atomic>
#include <functional>
#include <string>
#include <mutex>

namespace omnibinder {

/* @brief 所有生成 Proxy 的公共基类：封装 runtime 引用、连接状态与 RPC 超时等横切关注点 */
class ServiceProxyBase {
    friend class OmniRuntime;
public:
    /* @brief 创建 Proxy 基类实例
     * @param[in]  runtime      OmniRuntime 引用
     * @param[in]  service_name 目标服务名称 */
    ServiceProxyBase(OmniRuntime& runtime, const std::string& service_name);

    virtual ~ServiceProxyBase();
    
    /* @brief 设置实例选择器（下一次自动连接/connect() 生效）
     * @param[in]  host_id     目标板级主机身份
     * @param[in]  instance_id 目标实例标识
     * @note   仅改变后续 connect() 的目标；对已建立的连接不生效，需先 disconnect() */
    void setInstance(const std::string& host_id, const std::string& instance_id);

    /* @brief 断开到目标服务的连接 */
    void disconnect();
    
    /* @brief 查询连接状态
     * @return true 已连接，false 未连接 */
    bool isConnected() const;

    /* @brief 是否允许 runtime 自动连接/重连本 Proxy */
    bool shouldAutoConnect() const { return auto_connect_enabled_.load(); }
    /* @brief 是否曾经成功连接过（用于区分“首次等待服务”和“已连接后死亡”） */
    bool hasEverConnected() const { return ever_connected_.load(); }

    /* @brief 启用/禁用自动重连
     * @param[in]  enable true 启用（默认） */
    void enableAutoReconnect(bool enable = true);

    /* @brief 设置重连间隔
     * @param[in]  interval_ms 重连间隔（毫秒） */
    void setReconnectInterval(uint32_t interval_ms);

    /* @brief 启动到目标服务的数据面连接心跳检测
     * @param[in]  interval_ms 心跳间隔（毫秒，默认 DEFAULT_HEARTBEAT_INTERVAL）
     * @param[in]  timeout_ms  超时阈值（毫秒，默认 DEFAULT_HEARTBEAT_TIMEOUT）
     * @note   检测的是客户端到目标服务的直连保活，不涉及目标服务在
     *         ServiceManager 的注册心跳。建议在 connect() 前调用以参与 Session
     *         Hello 协商；connect() 后调用会触发一次会话参数更新。 */
    void startHeartbeat(uint32_t interval_ms = DEFAULT_HEARTBEAT_INTERVAL,
                        uint32_t timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT);

    /* @brief 停止到目标服务的数据面连接心跳检测
     * @note   只停止本代理的检测；不注销目标服务、不影响其 SM 注册心跳。
     *         disconnect() 内部会调用本函数 */
    void stopHeartbeat();

    /* @brief 注册服务死亡回调
     * @param[in]  callback 死亡时调用的回调函数 */
    void OnServiceDied(const std::function<void()>& callback);
    
protected:
    /* @brief 建立/重连目标服务的数据面连接（仅由 runtime 调用） */
    int connect();
    int connect(const std::string& host_id, const std::string& instance_id);

    /* @brief 当前实例绑定的数据面连接键
     * @return 默认实例返回服务名；绑定实例后返回 makeServiceConnectionKey(...)
     * @note   生成的 Proxy 方法据此寻址数据面连接（默认路径与服务名一致，
     *         行为与历史版本完全相同） */
    std::string connectionKey() const;

    /* @brief 服务死亡回调（子类可重写）
     * @note   默认行为：标记已断开 + 调用 OnServiceDied 注册的回调。
     *         本函数在 owner event-loop 的服务状态通知分发过程中被调用，不得在此
     *         直接退订（会修改正在遍历的订阅容器，违反约束 2）；退订由
     *         disconnect() 依据 watch_id_ 在析构/主动断开时完成。 */
    virtual void onServiceDeath();
    
    OmniRuntime& runtime_;
    std::string service_name_;
    std::string instance_host_id_;
    std::string instance_id_;
    std::string connection_key_;
    std::atomic<bool> connected_;
    std::atomic<bool> auto_connect_enabled_;
    std::atomic<bool> ever_connected_;
    // 是否已向 runtime 注册服务状态 watch。与 connected_ 分开：服务离线会把
    // connected_ 置 false，但 watch lambda 捕获 this，析构时必须据此退订，否则
    // 服务重启后状态事件会回调已析构的 proxy（UAF）
    std::atomic<uint64_t> watch_id_;
    // 是否绑定到显式实例（setInstance 时确定）。取代脆弱的
    // connection_key_ == service_name_ 字符串比较
    bool instance_bound_;
    bool heartbeat_configured_;
    uint32_t heartbeat_interval_ms_;
    uint32_t heartbeat_timeout_ms_;
    mutable std::mutex state_mutex_;
    std::function<void()> death_callback_;
};

} // namespace omnibinder

#endif // OMNIBINDER_PROXY_BASE_H
