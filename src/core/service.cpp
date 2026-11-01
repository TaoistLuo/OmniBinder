#include "omnibinder/service.h"

namespace omnibinder {

Service::Service(const std::string& name)
    : name_(name)
    , port_(0)
    , register_host_()
    , instance_id_()
    , shm_config_()
    , runtime_(NULL)
{
}

Service::~Service() {
}

const std::string& Service::name() const {
    return name_;
}

const char* Service::serviceName() const {
    return name_.c_str();
}

uint16_t Service::port() const {
    return port_.load(std::memory_order_acquire);
}

void Service::setPort(uint16_t p) {
    port_.store(p, std::memory_order_release);
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

void Service::setInstanceId(const std::string& instance_id) {
    instance_id_ = instance_id;
}

const std::string& Service::instanceId() const {
    return instance_id_;
}

OmniRuntime* Service::runtime() const {
    return runtime_;
}

void Service::declarePublishedTopic(const std::string& topic_name, uint32_t idl_hash) {
    for (size_t i = 0; i < published_topics_.size(); ++i) {
        if (published_topics_[i].name == topic_name) {
            published_topics_[i].idl_hash = idl_hash;
            return;
        }
    }
    published_topics_.push_back(PublishedTopicInfo(topic_name, idl_hash));
}

void Service::onStart() {}
void Service::onStop() {}
void Service::onClientConnected(const std::string&) {}
void Service::onClientDisconnected(const std::string&) {}

} // namespace omnibinder
