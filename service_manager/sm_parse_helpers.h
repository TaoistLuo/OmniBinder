#ifndef OMNIBINDER_SM_PARSE_HELPERS_H
#define OMNIBINDER_SM_PARSE_HELPERS_H

#include "omnibinder/message.h"
#include "omnibinder/types.h"

#include <string>

namespace omnibinder {
namespace sm_internal {

inline bool tryReadStringArg(const Message& msg, std::string& value) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    return payload.tryReadString(value);
}

inline bool tryReadExactStringArg(const Message& msg, std::string& value,
                                  size_t max_length) {
    Buffer payload(msg.payload.data(), msg.payload.size());
    return payload.tryReadString(value)
        && !value.empty()
        && value.size() <= max_length
        && payload.remaining() == 0;
}

inline bool tryReadExactTopicArg(const Message& msg, std::string& topic) {
    return tryReadExactStringArg(msg, topic, MAX_TOPIC_NAME_LENGTH);
}

} // namespace sm_internal
} // namespace omnibinder

#endif
