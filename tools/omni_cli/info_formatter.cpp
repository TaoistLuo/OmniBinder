#include "info_formatter.h"

#include <sstream>

namespace omni_cli {

namespace {

void appendEscapedByte(std::ostringstream& out, unsigned char value) {
    static const char hex[] = "0123456789ABCDEF";
    out << "\\x" << hex[value >> 4] << hex[value & 0x0F];
}

bool isUnsafeFormatCodepoint(uint32_t codepoint) {
    return codepoint == 0x061C
        || (codepoint >= 0x200B && codepoint <= 0x200F)
        || (codepoint >= 0x202A && codepoint <= 0x202E)
        || (codepoint >= 0x2060 && codepoint <= 0x206F)
        || codepoint == 0xFEFF;
}

} // namespace

std::string escapeForTerminal(const std::string& input) {
    std::ostringstream out;
    for (size_t i = 0; i < input.size();) {
        const unsigned char first = static_cast<unsigned char>(input[i]);
        if (first < 0x20 || first == 0x7F) {
            appendEscapedByte(out, first);
            ++i;
            continue;
        }
        if (first < 0x80) {
            out << input[i++];
            continue;
        }

        size_t length = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            length = 2; codepoint = first & 0x1F; minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3; codepoint = first & 0x0F; minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4; codepoint = first & 0x07; minimum = 0x10000;
        } else {
            appendEscapedByte(out, first);
            ++i;
            continue;
        }

        bool valid = i + length <= input.size();
        for (size_t j = 1; valid && j < length; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(input[i + j]);
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
            } else {
                codepoint = (codepoint << 6) | (continuation & 0x3F);
            }
        }
        valid = valid && codepoint >= minimum && codepoint <= 0x10FFFF
            && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
        if (!valid) {
            appendEscapedByte(out, first);
            ++i;
            continue;
        }
        if ((codepoint >= 0x80 && codepoint <= 0x9F)
            || isUnsafeFormatCodepoint(codepoint)) {
            for (size_t j = 0; j < length; ++j) {
                appendEscapedByte(out, static_cast<unsigned char>(input[i + j]));
            }
        } else {
            out.write(input.data() + i, static_cast<std::streamsize>(length));
        }
        i += length;
    }
    return out.str();
}

std::string formatPublishedTopicsSection(const std::vector<std::string>& topics,
                                         const std::string& unavailable_reason) {
    std::ostringstream out;
    out << "  Published Topics:\n";
    if (!unavailable_reason.empty()) {
        out << "    (unavailable: " << escapeForTerminal(unavailable_reason) << ")\n\n";
    } else if (topics.empty()) {
        out << "    (none)\n\n";
    } else {
        for (size_t i = 0; i < topics.size(); ++i) {
            out << "    - " << escapeForTerminal(topics[i]) << "\n";
        }
        out << "\n";
    }
    return out.str();
}

} // namespace omni_cli
