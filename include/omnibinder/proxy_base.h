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

namespace omnibinder {

/* @brief 所有生成 Proxy 的公共基类：封装 runtime 引用、连接状态与 RPC 超时等横切关注点 */
class ServiceProxyBase {
public:
    /* @brief 创建 Proxy 基类实例
     * @param[in]  runtime      OmniRuntime 引用
     * @param[in]  service_name 目标服务名称 */
    ServiceProxyBase(OmniRuntime& runtime, const std::string& service_name);

    virtual ~ServiceProxyBase();
    
    /* @brief 建立到目标服务的数据面连接
     * @return 0 成功，<0 失败
     * @note   成功后会启用自动重连和心跳 */
    int connect();

    /* @brief 绑定到目标服务的指定实例并建立数据面连接
     * @param[in]  host_id     目标板级主机身份（ServiceInfo.host_id）
     * @param[in]  instance_id 目标实例标识（ServiceInfo.instance_id）
     * @return 0 成功；ERR_SERVICE_NOT_FOUND 无匹配实例；网络/协议错误时返回对应错误码
     * @note   等价于先 setInstance(host_id, instance_id) 再 connect()。
     *         绑定后本代理的 invoke / 心跳 / 重连 / 断开均寻址该实例。
     *         话题的服务级订阅仍按 topic 全局身份；实例级订阅由生成 Proxy 的
     *         Subscribe<Topic>Instance 提供 */
    int connect(const std::string& host_id, const std::string& instance_id);

    /* @brief 设置实例选择器（下一次 connect() 生效）
     * @param[in]  host_id     目标板级主机身份
     * @param[in]  instance_id 目标实例标识
     * @note   仅改变后续 connect() 的目标；对已建立的连接不生效，需先 disconnect() */
    void setInstance(const std::string& host_id, const std::string& instance_id);

    /* @brief 断开到目标服务的连接 */
    void disconnect();
    
    /* @brief 查询连接状态
     * @return true 已连接，false 未连接 */
    bool isConnected() const;

    /* @brief 启用/禁用自动重连
     * @param[in]  enable true 启用（默认） */
    void enableAutoReconnect(bool enable = true);

    /* @brief 设置重连间隔
     * @param[in]  interval_ms 重连间隔（毫秒） */
    void setReconnectInterval(uint32_t interval_ms);

    /* @brief 启动到目标服务的数据面连接心跳检测
     * @param[in]  interval_ms 心跳间隔（毫秒，默认 5000）
     * @param[in]  timeout_ms  超时阈值（毫秒，默认 10000）
     * @note   检测的是客户端到目标服务的直连保活，不涉及目标服务在
     *         ServiceManager 的注册心跳 */
    void startHeartbeat(uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000);

    /* @brief 停止到目标服务的数据面连接心跳检测
     * @note   只停止本代理的检测；不注销目标服务、不影响其 SM 注册心跳。
     *         disconnect() 内部会调用本函数 */
    void stopHeartbeat();

    /* @brief 注册服务死亡回调
     * @param[in]  callback 死亡时调用的回调函数 */
    void OnServiceDied(const std::function<void()>& callback);
    
protected:
    /* @brief 当前实例绑定的数据面连接键
     * @return 默认实例返回服务名；绑定实例后返回 makeServiceConnectionKey(...)
     * @note   生成的 Proxy 方法据此寻址数据面连接（默认路径与服务名一致，
     *         行为与历史版本完全相同） */
    const std::string& connectionKey() const;

    /* @brief 服务死亡回调（子类可重写）
     * @note   默认行为：标记已断开 + 调用 OnServiceDied 注册的回调。
     *         本函数在 owner event-loop 的死亡通知分发过程中被调用，不得在此
     *         直接退订（会修改正在遍历的订阅容器，违反约束 2）；退订由
     *         disconnect() 依据 death_subscribed_ 在析构/主动断开时完成。 */
    virtual void onServiceDeath();
    
    OmniRuntime& runtime_;
    std::string service_name_;
    std::string instance_host_id_;
    std::string instance_id_;
    std::string connection_key_;
    std::atomic<bool> connected_;
    // 是否已向 runtime 订阅死亡通知。与 connected_ 分开：服务死亡会把 connected_
    // 置 false，但订阅仍然存在且 lambda 捕获 this，析构时必须据此退订，否则
    // 服务重启再死会回调已析构的 proxy（UAF）
    std::atomic<bool> death_subscribed_;
    // 是否绑定到显式实例（setInstance 时确定）。取代脆弱的
    // connection_key_ == service_name_ 字符串比较
    bool instance_bound_;
    std::function<void()> death_callback_;
};

} // namespace omnibinder

#endif // OMNIBINDER_PROXY_BASE_H
