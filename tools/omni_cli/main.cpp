#include <omnibinder/omnibinder.h>
#include <omnibinder/message.h>
#include "simple_json.h"
#include "type_codec.h"
#include "type_resolver.h"
#include "info_formatter.h"
#include "watch_monitor.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_validator.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <map>
#include <vector>

// 全局 IDL 解析上下文
static omnic::ParseContext* g_parse_ctx = NULL;
static std::string g_idl_package;

static void printUsage(const char* prog) {
    printf("Usage: %s [options] <command> [args]\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host <addr>   ServiceManager address (default: 127.0.0.1)\n");
    printf("  -p, --port <port>   ServiceManager port (default: 9900)\n");
    printf("  --idl <file.bidl>   IDL file for JSON I/O/watch support\n");
    printf("  --instance-id <id>  Target service instance (info/call)\n");
    printf("                      instance_id is part of the service identity; '' selects\n");
    printf("                      the default instance of a name\n");
    printf("  --host-id <id>      Target host_id, combined with --instance-id (info/call)\n");
    printf("  --help              Show this help\n\n");
    printf("Service identity: a service name may have many instances; an instance is\n");
    printf("  (service_name, host_id, instance_id). `list` enumerates instances.\n\n");
    printf("Commands:\n");
    printf("  list                List all online service instances\n");
    printf("  ps                  List online runtimes for --pid commands\n");
    printf("  info <service>      Enumerate service instances and show interfaces\n");
    printf("                      select one with --instance-id [--host-id]\n");
    printf("  call <service> <method> [params]\n");
    printf("                      Call a method on a specific instance\n");
    printf("                      --instance-id [--host-id] selects it; without a selector\n");
    printf("                      the deterministic first instance is used and reported\n");
    printf("                      params: hex string (without --idl) or JSON (with --idl)\n");
    printf("  log set --pid <pid> --level <F|E|W|I|D|V|O>\n");
    printf("                      Set a runtime log level by PID\n");
    printf("  watch --pid <pid> --idl <file.bidl> [--filter <method|topic>]\n");
    printf("                      Watch diagnostic data from a runtime PID\n");
}

/* @brief 将 ServiceInfo 列表转换为与 omnibinder 解耦的实例视图列表
 * @param[in] infos 服务实例信息列表
 * @return 实例视图列表
 */
static std::vector<omni_cli::ServiceInstanceView> toInstanceViews(
        const std::vector<omnibinder::ServiceInfo>& infos) {
    std::vector<omni_cli::ServiceInstanceView> views;
    views.reserve(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        omni_cli::ServiceInstanceView view;
        view.name = infos[i].name;
        view.host_id = infos[i].host_id;
        view.instance_id = infos[i].instance_id;
        view.host = infos[i].host;
        view.port = infos[i].port;
        views.push_back(view);
    }
    return views;
}

/* @brief 按 (host_id, instance_id) 从实例信息列表中定位对应条目
 * @param[in] infos    服务实例信息列表
 * @param[in] selected 已解析的目标实例
 * @return 匹配项指针；未找到返回 NULL
 */
static const omnibinder::ServiceInfo* findSelectedInfo(
        const std::vector<omnibinder::ServiceInfo>& infos,
        const omni_cli::ServiceInstanceView& selected) {
    for (size_t i = 0; i < infos.size(); ++i) {
        if (infos[i].host_id == selected.host_id
            && infos[i].instance_id == selected.instance_id) {
            return &infos[i];
        }
    }
    return NULL;
}

/* @brief 列出全部在线服务实例
 * @param[in,out] runtime OmniRuntime 实例
 * @return 成功返回 0，失败返回 1
 * @note listServices 返回注册表中每个 (name, host_id, instance_id) 一条，
 *       同名多实例即多行；单一实例只是同一展示的一行特例。
 */
static int cmdList(omnibinder::OmniRuntime& runtime) {
    std::vector<omnibinder::ServiceInfo> services;
    int ret = runtime.listServices(services);
    if (ret != 0) {
        fprintf(stderr, "Error: %s\n", omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    std::vector<omni_cli::ServiceInstanceView> instances = toInstanceViews(services);
    printf("%s", omni_cli::formatInstanceTable(instances).c_str());
    printf("\nTotal: %zu service instances online\n", instances.size());
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

static int resolveServiceInstance(const std::vector<omnibinder::ServiceInfo>& infos,
                                  const std::vector<omni_cli::ServiceInstanceView>& instances,
                                  const omni_cli::InstanceSelector& selector,
                                  omni_cli::ServiceInstanceView& selected,
                                  const omnibinder::ServiceInfo*& info);

static int cmdInfo(omnibinder::OmniRuntime& runtime, const char* service_name,
                   const omni_cli::InstanceSelector& selector) {
    std::vector<omnibinder::ServiceInfo> infos;
    int ret = runtime.listServiceInstances(service_name, infos);
    if (ret != 0) {
        fprintf(stderr, "Error: %s\n", omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    std::vector<omni_cli::ServiceInstanceView> instances = toInstanceViews(infos);
    printf("Service: %s\n", omni_cli::escapeForTerminal(service_name).c_str());
    printf("%s", omni_cli::formatInstanceTable(instances).c_str());
    printf("\n");

    const bool explicit_selection = selector.host_id_set || selector.instance_id_set;
    if (!explicit_selection && instances.size() > 1) {
        printf("%s", omni_cli::formatInstanceSelectionHint().c_str());
        return 0;
    }

    omni_cli::ServiceInstanceView selected;
    const omnibinder::ServiceInfo* info = NULL;
    if (resolveServiceInstance(infos, instances, selector, selected, info) != 0) {
        return 1;
    }

    printf("%s\n\n", omni_cli::formatInstanceIdentity(selected).c_str());

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

    for (size_t i = 0; i < info->interfaces.size(); ++i) {
        const omnibinder::InterfaceInfo& iface = info->interfaces[i];
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

/* @brief 判断字符是否为十六进制数字
 * @param[in] c 待判断字符
 * @return 是十六进制数字返回其值（0-15），否则返回 -1
 */
static int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
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
        int high = hexDigitValue(hex[i]);
        int low = hexDigitValue(hex[i + 1]);
        if (high < 0 || low < 0) return false;
        buf.writeUint8(static_cast<uint8_t>((high << 4) | low));
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

/* @brief 严格解析十进制无符号 32 位整数
 * @param[in] text 文本
 * @param[out] value 解析结果
 * @return 成功返回 true
 * @note 仅接受纯数字，拒绝空白/正负号/超范围
 */
static bool parseUint32Arg(const char* text, uint32_t& value) {
    if (!text || !text[0]) {
        return false;
    }
    unsigned long long acc = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        acc = acc * 10ull + static_cast<unsigned long long>(*p - '0');
        if (acc > 0xFFFFFFFFull) {
            return false;
        }
    }
    value = static_cast<uint32_t>(acc);
    return true;
}

/* @brief 解析 <service> 的目标实例：选择唯一实例并校验其存活
 * @param[in] infos     服务实例信息列表
 * @param[in] instances 实例视图列表
 * @param[in] selector  实例选择器
 * @param[out] selected 选中的实例视图
 * @param[out] info     选中的实例信息指针
 * @return 成功返回 0，失败返回 1
 */
static int resolveServiceInstance(const std::vector<omnibinder::ServiceInfo>& infos,
                                  const std::vector<omni_cli::ServiceInstanceView>& instances,
                                  const omni_cli::InstanceSelector& selector,
                                  omni_cli::ServiceInstanceView& selected,
                                  const omnibinder::ServiceInfo*& info) {
    std::string selection_error;
    if (omni_cli::selectInstance(instances, selector, selected, selection_error) != 0) {
        fprintf(stderr, "Error: %s\n", selection_error.c_str());
        return 1;
    }
    info = findSelectedInfo(infos, selected);
    if (!info) {
        fprintf(stderr, "Error: selected instance disappeared\n");
        return 1;
    }
    return 0;
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
                   const char* method_name, const char* params,
                   const omni_cli::InstanceSelector& selector) {
    // 按实例模型解析目标：未指定选择器时使用确定性第一实例，并在输出中说明
    std::vector<omnibinder::ServiceInfo> infos;
    int ret = runtime.listServiceInstances(service_name, infos);
    if (ret != 0) {
        fprintf(stderr, "Error: Cannot list instances of service '%s': %s\n",
                service_name, omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    std::vector<omni_cli::ServiceInstanceView> instances = toInstanceViews(infos);
    omni_cli::ServiceInstanceView selected;
    const omnibinder::ServiceInfo* info = NULL;
    const bool explicit_selection = selector.host_id_set || selector.instance_id_set;
    if (resolveServiceInstance(infos, instances, selector, selected, info) != 0) {
        return 1;
    }

    printf("%s\n", omni_cli::formatCallTargetNotice(selected, explicit_selection).c_str());

    ret = runtime.connectServiceInstance(service_name, selected.host_id, selected.instance_id);
    if(ret != 0){
        fprintf(stderr, "Error: Cannot connect service '%s' (instance_id=\"%s\" host_id=\"%s\"): %s\n",
                service_name, selected.instance_id.c_str(), selected.host_id.c_str(),
                omnibinder::errorCodeToString(static_cast<omnibinder::ErrorCode>(ret)));
        return 1;
    }

    // 查找方法
    uint32_t iface_id = 0;
    uint32_t method_id = 0;
    std::string param_type_name;
    std::string return_type_name;
    bool found = false;

    for (size_t i = 0; i < info->interfaces.size() && !found; ++i) {
        const omnibinder::InterfaceInfo& iface = info->interfaces[i];
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

    // 调用方法：连接键与 connectServiceInstance 一致，确保命中该实例
    omnibinder::Buffer response;
    const std::string connection_key = omnibinder::makeServiceConnectionKey(
        service_name, selected.host_id, selected.instance_id);
    ret = runtime.invoke(connection_key, iface_id, method_id, 0, request, response);
    
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

/* @brief 判断参数是否形如负数字面量（作为位置参数而非选项）
 * @param[in] arg 参数
 * @return 是负数形式返回 true
 */
static bool looksLikeNegativeNumber(const char* arg) {
    return arg && arg[0] == '-' && arg[1] >= '0' && arg[1] <= '9';
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 9900;
    const char* idl_file = NULL;
    const char* pid_arg = NULL;
    const char* level_arg = NULL;
    const char* diag_filter = NULL;
    const char* instance_id_arg = NULL;
    const char* host_id_arg = NULL;
    const char* positional[5] = {NULL, NULL, NULL, NULL, NULL};
    int pos_count = 0;
    
    // 解析命令行参数；未知选项显式报错，避免被静默吞成位置参数
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool needs_value = strcmp(arg, "-h") == 0 || strcmp(arg, "--host") == 0
            || strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0
            || strcmp(arg, "--idl") == 0 || strcmp(arg, "--pid") == 0
            || strcmp(arg, "--level") == 0 || strcmp(arg, "--filter") == 0
            || strcmp(arg, "--instance-id") == 0 || strcmp(arg, "--host-id") == 0;
        if (needs_value && i + 1 >= argc) {
            fprintf(stderr, "Error: option %s requires a value\n", arg);
            return 1;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--host") == 0) {
            host = argv[++i];
        } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            uint32_t parsed_port = 0;
            if (!parseUint32Arg(argv[++i], parsed_port) || parsed_port < 1 || parsed_port > 65535) {
                fprintf(stderr, "Error: invalid --port value: %s (expected 1..65535)\n", argv[i]);
                return 1;
            }
            port = static_cast<uint16_t>(parsed_port);
        } else if (strcmp(arg, "--idl") == 0) {
            idl_file = argv[++i];
        } else if (strcmp(arg, "--pid") == 0) {
            pid_arg = argv[++i];
        } else if (strcmp(arg, "--level") == 0) {
            level_arg = argv[++i];
        } else if (strcmp(arg, "--filter") == 0) {
            diag_filter = argv[++i];
        } else if (strcmp(arg, "--instance-id") == 0) {
            instance_id_arg = argv[++i];
        } else if (strcmp(arg, "--host-id") == 0) {
            host_id_arg = argv[++i];
        } else if (strcmp(arg, "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (arg[0] == '-' && arg[1] != '\0' && !looksLikeNegativeNumber(arg)) {
            fprintf(stderr, "Error: Unknown option: %s\n", arg);
            return 1;
        } else if (pos_count < 5) {
            positional[pos_count++] = arg;
        } else {
            fprintf(stderr, "Error: too many arguments\n");
            return 1;
        }
    }
    
    if (pos_count == 0) {
        printUsage(argv[0]);
        return 1;
    }

    // 分发前先校验命令/子命令/必需参数，避免未知命令或选项被静默吞掉后再去连 ServiceManager
    const char* command = positional[0];
    uint32_t watch_pid = 0;
    if (strcmp(command, "list") == 0 || strcmp(command, "ps") == 0) {
        if (pos_count > 1) {
            fprintf(stderr, "Error: '%s' takes no arguments\n", command);
            return 1;
        }
    } else if (strcmp(command, "info") == 0) {
        if (!positional[1]) {
            fprintf(stderr, "Error: 'info' requires a service name\n");
            return 1;
        }
    } else if (strcmp(command, "call") == 0) {
        if (!positional[1] || !positional[2]) {
            fprintf(stderr, "Error: 'call' requires <service> <method> [params]\n");
            return 1;
        }
    } else if (strcmp(command, "watch") == 0) {
        if (!pid_arg) {
            fprintf(stderr, "Error: 'watch' requires --pid <pid>\n");
            return 1;
        }
        if (!idl_file) {
            fprintf(stderr, "Error: 'watch' requires --idl <file.bidl>\n");
            return 1;
        }
        if (!parseUint32Arg(pid_arg, watch_pid) || watch_pid == 0) {
            fprintf(stderr, "Error: invalid --pid value: %s\n", pid_arg);
            return 1;
        }
    } else if (strcmp(command, "log") == 0) {
        if (!positional[1] || strcmp(positional[1], "set") != 0 || !pid_arg || !level_arg) {
            fprintf(stderr, "Error: usage: log set --pid <pid> --level <F|E|W|I|D|V|O>\n");
            return 1;
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
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
    
    omni_cli::InstanceSelector instance_selector;
    if (host_id_arg) {
        instance_selector.host_id_set = true;
        instance_selector.host_id = host_id_arg;
    }
    if (instance_id_arg) {
        instance_selector.instance_id_set = true;
        instance_selector.instance_id = instance_id_arg;
    }

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
        result = cmdInfo(runtime, positional[1], instance_selector);
    } else if (strcmp(command, "call") == 0) {
        result = cmdCall(runtime, positional[1], positional[2], positional[3], instance_selector);
    } else if (strcmp(command, "watch") == 0) {
        result = omni_cli::runWatchMonitor(runtime, watch_pid, diag_filter, *g_parse_ctx, g_idl_package);
    } else if (strcmp(command, "log") == 0) {
        result = cmdLogSet(runtime, pid_arg, level_arg);
    }
    
    runtime.stop();
    if (g_parse_ctx) delete g_parse_ctx;
    return result;
}
