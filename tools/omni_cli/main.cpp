#include <omnibinder/omnibinder.h>
#include "simple_json.h"
#include "type_codec.h"
#include "type_resolver.h"
#include "info_formatter.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_validator.h"
#include "codegen.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <chrono>
#include <csignal>
#include <map>
#include <vector>
#include <deque>

// 全局 IDL 解析上下文
static omnic::ParseContext* g_parse_ctx = NULL;
static std::string g_idl_package;

static void appendFormat(char* buf, size_t size, int& offset, const char* fmt, ...) {
    if (!buf || size == 0 || offset < 0) {
        return;
    }
    size_t pos = static_cast<size_t>(offset);
    if (pos >= size) {
        pos = size - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + pos, size - pos, fmt, ap);
    va_end(ap);
    if (written < 0) {
        return;
    }
    size_t available = size - pos;
    if (static_cast<size_t>(written) >= available) {
        offset = static_cast<int>(size - 1);
    } else {
        offset = static_cast<int>(pos + static_cast<size_t>(written));
    }
}

static void printUsage(const char* prog) {
    printf("Usage: %s [options] <command> [args]\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host <addr>   ServiceManager address (default: 127.0.0.1)\n");
    printf("  -p, --port <port>   ServiceManager port (default: 9900)\n");
    printf("  --idl <file.bidl>   IDL file for JSON I/O/watch support\n");
    printf("  --help              Show this help\n\n");
    printf("Commands:\n");
    printf("  list                List all online services\n");
    printf("  ps                  List online runtimes for --pid commands\n");
    printf("  info <service>      Show service details and interfaces\n");
    printf("  call <service> <method> [params]\n");
    printf("                      Call a service method\n");
    printf("                      params: hex string (without --idl) or JSON (with --idl)\n");
    printf("  log set --pid <pid> --level <F|E|W|I|D|V|O>\n");
    printf("                      Set a runtime log level by PID\n");
    printf("  watch --pid <pid> --idl <file.bidl> [--filter <method|topic>]\n");
    printf("                      Watch diagnostic data from a runtime PID\n");
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

static const char* cliLogLevelCode(uint32_t level) {
    switch (level) {
    case omnibinder::LOG_FATAL: return "F";
    case omnibinder::LOG_ERROR: return "E";
    case omnibinder::LOG_WARN: return "W";
    case omnibinder::LOG_INFO: return "I";
    case omnibinder::LOG_DEBUG: return "D";
    case omnibinder::LOG_VERBOSE: return "V";
    case omnibinder::LOG_OFF: return "O";
    default: return "?";
    }
}

static int cmdPs(omnibinder::OmniRuntime& runtime) {
    std::vector<omnibinder::RuntimeInfo> runtimes;
    int ret = runtime.listRuntimes(runtimes);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to list runtimes (status=%d)\n", ret);
        return 1;
    }
    printf("%-8s %-8s %-5s %-20s %s\n", "PID", "ROLE", "LOG", "PROCESS", "SERVICES");
    printf("%-8s %-8s %-5s %-20s %s\n", "--------", "--------", "-----", "--------------------", "----------------");
    for (size_t i = 0; i < runtimes.size(); ++i) {
        std::string services;
        for (size_t j = 0; j < runtimes[i].services.size(); ++j) {
            if (j > 0) services += ",";
            services += runtimes[i].services[j];
        }
        if (services.empty()) services = "-";
        printf("%-8u %-8s %-5s %-20s %s\n",
               runtimes[i].pid,
               runtimes[i].role.c_str(),
               cliLogLevelCode(runtimes[i].log_level),
               runtimes[i].process_name.c_str(),
               services.c_str());
    }
    return 0;
}

/* @brief 打印字段定义
 * @param[in] type 字段类型
 * @param[in] package 类型所属包
 * @param[in] indent 缩进层级
 * @note 用于详细模式
 */
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

    std::vector<std::string> published_topics;
    const uint32_t topics_query_timeout_ms = 1000;
    int topics_ret = runtime.queryPublishedTopics(
        service_name, published_topics, topics_query_timeout_ms);
    std::string topics_unavailable;
    if (topics_ret != 0) {
        topics_unavailable = omnibinder::errorCodeToString(
            static_cast<omnibinder::ErrorCode>(topics_ret));
    }
    printf("%s", omni_cli::formatPublishedTopicsSection(
        published_topics, topics_unavailable).c_str());
    
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

/* @brief 将 hex 字符串转换为字节流
 * @param[in] hex hex 字符串
 * @param[out] buf 输出 Buffer
 * @return 成功返回 true
 */
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

/* @brief 以 hex dump 形式打印字节流
 * @param[in] data 字节数据
 * @param[in] len 数据长度
 */
static void printHexDump(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i > 0 && i % 16 == 0) printf("\n  ");
        printf("%02x ", data[i]);
    }
    printf("\n");
}

static uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0])
        | (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

static bool isInvokeRequestType(uint16_t type) {
    return type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE)
        || type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE_ONEWAY);
}

static uint32_t parseLogLevelCode(const char* text, bool& ok) {
    ok = text && text[0] && text[1] == '\0';
    if (!ok) return 0;
    switch (text[0]) {
    case 'F': return static_cast<uint32_t>(omnibinder::LOG_FATAL);
    case 'E': return static_cast<uint32_t>(omnibinder::LOG_ERROR);
    case 'W': return static_cast<uint32_t>(omnibinder::LOG_WARN);
    case 'I': return static_cast<uint32_t>(omnibinder::LOG_INFO);
    case 'D': return static_cast<uint32_t>(omnibinder::LOG_DEBUG);
    case 'V': return static_cast<uint32_t>(omnibinder::LOG_VERBOSE);
    case 'O': return static_cast<uint32_t>(omnibinder::LOG_OFF);
    default: ok = false; return 0;
    }
}

static bool parseUint32Arg(const char* text, uint32_t& value) {
    if (!text || !text[0]) {
        return false;
    }
    char* end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 0xFFFFFFFFul) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

static std::string typeRefCliName(const omnic::TypeRef& type) {
    if (type.primitive == omnic::TYPE_CUSTOM) {
        if (!type.package_name.empty()) {
            return type.package_name + "::" + type.custom_name;
        }
        return type.custom_name;
    }
    if (type.primitive == omnic::TYPE_VOID) {
        return "void";
    }
    if (type.primitive == omnic::TYPE_ARRAY || type.element_type) {
        return "";
    }
    return omni_cli::primitiveTypeName(type.primitive);
}

static void buildIdlMethodMap(std::map<uint64_t, const omnibinder::MethodInfo*>& method_map,
                              std::deque<omnibinder::MethodInfo>& method_storage) {
    method_map.clear();
    method_storage.clear();
    if (!g_parse_ctx) {
        return;
    }
    for (std::map<std::string, omnic::AstFile>::const_iterator pit = g_parse_ctx->loaded_packages.begin();
         pit != g_parse_ctx->loaded_packages.end(); ++pit) {
        const omnic::AstFile& ast = pit->second;
        for (size_t si = 0; si < ast.services.size(); ++si) {
            const omnic::ServiceDef& svc = ast.services[si];
            uint32_t iface_id = omnic::fnv1a_hash(ast.package_name + "." + svc.name);
            for (size_t mi = 0; mi < svc.methods.size(); ++mi) {
                const omnic::MethodDef& m = svc.methods[mi];
                uint32_t method_id = omnic::fnv1a_hash(m.name);
                std::string param_type = m.has_param ? typeRefCliName(m.param.type) : "";
                std::string return_type = typeRefCliName(m.return_type);
                omnibinder::MethodInfo info(method_id, m.name, param_type, return_type);
                info.idl_hash = omnic::computeMethodHash(m, ast);
                method_storage.push_back(info);
                uint64_t key = (static_cast<uint64_t>(iface_id) << 32) | method_id;
                method_map[key] = &method_storage.back();
            }
        }
    }
}

/* @brief 解析并校验 log set 的 <pid> + <level> 参数，然后调用运行时接口
 * @param[in,out] runtime OmniRuntime 实例
 * @param[in] pid_text pid 文本
 * @param[in] level_text 日志级别文本
 * @return 成功返回 0，失败返回 1
 */
static int cmdLogSet(omnibinder::OmniRuntime& runtime, const char* pid_text, const char* level_text) {
    uint32_t pid = 0;
    if (!parseUint32Arg(pid_text, pid) || pid == 0) {
        fprintf(stderr, "Error: invalid --pid value: %s\n", pid_text);
        return 1;
    }
    bool ok = false;
    uint32_t level = parseLogLevelCode(level_text, ok);
    if (!ok) {
        fprintf(stderr, "Error: level must be one of F,E,W,I,D,V,O\n");
        return 1;
    }
    int ret = runtime.setLogLevelByPid(pid, level);
    if (ret != 0) {
        fprintf(stderr, "Error: failed to set log level for pid=%u (status=%d)\n", pid, ret);
        return 1;
    }
    printf("Set pid=%u log level to %s\n", pid, level_text);
    return 0;
}

static int cmdCall(omnibinder::OmniRuntime& runtime, const char* service_name,
                   const char* method_name, const char* params) {
    // 查找服务
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

    // 查找方法
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

    // 构造请求 payload
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

    // 调用方法
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

/**
 * @brief watch 诊断事件头
 * @details 对应 DIAG_EVENT_WIRE_HEADER_SIZE 布局
 */
struct DiagEventHeader {
    uint8_t  direction;
    uint64_t ts_us;
    uint16_t orig_type;
    uint32_t orig_seq;
    uint32_t orig_len;
};

/**
 * @brief watch 期间跨事件累积的状态
 * @details 包含 IDL 方法表、请求时间戳和待回复方法
 */
struct WatchState {
    WatchState() : pid(0), filter(NULL) {}
    uint32_t pid;
    const char* filter;
    std::map<uint64_t, const omnibinder::MethodInfo*> method_map;
    std::deque<omnibinder::MethodInfo> method_storage;
    std::map<uint32_t, uint64_t> req_timestamps;
    std::map<uint32_t, const omnibinder::MethodInfo*> pending_methods;
};

/**
 * @brief 单个诊断事件解析出的语义信息
 */
struct WatchEventInfo {
    WatchEventInfo()
        : method(NULL), param_len(0), resp_len(0), resp_status(0), name("?") {}
    const omnibinder::MethodInfo* method;
    uint32_t param_len;
    uint32_t resp_len;
    int32_t  resp_status;
    std::string name;
};

static bool parseDiagEventHeader(const omnibinder::Buffer& data, DiagEventHeader& hdr) {
    const size_t diag_hdr = omnibinder::DIAG_EVENT_WIRE_HEADER_SIZE;
    if (data.size() < diag_hdr) {
        return false;
    }
    const uint8_t* p = data.data();
    hdr.direction = p[0];
    hdr.ts_us = 0;
    for (int i = 0; i < 8; ++i) {
        hdr.ts_us |= static_cast<uint64_t>(p[1 + i]) << (8 * i);
    }
    hdr.orig_type = readLe16(p + 9);
    hdr.orig_seq  = readLe32(p + 11);
    hdr.orig_len  = readLe32(p + 15);
    return true;
}

static std::string watchedSourceName(uint32_t pid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "pid%u", pid);
    return std::string(buf);
}

static const char* diagDirectionLabel(uint8_t direction) {
    switch (direction) {
    case omnibinder::DIAG_EVENT_REQUEST:   return "REQUEST  ";
    case omnibinder::DIAG_EVENT_RESPONSE:  return "RESPONSE ";
    case omnibinder::DIAG_EVENT_ONE_WAY:   return "ONE_WAY  ";
    case omnibinder::DIAG_EVENT_SUBSCRIBE: return "SUBSCRIBE";
    case omnibinder::DIAG_EVENT_BROADCAST: return "BROADCAST";
    default: return "?";
    }
}

/* @brief 记录 REQUEST 时间戳并匹配 RESPONSE 耗时
 * @param[in,out] state watch 状态
 * @param[in] ev 诊断事件头
 * @return 本次事件耗时（us，0 表示无配对）
 */
static uint64_t trackWatchLatency(WatchState& state, const DiagEventHeader& ev) {
    if (ev.direction == omnibinder::DIAG_EVENT_REQUEST) {
        state.req_timestamps[ev.orig_seq] = ev.ts_us;
        return 0;
    }
    if (ev.direction == omnibinder::DIAG_EVENT_RESPONSE) {
        std::map<uint32_t, uint64_t>::iterator it = state.req_timestamps.find(ev.orig_seq);
        if (it != state.req_timestamps.end()) {
            uint64_t latency_us = ev.ts_us - it->second;
            state.req_timestamps.erase(it);
            return latency_us;
        }
    }
    return 0;
}

/* @brief 从 IDL 方法表解析方法名、请求参数长度、回复状态码
 * @param[in,out] state watch 状态
 * @param[in] ev 诊断事件头
 * @param[in] data 事件数据
 * @return 解析出的语义信息
 */
static WatchEventInfo resolveWatchEvent(WatchState& state, const DiagEventHeader& ev,
                                        const omnibinder::Buffer& data) {
    WatchEventInfo info;
    const size_t diag_hdr = omnibinder::DIAG_EVENT_WIRE_HEADER_SIZE;
    if (g_parse_ctx && ev.orig_len > 0 && data.size() >= diag_hdr + ev.orig_len) {
        const uint8_t* payload = data.data() + diag_hdr;
        if (isInvokeRequestType(ev.orig_type) && ev.orig_len >= 16) {
            uint32_t iface_id = readLe32(payload);
            uint32_t meth_id  = readLe32(payload + 8);
            info.param_len = readLe32(payload + 12);
            if (info.param_len > ev.orig_len - 16) {
                info.param_len = 0;
            }
            std::map<uint64_t, const omnibinder::MethodInfo*>::iterator mit =
                state.method_map.find((static_cast<uint64_t>(iface_id) << 32) | meth_id);
            if (mit != state.method_map.end()) {
                info.method = mit->second;
                info.name = mit->second->name;
                state.pending_methods[ev.orig_seq] = mit->second;
            }
        } else if (ev.orig_type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE_REPLY)
                   && ev.direction == omnibinder::DIAG_EVENT_RESPONSE && ev.orig_len >= 8) {
            info.resp_status = static_cast<int32_t>(readLe32(payload));
            info.resp_len = readLe32(payload + 4);
            if (info.resp_len > ev.orig_len - 8) {
                info.resp_len = 0;
            }
            std::map<uint32_t, const omnibinder::MethodInfo*>::iterator pm_it =
                state.pending_methods.find(ev.orig_seq);
            if (pm_it != state.pending_methods.end()) {
                info.method = pm_it->second;
                info.name = info.method->name;
                state.pending_methods.erase(pm_it);
            }
        }
    }
    if (ev.direction == omnibinder::DIAG_EVENT_BROADCAST) {
        info.name = "broadcast";
    } else if (ev.direction == omnibinder::DIAG_EVENT_SUBSCRIBE) {
        info.name = "subscribe";
    }
    return info;
}

/* @brief 按 IDL 类型解码 payload
 * @param[in,out] buf 数据 Buffer
 * @param[in] type 目标类型
 * @param[in] package 类型所属包
 * @param[in] type_name 类型名
 * @param[out] out 解码输出
 * @return 成功返回 true
 * @note 结构体输出 JSON，标量输出 "type: value"
 */
static bool decodeWatchValue(omnibinder::Buffer& buf, const omnic::TypeRef& type,
                             const std::string& package, const std::string& type_name,
                             std::string& out) {
    type_codec::TypeCodec codec(*g_parse_ctx);
    simple_json::Value json_out;
    if (!codec.decodeFromBuffer(buf, type, package, json_out)) {
        return false;
    }
    if (type.primitive == omnic::TYPE_CUSTOM) {
        out = json_out.toString(true, 1);
    } else {
        out = type_name + ": " + json_out.toString(false, 0);
    }
    return true;
}

/* @brief 输出 hex 预览
 * @param[in] line 日志行前缀
 * @param[in] payload 字节数据
 * @param[in] len 数据长度
 * @param[in] max_bytes 最大打印字节数
 * @param[in] show_ellipsis 是否在截断时追加 "..."
 */
static void logWatchHex(const char* line, const uint8_t* payload, uint32_t len,
                        uint32_t max_bytes, bool show_ellipsis) {
    char hex[256];
    int off = 0;
    uint32_t shown = len > max_bytes ? max_bytes : len;
    for (uint32_t i = 0; i < shown; ++i) {
        appendFormat(hex, sizeof(hex), off, "%02x ", payload[i]);
    }
    if (show_ellipsis && len > max_bytes) {
        appendFormat(hex, sizeof(hex), off, "...");
    }
    OMNI_LOG_INFO("Watch", "%s \n  HEX: %s", line, hex);
}

/* @brief 输出 REQUEST 事件的参数解码明细
 * @param[in] info 事件语义信息
 * @param[in] payload 事件 payload
 * @param[in] source_name 来源名称
 * @param[in] line 日志行前缀
 */
static void logWatchRequestDetail(const WatchEventInfo& info, const uint8_t* payload,
                                  const std::string& source_name, const char* line) {
    char detail[512];
    int off = snprintf(detail, sizeof(detail), "  %s.%s(", source_name.c_str(),
                       info.method->name.c_str());
    if (!info.method->param_types.empty() && info.param_len > 0) {
        omnic::TypeRef param_type;
        if (omni_cli::findTypeRef(g_parse_ctx, info.method->param_types, g_idl_package, param_type)) {
            omnibinder::Buffer param_buf;
            param_buf.assign(payload + 16, info.param_len);
            std::string text;
            if (decodeWatchValue(param_buf, param_type, g_idl_package,
                                 info.method->param_types, text)) {
                appendFormat(detail, sizeof(detail), off, "%s", text.c_str());
            } else {
                appendFormat(detail, sizeof(detail), off, "...");
            }
        } else {
            appendFormat(detail, sizeof(detail), off, "...");
        }
    }
    appendFormat(detail, sizeof(detail), off, ")");
    OMNI_LOG_INFO("Watch", "%s  \n%s", line, detail);
}

/* @brief 输出 RESPONSE 事件的状态码与返回值解码明细
 * @param[in] ev 诊断事件头
 * @param[in] info 事件语义信息
 * @param[in] payload 事件 payload
 * @param[in] line 日志行前缀
 */
static void logWatchReplyDetail(const DiagEventHeader& ev, const WatchEventInfo& info,
                                const uint8_t* payload, const char* line) {
    char detail[512];
    int off = snprintf(detail, sizeof(detail), "  -> status=%d", info.resp_status);
    if (info.resp_len > 0 && info.resp_len <= ev.orig_len - 8 && info.method &&
        !info.method->return_type.empty() && info.method->return_type != "void") {
        omnic::TypeRef ret_type;
        if (omni_cli::findTypeRef(g_parse_ctx, info.method->return_type, g_idl_package, ret_type)) {
            omnibinder::Buffer resp_buf;
            resp_buf.assign(payload + 8, info.resp_len);
            std::string text;
            if (decodeWatchValue(resp_buf, ret_type, g_idl_package,
                                 info.method->return_type, text)) {
                appendFormat(detail, sizeof(detail), off, " %s", text.c_str());
            }
        }
    }
    OMNI_LOG_INFO("Watch", "%s  \n%s", line, detail);
}

/* @brief 输出 BROADCAST 事件的 topic 解码明细
 * @param[in] ev 诊断事件头
 * @param[in] payload 事件 payload
 * @param[in] line 日志行前缀
 * @note 无法解码时回退 hex
 */
static void logWatchBroadcastDetail(const DiagEventHeader& ev, const uint8_t* payload,
                                    const char* line) {
    uint32_t data_len = readLe32(payload + 4);
    if (data_len == 0 || data_len > ev.orig_len - 8) {
        return;
    }
    omnibinder::Buffer data_buf;
    data_buf.assign(payload + 8, data_len);
    for (std::map<std::string, omnic::AstFile>::const_iterator pit =
             g_parse_ctx->loaded_packages.begin();
         pit != g_parse_ctx->loaded_packages.end(); ++pit) {
        const omnic::AstFile& ast = pit->second;
        for (size_t k = 0; k < ast.topics.size(); ++k) {
            const omnic::TopicDef& tdef = ast.topics[k];
            omnic::StructDef synth;
            synth.name = tdef.name;
            synth.fields = tdef.fields;
            g_parse_ctx->loaded_packages[pit->first].structs.push_back(synth);
            omnic::TypeRef msg_type;
            msg_type.primitive = omnic::TYPE_CUSTOM;
            msg_type.custom_name = tdef.name;
            std::string text;
            bool decoded = decodeWatchValue(data_buf, msg_type, pit->first, tdef.name, text);
            g_parse_ctx->loaded_packages[pit->first].structs.pop_back();
            if (decoded) {
                OMNI_LOG_INFO("Watch", "%s\n  %s %s", line, tdef.name.c_str(), text.c_str());
                return;
            }
        }
    }
    logWatchHex(line, payload + 8, data_len, 64, false);
}

/* @brief 处理单条诊断事件
 * @param[in,out] state watch 状态
 * @param[in] data 事件数据
 * @note 流程：解析头部 -> 过滤 -> 格式化输出
 */
static void handleWatchEvent(WatchState& state, const omnibinder::Buffer& data) {
    DiagEventHeader ev;
    if (!parseDiagEventHeader(data, ev)) {
        return;
    }
    std::string source_name = watchedSourceName(state.pid);
    uint64_t latency_us = trackWatchLatency(state, ev);
    WatchEventInfo info = resolveWatchEvent(state, ev, data);
    if (state.filter && state.filter[0] && info.name != "?" &&
        strcmp(info.name.c_str(), state.filter) != 0) {
        return;
    }

    char line[512];
    int off = snprintf(line, sizeof(line), "%s %s.%s() seq=%u len=%u",
                       diagDirectionLabel(ev.direction), source_name.c_str(),
                       info.name.c_str(), ev.orig_seq, ev.orig_len);
    if (ev.direction == omnibinder::DIAG_EVENT_RESPONSE && latency_us > 0) {
        if (latency_us >= 1000) {
            appendFormat(line, sizeof(line), off, " (%.2f ms)", latency_us / 1000.0);
        } else {
            appendFormat(line, sizeof(line), off, " (%llu us)", (unsigned long long)latency_us);
        }
    }

    const size_t diag_hdr = omnibinder::DIAG_EVENT_WIRE_HEADER_SIZE;
    const bool has_payload = ev.orig_len > 0 && data.size() >= diag_hdr + ev.orig_len;
    if (g_parse_ctx && has_payload) {
        const uint8_t* payload = data.data() + diag_hdr;
        if (isInvokeRequestType(ev.orig_type) && ev.orig_len >= 16 && info.method) {
            logWatchRequestDetail(info, payload, source_name, line);
        } else if (ev.orig_type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE_REPLY)
                   && ev.direction == omnibinder::DIAG_EVENT_RESPONSE && ev.orig_len >= 8) {
            logWatchReplyDetail(ev, info, payload, line);
        } else if (ev.orig_type == static_cast<uint16_t>(omnibinder::MessageType::MSG_BROADCAST)
                   && ev.orig_len >= 8) {
            logWatchBroadcastDetail(ev, payload, line);
        } else {
            logWatchHex(line, payload, ev.orig_len, 64, false);
        }
    } else if (has_payload) {
        logWatchHex(line, data.data() + diag_hdr, ev.orig_len, 128, true);
    }
}

static int cmdWatch(omnibinder::OmniRuntime& runtime, uint32_t pid, const char* filter) {
    WatchState state;
    state.pid = pid;
    state.filter = filter;
    buildIdlMethodMap(state.method_map, state.method_storage);

    int ret = runtime.watchPid(pid, [&state](const omnibinder::Buffer& data) {
        handleWatchEvent(state, data);
    });
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to start watch for pid=%u (status=%d)\n", pid, ret);
        return 1;
    }
    printf("Watching pid=%u. Press Ctrl+C to stop.\n", pid);

    signal(SIGINT, watch_sigint_handler);
    signal(SIGTERM, watch_sigint_handler);
    while (g_watch_running) {
        runtime.pollOnce(100);
    }
    runtime.unwatchPid(pid);
    printf("\nDiagnostic watch stopped.\n");
    return 0;
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 9900;
    const char* idl_file = NULL;
    const char* pid_arg = NULL;
    const char* level_arg = NULL;
    const char* diag_filter = NULL;
    const char* positional[5] = {NULL, NULL, NULL, NULL, NULL};
    int pos_count = 0;
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--idl") == 0 && i + 1 < argc) {
            idl_file = argv[++i];
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid_arg = argv[++i];
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            level_arg = argv[++i];
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
    
    // 如提供则解析 IDL 文件
    if (idl_file) {
        std::ifstream ifs(idl_file);
        if (!ifs.is_open()) {
            fprintf(stderr, "Error: Cannot open IDL file: %s\n", idl_file);
            return 1;
        }
        ifs.seekg(0, std::ios::end);
        const std::streamoff source_size = ifs.tellg();
        if (source_size < 0 ||
            static_cast<unsigned long long>(source_size) > omnic::IDLC_MAX_SOURCE_BYTES_PER_FILE) {
            fprintf(stderr, "Error: IDL source size limit exceeded: %s\n", idl_file);
            return 1;
        }
        ifs.seekg(0, std::ios::beg);
        
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
        std::string semantic_error;
        if (!omnic::validateSemantics(ast, *g_parse_ctx, semantic_error)) {
            fprintf(stderr, "Error: IDL semantic validation failed: %s\n", semantic_error.c_str());
            delete g_parse_ctx;
            g_parse_ctx = NULL;
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
    } else if (strcmp(command, "ps") == 0) {
        result = cmdPs(runtime);
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
        if (!pid_arg) {
            fprintf(stderr, "Error: 'watch' requires --pid <pid>\n");
            result = 1;
        } else if (!g_parse_ctx) {
            fprintf(stderr, "Error: 'watch' requires --idl <file.bidl>\n");
            result = 1;
        } else {
            uint32_t pid = 0;
            if (!parseUint32Arg(pid_arg, pid) || pid == 0) {
                fprintf(stderr, "Error: invalid --pid value: %s\n", pid_arg);
                result = 1;
            } else {
                result = cmdWatch(runtime, pid, diag_filter);
            }
        }
    } else if (strcmp(command, "log") == 0) {
        if (!positional[1] || strcmp(positional[1], "set") != 0 || !pid_arg || !level_arg) {
            fprintf(stderr, "Error: usage: log set --pid <pid> --level <F|E|W|I|D|V|O>\n");
            result = 1;
        } else {
            result = cmdLogSet(runtime, pid_arg, level_arg);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        result = 1;
    }
    
    runtime.stop();
    if (g_parse_ctx) delete g_parse_ctx;
    return result;
}
