#ifndef OMNI_CLI_INFO_FORMATTER_H
#define OMNI_CLI_INFO_FORMATTER_H

#include <string>
#include <vector>

namespace omni_cli {

std::string escapeForTerminal(const std::string& input);

std::string formatPublishedTopicsSection(const std::vector<std::string>& topics,
                                         const std::string& unavailable_reason);

} // namespace omni_cli

#endif // OMNI_CLI_INFO_FORMATTER_H
