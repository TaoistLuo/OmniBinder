#ifndef OMNIBINDER_RUNTIME_HELPERS_H
#define OMNIBINDER_RUNTIME_HELPERS_H

#include "omnibinder/message.h"
#include "omnibinder/error.h"
#include "omnibinder/transport.h"

#include <string>

namespace omnibinder {

extern const uint32_t OMNI_DIAG_IFACE_ID;

std::string normalizeAdvertiseHost(const std::string& host);
const char* dataChannelKindName(TransportType type);
void diag_serialize_event(Buffer& buf, uint8_t direction, const Message& msg);
bool decodeInvokePayload(const Message& msg, uint32_t& interface_id,
                         uint32_t& idl_hash, uint32_t& method_id, Buffer& request);
bool decodeSubscribeBroadcastPayload(const Message& msg, uint32_t& topic_id,
                                     std::string& topic_name);
bool decodeBroadcastPayload(const Message& msg, uint32_t& topic_id, Buffer& payload);
bool decodeSingleStringPayload(const Message& msg, std::string& value);
bool decodeBoolReplyPayload(const Message& msg, bool& value);
bool decodeUint32ReplyPayload(const Message& msg, uint32_t& value);
bool decodeInvokeReplyPayload(const Message& msg, int32_t& status, Buffer& response);
Message makeInvokeErrorReply(uint32_t seq, ErrorCode error);
Message makeInvokeSuccessReply(uint32_t seq, const Buffer& response);

} // namespace omnibinder

#endif
