#include "info_formatter.h"

#include <cstdio>
#include <iomanip>
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

namespace {

const size_t INSTANCE_NAME_WIDTH      = 24;
const size_t INSTANCE_ID_WIDTH        = 20;
const size_t INSTANCE_HOST_ID_WIDTH   = 32;
const size_t INSTANCE_ENDPOINT_WIDTH  = 21;
const size_t INSTANCE_STATUS_WIDTH    = 6;

std::string instanceEndpoint(const ServiceInstanceView& instance) {
    std::ostringstream endpoint;
    endpoint << escapeForTerminal(instance.host) << ':' << instance.port;
    return endpoint.str();
}

std::string columnSeparator(const size_t* widths, size_t count) {
    std::string separator;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) separator += ' ';
        separator.append(widths[i], '-');
    }
    separator += '\n';
    return separator;
}

std::string describeSelector(const InstanceSelector& selector) {
    std::ostringstream out;
    bool first = true;
    if (selector.instance_id_set) {
        out << "instance_id=\"" << escapeForTerminal(selector.instance_id) << "\"";
        first = false;
    }
    if (selector.host_id_set) {
        if (!first) out << ' ';
        out << "host_id=\"" << escapeForTerminal(selector.host_id) << "\"";
    }
    return out.str();
}

bool selectionMatches(const ServiceInstanceView& instance,
                      const InstanceSelector& selector) {
    if (selector.instance_id_set && instance.instance_id != selector.instance_id) {
        return false;
    }
    if (selector.host_id_set && instance.host_id != selector.host_id) {
        return false;
    }
    return true;
}

std::string formatInstanceTarget(const ServiceInstanceView& instance) {
    std::ostringstream out;
    out << "instance_id=\"" << escapeForTerminal(instance.instance_id) << "\"";
    if (instance.instance_id.empty()) {
        out << " (default)";
    }
    out << " host_id=\"" << escapeForTerminal(instance.host_id) << "\""
        << " endpoint=" << instanceEndpoint(instance);
    return out.str();
}

} // namespace

std::string displayInstanceId(const std::string& instance_id) {
    if (instance_id.empty()) {
        return "(default)";
    }
    return escapeForTerminal(instance_id);
}

std::string formatInstanceTableHeader() {
    char header[256];
    snprintf(header, sizeof(header),
             "%-*s %-*s %-*s %-*s %s\n",
             static_cast<int>(INSTANCE_NAME_WIDTH), "NAME",
             static_cast<int>(INSTANCE_ID_WIDTH), "INSTANCE_ID",
             static_cast<int>(INSTANCE_HOST_ID_WIDTH), "HOST_ID",
             static_cast<int>(INSTANCE_ENDPOINT_WIDTH), "ENDPOINT",
             "STATUS");
    const size_t widths[] = {
        INSTANCE_NAME_WIDTH, INSTANCE_ID_WIDTH, INSTANCE_HOST_ID_WIDTH,
        INSTANCE_ENDPOINT_WIDTH, INSTANCE_STATUS_WIDTH
    };
    return std::string(header)
        + columnSeparator(widths, sizeof(widths) / sizeof(widths[0]));
}

std::string formatInstanceListRow(const ServiceInstanceView& instance) {
    std::string name = escapeForTerminal(instance.name);
    std::string inst = displayInstanceId(instance.instance_id);
    std::string host_id = escapeForTerminal(instance.host_id);
    std::string endpoint = instanceEndpoint(instance);
    std::ostringstream out;
    out << std::left
        << std::setw(static_cast<int>(INSTANCE_NAME_WIDTH)) << name << ' '
        << std::setw(static_cast<int>(INSTANCE_ID_WIDTH)) << inst << ' '
        << std::setw(static_cast<int>(INSTANCE_HOST_ID_WIDTH)) << host_id << ' '
        << std::setw(static_cast<int>(INSTANCE_ENDPOINT_WIDTH)) << endpoint << ' '
        << "ONLINE" << '\n';
    return out.str();
}

std::string formatInstanceTable(const std::vector<ServiceInstanceView>& instances) {
    std::ostringstream out;
    out << formatInstanceTableHeader();
    for (size_t i = 0; i < instances.size(); ++i) {
        out << formatInstanceListRow(instances[i]);
    }
    return out.str();
}

int selectInstance(const std::vector<ServiceInstanceView>& instances,
                   const InstanceSelector& selector,
                   ServiceInstanceView& selected,
                   std::string& error) {
    error.clear();
    if (instances.empty()) {
        error = "no service instances available";
        return 1;
    }
    if (!selector.instance_id_set && !selector.host_id_set) {
        selected = instances[0];
        return 0;
    }

    size_t matches = 0;
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!selectionMatches(instances[i], selector)) {
            continue;
        }
        ++matches;
        selected = instances[i];
    }

    if (matches == 0) {
        error = "no instance matches " + describeSelector(selector);
        return 1;
    }
    if (matches > 1) {
        error = "multiple instances match " + describeSelector(selector)
            + (selector.host_id_set ? "; add --instance-id to disambiguate"
                                    : "; add --host-id to disambiguate");
        return 2;
    }
    return 0;
}

std::string formatInstanceIdentity(const ServiceInstanceView& instance) {
    return "Instance: " + formatInstanceTarget(instance);
}

std::string formatInstanceSelectionHint() {
    return "Multiple instances found; not selecting one automatically. "
           "Use --instance-id <id> [--host-id <id>] to inspect a specific instance.\n";
}

std::string formatCallTargetNotice(const ServiceInstanceView& instance,
                                   bool explicit_selection) {
    std::ostringstream out;
    if (explicit_selection) {
        out << "Target instance: " << formatInstanceTarget(instance);
    } else {
        out << "No instance selector given; using deterministic first instance: "
            << formatInstanceTarget(instance);
        out << "\n  Hint: use --instance-id <id> [--host-id <id>] to target another instance.";
    }
    return out.str();
}

} // namespace omni_cli
