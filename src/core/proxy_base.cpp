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
    : runtime_(runtime), service_name_(service_name) {
    connected_.store(false, std::memory_order_relaxed);
}

ServiceProxyBase::~ServiceProxyBase() {
    disconnect();
}

int ServiceProxyBase::connect() {
    int ret = runtime_.connectService(service_name_);
    if (ret == 0) {
        connected_.store(true, std::memory_order_release);
        enableAutoReconnect(true);
        startHeartbeat();
        runtime_.subscribeServiceDeath(service_name_,
            [this](const std::string&) { this->onServiceDeath(); });
    }
    return ret;
}

void ServiceProxyBase::disconnect() {
    if (connected_.load(std::memory_order_acquire)) {
        stopHeartbeat();
        runtime_.unsubscribeServiceDeath(service_name_);
        runtime_.disconnectService(service_name_);
        connected_.store(false, std::memory_order_release);
    }
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
    runtime_.enableAutoReconnect(service_name_, enable);
}

void ServiceProxyBase::setReconnectInterval(uint32_t interval_ms) {
    runtime_.setReconnectInterval(service_name_, interval_ms);
}

void ServiceProxyBase::startHeartbeat(uint32_t interval_ms, uint32_t timeout_ms) {
    runtime_.startHeartbeat(service_name_, interval_ms, timeout_ms);
}

void ServiceProxyBase::stopHeartbeat() {
    runtime_.stopHeartbeat(service_name_);
}

void ServiceProxyBase::OnServiceDied(const std::function<void()>& callback) {
    death_callback_ = callback;
}

} // namespace omnibinder
