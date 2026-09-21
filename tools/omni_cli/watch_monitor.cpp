/**************************************************************************************************
 * @file        watch_monitor.cpp
 * @brief       omni-cli watch 监控实现
 *************************************************************************************************/
#include "watch_monitor.h"
#include "simple_json.h"
#include "type_codec.h"
#include "type_resolver.h"
#include "parser.h"
#include "codegen.h"
#include <omnibinder/omnibinder.h>
#include <omnibinder/message.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <csignal>
#include <map>
#include <vector>
#include <deque>
#include <string>

namespace omni_cli {

namespace {

void appendFormat(char* buf, size_t size, int& offset, const char* fmt, ...) {
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

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0])
        | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

bool isInvokeRequestType(uint16_t type) {
    return type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE)
        || type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE_ONEWAY);
}

std::string typeRefCliName(const omnic::TypeRef& type) {
    if (type.primitive == omnic::TYPE_ARRAY) {
        if (type.element_type) {
            return "std::vector<" + typeRefCliName(*type.element_type) + ">";
        }
        return "std::vector<void>";
    }
    if (type.primitive == omnic::TYPE_CUSTOM) {
        if (!type.package_name.empty()) {
            return type.package_name + "::" + type.custom_name;
        }
        return type.custom_name;
    }
    if (type.primitive == omnic::TYPE_VOID) {
        return "void";
    }
    return omni_cli::primitiveTypeName(type.primitive);
}

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
    WatchState() : pid(0), filter(NULL), ctx(NULL), package(NULL) {}
    uint32_t pid;
    const char* filter;
    omnic::ParseContext* ctx;
    const std::string* package;
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

bool parseDiagEventHeader(const omnibinder::Buffer& data, DiagEventHeader& hdr) {
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

std::string watchedSourceName(uint32_t pid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "pid%u", pid);
    return std::string(buf);
}

const char* diagDirectionLabel(uint8_t direction) {
    switch (direction) {
    case omnibinder::DIAG_EVENT_REQUEST:   return "REQUEST  ";
    case omnibinder::DIAG_EVENT_RESPONSE:  return "RESPONSE ";
    case omnibinder::DIAG_EVENT_ONE_WAY:   return "ONE_WAY  ";
    case omnibinder::DIAG_EVENT_SUBSCRIBE: return "SUBSCRIBE";
    case omnibinder::DIAG_EVENT_BROADCAST: return "BROADCAST";
    default: return "?";
    }
}

// 未应答请求登记上限：长 watch 下 RPC 超时 / diag 丢帧会让未配对条目累积；
// seq 单调递增，超限时淘汰最旧（begin）以界定内存
const size_t MAX_PENDING_REQUESTS = 4096u;

/* @brief 记录 REQUEST 时间戳并匹配 RESPONSE 耗时
 * @param[in,out] state watch 状态
 * @param[in] ev 诊断事件头
 * @return 本次事件耗时（us，0 表示无配对）
 */
uint64_t trackWatchLatency(WatchState& state, const DiagEventHeader& ev) {
    if (ev.direction == omnibinder::DIAG_EVENT_REQUEST) {
        state.req_timestamps[ev.orig_seq] = ev.ts_us;
        if (state.req_timestamps.size() > MAX_PENDING_REQUESTS) {
            state.req_timestamps.erase(state.req_timestamps.begin());
        }
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
WatchEventInfo resolveWatchEvent(WatchState& state, const DiagEventHeader& ev,
                                 const omnibinder::Buffer& data) {
    WatchEventInfo info;
    const size_t diag_hdr = omnibinder::DIAG_EVENT_WIRE_HEADER_SIZE;
    if (state.ctx && ev.orig_len > 0 && data.size() >= diag_hdr + ev.orig_len) {
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
                // 仅普通 INVOKE 期待回复；ONEWAY 无回复，登记会造成 pending_methods 永久泄漏
                if (ev.orig_type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE)) {
                    state.pending_methods[ev.orig_seq] = mit->second;
                    if (state.pending_methods.size() > MAX_PENDING_REQUESTS) {
                        state.pending_methods.erase(state.pending_methods.begin());
                    }
                }
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
 * @param[in] ctx IDL 解析上下文
 * @param[in,out] buf 数据 Buffer
 * @param[in] type 目标类型
 * @param[in] package 类型所属包
 * @param[in] type_name 类型名
 * @param[out] out 解码输出
 * @return 成功返回 true
 * @note 结构体输出 JSON，标量输出 "type: value"
 */
bool decodeWatchValue(const omnic::ParseContext& ctx, omnibinder::Buffer& buf,
                      const omnic::TypeRef& type, const std::string& package,
                      const std::string& type_name, std::string& out) {
    type_codec::TypeCodec codec(ctx);
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
void logWatchHex(const char* line, const uint8_t* payload, uint32_t len,
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
 * @param[in] state watch 状态
 * @param[in] info 事件语义信息
 * @param[in] payload 事件 payload
 * @param[in] source_name 来源名称
 * @param[in] line 日志行前缀
 */
void logWatchRequestDetail(WatchState& state, const WatchEventInfo& info, const uint8_t* payload,
                           const std::string& source_name, const char* line) {
    char detail[512];
    int off = snprintf(detail, sizeof(detail), "  %s.%s(", source_name.c_str(),
                       info.method->name.c_str());
    if (!info.method->param_types.empty() && info.param_len > 0) {
        omnic::TypeRef param_type;
        if (omni_cli::findTypeRef(state.ctx, info.method->param_types, *state.package, param_type)) {
            omnibinder::Buffer param_buf;
            param_buf.assign(payload + 16, info.param_len);
            std::string text;
            if (decodeWatchValue(*state.ctx, param_buf, param_type, *state.package,
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
 * @param[in] state watch 状态
 * @param[in] ev 诊断事件头
 * @param[in] info 事件语义信息
 * @param[in] payload 事件 payload
 * @param[in] line 日志行前缀
 */
void logWatchReplyDetail(WatchState& state, const DiagEventHeader& ev, const WatchEventInfo& info,
                         const uint8_t* payload, const char* line) {
    char detail[512];
    int off = snprintf(detail, sizeof(detail), "  -> status=%d", info.resp_status);
    if (info.resp_len > 0 && info.resp_len <= ev.orig_len - 8 && info.method &&
        !info.method->return_type.empty() && info.method->return_type != "void") {
        omnic::TypeRef ret_type;
        if (omni_cli::findTypeRef(state.ctx, info.method->return_type, *state.package, ret_type)) {
            omnibinder::Buffer resp_buf;
            resp_buf.assign(payload + 8, info.resp_len);
            std::string text;
            if (decodeWatchValue(*state.ctx, resp_buf, ret_type, *state.package,
                                 info.method->return_type, text)) {
                appendFormat(detail, sizeof(detail), off, " %s", text.c_str());
            }
        }
    }
    OMNI_LOG_INFO("Watch", "%s  \n%s", line, detail);
}

/* @brief 作用域内临时向 AST 追加伪 struct，析构时保证弹出
 * @note 保证解码异常时全局 AST 不被永久污染（约束：RAII 恢复）
 */
struct ScopedStructPop {
    explicit ScopedStructPop(std::vector<omnic::StructDef>& structs) : structs_(&structs) {}
    ~ScopedStructPop() {
        if (structs_ && !structs_->empty()) {
            structs_->pop_back();
        }
    }
    std::vector<omnic::StructDef>* structs_;
};

/* @brief 输出 BROADCAST 事件的 topic 解码明细
 * @param[in] state watch 状态
 * @param[in] ev 诊断事件头
 * @param[in] payload 事件 payload
 * @param[in] line 日志行前缀
 * @note 按 wire topic_id 选定话题包，无法解码时回退 hex
 */
void logWatchBroadcastDetail(WatchState& state, const DiagEventHeader& ev,
                             const uint8_t* payload, const char* line) {
    const uint32_t topic_id = readLe32(payload);
    uint32_t data_len = readLe32(payload + 4);
    if (data_len == 0 || data_len > ev.orig_len - 8) {
        return;
    }
    omnibinder::Buffer data_buf;
    data_buf.assign(payload + 8, data_len);
    for (std::map<std::string, omnic::AstFile>::iterator pit =
             state.ctx->loaded_packages.begin();
         pit != state.ctx->loaded_packages.end(); ++pit) {
        omnic::AstFile& ast = pit->second;
        for (size_t k = 0; k < ast.topics.size(); ++k) {
            const omnic::TopicDef& tdef = ast.topics[k];
            if (omnic::fnv1a_hash(tdef.name) != topic_id) {
                continue;
            }
            omnic::StructDef synth;
            synth.name = tdef.name;
            synth.fields = tdef.fields;
            ast.structs.push_back(synth);
            ScopedStructPop guard(ast.structs);
            omnic::TypeRef msg_type;
            msg_type.primitive = omnic::TYPE_CUSTOM;
            msg_type.custom_name = tdef.name;
            std::string text;
            bool decoded = decodeWatchValue(*state.ctx, data_buf, msg_type, pit->first,
                                            tdef.name, text);
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
void handleWatchEvent(WatchState& state, const omnibinder::Buffer& data) {
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
    if (state.ctx && has_payload) {
        const uint8_t* payload = data.data() + diag_hdr;
        if (isInvokeRequestType(ev.orig_type) && ev.orig_len >= 16 && info.method) {
            logWatchRequestDetail(state, info, payload, source_name, line);
        } else if (ev.orig_type == static_cast<uint16_t>(omnibinder::MessageType::MSG_INVOKE_REPLY)
                   && ev.direction == omnibinder::DIAG_EVENT_RESPONSE && ev.orig_len >= 8) {
            logWatchReplyDetail(state, ev, info, payload, line);
        } else if (ev.orig_type == static_cast<uint16_t>(omnibinder::MessageType::MSG_BROADCAST)
                   && ev.orig_len >= 8) {
            logWatchBroadcastDetail(state, ev, payload, line);
        } else {
            logWatchHex(line, payload, ev.orig_len, 64, false);
        }
    }
}

void buildIdlMethodMap(WatchState& state) {
    state.method_map.clear();
    state.method_storage.clear();
    if (!state.ctx) {
        return;
    }
    for (std::map<std::string, omnic::AstFile>::const_iterator pit =
             state.ctx->loaded_packages.begin();
         pit != state.ctx->loaded_packages.end(); ++pit) {
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
                state.method_storage.push_back(info);
                uint64_t key = (static_cast<uint64_t>(iface_id) << 32) | method_id;
                state.method_map[key] = &state.method_storage.back();
            }
        }
    }
}

volatile sig_atomic_t g_watch_running = 1;
void watch_sigint_handler(int) { g_watch_running = 0; }

} // namespace

int runWatchMonitor(omnibinder::OmniRuntime& runtime, uint32_t pid, const char* filter,
                    omnic::ParseContext& parse_ctx, const std::string& package) {
    WatchState state;
    state.pid = pid;
    state.filter = filter;
    state.ctx = &parse_ctx;
    state.package = &package;
    buildIdlMethodMap(state);

    int ret = runtime.watchPid(pid, [&state](const omnibinder::Buffer& data) {
        handleWatchEvent(state, data);
    });
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to start watch for pid=%u (status=%d)\n", pid, ret);
        return 1;
    }
    printf("Watching pid=%u. Press Ctrl+C to stop.\n", pid);

    g_watch_running = 1;
    signal(SIGINT, watch_sigint_handler);
    signal(SIGTERM, watch_sigint_handler);
    while (g_watch_running) {
        runtime.pollOnce(100);
    }
    runtime.unwatchPid(pid);
    printf("\nDiagnostic watch stopped.\n");
    return 0;
}

} // namespace omni_cli
