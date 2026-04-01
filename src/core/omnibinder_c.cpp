/**************************************************************************************************
 * @file        omnibinder_c.cpp
 * @brief       OmniBinder C 语言接口封装实现
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-02-28
 *************************************************************************************************/
#include "omnibinder/omnibinder_c.h"
#include "omnibinder/omnibinder.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

/* ============================================================
 * 内部结构体定义
 * ============================================================ */

struct omni_buffer_t {
    omnibinder::Buffer buf;
    bool read_ok;
    int32_t error_code;

    omni_buffer_t() : read_ok(true), error_code(0) {}
    omni_buffer_t(const uint8_t* data, size_t len) : buf(data, len), read_ok(true), error_code(0) {}
};

/* C 服务桥接：将 C++ Service 的 onInvoke 转发到 C 回调 */
class CServiceBridge : public omnibinder::Service {
public:
    CServiceBridge(const char* name, uint32_t interface_id,
                   omni_invoke_callback_t callback, void* user_data)
        : Service(name)
        , callback_(callback)
        , user_data_(user_data)
    {
        iface_.interface_id = interface_id;
        iface_.name = name;
    }

    const char* serviceName() const override { return name().c_str(); }
    const omnibinder::InterfaceInfo& interfaceInfo() const override { return iface_; }

    void addMethod(uint32_t method_id, const char* method_name,
                   const char* param_types, const char* return_type) {
        iface_.methods.push_back(omnibinder::MethodInfo(
            method_id,
            method_name ? method_name : "",
            param_types ? param_types : "",
            return_type ? return_type : "void"));
    }

    omnibinder::InterfaceInfo iface_;

protected:
    int onInvoke(uint32_t method_id, const omnibinder::Buffer& request,
                  omnibinder::Buffer& response) override {
        /* 将 C++ Buffer 包装为 omni_buffer_t 供 C 回调使用 */
        omni_buffer_t req_wrap;
        req_wrap.buf = omnibinder::Buffer(request.data(), request.size());

        omni_buffer_t resp_wrap;

        if (!callback_) {
            return static_cast<int>(omnibinder::ErrorCode::ERR_INVOKE_FAILED);
        }

        int status = callback_(method_id, &req_wrap, &resp_wrap, user_data_);
        if (status != 0) {
            return status;
        }

        /* 将 C 回调写入的响应数据拷贝回 C++ Buffer */
        if (resp_wrap.buf.size() > 0) {
            if (!response.writeRaw(resp_wrap.buf.data(), resp_wrap.buf.size())) {
                return static_cast<int>(omnibinder::ErrorCode::ERR_SERIALIZE);
            }
        }

        return 0;
    }

private:
    omni_invoke_callback_t callback_;
    void* user_data_;
};

struct omni_service_t {
    CServiceBridge* bridge;
};

struct omni_runtime_t {
    omnibinder::OmniRuntime runtime;
};

namespace {

inline void markReadError(omni_buffer_t* buf) {
    buf->read_ok = false;
    buf->error_code = -501;
}

}

/* ============================================================
 * Buffer API 实现
 * ============================================================ */

extern "C" {

omni_buffer_t* omni_buffer_create(void) {
    return new omni_buffer_t();
}

omni_buffer_t* omni_buffer_create_from(const uint8_t* data, size_t len) {
    return new omni_buffer_t(data, len);
}

void omni_buffer_destroy(omni_buffer_t* buf) {
    delete buf;
}

void omni_buffer_reset(omni_buffer_t* buf) {
    if (buf) {
        buf->buf.reset();
        buf->read_ok = true;
        buf->error_code = 0;
    }
}

const uint8_t* omni_buffer_data(const omni_buffer_t* buf) {
    return buf ? buf->buf.data() : NULL;
}

size_t omni_buffer_size(const omni_buffer_t* buf) {
    return buf ? buf->buf.size() : 0;
}

int omni_buffer_read_ok(const omni_buffer_t* buf) {
    return (buf && buf->read_ok) ? 1 : 0;
}

void omni_buffer_clear_error(omni_buffer_t* buf) {
    if (buf) {
        buf->read_ok = true;
        buf->error_code = 0;
    }
}

void omni_buffer_mark_error(omni_buffer_t* buf, int32_t error_code) {
    if (buf) {
        buf->read_ok = false;
        buf->error_code = error_code;
    }
}

int32_t omni_buffer_error(const omni_buffer_t* buf) {
    return buf ? buf->error_code : 0;
}

/* 写入 */
void omni_buffer_write_bool(omni_buffer_t* buf, uint8_t val) {
    if (buf) (void)buf->buf.writeBool(val != 0);
}
void omni_buffer_write_int8(omni_buffer_t* buf, int8_t val) {
    if (buf) (void)buf->buf.writeInt8(val);
}
void omni_buffer_write_uint8(omni_buffer_t* buf, uint8_t val) {
    if (buf) (void)buf->buf.writeUint8(val);
}
void omni_buffer_write_int16(omni_buffer_t* buf, int16_t val) {
    if (buf) (void)buf->buf.writeInt16(val);
}
void omni_buffer_write_uint16(omni_buffer_t* buf, uint16_t val) {
    if (buf) (void)buf->buf.writeUint16(val);
}
void omni_buffer_write_int32(omni_buffer_t* buf, int32_t val) {
    if (buf) (void)buf->buf.writeInt32(val);
}
void omni_buffer_write_uint32(omni_buffer_t* buf, uint32_t val) {
    if (buf) (void)buf->buf.writeUint32(val);
}
void omni_buffer_write_int64(omni_buffer_t* buf, int64_t val) {
    if (buf) (void)buf->buf.writeInt64(val);
}
void omni_buffer_write_uint64(omni_buffer_t* buf, uint64_t val) {
    if (buf) (void)buf->buf.writeUint64(val);
}
void omni_buffer_write_float32(omni_buffer_t* buf, float val) {
    if (buf) (void)buf->buf.writeFloat32(val);
}
void omni_buffer_write_float64(omni_buffer_t* buf, double val) {
    if (buf) (void)buf->buf.writeFloat64(val);
}
void omni_buffer_write_string(omni_buffer_t* buf, const char* val, uint32_t len) {
    if (buf) {
        if (!val) {
            len = 0;
        }
        std::string s(val ? val : "", len);
        (void)buf->buf.writeString(s);
    }
}
void omni_buffer_write_bytes(omni_buffer_t* buf, const uint8_t* data, uint32_t len) {
    if (buf) {
        std::vector<uint8_t> v;
        if (data && len > 0) {
            v.assign(data, data + len);
        }
        (void)buf->buf.writeBytes(v);
    }
}

/* 读取 */
uint8_t omni_buffer_read_bool(omni_buffer_t* buf) {
    if (!buf) return 0;
    bool value = false;
    if (!buf->buf.tryReadBool(value)) {
        markReadError(buf);
        return 0;
    }
    return value ? 1 : 0;
}

int8_t omni_buffer_read_int8(omni_buffer_t* buf) {
    if (!buf) return 0;
    int8_t value = 0;
    if (!buf->buf.tryReadInt8(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

uint8_t omni_buffer_read_uint8(omni_buffer_t* buf) {
    if (!buf) return 0;
    uint8_t value = 0;
    if (!buf->buf.tryReadUint8(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

int16_t omni_buffer_read_int16(omni_buffer_t* buf) {
    if (!buf) return 0;
    int16_t value = 0;
    if (!buf->buf.tryReadInt16(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

uint16_t omni_buffer_read_uint16(omni_buffer_t* buf) {
    if (!buf) return 0;
    uint16_t value = 0;
    if (!buf->buf.tryReadUint16(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

int32_t omni_buffer_read_int32(omni_buffer_t* buf) {
    if (!buf) return 0;
    int32_t value = 0;
    if (!buf->buf.tryReadInt32(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

uint32_t omni_buffer_read_uint32(omni_buffer_t* buf) {
    if (!buf) return 0;
    uint32_t value = 0;
    if (!buf->buf.tryReadUint32(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

int64_t omni_buffer_read_int64(omni_buffer_t* buf) {
    if (!buf) return 0;
    int64_t value = 0;
    if (!buf->buf.tryReadInt64(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

uint64_t omni_buffer_read_uint64(omni_buffer_t* buf) {
    if (!buf) return 0;
    uint64_t value = 0;
    if (!buf->buf.tryReadUint64(value)) {
        markReadError(buf);
        return 0;
    }
    return value;
}

float omni_buffer_read_float32(omni_buffer_t* buf) {
    if (!buf) return 0.0f;
    float value = 0.0f;
    if (!buf->buf.tryReadFloat32(value)) {
        markReadError(buf);
        return 0.0f;
    }
    return value;
}

double omni_buffer_read_float64(omni_buffer_t* buf) {
    if (!buf) return 0.0;
    double value = 0.0;
    if (!buf->buf.tryReadFloat64(value)) {
        markReadError(buf);
        return 0.0;
    }
    return value;
}

char* omni_buffer_read_string(omni_buffer_t* buf, uint32_t* out_len) {
    if (!buf) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    std::string value;
    if (!buf->buf.tryReadString(value)) {
        markReadError(buf);
        if (out_len) *out_len = 0;
        return NULL;
    }
    uint32_t len = static_cast<uint32_t>(value.size());
    char* result = (char*)malloc(len + 1);
    if (result) {
        memcpy(result, value.c_str(), len + 1);
    }
    if (out_len) *out_len = len;
    return result;
}

uint8_t* omni_buffer_read_bytes(omni_buffer_t* buf, uint32_t* out_len) {
    if (!buf || !out_len) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    std::vector<uint8_t> value;
    if (!buf->buf.tryReadBytes(value)) {
        markReadError(buf);
        *out_len = 0;
        return NULL;
    }
    uint32_t len = static_cast<uint32_t>(value.size());
    uint8_t* result = (uint8_t*)malloc(len);
    if (result && len > 0) {
        memcpy(result, value.data(), len);
    }
    *out_len = len;
    return result;
}

/* ============================================================
 * Service API 实现
 * ============================================================ */

omni_service_t* omni_service_create(const char* name, uint32_t interface_id,
    omni_invoke_callback_t callback, void* user_data)
{
    omni_service_t* svc = new omni_service_t();
    svc->bridge = new CServiceBridge(name, interface_id, callback, user_data);
    return svc;
}

void omni_service_destroy(omni_service_t* svc) {
    if (svc) {
        delete svc->bridge;
        delete svc;
    }
}

void omni_service_add_method(omni_service_t* svc, uint32_t method_id, const char* method_name) {
    omni_service_add_method_ex(svc, method_id, method_name, "", "void");
}

void omni_service_add_method_ex(omni_service_t* svc, uint32_t method_id, const char* method_name,
    const char* param_types, const char* return_type)
{
    if (svc && svc->bridge) {
        svc->bridge->addMethod(method_id, method_name, param_types, return_type);
    }
}

uint16_t omni_service_port(const omni_service_t* svc) {
    return (svc && svc->bridge) ? svc->bridge->port() : 0;
}

void omni_service_set_register_host(omni_service_t* svc, const char* host) {
    if (svc && svc->bridge) {
        svc->bridge->setRegisterHost(host ? host : "");
    }
}

const char* omni_service_get_register_host(const omni_service_t* svc) {
    if (!svc || !svc->bridge) {
        return NULL;
    }
    return svc->bridge->getRegisterHost().c_str();
}

/* ============================================================
 * Runtime API 实现
 * ============================================================ */

omni_runtime_t* omni_runtime_create(void) {
    return new omni_runtime_t();
}

void omni_runtime_destroy(omni_runtime_t* runtime) {
    delete runtime;
}

int omni_runtime_init(omni_runtime_t* runtime, const char* sm_host, uint16_t sm_port) {
    if (!runtime) return -1;
    return runtime->runtime.init(sm_host, sm_port);
}

void omni_runtime_set_register_host(omni_runtime_t* runtime, const char* host) {
    if (runtime) {
        runtime->runtime.setRegisterHost(host ? host : "");
    }
}

const char* omni_runtime_get_register_host(const omni_runtime_t* runtime) {
    if (!runtime) {
        return NULL;
    }
    return runtime->runtime.getRegisterHost().c_str();
}

void omni_runtime_poll_once(omni_runtime_t* runtime, int timeout_ms) {
    if (runtime) runtime->runtime.pollOnce(timeout_ms);
}

void omni_runtime_stop(omni_runtime_t* runtime) {
    if (runtime) runtime->runtime.stop();
}

int omni_runtime_register_service(omni_runtime_t* runtime, omni_service_t* svc) {
    if (!runtime || !svc || !svc->bridge) return -1;
    return runtime->runtime.registerService(svc->bridge);
}

int omni_runtime_unregister_service(omni_runtime_t* runtime, omni_service_t* svc) {
    if (!runtime || !svc || !svc->bridge) return -1;
    return runtime->runtime.unregisterService(svc->bridge);
}

int omni_runtime_invoke(omni_runtime_t* runtime, const char* service_name,
    uint32_t interface_id, uint32_t method_id,
    const omni_buffer_t* request, omni_buffer_t* response,
    uint32_t timeout_ms)
{
    if (!runtime || !request || !response) return -1;
    return runtime->runtime.invoke(service_name, interface_id, method_id,
                                 request->buf, response->buf, timeout_ms);
}

int omni_runtime_invoke_oneway(omni_runtime_t* runtime, const char* service_name,
    uint32_t interface_id, uint32_t method_id,
    const omni_buffer_t* request)
{
    if (!runtime || !request) return -1;
    return runtime->runtime.invokeOneWay(service_name, interface_id, method_id, request->buf);
}

int omni_runtime_publish_topic(omni_runtime_t* runtime, const char* topic_name) {
    if (!runtime) return -1;
    return runtime->runtime.publishTopic(topic_name);
}

int omni_runtime_broadcast(omni_runtime_t* runtime, uint32_t topic_id,
    const omni_buffer_t* data)
{
    if (!runtime || !data) return -1;
    return runtime->runtime.broadcast(topic_id, data->buf);
}

int omni_runtime_subscribe_topic(omni_runtime_t* runtime, const char* topic_name,
    omni_topic_callback_t callback, void* user_data)
{
    if (!runtime || !callback) return -1;
    return runtime->runtime.subscribeTopic(topic_name,
        [callback, user_data](uint32_t topic_id, const omnibinder::Buffer& data) {
            omni_buffer_t wrap;
            wrap.buf = omnibinder::Buffer(data.data(), data.size());
            callback(topic_id, &wrap, user_data);
        });
}

int omni_runtime_unsubscribe_topic(omni_runtime_t* runtime, const char* topic_name) {
    if (!runtime) return -1;
    return runtime->runtime.unsubscribeTopic(topic_name);
}

int omni_runtime_subscribe_death(omni_runtime_t* runtime, const char* service_name,
    omni_death_callback_t callback, void* user_data)
{
    if (!runtime || !callback) return -1;
    return runtime->runtime.subscribeServiceDeath(service_name,
        [callback, user_data](const std::string& name) {
            callback(name.c_str(), user_data);
        });
}

int omni_runtime_unsubscribe_death(omni_runtime_t* runtime, const char* service_name) {
    if (!runtime) return -1;
    return runtime->runtime.unsubscribeServiceDeath(service_name);
}

int omni_runtime_get_stats(omni_runtime_t* runtime, omni_runtime_stats_t* stats) {
    if (!runtime || !stats) return -1;
    omnibinder::RuntimeStats cpp_stats;
    int ret = runtime->runtime.getStats(cpp_stats);
    if (ret != 0) {
        return ret;
    }
    stats->total_rpc_calls = cpp_stats.total_rpc_calls;
    stats->total_rpc_success = cpp_stats.total_rpc_success;
    stats->total_rpc_failures = cpp_stats.total_rpc_failures;
    stats->total_rpc_timeouts = cpp_stats.total_rpc_timeouts;
    stats->connection_errors = cpp_stats.connection_errors;
    stats->sm_reconnect_attempts = cpp_stats.sm_reconnect_attempts;
    stats->sm_reconnect_successes = cpp_stats.sm_reconnect_successes;
    stats->active_connections = cpp_stats.active_connections;
    stats->tcp_connections = cpp_stats.tcp_connections;
    stats->shm_connections = cpp_stats.shm_connections;
    return 0;
}

void omni_runtime_reset_stats(omni_runtime_t* runtime) {
    if (runtime) {
        runtime->runtime.resetStats();
    }
}

/* ============================================================
 * 工具函数
 * ============================================================ */

uint32_t omni_fnv1a_32(const char* str) {
    uint32_t hash = 0x811c9dc5u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 0x01000193u;
    }
    return hash;
}

} /* extern "C" */
