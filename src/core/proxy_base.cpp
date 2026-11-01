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
      instance_bound_(false), heartbeat_configured_(false),
      heartbeat_interval_ms_(DEFAULT_HEARTBEAT_INTERVAL),
      heartbeat_timeout_ms_(DEFAULT_HEARTBEAT_TIMEOUT) {
    connected_.store(false, std::memory_order_relaxed);
    death_subscribed_.store(false, std::memory_order_relaxed);
    auto_connect_enabled_.store(true, std::memory_order_relaxed);
    ever_connected_.store(false, std::memory_order_relaxed);
    runtime_.registerProxy(this);
}

ServiceProxyBase::~ServiceProxyBase() {
    runtime_.unregisterProxy(this);
    disconnect();
}

void ServiceProxyBase::setInstance(const std::string& host_id, const std::string& instance_id) {
    const bool keep_auto_connect = auto_connect_enabled_.load();
    const bool next_bound = !(host_id.empty() && instance_id.empty());
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const std::string next_key = next_bound
            ? makeServiceConnectionKey(service_name_, host_id, instance_id)
            : service_name_;
        changed = next_bound != instance_bound_ || next_key != connection_key_;
    }

    // 运行期切换目标实例：旧连接必须断开，交由 runtime 的自动重连/重试重新连到新目标。
    if (changed && runtime_.isRunning()) {
        disconnect();
        ever_connected_.store(false, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    instance_host_id_ = host_id;
    instance_id_ = instance_id;
    instance_bound_ = next_bound;
    connection_key_ = next_bound
        ? makeServiceConnectionKey(service_name_, host_id, instance_id)
        : service_name_;
    auto_connect_enabled_.store(keep_auto_connect, std::memory_order_relaxed);
}

int ServiceProxyBase::connect(const std::string& host_id, const std::string& instance_id) {
    setInstance(host_id, instance_id);
    return connect();
}

int ServiceProxyBase::connect() {
    auto_connect_enabled_.store(true, std::memory_order_relaxed);

    std::string service_name;
    std::string host_id;
    std::string instance_id;
    std::string connection_key;
    bool instance_bound = false;
    bool heartbeat_configured = false;
    uint32_t heartbeat_interval_ms = DEFAULT_HEARTBEAT_INTERVAL;
    uint32_t heartbeat_timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        service_name = service_name_;
        host_id = instance_host_id_;
        instance_id = instance_id_;
        connection_key = connection_key_;
        instance_bound = instance_bound_;
        heartbeat_configured = heartbeat_configured_;
        heartbeat_interval_ms = heartbeat_interval_ms_;
        heartbeat_timeout_ms = heartbeat_timeout_ms_;
    }

    // 重连时把用户配置过的策略重新登记到 runtime，使 Session Hello 携带该值；
    // 未配置时 ensureDataSession 使用默认 3s/10s。
    if (heartbeat_configured) {
        runtime_.startHeartbeat(connection_key, heartbeat_interval_ms, heartbeat_timeout_ms);
    }
    int ret = instance_bound
        ? runtime_.connectServiceInstance(service_name, host_id, instance_id)
        : runtime_.connectService(service_name);
    if (ret == 0) {
        connected_.store(true, std::memory_order_release);
        ever_connected_.store(true, std::memory_order_release);
        // 连接键即数据面索引：显式实例时后续心跳/重连均作用在该实例的条目上。
        // 数据面心跳已由 ensureDataSession 在 Session Hello 后自动启动；
        // 这里仅保留自动重连。若用户在 connect() 前配置过心跳，connectService
        // 内的 Session Hello 会直接携带该策略，无需再次协商。
        enableAutoReconnect(true);
        // 未绑定实例时按名订阅；绑定实例后按实例订阅，只有目标实例死亡才触发
        int sub_ret = 0;
        if (instance_bound) {
            sub_ret = runtime_.subscribeServiceInstanceDeath(service_name, host_id,
                instance_id,
                [this](const std::string&, const std::string&, const std::string&) {
                    this->onServiceDeath();
                });
        } else {
            sub_ret = runtime_.subscribeServiceDeath(service_name,
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
    auto_connect_enabled_.store(false, std::memory_order_relaxed);

    std::string service_name;
    std::string host_id;
    std::string instance_id;
    std::string connection_key;
    bool instance_bound = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        service_name = service_name_;
        host_id = instance_host_id_;
        instance_id = instance_id_;
        connection_key = connection_key_;
        instance_bound = instance_bound_;
    }

    // 退订依据 death_subscribed_ 而非 connected_：服务死亡会把 connected_ 置 false，
    // 但死亡订阅（lambda 捕获 this）必须在此清除，否则服务重启再死即 UAF。
    // unsubscribe* 幂等，重复调用安全。
    if (death_subscribed_.load(std::memory_order_acquire)) {
        if (instance_bound) {
            runtime_.unsubscribeServiceInstanceDeath(service_name, host_id, instance_id);
        } else {
            runtime_.unsubscribeServiceDeath(service_name);
        }
        death_subscribed_.store(false, std::memory_order_release);
    }
    if (connected_.load(std::memory_order_acquire)) {
        runtime_.stopHeartbeat(connection_key);
        if (instance_bound) {
            runtime_.disconnectServiceInstance(service_name, host_id, instance_id);
        } else {
            runtime_.disconnectService(service_name);
        }
        connected_.store(false, std::memory_order_release);
    }
}

std::string ServiceProxyBase::connectionKey() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return connection_key_;
}

void ServiceProxyBase::onServiceDeath() {
    connected_.store(false, std::memory_order_release);
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callback = death_callback_;
    }
    if (callback) {
        callback();
    }
}

bool ServiceProxyBase::isConnected() const {
    return connected_.load(std::memory_order_acquire);
}

void ServiceProxyBase::enableAutoReconnect(bool enable) {
    auto_connect_enabled_.store(enable, std::memory_order_relaxed);
    runtime_.enableAutoReconnect(connectionKey(), enable);
}

void ServiceProxyBase::setReconnectInterval(uint32_t interval_ms) {
    runtime_.setReconnectInterval(connectionKey(), interval_ms);
}

void ServiceProxyBase::startHeartbeat(uint32_t interval_ms, uint32_t timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        heartbeat_configured_ = true;
        heartbeat_interval_ms_ = interval_ms;
        heartbeat_timeout_ms_ = timeout_ms;
    }
    runtime_.startHeartbeat(connectionKey(), interval_ms, timeout_ms);
}

void ServiceProxyBase::stopHeartbeat() {
    runtime_.stopHeartbeat(connectionKey());
}

void ServiceProxyBase::OnServiceDied(const std::function<void()>& callback) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    death_callback_ = callback;
}

} // namespace omnibinder
