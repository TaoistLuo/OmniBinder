// test_performance.cpp - 性能基准测试
//
// 测试项目:
// 1. RPC 调用往返延迟 (TCP / SHM)
// 2. 话题发布到接收延迟 (pub/sub latency)
//
// 输出: 控制台统计报告 + docs/performance-report.md

#include <omnibinder/omnibinder.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>
#include <chrono>

using namespace omnibinder;

// ============================================================
// 高精度计时器 (微秒级)
// ============================================================
static inline int64_t nowUs() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        tp.time_since_epoch()).count();
}

// ============================================================
// 统计工具
// ============================================================
struct LatencyStats {
    std::string name;
    std::vector<double> samples_us;  // 每次延迟 (微秒)

    void add(double us) { samples_us.push_back(us); }

    size_t count() const { return samples_us.size(); }

    double min() const {
        if (samples_us.empty()) return 0;
        return *std::min_element(samples_us.begin(), samples_us.end());
    }

    double max() const {
        if (samples_us.empty()) return 0;
        return *std::max_element(samples_us.begin(), samples_us.end());
    }

    double avg() const {
        if (samples_us.empty()) return 0;
        double sum = std::accumulate(samples_us.begin(), samples_us.end(), 0.0);
        return sum / samples_us.size();
    }

    double median() const {
        if (samples_us.empty()) return 0;
        std::vector<double> sorted = samples_us;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();
        if (n % 2 == 0) return (sorted[n/2 - 1] + sorted[n/2]) / 2.0;
        return sorted[n/2];
    }

    double percentile(double p) const {
        if (samples_us.empty()) return 0;
        std::vector<double> sorted = samples_us;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(p / 100.0 * (sorted.size() - 1));
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    double stddev() const {
        if (samples_us.size() < 2) return 0;
        double m = avg();
        double sum = 0;
        for (size_t i = 0; i < samples_us.size(); i++) {
            double d = samples_us[i] - m;
            sum += d * d;
        }
        return std::sqrt(sum / (samples_us.size() - 1));
    }

    void printReport() const {
        printf("  %-40s\n", name.c_str());
        printf("    Samples:  %zu\n", count());
        printf("    Min:      %.1f us\n", min());
        printf("    Max:      %.1f us\n", max());
        printf("    Avg:      %.1f us\n", avg());
        printf("    Median:   %.1f us\n", median());
        printf("    P95:      %.1f us\n", percentile(95));
        printf("    P99:      %.1f us\n", percentile(99));
        printf("    StdDev:   %.1f us\n", stddev());
        printf("\n");
    }

    std::string toMarkdownTable() const {
        std::ostringstream oss;
        oss << "| " << name << " | "
            << count() << " | "
            << std::fixed;
        oss.precision(1);
        oss << min() << " | "
            << max() << " | "
            << avg() << " | "
            << median() << " | "
            << percentile(95) << " | "
            << percentile(99) << " | "
            << stddev() << " |";
        return oss.str();
    }
};

// ============================================================
// 常量
// ============================================================
static const uint32_t METHOD_ECHO = fnv1a_32("Echo");
static const uint32_t METHOD_ADD  = fnv1a_32("Add");
static const uint32_t IFACE_ID   = fnv1a_32("PerfService");
static const uint16_t SM_PORT    = 19910;

static const int WARMUP_ROUNDS   = 50;
static const int RPC_ROUNDS      = 1000;
static const int TOPIC_WARMUP_ROUNDS = 10;
static const int TOPIC_ROUNDS    = 50;
static const bool RUN_TOPIC_BENCH = true;

// ============================================================
// 测试服务: PerfService
// ============================================================
class PerfService : public Service {
public:
    PerfService() : Service("PerfService") {
        setShmConfig(ShmConfig(16 * 1024, 16 * 1024));
        iface_.interface_id = IFACE_ID;
        iface_.name = "PerfService";
        iface_.methods.push_back(MethodInfo(METHOD_ECHO, "Echo"));
        iface_.methods.push_back(MethodInfo(METHOD_ADD, "Add"));
    }
    const char* serviceName() const override { return "PerfService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    void onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == METHOD_ECHO) {
            // 原样返回
            if (request.size() > 0) {
                response.writeRaw(request.data(), request.size());
            }
        } else if (method_id == METHOD_ADD) {
            Buffer req(request.data(), request.size());
            int32_t a = req.readInt32();
            int32_t b = req.readInt32();
            response.writeInt32(a + b);
        }
    }
private:
    InterfaceInfo iface_;
};

class PerfTopicService : public Service {
public:
    PerfTopicService() : Service("PerfTopicService") {
        setShmConfig(ShmConfig(16 * 1024, 16 * 1024));
        iface_.interface_id = fnv1a_32("PerfTopicService");
        iface_.name = "PerfTopicService";
    }
    const char* serviceName() const override { return "PerfTopicService"; }
    const InterfaceInfo& interfaceInfo() const override { return iface_; }
protected:
    void onInvoke(uint32_t, const Buffer&, Buffer&) override {}
private:
    InterfaceInfo iface_;
};

// ============================================================
// 服务端线程
// ============================================================
struct ServerContext {
    OmniRuntime runtime;
    PerfService service;
    std::atomic<bool> registered;
    std::atomic<bool> should_stop;
    uint16_t sm_port;
    std::atomic<bool> topic_broadcast_pending;
    std::atomic<bool> topic_publish_requested;
    std::vector<uint8_t> topic_payload;
    std::atomic<int64_t> topic_send_time_us;
    std::atomic<bool> topic_ready;
    ServerContext()
        : registered(false)
        , should_stop(false)
        , sm_port(0)
        , topic_broadcast_pending(false)
        , topic_publish_requested(false)
        , topic_payload()
        , topic_send_time_us(0)
        , topic_ready(false) {}
};

static void* serverThread(void* arg) {
    ServerContext* ctx = static_cast<ServerContext*>(arg);
    int ret = ctx->runtime.init("127.0.0.1", ctx->sm_port);
    if (ret != 0) {
        fprintf(stderr, "Server: init failed (%d)\n", ret);
        return NULL;
    }
    ret = ctx->runtime.registerService(&ctx->service);
    if (ret != 0) {
        fprintf(stderr, "Server: register failed (%d)\n", ret);
        ctx->runtime.stop();
        return NULL;
    }
    ctx->registered = true;

    uint32_t perf_topic_id = fnv1a_32("PerfTopic");

    while (!ctx->should_stop) {
        if (ctx->topic_publish_requested.exchange(false) && !ctx->topic_ready) {
            ret = ctx->runtime.publishTopic("PerfTopic");
            if (ret != 0) {
                fprintf(stderr, "Server: publishTopic failed (%d)\n", ret);
            } else {
                ctx->topic_ready = true;
            }
        }
        if (ctx->topic_broadcast_pending.exchange(false)) {
            Buffer buf;
            if (!ctx->topic_payload.empty()) {
                buf.writeRaw(ctx->topic_payload.data(), ctx->topic_payload.size());
            }
            ctx->topic_send_time_us.store(nowUs());
            ctx->runtime.broadcast(perf_topic_id, buf);
        }
        // 驱动服务端 EventLoop，使控制面、TCP、eventfd 等事件得到处理。
        // 当前 SHM 已是 eventfd 事件驱动，这里的 1ms 只是 benchmark 线程
        // 主动让出 CPU 并及时处理待办事件，而不是旧轮询模型中的“等待 SHM 请求”。
        ctx->runtime.pollOnce(1);
    }
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

// ============================================================
// SM 进程管理
// ============================================================
static pid_t startSM(uint16_t port) {
    char kill_cmd[128];
    snprintf(kill_cmd, sizeof(kill_cmd), "pkill -f 'service_manager --port %u' >/dev/null 2>&1 || true", port);
    int kill_rc = system(kill_cmd);
    (void)kill_rc;
    usleep(100000);

    // 通过 /proc/self/exe 推断 SM 可执行文件的绝对路径
    char self_path[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    std::string sm_path;
    if (len > 0) {
        self_path[len] = '\0';
        std::string exe_dir(self_path);
        size_t pos = exe_dir.rfind('/');
        if (pos != std::string::npos) {
            exe_dir = exe_dir.substr(0, pos);
            // 从 tests/ 目录回退到 build 根目录
            pos = exe_dir.rfind('/');
            if (pos != std::string::npos) {
                sm_path = exe_dir.substr(0, pos) + "/service_manager/service_manager";
            }
        }
    }

    // 检查文件是否存在
    if (sm_path.empty() || access(sm_path.c_str(), X_OK) != 0) {
        // 回退到相对路径
        const char* fallbacks[] = {
            "./target/bin/service_manager",
            "./build/target/bin/service_manager",
            "./service_manager/service_manager",
            "../service_manager/service_manager",
            NULL
        };
        sm_path.clear();
        for (int i = 0; fallbacks[i]; i++) {
            if (access(fallbacks[i], X_OK) == 0) {
                sm_path = fallbacks[i];
                break;
            }
        }
    }

    if (sm_path.empty()) {
        fprintf(stderr, "FATAL: Could not find service_manager binary\n");
        return -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    pid_t pid = fork();
    if (pid == 0) {
        execl(sm_path.c_str(), "service_manager", "--port", port_str,
              "--log-level", "3", (char*)NULL);
        fprintf(stderr, "FATAL: execl(%s) failed: %s\n", sm_path.c_str(), strerror(errno));
        _exit(1);
    }

    if (pid < 0) {
        fprintf(stderr, "FATAL: fork() failed\n");
        return -1;
    }

    // 检查子进程是否立即退出（exec 失败）
    usleep(50000);  // 50ms
    int status;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
        fprintf(stderr, "FATAL: SM process exited immediately (status=%d)\n", status);
        return -1;
    }

    return pid;
}

static void stopSM(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int s;
        for (int i = 0; i < 20; ++i) {
            pid_t ret = waitpid(pid, &s, WNOHANG);
            if (ret == pid) {
                return;
            }
            usleep(100000);
        }
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &s, 0);
        }
    }
}

static bool waitSM(uint16_t port, int retries) {
    for (int i = 0; i < retries; i++) {
        OmniRuntime probe;
        if (probe.init("127.0.0.1", port) == 0) {
            probe.stop();
            return true;
        }
        usleep(100000);
    }
    return false;
}

// ============================================================
// 性能测试: RPC 往返延迟
// ============================================================
static LatencyStats benchRpcEcho(OmniRuntime& runtime, int payload_size,
                                   int warmup, int rounds) {
    LatencyStats stats;
    std::ostringstream oss;
    oss << "RPC Echo (payload=" << payload_size << " bytes)";
    stats.name = oss.str();

    // 构造固定 payload
    std::vector<uint8_t> payload_data(payload_size, 0xAB);

    // 预热
    for (int i = 0; i < warmup; i++) {
        Buffer req;
        if (payload_size > 0) req.writeRaw(payload_data.data(), payload_size);
        Buffer resp;
        runtime.invoke("PerfService", IFACE_ID, METHOD_ECHO, req, resp, 5000);
    }

    // 正式测试
    for (int i = 0; i < rounds; i++) {
        Buffer req;
        if (payload_size > 0) req.writeRaw(payload_data.data(), payload_size);
        Buffer resp;

        int64_t t0 = nowUs();
        int ret = runtime.invoke("PerfService", IFACE_ID, METHOD_ECHO, req, resp, 5000);
        int64_t t1 = nowUs();

        if (ret == 0) {
            stats.add(static_cast<double>(t1 - t0));
        }
    }

    return stats;
}

static LatencyStats benchRpcAdd(OmniRuntime& runtime, int warmup, int rounds) {
    LatencyStats stats;
    stats.name = "RPC Add (2 x int32)";

    // 预热
    for (int i = 0; i < warmup; i++) {
        Buffer req;
        req.writeInt32(i);
        req.writeInt32(i + 1);
        Buffer resp;
        runtime.invoke("PerfService", IFACE_ID, METHOD_ADD, req, resp, 5000);
    }

    // 正式测试
    for (int i = 0; i < rounds; i++) {
        Buffer req;
        req.writeInt32(i);
        req.writeInt32(i + 1);
        Buffer resp;

        int64_t t0 = nowUs();
        int ret = runtime.invoke("PerfService", IFACE_ID, METHOD_ADD, req, resp, 5000);
        int64_t t1 = nowUs();

        if (ret == 0) {
            stats.add(static_cast<double>(t1 - t0));
        }
    }

    return stats;
}

// ============================================================
// 性能测试: 话题发布到接收延迟
// ============================================================
struct TopicBenchContext {
    std::atomic<bool> received;
    int64_t recv_time_us;
    TopicBenchContext() : received(false), recv_time_us(0) {}
};

struct TopicSubscriberContext {
    OmniRuntime runtime;
    std::atomic<bool> ready;
    std::atomic<bool> should_stop;
    TopicBenchContext* bench;

    TopicSubscriberContext()
        : ready(false)
        , should_stop(false)
        , bench(NULL) {}
};

struct TopicPublisherContext {
    OmniRuntime runtime;
    PerfTopicService service;
    std::atomic<bool> ready;
    std::atomic<bool> should_stop;
    std::atomic<bool> broadcast_pending;
    std::vector<uint8_t> payload;
    std::atomic<int64_t> send_time_us;

    TopicPublisherContext()
        : ready(false)
        , should_stop(false)
        , broadcast_pending(false)
        , payload()
        , send_time_us(0) {}
};

static void* topicPublisherThread(void* arg) {
    TopicPublisherContext* ctx = static_cast<TopicPublisherContext*>(arg);
    if (ctx->runtime.init("127.0.0.1", SM_PORT) != 0) {
        return NULL;
    }
    if (ctx->runtime.registerService(&ctx->service) != 0) {
        ctx->runtime.stop();
        return NULL;
    }
    if (ctx->runtime.publishTopic("PerfTopic") != 0) {
        ctx->runtime.unregisterService(&ctx->service);
        ctx->runtime.stop();
        return NULL;
    }
    ctx->ready = true;

    uint32_t topic_id = fnv1a_32("PerfTopic");
    while (!ctx->should_stop) {
        if (ctx->broadcast_pending.exchange(false)) {
            Buffer buf;
            if (!ctx->payload.empty()) {
                buf.writeRaw(ctx->payload.data(), ctx->payload.size());
            }
            ctx->send_time_us.store(nowUs());
            ctx->runtime.broadcast(topic_id, buf);
        }
        ctx->runtime.pollOnce(1);
    }

    ctx->ready = false;
    ctx->runtime.unregisterService(&ctx->service);
    ctx->runtime.stop();
    return NULL;
}

static void* topicSubscriberThread(void* arg) {
    TopicSubscriberContext* ctx = static_cast<TopicSubscriberContext*>(arg);
    if (ctx->runtime.init("127.0.0.1", SM_PORT) != 0) {
        return NULL;
    }

    int ret = ctx->runtime.subscribeTopic("PerfTopic",
        [ctx](uint32_t tid, const Buffer& data) {
            (void)tid;
            (void)data;
            if (ctx->bench) {
                ctx->bench->recv_time_us = nowUs();
                ctx->bench->received.store(true);
            }
        });
    if (ret != 0) {
        ctx->runtime.stop();
        return NULL;
    }

    ctx->ready = true;
    while (!ctx->should_stop) {
        ctx->runtime.pollOnce(1);
    }

    ctx->ready = false;
    ctx->runtime.unsubscribeTopic("PerfTopic");
    ctx->runtime.stop();
    return NULL;
}

static LatencyStats benchTopicLatency(int payload_size, int warmup, int rounds) {
    LatencyStats stats;
    std::ostringstream oss;
    oss << "Topic pub/sub (payload=" << payload_size << " bytes)";
    stats.name = oss.str();
    TopicPublisherContext pub_ctx;
    pub_ctx.payload.assign(static_cast<size_t>(payload_size), 0xCD);
    pthread_t pub_tid;
    if (pthread_create(&pub_tid, NULL, topicPublisherThread, &pub_ctx) != 0) {
        fprintf(stderr, "Topic publisher thread create failed\n");
        return stats;
    }
    for (int i = 0; i < 50 && !pub_ctx.ready; ++i) usleep(100000);
    if (!pub_ctx.ready) {
        fprintf(stderr, "Topic publisher thread not ready\n");
        pub_ctx.should_stop = true;
        pthread_join(pub_tid, NULL);
        return stats;
    }

    TopicBenchContext ctx;
    TopicSubscriberContext sub_ctx;
    sub_ctx.bench = &ctx;
    pthread_t sub_tid;
    if (pthread_create(&sub_tid, NULL, topicSubscriberThread, &sub_ctx) != 0) {
        fprintf(stderr, "Topic subscriber thread create failed\n");
        pub_ctx.should_stop = true;
        pthread_join(pub_tid, NULL);
        return stats;
    }
    for (int i = 0; i < 50 && !sub_ctx.ready; ++i) usleep(100000);
    if (!sub_ctx.ready) {
        fprintf(stderr, "Topic subscriber thread not ready\n");
        sub_ctx.should_stop = true;
        pthread_join(sub_tid, NULL);
        pub_ctx.should_stop = true;
        pthread_join(pub_tid, NULL);
        return stats;
    }
    for (int i = 0; i < 30; i++) usleep(5000);

    bool subscriber_ready = false;
    for (int i = 0; i < 100 && !subscriber_ready; ++i) {
        ctx.received.store(false);
        pub_ctx.broadcast_pending.store(true);
        for (int j = 0; j < 200 && !ctx.received.load(); ++j) {
            usleep(500);
        }
        subscriber_ready = ctx.received.load();
    }
    if (!subscriber_ready) {
        fprintf(stderr, "Topic subscriber never received preflight broadcast\n");
        sub_ctx.should_stop = true;
        pthread_join(sub_tid, NULL);
        pub_ctx.should_stop = true;
        pthread_join(pub_tid, NULL);
        return stats;
    }
    for (int i = 0; i < warmup; i++) {
        ctx.received.store(false);
        pub_ctx.broadcast_pending.store(true);
        for (int j = 0; j < 200 && !ctx.received.load(); j++) {
            usleep(500);
        }
    }
    for (int i = 0; i < rounds; i++) {
        ctx.received.store(false);
        pub_ctx.broadcast_pending.store(true);
        for (int j = 0; j < 200 && !ctx.received.load(); j++) {
            usleep(500);
        }

        if (ctx.received.load()) {
            stats.add(static_cast<double>(ctx.recv_time_us - pub_ctx.send_time_us.load()));
        }
    }

    sub_ctx.should_stop = true;
    pthread_join(sub_tid, NULL);
    pub_ctx.should_stop = true;
    pthread_join(pub_tid, NULL);
    return stats;
}

// ============================================================
// 生成 Markdown 报告
// ============================================================
static void generateReport(const std::vector<LatencyStats>& all_stats,
                            const std::string& filepath) {
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
        fprintf(stderr, "Failed to open %s for writing\n", filepath.c_str());
        return;
    }

    // 获取当前时间
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    ofs << "# OmniBinder 性能测试报告\n\n";
    ofs << "**生成时间:** " << time_str << "\n\n";
    ofs << "**测试环境:**\n";
    ofs << "- 传输方式: TCP + SHM（自动选择，同机通信使用 SHM）\n";
    ofs << "- RPC 预热轮数: " << WARMUP_ROUNDS << "\n";
    ofs << "- RPC 测试轮数: " << RPC_ROUNDS << "（每个用例）\n";
    ofs << "- 话题预热轮数: " << TOPIC_WARMUP_ROUNDS << "\n";
    ofs << "- 话题测试轮数: " << TOPIC_ROUNDS << "（每个用例）\n\n";

    ofs << "## 总览\n\n";
    ofs << "| 测试用例 | 样本数 | 最小值 (us) | 最大值 (us) | 平均值 (us) | 中位数 (us) | P95 (us) | P99 (us) | 标准差 (us) |\n";
    ofs << "|----------|--------|-------------|-------------|-------------|-------------|----------|----------|-------------|\n";

    for (size_t i = 0; i < all_stats.size(); i++) {
        ofs << all_stats[i].toMarkdownTable() << "\n";
    }

    ofs << "\n## 详细数据\n\n";

    // RPC 部分
    ofs << "### RPC 调用往返延迟\n\n";
    ofs << "测量从客户端发起 invoke 请求到收到响应的完整往返时间。\n";
    ofs << "服务端原样返回数据（Echo）或执行简单加法运算（Add）。\n\n";

    for (size_t i = 0; i < all_stats.size(); i++) {
        if (all_stats[i].name.find("RPC") != std::string::npos) {
            ofs << "**" << all_stats[i].name << "**\n\n";
            ofs << "- 样本数: " << all_stats[i].count() << "\n";
            ofs << "- 最小值: " << all_stats[i].min() << " us\n";
            ofs << "- 最大值: " << all_stats[i].max() << " us\n";
            ofs << "- 平均值: " << all_stats[i].avg() << " us\n";
            ofs << "- 中位数: " << all_stats[i].median() << " us\n";
            ofs << "- P95: " << all_stats[i].percentile(95) << " us\n";
            ofs << "- P99: " << all_stats[i].percentile(99) << " us\n\n";
        }
    }

    ofs << "### 话题发布/订阅延迟\n\n";
    bool has_topic_stats = false;
    for (size_t i = 0; i < all_stats.size(); i++) {
        if (all_stats[i].name.find("Topic") != std::string::npos && all_stats[i].count() > 0) {
            has_topic_stats = true;
            ofs << "**" << all_stats[i].name << "**\n\n";
            ofs << "- 样本数: " << all_stats[i].count() << "\n";
            ofs << "- 最小值: " << all_stats[i].min() << " us\n";
            ofs << "- 最大值: " << all_stats[i].max() << " us\n";
            ofs << "- 平均值: " << all_stats[i].avg() << " us\n";
            ofs << "- 中位数: " << all_stats[i].median() << " us\n";
            ofs << "- P95: " << all_stats[i].percentile(95) << " us\n";
            ofs << "- P99: " << all_stats[i].percentile(99) << " us\n\n";
        }
    }
    if (!has_topic_stats) {
        ofs << "本轮性能报告未纳入 topic latency 数值。当前 topic 功能路径已由 examples 与集成测试验证，\n";
        ofs << "但性能 harness 仍在进一步稳定中，因此本报告只保留已完成且可信的 RPC 基准结果。\n\n";
    }

    ofs << "---\n\n";

    ofs << "## 性能分析\n\n";
    ofs << "### SHM eventfd 事件驱动路径\n\n";
    ofs << "当前 SHM 传输已使用 eventfd + EventLoop 的事件驱动路径。\n";
    ofs << "客户端写入请求后会触发服务端 req_eventfd，服务端写入响应后会触发客户端 resp_eventfd。\n";
    ofs << "这意味着当前延迟数据主要反映：序列化、共享内存拷贝、eventfd 唤醒、epoll 调度与业务处理开销。\n\n";
    ofs << "### 默认 SHM 容量说明\n\n";
    ofs << "项目默认 SHM 配置为 `4KB / 4KB`，但本性能测试中的 `PerfService` / `PerfTopicService` 为了覆盖\n";
    ofs << "4096-byte 与 8192-byte payload，显式使用了更大的服务级 SHM 配置（`16KB / 16KB`）。\n";
    ofs << "因此本报告中的 4096-byte / 8192-byte 用例数据反映的是“显式放大 ring 后”的性能，而不是默认配置下的行为。\n\n";
    ofs << "### 结论\n\n";
    double rpc_min_median = 0.0;
    double rpc_max_median = 0.0;
    double rpc_large_median = 0.0;
    double topic_min_median = 0.0;
    double topic_max_median = 0.0;
    bool rpc_range_initialized = false;
    bool topic_range_initialized = false;

    for (size_t i = 0; i < all_stats.size(); ++i) {
        double med = all_stats[i].median();
        if (all_stats[i].name.find("RPC Echo (payload=4096") != std::string::npos) {
            rpc_large_median = med;
        }
        if (all_stats[i].name.find("RPC") != std::string::npos &&
            all_stats[i].name.find("4096") == std::string::npos) {
            if (!rpc_range_initialized) {
                rpc_min_median = rpc_max_median = med;
                rpc_range_initialized = true;
            } else {
                rpc_min_median = std::min(rpc_min_median, med);
                rpc_max_median = std::max(rpc_max_median, med);
            }
        }
        if (all_stats[i].name.find("Topic") != std::string::npos && all_stats[i].count() > 0) {
            if (!topic_range_initialized) {
                topic_min_median = topic_max_median = med;
                topic_range_initialized = true;
            } else {
                topic_min_median = std::min(topic_min_median, med);
                topic_max_median = std::max(topic_max_median, med);
            }
        }
    }

    if (rpc_range_initialized) {
        ofs << "- 0~1024 byte 常见 RPC 在当前机器上中位数约为 "
            << rpc_min_median << "~" << rpc_max_median << " us\n";
    }
    if (rpc_large_median > 0.0) {
        ofs << "- 4096 byte payload 在当前测试配置下中位数约为 "
            << rpc_large_median << " us\n";
    }
    if (topic_range_initialized) {
        ofs << "- topic pub/sub 在当前机器上中位数约为 "
            << topic_min_median << "~" << topic_max_median << " us\n";
    }
    ofs.close();

    printf("Report written to: %s\n", filepath.c_str());
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    // 禁用 stdout 缓冲，确保输出立即可见
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // 支持指定报告输出路径
    std::string report_path = "docs/performance-report.md";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report_path = argv[++i];
        }
    }

    // 关闭框架日志，避免干扰性能测量
    setLogLevel(LogLevel::LOG_ERROR);

    printf("=== OmniBinder Performance Benchmark ===\n\n");

    // 清理残留资源
    unlink("/dev/shm/binder_PerfService");

    // 启动 SM
    printf("Starting ServiceManager on port %u...\n", SM_PORT);
    pid_t sm_pid = startSM(SM_PORT);
    if (sm_pid <= 0) {
        fprintf(stderr, "FATAL: Failed to fork SM process\n");
        return 1;
    }
    if (!waitSM(SM_PORT, 30)) {
        fprintf(stderr, "FATAL: SM did not start in time\n");
        stopSM(sm_pid);
        return 1;
    }
    printf("ServiceManager ready (pid=%d)\n\n", sm_pid);

    // 启动服务端线程
    ServerContext srv;
    srv.sm_port = SM_PORT;
    pthread_t srv_tid;
    if (pthread_create(&srv_tid, NULL, serverThread, &srv) != 0) {
        fprintf(stderr, "FATAL: Failed to create server thread\n");
        stopSM(sm_pid);
        return 1;
    }
    for (int i = 0; i < 50 && !srv.registered; i++) usleep(100000);
    if (!srv.registered) {
        fprintf(stderr, "FATAL: PerfService failed to register\n");
        srv.should_stop = true;
        pthread_join(srv_tid, NULL);
        stopSM(sm_pid);
        return 1;
    }
    printf("PerfService registered on port %u\n\n", srv.service.port());

    // 等待话题发布完成
    usleep(200000);

    std::vector<LatencyStats> all_stats;

    // ============================================================
    // RPC 性能测试
    // ============================================================
    printf("--- RPC Round-Trip Latency ---\n\n");

    {
        OmniRuntime runtime;
        int init_ret = runtime.init("127.0.0.1", SM_PORT);
        if (init_ret != 0) {
            fprintf(stderr, "FATAL: RPC client init failed (%d)\n", init_ret);
            srv.should_stop = true;
            pthread_join(srv_tid, NULL);
            stopSM(sm_pid);
            return 1;
        }

        // 不同 payload 大小的 Echo 测试
        int payload_sizes[] = {0, 64, 256, 1024, 4096, 8192};
        for (int k = 0; k < 6; k++) {
            printf("  Testing Echo (payload=%d bytes)...\n", payload_sizes[k]);
            LatencyStats s = benchRpcEcho(runtime, payload_sizes[k],
                                          WARMUP_ROUNDS, RPC_ROUNDS);
            s.printReport();
            all_stats.push_back(s);
        }

        // Add 测试
        printf("  Testing Add...\n");
        LatencyStats s_add = benchRpcAdd(runtime, WARMUP_ROUNDS, RPC_ROUNDS);
        s_add.printReport();
        all_stats.push_back(s_add);

        runtime.stop();
    }

    // ============================================================
    // Topic 性能测试
    // ============================================================
    if (RUN_TOPIC_BENCH) {
        printf("--- Topic Pub/Sub Latency ---\n\n");
        int topic_payload_sizes[] = {64, 256, 1024, 8192};
        for (int k = 0; k < 4; k++) {
            printf("  Testing Topic (payload=%d bytes)...\n", topic_payload_sizes[k]);
            LatencyStats s = benchTopicLatency(topic_payload_sizes[k],
                                               TOPIC_WARMUP_ROUNDS, TOPIC_ROUNDS);
            s.printReport();
            all_stats.push_back(s);
        }
    } else {
        printf("--- Topic Pub/Sub Latency ---\n\n");
        printf("  Skipped in this run: harness under stabilization, functional coverage remains in integration/examples.\n\n");
    }

    // ============================================================
    // 生成报告
    // ============================================================
    printf("--- Generating Report ---\n\n");
    generateReport(all_stats, report_path);

    // ============================================================
    // 清理
    // ============================================================
    printf("\nCleaning up...\n");
    kill(sm_pid, SIGKILL);
    printf("\n=== Performance benchmark completed ===\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
