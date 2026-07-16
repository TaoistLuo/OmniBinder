#ifndef OMNIC_SEMANTIC_VALIDATOR_H
#define OMNIC_SEMANTIC_VALIDATOR_H

#include "parser.h"

#include <string>

namespace omnic {

// Validates all packages reachable through the parse context and stably orders
// local structs so by-value dependencies are declared before their users.
bool validateSemantics(AstFile& root, ParseContext& context, std::string& error);

} // namespace omnic

#endif
