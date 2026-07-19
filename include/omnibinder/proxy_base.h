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

class ServiceProxyBase {
public:
    /*
     * @brief  创建 Proxy 基类实例
     * @param[in]  runtime      OmniRuntime 引用
     * @param[in]  service_name 目标服务名称
     */
    ServiceProxyBase(OmniRuntime& runtime, const std::string& service_name);

    virtual ~ServiceProxyBase();
    
    /*
     * @brief  建立到目标服务的数据面连接
     * @return 0 成功，<0 失败
     * @note   成功后会启用自动重连和心跳
     */
    int connect();
    
    /*
     * @brief  断开到目标服务的连接
     */
    void disconnect();
    
    /*
     * @brief  查询连接状态
     * @return true 已连接，false 未连接
     */
    bool isConnected() const;

    /*
     * @brief  启用/禁用自动重连
     * @param[in]  enable true 启用（默认）
     */
    void enableAutoReconnect(bool enable = true);

    /*
     * @brief  设置重连间隔
     * @param[in]  interval_ms 重连间隔（毫秒）
     */
    void setReconnectInterval(uint32_t interval_ms);

    /*
     * @brief  启动心跳检测
     * @param[in]  interval_ms 心跳间隔（毫秒，默认 5000）
     * @param[in]  timeout_ms  超时阈值（毫秒，默认 10000）
     */
    void startHeartbeat(uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000);

    /*
     * @brief  停止心跳检测
     */
    void stopHeartbeat();

    /*
     * @brief  注册服务死亡回调
     * @param[in]  callback 死亡时调用的回调函数
     */
    void OnServiceDied(const std::function<void()>& callback);
    
protected:
    /*
     * @brief  服务死亡回调（子类可重写）
     * @note   默认行为：标记已断开 + 调用 OnServiceDied 注册的回调
     */
    virtual void onServiceDeath();
    
    OmniRuntime& runtime_;
    std::string service_name_;
    std::atomic<bool> connected_;
    std::function<void()> death_callback_;
};

} // namespace omnibinder

#endif // OMNIBINDER_PROXY_BASE_H
