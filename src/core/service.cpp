#include "omnibinder/service.h"

namespace omnibinder {

Service::Service(const std::string& name)
    : name_(name)
    , port_(0)
    , register_host_()
    , shm_config_()
    , runtime_(NULL)
    , invoke_error_code_(0)
{
}

Service::~Service() {
}

const std::string& Service::name() const {
    return name_;
}

uint16_t Service::port() const {
    return port_;
}

void Service::setPort(uint16_t p) {
    port_ = p;
}

void Service::setRegisterHost(const std::string& host) {
    register_host_ = host;
}

const std::string& Service::getRegisterHost() const {
    return register_host_;
}

void Service::setShmConfig(const ShmConfig& config) {
    shm_config_ = config;
}

ShmConfig Service::shmConfig() const {
    return shm_config_;
}

void Service::reportInvokeError(int32_t error_code) {
    invoke_error_code_ = error_code;
}

int32_t Service::consumeInvokeError() {
    const int32_t error_code = invoke_error_code_;
    invoke_error_code_ = 0;
    return error_code;
}

OmniRuntime* Service::runtime() const {
    return runtime_;
}

void Service::onStart() {}
void Service::onStop() {}
void Service::onClientConnected(const std::string&) {}
void Service::onClientDisconnected(const std::string&) {}

} // namespace omnibinder
