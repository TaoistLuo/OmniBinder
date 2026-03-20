#ifndef OMNI_CLI_TYPE_RESOLVER_H
#define OMNI_CLI_TYPE_RESOLVER_H

#include "simple_json.h"
#include "parser.h"

namespace omni_cli {

const char* primitiveTypeName(omnic::PrimitiveType primitive);
bool fillPrimitiveTypeRef(const std::string& type_name, omnic::TypeRef& type_ref);
bool findTypeRef(const omnic::ParseContext* parse_ctx, const std::string& type_name,
                 const std::string& package, omnic::TypeRef& type_ref);
bool isScalarCliType(const omnic::TypeRef& type_ref);
bool parseScalarCliValue(const char* text, const omnic::TypeRef& type_ref, simple_json::Value& value);

} // namespace omni_cli

#endif // OMNI_CLI_TYPE_RESOLVER_H
