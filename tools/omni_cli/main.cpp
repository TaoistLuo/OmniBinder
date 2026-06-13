#include <omnibinder/omnibinder.h>
#include "simple_json.h"
#include "type_codec.h"
#include "type_resolver.h"
#include "lexer.h"
#include "parser.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <csignal>
#include <map>

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
    printf("  watch <service> [--filter <method|topic>]\n");
    printf("                      Watch diagnostic data from a service\n");
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

// 辅助函数：打印字段定义（用于详细模式）
static void printFieldSchema(const omnic::TypeRef& type, const std::string& package, int indent) {
    std::string indentStr(indent * 2, ' ');

    if (type.primitive == omnic::TYPE_ARRAY) {
        printf("%sarray", indentStr.c_str());
        if (type.element_type) {
            printf("<\n");
            printFieldSchema(*type.element_type, package, indent + 1);
            printf("%s>\n", indentStr.c_str());
        } else {
            printf("<unknown>\n");
        }
        return;
    }

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
                            printf("%s", omni_cli::primitiveTypeName(field.type.primitive));
                        }
                        printf("\n");
                    }
                    printf("%s}\n", indentStr.c_str());
                    return;
                }
            }
        }
    }

    switch (type.primitive) {
    case omnic::TYPE_CUSTOM:
        if (!type.package_name.empty()) {
            printf("%s%s::%s\n", indentStr.c_str(), type.package_name.c_str(), type.custom_name.c_str());
        } else {
            printf("%s%s\n", indentStr.c_str(), type.custom_name.c_str());
        }
        return;
    default:
        break;
    }

    printf("%s%s\n", indentStr.c_str(), omni_cli::primitiveTypeName(type.primitive));
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
                    if (omni_cli::findTypeRef(g_parse_ctx, method.param_types, g_idl_package, paramType)) {
                        printf("          param: ");
                        printFieldSchema(paramType, g_idl_package, 5);
                    }
                }
                
                // 打印返回值详情
                if (method.return_type != "void") {
                    omnic::TypeRef returnType;
                    if (omni_cli::findTypeRef(g_parse_ctx, method.return_type, g_idl_package, returnType)) {
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
    ret = runtime.connectService(service_name);
    if(ret != 0){
        fprintf(stderr, "Error: Cannot connect service '%s': %s\n",
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
        if (g_parse_ctx && !param_type_name.empty()) {
            omnic::TypeRef paramType;
            if (!omni_cli::findTypeRef(g_parse_ctx, param_type_name, g_idl_package, paramType)) {
                fprintf(stderr, "Error: Cannot find type '%s' in IDL\n", param_type_name.c_str());
                return 1;
            }

            type_codec::TypeCodec codec(*g_parse_ctx);
            if (params[0] == '{' || params[0] == '[') {
                try {
                    simple_json::Value jsonInput = simple_json::parse(params);
                    if (!codec.encodeToBuffer(jsonInput, paramType, g_idl_package, request)) {
                        fprintf(stderr, "Error: Failed to encode JSON to buffer\n");
                        return 1;
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "Error: JSON parse failed: %s\n", e.what());
                    return 1;
                }
            } else if (omni_cli::isScalarCliType(paramType)) {
                simple_json::Value scalarInput;
                if (!omni_cli::parseScalarCliValue(params, paramType, scalarInput)) {
                    fprintf(stderr, "Error: Failed to parse scalar parameter '%s' for type '%s'\n",
                            params, param_type_name.c_str());
                    return 1;
                }
                if (!codec.encodeToBuffer(scalarInput, paramType, g_idl_package, request)) {
                    fprintf(stderr, "Error: Failed to encode scalar parameter to buffer\n");
                    return 1;
                }
            } else {
                if (!hexToBytes(params, request)) {
                    fprintf(stderr, "Error: Invalid hex parameter string: %s\n", params);
                    return 1;
                }
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
    auto start_time = std::chrono::steady_clock::now();

    // Invoke the method
    omnibinder::Buffer response;
    ret = runtime.invoke(info.name.c_str(), iface_id, method_id, 0, request, response);
    
    // 记录结束时间
    auto end_time = std::chrono::steady_clock::now();
    
    // 计算耗时（毫秒）
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    
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
            if (omni_cli::findTypeRef(g_parse_ctx, return_type_name, g_idl_package, returnType)) {
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

static volatile bool g_watch_running = true;
static void watch_sigint_handler(int) { g_watch_running = false; }

static int cmdWatch(omnibinder::OmniRuntime& runtime, const char* service_name, const char* filter) {
    omnibinder::ServiceInfo info;
    int ret = runtime.lookupService(service_name, info);
    if (ret != 0) {
        fprintf(stderr, "Error: Service not found: %s\n", service_name);
        return 1;
    }

    std::map<uint64_t, const omnibinder::MethodInfo*> method_map;
    for (size_t i = 0; i < info.interfaces.size(); ++i) {
        for (size_t j = 0; j < info.interfaces[i].methods.size(); ++j) {
            uint64_t key = (static_cast<uint64_t>(info.interfaces[i].interface_id) << 32)
                         | info.interfaces[i].methods[j].method_id;
            method_map[key] = &info.interfaces[i].methods[j];
        }
    }

    ret = runtime.connectService(service_name);
    if (ret != 0) {
        fprintf(stderr, "Error: Cannot connect to %s\n", service_name);
        return 1;
    }
    ret = runtime.enableDiagnostic(service_name);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to enable diagnostic (status=%d)\n", ret);
        return 1;
    }
    printf("Diagnostic enabled on %s. Press Ctrl+C to stop.\n", service_name);

    std::string diag_topic = "__diag__" + std::string(service_name);
    std::map<uint32_t, uint64_t> req_timestamps;
    std::map<uint32_t, const omnibinder::MethodInfo*> pending_methods;

    runtime.subscribeTopic(diag_topic,
        [&method_map, &req_timestamps, &pending_methods, &info, filter](uint32_t, const omnibinder::Buffer& data) {

        const size_t DIAG_HDR = 19;
        if (data.size() < DIAG_HDR) {
            return;
        }

        const uint8_t* p = data.data();
        uint8_t direction = p[0];
        uint64_t ts_us = 0;
        for (int i = 0; i < 8; ++i) {
            ts_us = (ts_us << 8) | p[1 + i];
        }
        uint16_t orig_type = static_cast<uint16_t>(p[9]) | (static_cast<uint16_t>(p[10]) << 8);
        uint32_t orig_seq  = static_cast<uint32_t>(p[11]) | (static_cast<uint32_t>(p[12]) << 8)
                           | (static_cast<uint32_t>(p[13]) << 16) | (static_cast<uint32_t>(p[14]) << 24);
        uint32_t orig_len  = static_cast<uint32_t>(p[15]) | (static_cast<uint32_t>(p[16]) << 8)
                           | (static_cast<uint32_t>(p[17]) << 16) | (static_cast<uint32_t>(p[18]) << 24);

        const char* dir_str = "?";
        switch (direction) {
        case 0: dir_str = "REQUEST  "; break;
        case 1: dir_str = "RESPONSE "; break;
        case 2: dir_str = "ONE_WAY  "; break;
        case 3: dir_str = "SUBSCRIBE"; break;
        case 4: dir_str = "BROADCAST"; break;
        }

        uint64_t latency_us = 0;
        if (direction == 0) {
            req_timestamps[orig_seq] = ts_us;
        } else if (direction == 1) {
            auto it = req_timestamps.find(orig_seq);
            if (it != req_timestamps.end()) {
                latency_us = ts_us - it->second;
                req_timestamps.erase(it);
            }
        }

        std::string method_name = "?";
        uint32_t param_len = 0, resp_len = 0;
        int32_t resp_status = 0;
        const omnibinder::MethodInfo* pm = nullptr;

        if (g_parse_ctx && orig_len > 0 && data.size() >= DIAG_HDR + orig_len) {
            const uint8_t* payload = p + DIAG_HDR;
            if (orig_type == 0x0100 && orig_len >= 12) {
                uint32_t iface_id = static_cast<uint32_t>(payload[0])
                    | (static_cast<uint32_t>(payload[1]) << 8)
                    | (static_cast<uint32_t>(payload[2]) << 16)
                    | (static_cast<uint32_t>(payload[3]) << 24);
                uint32_t meth_id  = static_cast<uint32_t>(payload[4])
                    | (static_cast<uint32_t>(payload[5]) << 8)
                    | (static_cast<uint32_t>(payload[6]) << 16)
                    | (static_cast<uint32_t>(payload[7]) << 24);
                param_len = static_cast<uint32_t>(payload[8])
                    | (static_cast<uint32_t>(payload[9]) << 8)
                    | (static_cast<uint32_t>(payload[10]) << 16)
                    | (static_cast<uint32_t>(payload[11]) << 24);
                auto mit = method_map.find((static_cast<uint64_t>(iface_id) << 32) | meth_id);
                if (mit != method_map.end()) {
                    method_name = mit->second->name;
                    pm = mit->second;
                    pending_methods[orig_seq] = mit->second;
                }
            } else if (orig_type == 0x0101 && direction == 1 && orig_len >= 8) {
                resp_status = static_cast<int32_t>(
                    static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8)
                    | (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24));
                resp_len = static_cast<uint32_t>(payload[4]) | (static_cast<uint32_t>(payload[5]) << 8)
                    | (static_cast<uint32_t>(payload[6]) << 16) | (static_cast<uint32_t>(payload[7]) << 24);
                auto pm_it = pending_methods.find(orig_seq);
                if (pm_it != pending_methods.end()) {
                    pm = pm_it->second;
                    method_name = pm->name;
                    pending_methods.erase(pm_it);
                }
            }
        }
        if (direction == 4) {
            method_name = "broadcast";
        } else if (direction == 3) {
            method_name = "subscribe";
        }
        if (filter && filter[0] && method_name != "?" && strcmp(method_name.c_str(), filter) != 0) {
            return;
        }

        char line[512];
        int off = snprintf(line, sizeof(line), "%s %s.%s() seq=%u len=%u",
                           dir_str, info.name.c_str(), method_name.c_str(), orig_seq, orig_len);
        if (direction == 1 && latency_us > 0) {
            if (latency_us >= 1000) {
                off += snprintf(line + off, sizeof(line) - off, " (%.2f ms)", latency_us / 1000.0);
            } else {
                off += snprintf(line + off, sizeof(line) - off, " (%llu us)", (unsigned long long)latency_us);
            }
        }
        // OMNI_LOG_INFO("Watch", "%s", line);

        if (g_parse_ctx && orig_len > 0 && data.size() >= DIAG_HDR + orig_len) {
            const uint8_t* payload = p + DIAG_HDR;

            if (orig_type == 0x0100 && orig_len >= 12 && pm) {
                char detail[512];
                off = snprintf(detail, sizeof(detail), "  %s.%s(", info.name.c_str(), pm->name.c_str());
                if (!pm->param_types.empty() && param_len > 0) {
                    omnic::TypeRef paramType;
                    if (omni_cli::findTypeRef(g_parse_ctx, pm->param_types, g_idl_package, paramType)) {
                        omnibinder::Buffer param_buf;
                        param_buf.assign(payload + 12, param_len);
                        type_codec::TypeCodec codec(*g_parse_ctx);
                        simple_json::Value jsonOut;
                        if (codec.decodeFromBuffer(param_buf, paramType, g_idl_package, jsonOut)) {
                            if (paramType.primitive == omnic::TYPE_CUSTOM) {
                                off += snprintf(detail + off, sizeof(detail) - off, "%s",
                                               jsonOut.toString(true, 1).c_str());
                            } else {
                                off += snprintf(detail + off, sizeof(detail) - off, "%s: %s",
                                               pm->param_types.c_str(), jsonOut.toString(false, 0).c_str());
                            }
                        } else {
                            off += snprintf(detail + off, sizeof(detail) - off, "...");
                        }
                    } else {
                        off += snprintf(detail + off, sizeof(detail) - off, "...");
                    }
                }
                snprintf(detail + off, sizeof(detail) - off, ")");
                OMNI_LOG_INFO("Watch", "%s  \n%s",line, detail);

            } else if (orig_type == 0x0101 && direction == 1 && orig_len >= 8) {
                char detail[512];
                off = snprintf(detail, sizeof(detail), "  -> status=%d", resp_status);
                if (resp_len > 0 && pm && !pm->return_type.empty() && pm->return_type != "void") {
                    omnic::TypeRef retType;
                    if (omni_cli::findTypeRef(g_parse_ctx, pm->return_type, g_idl_package, retType)) {
                        omnibinder::Buffer resp_buf;
                        resp_buf.assign(payload + 8, resp_len);
                        type_codec::TypeCodec codec(*g_parse_ctx);
                        simple_json::Value jsonOut;
                        if (codec.decodeFromBuffer(resp_buf, retType, g_idl_package, jsonOut)) {
                            if (retType.primitive == omnic::TYPE_CUSTOM) {
                                off += snprintf(detail + off, sizeof(detail) - off, " %s",
                                               jsonOut.toString(true, 1).c_str());
                            } else {
                                off += snprintf(detail + off, sizeof(detail) - off, " %s: %s",
                                               pm->return_type.c_str(), jsonOut.toString(false, 0).c_str());
                            }
                        }
                    }
                }
                OMNI_LOG_INFO("Watch", "%s  \n%s",line, detail);

            } else if (orig_type == 0x0110 && orig_len >= 8) {
                uint32_t data_len = static_cast<uint32_t>(payload[4])
                    | (static_cast<uint32_t>(payload[5]) << 8)
                    | (static_cast<uint32_t>(payload[6]) << 16)
                    | (static_cast<uint32_t>(payload[7]) << 24);
                if (data_len > 0) {
                    omnibinder::Buffer data_buf;
                    data_buf.assign(payload + 8, data_len);
                    bool decoded = false;
                    for (const auto& pkg : g_parse_ctx->loaded_packages) {
                        for (size_t k = 0; k < pkg.second.topics.size(); ++k) {
                            const omnic::TopicDef& tdef = pkg.second.topics[k];
                            omnic::StructDef synth;
                            synth.name = tdef.name;
                            synth.fields = tdef.fields;
                            g_parse_ctx->loaded_packages[pkg.first].structs.push_back(synth);
                            omnic::TypeRef msgType;
                            msgType.primitive = omnic::TYPE_CUSTOM;
                            msgType.custom_name = tdef.name;
                            type_codec::TypeCodec codec(*g_parse_ctx);
                            simple_json::Value jsonOut;
                            if (codec.decodeFromBuffer(data_buf, msgType, pkg.first, jsonOut)) {
                                OMNI_LOG_INFO("Watch", "%s\n  %s %s",line,
                                              tdef.name.c_str(), jsonOut.toString(true, 1).c_str());
                                decoded = true;
                            }
                            g_parse_ctx->loaded_packages[pkg.first].structs.pop_back();
                            if (decoded) {
                                break;
                            }
                        }
                        if (decoded) {
                            break;
                        }
                    }
                    if (!decoded) {
                        char hex[256];
                        int hoff = 0;
                        uint32_t d = data_len > 64 ? 64 : data_len;
                        for (uint32_t i = 0; i < d; ++i) {
                            hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02x ", payload[i]);
                        }
                        OMNI_LOG_INFO("Watch", "%s \n  HEX: %s", hex);
                    }
                }
            } else {
                char hex[256];
                int hoff = 0;
                uint32_t d = orig_len > 64 ? 64 : orig_len;
                for (uint32_t i = 0; i < d; ++i) {
                    hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02x ", payload[i]);
                }
                OMNI_LOG_INFO("Watch", "%s \n  HEX: %s",line, hex);
            }
        } else if (orig_len > 0 && data.size() >= DIAG_HDR + orig_len) {
            const uint8_t* payload = p + DIAG_HDR;
            char hex[256];
            int hoff = 0;
            uint32_t d = orig_len > 128 ? 128 : orig_len;
            for (uint32_t i = 0; i < d; ++i) {
                hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02x ", payload[i]);
            }
            if (orig_len > 128) {
                snprintf(hex + hoff, sizeof(hex) - hoff, "...");
            }
            OMNI_LOG_INFO("Watch", "%s \n  HEX: %s",line, hex);
        }
    }, nullptr);

    signal(SIGINT, watch_sigint_handler);
    signal(SIGTERM, watch_sigint_handler);
    while (g_watch_running) {
        runtime.pollOnce(100);
    }
    runtime.unsubscribeTopic(diag_topic);
    runtime.disableDiagnostic(service_name);
    printf("\nDiagnostic watch stopped.\n");
    return 0;
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 9900;
    const char* idl_file = NULL;
    const char* diag_filter = NULL;
    const char* positional[5] = {NULL, NULL, NULL, NULL, NULL};
    int pos_count = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--idl") == 0 && i + 1 < argc) {
            idl_file = argv[++i];
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            diag_filter = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (pos_count < 5) {
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
    } else if (strcmp(command, "watch") == 0) {
        if (!positional[1]) { fprintf(stderr, "Error: 'watch' requires <service>\n"); result = 1; }
        else result = cmdWatch(runtime, positional[1], diag_filter);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        result = 1;
    }
    
    runtime.stop();
    if (g_parse_ctx) delete g_parse_ctx;
    return result;
}
