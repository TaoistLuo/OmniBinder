/**************************************************************************************************
 * @file        death_notifier.h
 * @brief       服务死亡通知管理器
 * @details     管理服务死亡通知的订阅关系。客户端可订阅目标服务的死亡事件，
 *              当目标服务断开或心跳超时时，DeathNotifier 返回所有需要通知的
 *              订阅者 fd 列表。内部维护双向映射（服务→订阅者、订阅者→服务），
 *              支持按 fd 批量清理断开的订阅者。仅 ServiceManager owner 线程访问。
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
#ifndef OMNIBINDER_DEATH_NOTIFIER_H
#define OMNIBINDER_DEATH_NOTIFIER_H

#include <string>
#include <map>
#include <set>
#include <vector>

namespace omnibinder {

// ============================================================
// DeathNotifier - 管理死亡通知订阅关系
//
// @brief  管理死亡通知订阅关系
// @details 服务可订阅目标服务死亡（断开或超时）的通知。服务死亡时，
//          通过其控制连接 fd 通知全部订阅者。
// ============================================================
class DeathNotifier {
public:
    DeathNotifier();
    ~DeathNotifier();

    // 禁止拷贝
    DeathNotifier(const DeathNotifier&) = delete;
    DeathNotifier& operator=(const DeathNotifier&) = delete;

    /*
     * @brief  订阅目标服务的死亡通知
     * @param[in]  subscriber_fd  订阅者控制连接 fd
     * @param[in]  target_service 目标服务名称
     * @return true 表示订阅成功；重复订阅返回 false
     */
    bool subscribe(int subscriber_fd, const std::string& target_service);

    /*
     * @brief  取消订阅目标服务的死亡通知
     * @param[in]  subscriber_fd  订阅者控制连接 fd
     * @param[in]  target_service 目标服务名称
     * @return true 表示订阅已找到并移除
     */
    bool unsubscribe(int subscriber_fd, const std::string& target_service);

    /*
     * @brief  通知服务死亡，返回需要通知的订阅者
     * @param[in]  dead_service_name 死亡服务名称
     * @return 需要通知的订阅者 fd 列表
     * @note   同时移除该死亡服务的全部订阅关系
     */
    std::vector<int> notify(const std::string& dead_service_name);

    /*
     * @brief  移除该订阅者的全部订阅关系
     * @param[in]  subscriber_fd 订阅者控制连接 fd
     * @note   订阅者断开连接时调用
     */
    void removeSubscriber(int subscriber_fd);

    /*
     * @brief  获取该订阅者关注的服务列表
     * @param[in]  subscriber_fd 订阅者控制连接 fd
     * @return 被关注的服务名列表
     */
    std::vector<std::string> getWatchedServices(int subscriber_fd) const;

    /*
     * @brief  获取指定服务的订阅者数量
     * @param[in]  service_name 服务名称
     * @return 订阅者数量
     */
    size_t subscriberCount(const std::string& service_name) const;

private:

    // target_service -> 订阅者 fd 集合
    std::map<std::string, std::set<int>> service_to_subscribers_;

    // subscriber_fd -> 目标服务集合
    std::map<int, std::set<std::string>> subscriber_to_services_;
};

} // namespace omnibinder

#endif // OMNIBINDER_DEATH_NOTIFIER_H
