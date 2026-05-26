#ifndef OMNIBINDER_PROXY_BASE_H
#define OMNIBINDER_PROXY_BASE_H

#include "omnibinder/runtime.h"
#include <atomic>
#include <functional>
#include <string>

namespace omnibinder {

class ServiceProxyBase {
public:
    ServiceProxyBase(OmniRuntime& runtime, const std::string& service_name)
        : runtime_(runtime), service_name_(service_name) {
        connected_.store(false, std::memory_order_relaxed);
    }
    
    virtual ~ServiceProxyBase() { disconnect(); }
    
    int connect() {
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
    
    void disconnect() {
        if (connected_.load(std::memory_order_acquire)) {
            stopHeartbeat();
            runtime_.unsubscribeServiceDeath(service_name_);
            runtime_.disconnectService(service_name_);
            connected_.store(false, std::memory_order_release);
        }
    }
    
    bool isConnected() const { return connected_.load(std::memory_order_acquire); }
    
    void enableAutoReconnect(bool enable = true) {
        runtime_.enableAutoReconnect(service_name_, enable);
    }
    
    void setReconnectInterval(uint32_t interval_ms) {
        runtime_.setReconnectInterval(service_name_, interval_ms);
    }
    
    void startHeartbeat(uint32_t interval_ms = 5000, uint32_t timeout_ms = 10000) {
        runtime_.startHeartbeat(service_name_, interval_ms, timeout_ms);
    }
    
    void stopHeartbeat() {
        runtime_.stopHeartbeat(service_name_);
    }
    
    void OnServiceDied(const std::function<void()>& callback) {
        death_callback_ = callback;
    }
    
protected:
    virtual void onServiceDeath() {
        connected_.store(false, std::memory_order_release);
        if (death_callback_) {
            death_callback_();
        }
    }
    
    OmniRuntime& runtime_;
    std::string service_name_;
    std::atomic<bool> connected_;
    std::function<void()> death_callback_;
};

} // namespace omnibinder

#endif // OMNIBINDER_PROXY_BASE_H
