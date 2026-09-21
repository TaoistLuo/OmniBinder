/**************************************************************************************************
 * @file        proxy_base.cpp
 * @brief       ServiceProxyBase 实现
 * @details     所有非构造/析构的方法实现。
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

#include "omnibinder/proxy_base.h"

namespace omnibinder {

ServiceProxyBase::ServiceProxyBase(OmniRuntime& runtime, const std::string& service_name)
    : runtime_(runtime), service_name_(service_name), connection_key_(service_name),
      instance_bound_(false) {
    connected_.store(false, std::memory_order_relaxed);
    death_subscribed_.store(false, std::memory_order_relaxed);
}

ServiceProxyBase::~ServiceProxyBase() {
    disconnect();
}

void ServiceProxyBase::setInstance(const std::string& host_id, const std::string& instance_id) {
    instance_host_id_ = host_id;
    instance_id_ = instance_id;
    instance_bound_ = !(host_id.empty() && instance_id.empty());
    if (!instance_bound_) {
        connection_key_ = service_name_;
    } else {
        connection_key_ = makeServiceConnectionKey(service_name_, host_id, instance_id);
    }
}

int ServiceProxyBase::connect(const std::string& host_id, const std::string& instance_id) {
    setInstance(host_id, instance_id);
    return connect();
}

int ServiceProxyBase::connect() {
    int ret = instance_bound_
        ? runtime_.connectServiceInstance(service_name_, instance_host_id_, instance_id_)
        : runtime_.connectService(service_name_);
    if (ret == 0) {
        connected_.store(true, std::memory_order_release);
        // 连接键即数据面索引：显式实例时后续心跳/重连均作用在该实例的条目上
        enableAutoReconnect(true);
        startHeartbeat();
        // 未绑定实例时按名订阅；绑定实例后按实例订阅，只有目标实例死亡才触发
        int sub_ret = 0;
        if (instance_bound_) {
            sub_ret = runtime_.subscribeServiceInstanceDeath(service_name_, instance_host_id_,
                instance_id_,
                [this](const std::string&, const std::string&, const std::string&) {
                    this->onServiceDeath();
                });
        } else {
            sub_ret = runtime_.subscribeServiceDeath(service_name_,
                [this](const std::string&) { this->onServiceDeath(); });
        }
        // 只置真不清零：重复 connect() 时 SM 以"已订阅"拒绝，若清 false 会导致
        // 析构漏退订、残留捕获 this 的回调在服务再死时 UAF（约束 1）
        if (sub_ret == 0) {
            death_subscribed_.store(true, std::memory_order_release);
        }
    }
    return ret;
}

void ServiceProxyBase::disconnect() {
    // 退订依据 death_subscribed_ 而非 connected_：服务死亡会把 connected_ 置 false，
    // 但死亡订阅（lambda 捕获 this）必须在此清除，否则服务重启再死即 UAF。
    // unsubscribe* 幂等，重复调用安全。
    if (death_subscribed_.load(std::memory_order_acquire)) {
        if (instance_bound_) {
            runtime_.unsubscribeServiceInstanceDeath(service_name_, instance_host_id_, instance_id_);
        } else {
            runtime_.unsubscribeServiceDeath(service_name_);
        }
        death_subscribed_.store(false, std::memory_order_release);
    }
    if (connected_.load(std::memory_order_acquire)) {
        stopHeartbeat();
        if (instance_bound_) {
            runtime_.disconnectServiceInstance(service_name_, instance_host_id_, instance_id_);
        } else {
            runtime_.disconnectService(service_name_);
        }
        connected_.store(false, std::memory_order_release);
    }
}

const std::string& ServiceProxyBase::connectionKey() const {
    return connection_key_;
}

void ServiceProxyBase::onServiceDeath() {
    connected_.store(false, std::memory_order_release);
    if (death_callback_) {
        death_callback_();
    }
}

bool ServiceProxyBase::isConnected() const {
    return connected_.load(std::memory_order_acquire);
}

void ServiceProxyBase::enableAutoReconnect(bool enable) {
    runtime_.enableAutoReconnect(connection_key_, enable);
}

void ServiceProxyBase::setReconnectInterval(uint32_t interval_ms) {
    runtime_.setReconnectInterval(connection_key_, interval_ms);
}

void ServiceProxyBase::startHeartbeat(uint32_t interval_ms, uint32_t timeout_ms) {
    runtime_.startHeartbeat(connection_key_, interval_ms, timeout_ms);
}

void ServiceProxyBase::stopHeartbeat() {
    runtime_.stopHeartbeat(connection_key_);
}

void ServiceProxyBase::OnServiceDied(const std::function<void()>& callback) {
    death_callback_ = callback;
}

} // namespace omnibinder
