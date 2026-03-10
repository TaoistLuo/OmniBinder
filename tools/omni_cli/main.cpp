#include <omnibinder/omnibinder.h>
#include "simple_json.h"
#include "type_codec.h"
#include "../omni_idlc/lexer.h"
#include "../omni_idlc/parser.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/time.h>

// 全局 IDL 解析上下文
static omnic::ParseContext* g_parse_ctx = NULL;
static std::string g_idl_package;

static void printUsage(const char* prog) {
    printf("Usage: %s [options] <command> [args]\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host <addr>   ServiceManager address (default: 127.0.0.1)\n");
    printf("  -p, --port <port>   ServiceManager port (default: 9900)\n");
    printf("  --idl <file.bidl>   IDL file for JSON I/O support\n");
    printf("  --help              Show this help\n\n");
    printf("Commands:\n");
    printf("  list                List all online services\n");
    printf("  info <service>      Show service details and interfaces\n");
    printf("  call <service> <method> [params]\n");
    printf("                      Call a service method\n");
    printf("                      params: hex string (without --idl) or JSON (with --idl)\n");
}

static int cmdList(omnibinder::OmniRuntime& runtime) {
    std::vector<omnibinder::ServiceInfo> services;
    int ret = runtime.listServices(services);
    if (ret != 0) {
        fprintf(stderr, "Error: %s\n", omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }
    
    printf("%-24s %-16s %-8s %s\n", "NAME", "HOST", "PORT", "STATUS");
    printf("%-24s %-16s %-8s %s\n", "----", "----", "----", "------");
    for (size_t i = 0; i < services.size(); ++i) {
        printf("%-24s %-16s %-8u %s\n",
               services[i].name.c_str(),
               services[i].host.c_str(),
               services[i].port,
               "ONLINE");
    }
    printf("\nTotal: %zu services online\n", services.size());
    return 0;
}

// 辅助函数：根据类型名查找 TypeRef
static bool findTypeRef(const std::string& typeName, const std::string& package, omnic::TypeRef& typeRef) {
    if (!g_parse_ctx) return false;
    
    // 查找包
    std::map<std::string, omnic::AstFile>::const_iterator pkgIt = g_parse_ctx->loaded_packages.find(package);
    if (pkgIt == g_parse_ctx->loaded_packages.end()) {
        return false;
    }
    
    const omnic::AstFile& ast = pkgIt->second;
    
    // 查找结构体
    for (size_t i = 0; i < ast.structs.size(); ++i) {
        if (ast.structs[i].name == typeName) {
            typeRef.primitive = omnic::TYPE_CUSTOM;
            typeRef.custom_name = typeName;
            typeRef.package_name = "";
            return true;
        }
    }
    
    return false;
}

// 辅助函数：打印字段定义（用于详细模式）
static void printFieldSchema(const omnic::TypeRef& type, const std::string& package, int indent) {
    std::string indentStr(indent * 2, ' ');
    
    if (type.primitive == omnic::TYPE_CUSTOM && g_parse_ctx) {
        // 查找结构体定义
        std::string structPackage = type.package_name.empty() ? package : type.package_name;
        std::map<std::string, omnic::AstFile>::const_iterator it = g_parse_ctx->loaded_packages.find(structPackage);
        if (it != g_parse_ctx->loaded_packages.end()) {
            const omnic::AstFile& ast = it->second;
            for (size_t i = 0; i < ast.structs.size(); ++i) {
                if (ast.structs[i].name == type.custom_name) {
                    printf("%s{\n", indentStr.c_str());
                    for (size_t j = 0; j < ast.structs[i].fields.size(); ++j) {
                        const omnic::FieldDef& field = ast.structs[i].fields[j];
                        printf("%s  %s: ", indentStr.c_str(), field.name.c_str());
                        
                        // 打印字段类型
                        if (field.type.primitive == omnic::TYPE_CUSTOM) {
                            printf("%s", field.type.custom_name.c_str());
                        } else if (field.type.primitive == omnic::TYPE_ARRAY) {
                            printf("array<...>");
                        } else {
                            // 基础类型
                            const char* typeName = "unknown";
                            switch (field.type.primitive) {
                            case omnic::TYPE_BOOL: typeName = "bool"; break;
                            case omnic::TYPE_INT8: typeName = "int8"; break;
                            case omnic::TYPE_UINT8: typeName = "uint8"; break;
                            case omnic::TYPE_INT16: typeName = "int16"; break;
                            case omnic::TYPE_UINT16: typeName = "uint16"; break;
                            case omnic::TYPE_INT32: typeName = "int32"; break;
                            case omnic::TYPE_UINT32: typeName = "uint32"; break;
                            case omnic::TYPE_INT64: typeName = "int64"; break;
                            case omnic::TYPE_UINT64: typeName = "uint64"; break;
                            case omnic::TYPE_FLOAT32: typeName = "float32"; break;
                            case omnic::TYPE_FLOAT64: typeName = "float64"; break;
                            case omnic::TYPE_STRING: typeName = "string"; break;
                            case omnic::TYPE_BYTES: typeName = "bytes"; break;
                            default: break;
                            }
                            printf("%s", typeName);
                        }
                        printf("\n");
                    }
                    printf("%s}\n", indentStr.c_str());
                    return;
                }
            }
        }
    }
    
    // 如果找不到定义，只打印类型名
    printf("%s%s\n", indentStr.c_str(), type.custom_name.c_str());
}

static int cmdInfo(omnibinder::OmniRuntime& runtime, const char* service_name) {
    omnibinder::ServiceInfo info;
    int ret = runtime.lookupService(service_name, info);
    if (ret != 0) {
        fprintf(stderr, "Error: %s\n", omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }
    
    printf("Service: %s\n", info.name.c_str());
    printf("  Host:    %s\n", info.host.c_str());
    printf("  Port:    %u\n", info.port);
    printf("  HostID:  %s\n", info.host_id.c_str());
    printf("  Status:  ONLINE\n\n");
    
    for (size_t i = 0; i < info.interfaces.size(); ++i) {
        const omnibinder::InterfaceInfo& iface = info.interfaces[i];
        printf("  Interface: %s (id=0x%08x)\n", iface.name.c_str(), iface.interface_id);
        printf("    Methods:\n");
        
        for (size_t j = 0; j < iface.methods.size(); ++j) {
            const omnibinder::MethodInfo& method = iface.methods[j];
            
            // 基础模式：显示方法签名
            printf("      - %s", method.name.c_str());
            if (!method.param_types.empty()) {
                printf("(%s)", method.param_types.c_str());
            } else {
                printf("()");
            }
            printf(" -> %s", method.return_type.c_str());
            printf("  (id=0x%08x)\n", method.method_id);
            
            // 详细模式：展开字段定义
            if (g_parse_ctx) {
                // 打印参数详情
                if (!method.param_types.empty()) {
                    omnic::TypeRef paramType;
                    if (findTypeRef(method.param_types, g_idl_package, paramType)) {
                        printf("          param: ");
                        printFieldSchema(paramType, g_idl_package, 5);
                    }
                }
                
                // 打印返回值详情
                if (method.return_type != "void") {
                    omnic::TypeRef returnType;
                    if (findTypeRef(method.return_type, g_idl_package, returnType)) {
                        printf("          return: ");
                        printFieldSchema(returnType, g_idl_package, 5);
                    }
                }
            }
        }
        printf("\n");
    }
    return 0;
}

// Helper: convert hex string to bytes
static bool hexToBytes(const char* hex, omnibinder::Buffer& buf) {
    size_t len = strlen(hex);
    if (len % 2 != 0) return false;
    for (size_t i = 0; i < len; i += 2) {
        char byte_str[3] = { hex[i], hex[i + 1], '\0' };
        char* end = NULL;
        unsigned long val = strtoul(byte_str, &end, 16);
        if (end != byte_str + 2 || val > 255) return false;
        buf.writeUint8(static_cast<uint8_t>(val));
    }
    return true;
}

// Helper: print bytes as hex dump
static void printHexDump(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i > 0 && i % 16 == 0) printf("\n  ");
        printf("%02x ", data[i]);
    }
    printf("\n");
}

static int cmdCall(omnibinder::OmniRuntime& runtime, const char* service_name,
                   const char* method_name, const char* params) {
    // Look up the service
    omnibinder::ServiceInfo info;
    int ret = runtime.lookupService(service_name, info);
    if (ret != 0) {
        fprintf(stderr, "Error: Cannot find service '%s': %s\n",
                service_name, omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    // Find the method
    uint32_t iface_id = 0;
    uint32_t method_id = 0;
    std::string param_type_name;
    std::string return_type_name;
    bool found = false;

    for (size_t i = 0; i < info.interfaces.size() && !found; ++i) {
        const omnibinder::InterfaceInfo& iface = info.interfaces[i];
        for (size_t j = 0; j < iface.methods.size(); ++j) {
            if (iface.methods[j].name == method_name) {
                iface_id = iface.interface_id;
                method_id = iface.methods[j].method_id;
                param_type_name = iface.methods[j].param_types;
                return_type_name = iface.methods[j].return_type;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        fprintf(stderr, "Error: Method '%s' not found in service '%s'\n", method_name, service_name);
        return 1;
    }

    // Build request payload
    omnibinder::Buffer request;
    
    if (params && strlen(params) > 0) {
        if (g_parse_ctx && params[0] == '{') {
            // JSON 模式
            try {
                simple_json::Value jsonInput = simple_json::parse(params);
                
                // 查找参数类型
                omnic::TypeRef paramType;
                if (!findTypeRef(param_type_name, g_idl_package, paramType)) {
                    fprintf(stderr, "Error: Cannot find type '%s' in IDL\n", param_type_name.c_str());
                    return 1;
                }
                
                // 编码为 Buffer
                type_codec::TypeCodec codec(*g_parse_ctx);
                if (!codec.encodeToBuffer(jsonInput, paramType, g_idl_package, request)) {
                    fprintf(stderr, "Error: Failed to encode JSON to buffer\n");
                    return 1;
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "Error: JSON parse failed: %s\n", e.what());
                return 1;
            }
        } else {
            // Hex 模式
            if (!hexToBytes(params, request)) {
                fprintf(stderr, "Error: Invalid hex parameter string: %s\n", params);
                return 1;
            }
        }
    }

    printf("Calling %s.%s() ...\n", service_name, method_name);
    printf("  interface_id = 0x%08x\n", iface_id);
    printf("  method_id    = 0x%08x\n", method_id);
    if (request.size() > 0) {
        printf("  request (%zu bytes)\n", request.size());
    }

    // 记录开始时间
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    // Invoke the method
    omnibinder::Buffer response;
    ret = runtime.invoke(info.name.c_str(), iface_id, method_id, request, response);
    
    // 记录结束时间
    gettimeofday(&end_time, NULL);
    
    // 计算耗时（毫秒）
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_usec - start_time.tv_usec) / 1000.0;
    
    if (ret != 0) {
        fprintf(stderr, "Error: Invoke failed: %s\n",
                omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    printf("Response (status=OK, %zu bytes, %.2f ms):\n", response.size(), elapsed_ms);
    
    if (response.size() > 0) {
        if (g_parse_ctx && return_type_name != "void") {
            // JSON 模式 - 解码响应
            omnic::TypeRef returnType;
            if (findTypeRef(return_type_name, g_idl_package, returnType)) {
                type_codec::TypeCodec codec(*g_parse_ctx);
                simple_json::Value jsonOutput;
                
                if (codec.decodeFromBuffer(response, returnType, g_idl_package, jsonOutput)) {
                    printf("  %s\n", jsonOutput.toString(true, 1).c_str());
                } else {
                    fprintf(stderr, "Warning: Failed to decode response, showing hex:\n");
                    printf("  Hex: ");
                    printHexDump(response.data(), response.size());
                }
            } else {
                // 类型未找到，显示 hex
                printf("  Hex: ");
                printHexDump(response.data(), response.size());
            }
        } else {
            // Hex 模式
            printf("  Hex: ");
            printHexDump(response.data(), response.size());
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 9900;
    const char* idl_file = NULL;
    const char* positional[4] = {NULL, NULL, NULL, NULL};
    int pos_count = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--idl") == 0 && i + 1 < argc) {
            idl_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (pos_count < 4) {
            positional[pos_count++] = argv[i];
        }
    }
    
    if (pos_count == 0) {
        printUsage(argv[0]);
        return 1;
    }
    
    // Parse IDL file if provided
    if (idl_file) {
        std::ifstream ifs(idl_file);
        if (!ifs.is_open()) {
            fprintf(stderr, "Error: Cannot open IDL file: %s\n", idl_file);
            return 1;
        }
        
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        std::string content = buffer.str();
        
        omnic::Lexer lexer(content);
        g_parse_ctx = new omnic::ParseContext();
        omnic::Parser parser(lexer, *g_parse_ctx, idl_file);
        
        omnic::AstFile ast;
        if (!parser.parse(ast)) {
            fprintf(stderr, "Error: IDL parse failed: %s\n", parser.errorMessage().c_str());
            delete g_parse_ctx;
            return 1;
        }
        
        g_idl_package = ast.package_name;
    }
    
    const char* command = positional[0];
    
    omnibinder::OmniRuntime runtime;
    int ret = runtime.init(host, port);
    if (ret != 0) {
        fprintf(stderr, "Error: Cannot connect to ServiceManager at %s:%u\n", host, port);
        fprintf(stderr, "  %s\n", omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        if (g_parse_ctx) delete g_parse_ctx;
        return 1;
    }
    
    int result = 0;
    if (strcmp(command, "list") == 0) {
        result = cmdList(runtime);
    } else if (strcmp(command, "info") == 0) {
        if (!positional[1]) {
            fprintf(stderr, "Error: 'info' requires a service name\n");
            result = 1;
        } else {
            result = cmdInfo(runtime, positional[1]);
        }
    } else if (strcmp(command, "call") == 0) {
        if (!positional[1] || !positional[2]) {
            fprintf(stderr, "Error: 'call' requires <service> <method> [params]\n");
            result = 1;
        } else {
            result = cmdCall(runtime, positional[1], positional[2], positional[3]);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        result = 1;
    }
    
    runtime.stop();
    if (g_parse_ctx) delete g_parse_ctx;
    return result;
}
