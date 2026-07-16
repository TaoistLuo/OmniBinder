#include "semantic_validator.h"

#include <map>
#include <set>
#include <vector>

namespace omnic {
namespace {

static const size_t IDLC_MAX_SEMANTIC_TRAVERSAL = 1048576u;

enum DeclarationKind { DECL_STRUCT, DECL_TOPIC, DECL_SERVICE };
typedef std::map<std::string, DeclarationKind> DeclarationMap;
typedef std::map<std::string, const StructDef*> StructMap;

std::string qualifiedName(const AstFile& ast, const TypeRef& type) {
    return (type.package_name.empty() ? ast.package_name : type.package_name) +
           "." + type.custom_name;
}

bool addDeclaration(const AstFile& ast, const std::string& name,
                    DeclarationKind kind, DeclarationMap& declarations,
                    std::string& error) {
    const std::string qualified = ast.package_name + "." + name;
    if (!declarations.insert(std::make_pair(qualified, kind)).second) {
        error = "duplicate declaration '" + qualified + "'";
        return false;
    }
    return true;
}

bool collectDeclarations(const AstFile& ast, DeclarationMap& declarations,
                         std::string& error) {
    for (size_t i = 0; i < ast.structs.size(); ++i)
        if (!addDeclaration(ast, ast.structs[i].name, DECL_STRUCT, declarations, error)) return false;
    for (size_t i = 0; i < ast.topics.size(); ++i)
        if (!addDeclaration(ast, ast.topics[i].name, DECL_TOPIC, declarations, error)) return false;
    for (size_t i = 0; i < ast.services.size(); ++i)
        if (!addDeclaration(ast, ast.services[i].name, DECL_SERVICE, declarations, error)) return false;
    return true;
}

bool collectVisiblePackages(const std::string& package_name,
                            const std::map<std::string, AstFile>& packages,
                            std::set<std::string>& visible, size_t& traversal,
                            std::string& error) {
    if (++traversal > IDLC_MAX_SEMANTIC_TRAVERSAL) {
        error = "semantic traversal limit exceeded";
        return false;
    }
    if (!visible.insert(package_name).second) return true;
    std::map<std::string, AstFile>::const_iterator found = packages.find(package_name);
    if (found == packages.end()) return true;
    for (size_t i = 0; i < found->second.imported_packages.size(); ++i) {
        if (packages.find(found->second.imported_packages[i]) == packages.end()) {
            error = "package '" + package_name + "' has unresolved import package '" +
                    found->second.imported_packages[i] + "'";
            return false;
        }
        if (!collectVisiblePackages(found->second.imported_packages[i], packages,
                                    visible, traversal, error)) return false;
    }
    return true;
}

bool buildVisibleDeclarations(const AstFile& ast,
                              const std::map<std::string, AstFile>& packages,
                              DeclarationMap& declarations, size_t& traversal,
                              std::string& error) {
    std::set<std::string> visible;
    if (!collectVisiblePackages(ast.package_name, packages, visible, traversal, error)) return false;
    for (std::set<std::string>::const_iterator it = visible.begin(); it != visible.end(); ++it) {
        std::map<std::string, AstFile>::const_iterator package = packages.find(*it);
        if (package != packages.end() &&
            !collectDeclarations(package->second, declarations, error)) return false;
    }
    return true;
}

bool validateValueType(const AstFile& ast, const TypeRef& type,
                       const DeclarationMap& declarations,
                       const std::string& where, bool allow_void,
                       size_t nesting, size_t& traversal, std::string& error) {
    if (++traversal > IDLC_MAX_SEMANTIC_TRAVERSAL) {
        error = "semantic traversal limit exceeded";
        return false;
    }
    if (nesting >= IDLC_MAX_TYPE_NESTING) {
        error = where + " exceeds type nesting limit";
        return false;
    }
    if (type.isVoid()) {
        if (allow_void) return true;
        error = where + " cannot use void";
        return false;
    }
    if (type.isArray()) {
        if (type.element_type == NULL) {
            error = where + " has an array with no element type";
            return false;
        }
        return validateValueType(ast, *type.element_type, declarations,
                                 where + " array element", false,
                                 nesting + 1, traversal, error);
    }
    if (!type.isCustom()) return true;
    const std::string qualified = qualifiedName(ast, type);
    DeclarationMap::const_iterator found = declarations.find(qualified);
    if (found == declarations.end()) {
        error = where + " references unknown or non-imported type '" + qualified + "'";
        return false;
    }
    if (found->second != DECL_STRUCT) {
        error = where + " references non-struct type '" + qualified + "'";
        return false;
    }
    return true;
}

bool visitLocalStruct(size_t index, const AstFile& ast,
                      const std::map<std::string, size_t>& local,
                      std::vector<int>& states, std::vector<size_t>& order,
                      size_t depth, size_t& traversal, std::string& error) {
    if (depth > IDLC_MAX_TYPE_NESTING) { error = "semantic dependency depth limit exceeded"; return false; }
    if (++traversal > IDLC_MAX_SEMANTIC_TRAVERSAL) { error = "semantic traversal limit exceeded"; return false; }
    if (states[index] == 2) return true;
    if (states[index] == 1) { error = "by-value struct cycle involving '" + ast.package_name + "." + ast.structs[index].name + "'"; return false; }
    states[index] = 1;
    for (size_t i = 0; i < ast.structs[index].fields.size(); ++i) {
        const TypeRef& type = ast.structs[index].fields[i].type;
        if (!type.isCustom() || (!type.package_name.empty() && type.package_name != ast.package_name)) continue;
        std::map<std::string, size_t>::const_iterator dep = local.find(type.custom_name);
        if (dep != local.end() && !visitLocalStruct(dep->second, ast, local, states, order, depth + 1, traversal, error)) return false;
    }
    states[index] = 2;
    order.push_back(index);
    return true;
}

bool orderStructs(AstFile& ast, size_t& traversal, std::string& error) {
    std::map<std::string, size_t> local;
    for (size_t i = 0; i < ast.structs.size(); ++i) local[ast.structs[i].name] = i;
    std::vector<int> states(ast.structs.size(), 0);
    std::vector<size_t> order;
    for (size_t i = 0; i < ast.structs.size(); ++i)
        if (!visitLocalStruct(i, ast, local, states, order, 0, traversal, error)) return false;
    std::vector<StructDef> sorted;
    for (size_t i = 0; i < order.size(); ++i) sorted.push_back(ast.structs[order[i]]);
    ast.structs.swap(sorted);
    return true;
}

bool validateFile(AstFile& ast, const std::map<std::string, AstFile>& packages,
                  size_t& traversal, std::string& error) {
    if (ast.package_declaration_count == 0 || ast.package_name.empty()) { error = ast.file_path + ": package declaration is required"; return false; }
    if (ast.package_declaration_count > 1) { error = ast.file_path + ": package may be declared only once"; return false; }
    if (!ast.package_before_imports_and_declarations) { error = ast.file_path + ": package must appear before imports and declarations"; return false; }
    if (!ast.imports_before_declarations) { error = ast.file_path + ": imports must appear before declarations"; return false; }

    DeclarationMap declarations;
    if (!buildVisibleDeclarations(ast, packages, declarations, traversal, error)) return false;
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        std::set<std::string> fields;
        for (size_t j = 0; j < ast.structs[i].fields.size(); ++j) {
            const FieldDef& field = ast.structs[i].fields[j];
            if (!fields.insert(field.name).second) { error = "struct '" + ast.structs[i].name + "' has duplicate field '" + field.name + "'"; return false; }
            if (!validateValueType(ast, field.type, declarations, "struct '" + ast.structs[i].name + "' field '" + field.name + "'", false, 0, traversal, error)) return false;
        }
    }
    for (size_t i = 0; i < ast.topics.size(); ++i) {
        std::set<std::string> fields;
        for (size_t j = 0; j < ast.topics[i].fields.size(); ++j) {
            const FieldDef& field = ast.topics[i].fields[j];
            if (!fields.insert(field.name).second) { error = "topic '" + ast.topics[i].name + "' has duplicate field '" + field.name + "'"; return false; }
            if (!validateValueType(ast, field.type, declarations, "topic '" + ast.topics[i].name + "' field '" + field.name + "'", false, 0, traversal, error)) return false;
        }
    }
    for (size_t i = 0; i < ast.services.size(); ++i) {
        const ServiceDef& service = ast.services[i];
        std::set<std::string> methods;
        for (size_t j = 0; j < service.methods.size(); ++j) {
            const MethodDef& method = service.methods[j];
            if (!methods.insert(method.name).second) { error = "service '" + service.name + "' has duplicate method '" + method.name + "'"; return false; }
            if (!validateValueType(ast, method.return_type, declarations, "service '" + service.name + "' method '" + method.name + "' return type", true, 0, traversal, error)) return false;
            if (method.has_param && !validateValueType(ast, method.param.type, declarations, "service '" + service.name + "' method '" + method.name + "' parameter '" + method.param.name + "'", false, 0, traversal, error)) return false;
        }
        std::set<std::string> published;
        for (size_t j = 0; j < service.publishes.size(); ++j) {
            if (!published.insert(service.publishes[j]).second) { error = "service '" + service.name + "' has duplicate publishes entry '" + service.publishes[j] + "'"; return false; }
            const std::string qualified = ast.package_name + "." + service.publishes[j];
            DeclarationMap::const_iterator found = declarations.find(qualified);
            if (found == declarations.end()) { error = "service '" + service.name + "' publishes unknown topic '" + qualified + "'"; return false; }
            if (found->second != DECL_TOPIC) { error = "service '" + service.name + "' publishes non-topic '" + qualified + "'"; return false; }
        }
    }
    return orderStructs(ast, traversal, error);
}

bool visitQualified(const std::string& name, const StructMap& structs,
                     std::map<std::string, int>& states,
                     size_t depth, size_t& traversal, std::string& error) {
    if (depth > IDLC_MAX_TYPE_NESTING) { error = "semantic dependency depth limit exceeded"; return false; }
    if (++traversal > IDLC_MAX_SEMANTIC_TRAVERSAL) { error = "semantic traversal limit exceeded"; return false; }
    if (states[name] == 2) return true;
    if (states[name] == 1) { error = "by-value struct cycle involving '" + name + "'"; return false; }
    states[name] = 1;
    const StructDef& def = *structs.find(name)->second;
    const std::string package_name = name.substr(0, name.rfind('.'));
    for (size_t i = 0; i < def.fields.size(); ++i) {
        const TypeRef& type = def.fields[i].type;
        if (!type.isCustom()) continue;
        const std::string target = (type.package_name.empty() ? package_name : type.package_name) + "." + type.custom_name;
        if (structs.find(target) != structs.end() && !visitQualified(target, structs, states, depth + 1, traversal, error)) return false;
    }
    states[name] = 2;
    return true;
}

bool validateCycles(const std::map<std::string, AstFile>& packages,
                    size_t& traversal, std::string& error) {
    StructMap structs;
    for (std::map<std::string, AstFile>::const_iterator p = packages.begin(); p != packages.end(); ++p)
        for (size_t i = 0; i < p->second.structs.size(); ++i) structs[p->first + "." + p->second.structs[i].name] = &p->second.structs[i];
    std::map<std::string, int> states;
    for (StructMap::const_iterator it = structs.begin(); it != structs.end(); ++it)
        if (!visitQualified(it->first, structs, states, 0, traversal, error)) return false;
    return true;
}

} // namespace

bool validateSemantics(AstFile& root, ParseContext& context, std::string& error) {
    std::map<std::string, AstFile> working = context.loaded_packages;
    if (!root.package_name.empty()) working[root.package_name] = root;
    size_t traversal = 0;
    for (std::map<std::string, AstFile>::iterator it = working.begin(); it != working.end(); ++it)
        if (!validateFile(it->second, working, traversal, error)) return false;
    if (root.package_name.empty()) {
        AstFile root_copy = root;
        if (!validateFile(root_copy, working, traversal, error)) return false;
    }
    if (!validateCycles(working, traversal, error)) return false;
    context.loaded_packages.swap(working);
    std::map<std::string, AstFile>::const_iterator validated = context.loaded_packages.find(root.package_name);
    if (validated != context.loaded_packages.end()) root = validated->second;
    return true;
}

} // namespace omnic
