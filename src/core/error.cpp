#include "omnibinder/error.h"

namespace omnibinder {

const char* errorCodeToString(ErrorCode code) {
    switch (code) {
    case ErrorCode::OK:                      return "OK";
    case ErrorCode::ERR_UNKNOWN:             return "Unknown error";
    case ErrorCode::ERR_INVALID_PARAM:       return "Invalid parameter";
    case ErrorCode::ERR_OUT_OF_MEMORY:       return "Out of memory";
    case ErrorCode::ERR_TIMEOUT:             return "Timeout";
    case ErrorCode::ERR_NOT_INITIALIZED:     return "Not initialized";
    case ErrorCode::ERR_ALREADY_INITIALIZED: return "Already initialized";
    case ErrorCode::ERR_NOT_SUPPORTED:       return "Not supported";
    case ErrorCode::ERR_INTERNAL:            return "Internal error";
    case ErrorCode::ERR_CONNECT_FAILED:      return "Connection failed";
    case ErrorCode::ERR_CONNECTION_CLOSED:   return "Connection closed";
    case ErrorCode::ERR_SEND_FAILED:         return "Send failed";
    case ErrorCode::ERR_RECV_FAILED:         return "Receive failed";
    case ErrorCode::ERR_PROTOCOL_ERROR:      return "Protocol error";
    case ErrorCode::ERR_SM_UNREACHABLE:      return "ServiceManager unreachable";
    case ErrorCode::ERR_BIND_FAILED:         return "Bind failed";
    case ErrorCode::ERR_LISTEN_FAILED:       return "Listen failed";
    case ErrorCode::ERR_ACCEPT_FAILED:       return "Accept failed";
    case ErrorCode::ERR_SERVICE_NOT_FOUND:   return "Service not found";
    case ErrorCode::ERR_SERVICE_EXISTS:      return "Service already exists";
    case ErrorCode::ERR_SERVICE_OFFLINE:     return "Service offline";
    case ErrorCode::ERR_INTERFACE_NOT_FOUND: return "Interface not found";
    case ErrorCode::ERR_METHOD_NOT_FOUND:    return "Method not found";
    case ErrorCode::ERR_INVOKE_FAILED:       return "Invoke failed";
    case ErrorCode::ERR_REGISTER_FAILED:     return "Register failed";
    case ErrorCode::ERR_UNREGISTER_FAILED:   return "Unregister failed";
    case ErrorCode::ERR_TOPIC_NOT_FOUND:     return "Topic not found";
    case ErrorCode::ERR_TOPIC_EXISTS:        return "Topic already exists";
    case ErrorCode::ERR_NOT_SUBSCRIBED:      return "Not subscribed";
    case ErrorCode::ERR_NOT_PUBLISHER:       return "Not a publisher";
    case ErrorCode::ERR_TRANSPORT_INIT:      return "Transport init failed";
    case ErrorCode::ERR_SHM_CREATE:          return "Shared memory create failed";
    case ErrorCode::ERR_SHM_ATTACH:          return "Shared memory attach failed";
    case ErrorCode::ERR_SHM_FULL:            return "Shared memory full";
    case ErrorCode::ERR_SHM_TIMEOUT:         return "Shared memory timeout";
    case ErrorCode::ERR_SERIALIZE:           return "Serialize error";
    case ErrorCode::ERR_DESERIALIZE:         return "Deserialize error";
    case ErrorCode::ERR_BUFFER_OVERFLOW:     return "Buffer overflow";
    case ErrorCode::ERR_BUFFER_UNDERFLOW:    return "Buffer underflow";
    default:                                 return "Unknown error code";
    }
}

} // namespace omnibinder
