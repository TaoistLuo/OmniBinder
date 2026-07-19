#include "perf_service.bidl.h"
#include <omnibinder/omnibinder.h>
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#ifdef IGNORE
#undef IGNORE
#endif
#endif

using namespace omnibinder;

namespace {

typedef intptr_t TestPid;

TestPid startProcess(const char* binary, const char* a1 = nullptr,
                     const char* a2 = nullptr, const char* a3 = nullptr,
                     const char* a4 = nullptr) {
#ifdef _WIN32
    std::string cmd = "\"" + std::string(binary) + "\"";
    if (a1) { cmd += " "; cmd += a1; }
    if (a2) { cmd += " "; cmd += a2; }
    if (a3) { cmd += " "; cmd += a3; }
    if (a4) { cmd += " "; cmd += a4; }
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    return reinterpret_cast<TestPid>(pi.hProcess);
#else
    pid_t pid = fork();
    if (pid == 0) {
        if (a1 && a2 && a3 && a4)
            execl(binary, binary, a1, a2, a3, a4, (char*)NULL);
        else if (a1 && a2 && a3)
            execl(binary, binary, a1, a2, a3, (char*)NULL);
        else if (a1 && a2)
            execl(binary, binary, a1, a2, (char*)NULL);
        else if (a1)
            execl(binary, binary, a1, (char*)NULL);
        else
            execl(binary, binary, (char*)NULL);
        _exit(1);
    }
    return static_cast<TestPid>(pid);
#endif
}

void stopProcess(TestPid pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    TerminateProcess(reinterpret_cast<HANDLE>(pid), 0);
    WaitForSingleObject(reinterpret_cast<HANDLE>(pid), 5000);
    CloseHandle(reinterpret_cast<HANDLE>(pid));
#else
    kill(static_cast<pid_t>(pid), SIGTERM);
    int status = 0;
    for (int i = 0; i < 50; ++i) {
        if (waitpid(static_cast<pid_t>(pid), &status, WNOHANG) > 0) return;
        platform::sleepMs(100);
    }
    kill(static_cast<pid_t>(pid), SIGKILL);
    waitpid(static_cast<pid_t>(pid), &status, 0);
#endif
}

bool waitPortReady(uint16_t port, int timeout_sec) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        platform::SocketFd fd = platform::createTcpSocket();
        if (fd != platform::INVALID_SOCKET_FD) {
            int result = platform::connectSocket(fd, "127.0.0.1", port);
            platform::closeSocket(fd);
            if (result >= 0) return true;
        }
        platform::sleepMs(100);
    }
    return false;
}

const char* findBinary(const char* name) {
    const char* paths[] = {
        "target/test/", "./build/target/test/",
        "target/bin/", "./build/target/bin/",
        nullptr};
    static char full[512];
    for (int i = 0; paths[i]; ++i) {
        snprintf(full, sizeof(full), "%s%s", paths[i], name);
#ifdef _WIN32
        if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) return full;
        snprintf(full, sizeof(full), "%s%s.exe", paths[i], name);
        if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) return full;
#else
        if (access(full, X_OK) == 0) return full;
#endif
    }
    return nullptr;
}

} // anonymous namespace

static inline int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct LatencyStats {
    std::string name;
    std::vector<double> samples_us;
    mutable std::vector<double> sorted;  // 惰性排序，所有统计复用

    void add(double us) { samples_us.push_back(us); sorted.clear(); }
    size_t count() const { return samples_us.size(); }

    void ensureSorted() const {
        if (!sorted.empty()) return;
        sorted = samples_us;
        std::sort(sorted.begin(), sorted.end());
    }

    double min() const { ensureSorted(); return sorted.empty() ? 0 : sorted.front(); }
    double max() const { ensureSorted(); return sorted.empty() ? 0 : sorted.back(); }
    double median() const {
        ensureSorted();
        if (sorted.empty()) return 0;
        return sorted.size() % 2 ? sorted[sorted.size()/2]
                                  : (sorted[sorted.size()/2-1] + sorted[sorted.size()/2]) / 2.0;
    }
    double p95() const { ensureSorted(); return sorted.empty() ? 0 : sorted[sorted.size()*95/100]; }
    double p99() const { ensureSorted(); return sorted.empty() ? 0 : sorted[sorted.size()*99/100]; }
    double stddev() const {
        if (samples_us.size() < 2) return 0;
        double m = avg(), sum = 0;
        for (auto v : samples_us) { double d = v - m; sum += d*d; }
        return std::sqrt(sum / (samples_us.size()-1));
    }
    double avg() const {
        return samples_us.empty() ? 0 : std::accumulate(samples_us.begin(), samples_us.end(), 0.0) / samples_us.size();
    }
    // 去头尾各 1% 后的均值，消除 WSL2 调度抖动对平均值的污染
    double trimmedMean() const {
        ensureSorted();
        if (sorted.empty()) return 0;
        size_t trim = sorted.size() / 100;
        if (trim * 2 >= sorted.size()) return median();
        double sum = 0;
        for (size_t i = trim; i < sorted.size() - trim; ++i) sum += sorted[i];
        return sum / (sorted.size() - trim * 2);
    }

    void print() const {
        printf("  %-40s\n", name.c_str());
        printf("    samples:  %zu\n", count());
        printf("    min:      %.1f us\n", min());
        printf("    max:      %.1f us\n", max());
        printf("    avg:      %.1f us\n", avg());
        printf("    trim-avg: %.1f us\n", trimmedMean());
        printf("    median:   %.1f us\n", median());
        printf("    p95:      %.1f us\n", p95());
        printf("    p99:      %.1f us\n", p99());
        printf("    stddev:   %.1f us\n\n", stddev());
    }

    std::string toRow() const {
        char buf[256];
        snprintf(buf, sizeof(buf), "| %s | %zu | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |",
            name.c_str(), count(), min(), max(), avg(), trimmedMean(), median(), p95(), p99(), stddev());
        return buf;
    }
};

static bool waitForService(const char* name, uint16_t sm_port, int timeout_sec) {
    omnibinder::OmniRuntime rt;
    if (rt.init("127.0.0.1", sm_port) != 0) return false;
    omnibinder::ServiceInfo info;
    for (int i = 0; i < timeout_sec * 10; ++i) {
        if (rt.lookupService(name, info) == 0) return true;
        omnibinder::platform::sleepMs(100);
    }
    return false;
}

static const uint16_t SM_PORT = 19910;
static const int RPC_ROUNDS = 1000;
static const int RPC_WARMUP = 50;
static const int TOPIC_ROUNDS = 1000;
static const int TOPIC_WARMUP = 100;

static TestPid g_sm_pid = -1;
static TestPid g_srv_pid = -1;

static void fatalExit(int code) {
    stopProcess(g_srv_pid);
    stopProcess(g_sm_pid);
    std::exit(code);
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    std::string report_path = "docs/performance-report.md";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            report_path = argv[++i];
        }
    }

    omnibinder::setLogLevel(omnibinder::LOG_ERROR);
    printf("=== OmniBinder Performance Benchmark ===\n");
    printf("    version %s\n\n", omnibinder::version());

    const char* sm_bin = findBinary("service_manager");
    if (!sm_bin) {
        fprintf(stderr, "FATAL: service_manager binary not found\n");
        return 1;
    }
    TestPid sm_pid = startProcess(sm_bin, "--port", "19910", "--log-level", "3");
    g_sm_pid = sm_pid;
    if (sm_pid <= 0 || !waitPortReady(SM_PORT, 15)) {
        fprintf(stderr, "FATAL: SM failed to start\n");
        return 1;
    }
    printf("[OK] ServiceManager started on port %u\n", SM_PORT);

    const char* srv_bin = findBinary("test_performance_server");
    if (!srv_bin) {
        fprintf(stderr, "FATAL: server binary not found\n");
        stopProcess(sm_pid);
        return 1;
    }
    TestPid srv_pid = startProcess(srv_bin, "--sm-port", "19910");
    g_srv_pid = srv_pid;

    if (!waitForService("PerfService", SM_PORT, 10)) {
        fprintf(stderr, "FATAL: PerfService not registered\n");
        stopProcess(srv_pid);
        stopProcess(sm_pid);
        return 1;
    }
    printf("[OK] PerfService registered\n\n");

    std::vector<LatencyStats> all;

    // ============================================================
    // RPC via IDL Proxy
    // ============================================================
    printf("--- RPC Round-Trip Latency ---\n\n");
    {
        omnibinder::OmniRuntime rt;
        rt.init("127.0.0.1", SM_PORT);
        perf::PerfServiceProxy proxy(rt);
        proxy.connect();

        // EchoBytes with various payload sizes — 随机化顺序消除系统性偏差
        const int sizes[] = {0, 64, 256, 1024, 4096, 8192};
        const int n_sizes = sizeof(sizes) / sizeof(sizes[0]);
        LatencyStats echo_stats[n_sizes];
        for (int i = 0; i < n_sizes; ++i) {
            char buf[64];
            snprintf(buf, sizeof(buf), "RPC EchoBytes (%d bytes)", sizes[i]);
            echo_stats[i].name = buf;
        }

        // 生成随机化测试序列：每种 size 跑 RPC_ROUNDS 次，打乱顺序
        std::vector<int> sequence;
        for (int i = 0; i < n_sizes; ++i)
            for (int r = 0; r < RPC_ROUNDS; ++r)
                sequence.push_back(i);
        std::mt19937 rng(42);
        std::shuffle(sequence.begin(), sequence.end(), rng);

        // 预分配每种 size 的 payload 和输出缓冲
        std::vector<std::vector<uint8_t>> payloads(n_sizes), outputs(n_sizes);
        for (int i = 0; i < n_sizes; ++i) {
            payloads[i].assign(sizes[i], 0xAB);
            outputs[i].reserve(sizes[i]);
        }

        // 每个 size 做预热（复用预分配缓冲）
        for (int i = 0; i < n_sizes; ++i) {
            for (int w = 0; w < RPC_WARMUP; ++w)
                proxy.EchoBytes(payloads[i], outputs[i]);
        }

        // 正式测试：复用预分配缓冲，只测量纯 RPC 耗时
        for (int idx : sequence) {
            std::vector<uint8_t>& out = outputs[idx];
            int64_t t0 = nowUs();
            int ret = proxy.EchoBytes(payloads[idx], out);
            int64_t dt = nowUs() - t0;
            if (ret != 0 || out != payloads[idx]) {
                fprintf(stderr, "FATAL: EchoBytes(%d) failed ret=%d\n", sizes[idx], ret);
                fatalExit(2);
            }
            echo_stats[idx].add(static_cast<double>(dt));
        }
        for (int i = 0; i < n_sizes; ++i) {
            echo_stats[i].print();
            all.push_back(echo_stats[i]);
        }

        // EchoInt32
        {
            LatencyStats s;
            s.name = "RPC EchoInt32";
            int32_t result;
            for (int i = 0; i < RPC_WARMUP; ++i) proxy.EchoInt32(42, result);
            for (int i = 0; i < RPC_ROUNDS; ++i) {
                int64_t t0 = nowUs();
                int ret = proxy.EchoInt32(i, result);
                s.add(static_cast<double>(nowUs() - t0));
                if (ret != 0 || result != i + 1) {
                    fprintf(stderr, "FATAL: EchoInt32 failed i=%d ret=%d\n", i, ret);
                    fatalExit(2);
                }
            }
            s.print();
            all.push_back(s);
        }

        // EchoStruct
        {
            LatencyStats s;
            s.name = "RPC EchoStruct";
            perf::PerfData input, output;
            input.id = 1; input.value = 3.14; input.tag = "benchmark";
            input.blob = std::vector<uint8_t>(64, 0xCD);
            for (int i = 0; i < RPC_WARMUP; ++i) proxy.EchoStruct(input, output);
            for (int i = 0; i < RPC_ROUNDS; ++i) {
                input.id = i;
                int64_t t0 = nowUs();
                int ret = proxy.EchoStruct(input, output);
                s.add(static_cast<double>(nowUs() - t0));
                if (ret != 0) {
                    fprintf(stderr, "FATAL: EchoStruct failed i=%d ret=%d\n", i, ret);
                    fatalExit(2);
                }
            }
            s.print();
            all.push_back(s);
        }

        // Add
        {
            LatencyStats s;
            s.name = "RPC Add (2 x int32)";
            int32_t result;
            perf::AddParams params;
            params.a = 42; params.b = 58;
            for (int i = 0; i < RPC_WARMUP; ++i) proxy.Add(params, result);
            for (int i = 0; i < RPC_ROUNDS; ++i) {
                params.a = i; params.b = i * 2;
                int64_t t0 = nowUs();
                int ret = proxy.Add(params, result);
                s.add(static_cast<double>(nowUs() - t0));
                if (ret != 0 || result != (i + i * 2)) {
                    fprintf(stderr, "FATAL: Add failed i=%d ret=%d\n", i, ret);
                    fatalExit(2);
                }
            }
            s.print();
            all.push_back(s);
        }
    }

    // ============================================================
    // Topic via IDL Proxy
    // ============================================================
    printf("--- Topic Pub/Sub Latency ---\n\n");
    {
        omnibinder::OmniRuntime rt;
        rt.init("127.0.0.1", SM_PORT);
        perf::PerfServiceProxy proxy(rt);
        proxy.connect();

        const int sizes[] = {0, 64, 256, 1024, 4096, 8192};
        const int n_sizes = sizeof(sizes) / sizeof(sizes[0]);

        LatencyStats stats[n_sizes];
        int warmup[n_sizes];
        int needed[n_sizes];

        for (int i = 0; i < n_sizes; ++i) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Topic pub/sub (%d bytes)", sizes[i]);
            stats[i].name = buf;
            warmup[i] = TOPIC_WARMUP;
            needed[i] = TOPIC_ROUNDS;
        }

        proxy.SubscribePerfTopic(
            [&](const perf::PerfTopic& msg) {
                int sz = static_cast<int>(msg.payload.size());
                int idx = -1;
                for (int i = 0; i < n_sizes; ++i)
                    if (sizes[i] == sz) { idx = i; break; }
                if (idx < 0) return;

                if (warmup[idx] > 0) { --warmup[idx]; return; }
                if (needed[idx] > 0) {
                    stats[idx].add(nowUs() - msg.send_time_us);
                    --needed[idx];
                }
                // 校验在计时之后，避免 O(N) 扫描污染测量
                if (needed[idx] == 0 && warmup[idx] == 0) {
                    for (size_t j = 0; j < msg.payload.size(); ++j) {
                        if (msg.payload[j] != static_cast<uint8_t>(0xCD)) {
                            fprintf(stderr, "FATAL: Topic payload mismatch: sz=%d pos=%zu\n", sz, j);
                            fatalExit(2);
                        }
                    }
                }
            });

        int pending = n_sizes * TOPIC_ROUNDS;
        int64_t deadline = nowUs() + 60 * 1000 * 1000LL;
        while (pending > 0 && nowUs() < deadline) {
            rt.pollOnce(0);
            pending = 0;
            for (int i = 0; i < n_sizes; ++i) pending += needed[i];
        }

        for (int i = 0; i < n_sizes; ++i) {
            if (needed[i] > 0) {
                fprintf(stderr, "FATAL: topic %s only %d/%d samples\n",
                        stats[i].name.c_str(), TOPIC_ROUNDS - needed[i], TOPIC_ROUNDS);
                fatalExit(2);
            }
            stats[i].print();
            all.push_back(stats[i]);
        }
    }

    // ============================================================
    // Report
    // ============================================================
    {
        std::ofstream ofs(report_path);
        if (ofs.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);

            ofs << "# OmniBinder 性能测试报告\n\n"
                << "**生成时间:** " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "\n\n"
                << "**测试环境:**\n"
                << "- 传输方式: TCP + SHM（自动选择，同机通信使用 SHM）\n"
                << "- RPC 预热轮数: " << RPC_WARMUP << "\n"
                << "- RPC 测试轮数: " << RPC_ROUNDS << "（每个用例）\n"
                << "- 话题模式: 服务端每 500us 轮转发布 topic，客户端 pollOnce(0) busy-spin 收包，统计服务端 publish 时间戳到客户端 callback 的延迟\n"
                << "- 测试服务 SHM ring: 64KB / 64KB\n"
                << "- 话题预热轮数: " << TOPIC_WARMUP << "\n"
                << "- 话题测试轮数: " << TOPIC_ROUNDS << "（每个用例）\n\n"
                << "## 总览\n\n"
        << "| 测试用例 | 样本数 | 最小值 (us) | 最大值 (us) | 平均值 (us) | 去极值均值 (us) | 中位数 (us) | 95% 情况 (us) | 99% 情况 (us) | 标准差 (us) |\n"
        << "|----------|--------|-------------|-------------|-------------|-------------|----------|----------|----------|-------------|\n";
            for (auto& s : all) {
                ofs << s.toRow() << "\n";
            }

            ofs << "\n## 详细数据\n\n"
                << "### RPC 调用往返延迟\n\n"
                << "测量从客户端发起 invoke 请求到收到响应的完整往返时间。\n"
                << "服务端原样返回数据（EchoBytes）或执行简单运算（EchoInt32/Add）。\n\n";

            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].name.find("Topic") != std::string::npos) break;
                ofs << "**" << all[i].name << "**\n\n"
                    << "- 样本数: " << all[i].count() << "\n"
                    << "- 最小值: " << std::fixed << std::setprecision(0) << all[i].min() << " us\n"
                    << "- 最大值: " << all[i].max() << " us\n"
                    << "- 平均值: " << std::setprecision(3) << all[i].avg() << " us\n"
                    << "- 去极值均值: " << all[i].trimmedMean() << " us\n"
                    << "- 中位数: " << std::setprecision(0) << all[i].median() << " us\n"
                    << "- 95% 情况: " << all[i].p95() << " us\n"
                    << "- 99% 情况: " << all[i].p99() << " us\n\n";
            }

            ofs << "### 话题发布/订阅延迟\n\n"
                << "测量服务端执行 `BroadcastPerfTopic` 时写入的 `send_time_us` 到客户端订阅回调执行的时间。\n"
                << "服务端以 500us 间隔轮转发布各 payload size，客户端使用 pollOnce(0) busy-spin 收包，与 RPC benchmark 的热事件循环条件对齐。\n\n";
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].name.find("Topic") == std::string::npos) continue;
                ofs << "**" << all[i].name << "**\n\n"
                    << "- 样本数: " << all[i].count() << "\n"
                    << "- 最小值: " << std::fixed << std::setprecision(0) << all[i].min() << " us\n"
                    << "- 最大值: " << all[i].max() << " us\n"
                    << "- 平均值: " << std::setprecision(3) << all[i].avg() << " us\n"
                    << "- 去极值均值: " << all[i].trimmedMean() << " us\n"
                    << "- 中位数: " << std::setprecision(0) << all[i].median() << " us\n"
                    << "- 95% 情况: " << all[i].p95() << " us\n"
                    << "- 99% 情况: " << all[i].p99() << " us\n\n";
            }

            ofs << "---\n\n"
                << "## 性能分析\n\n"
                << "### 结论\n\n";

            double rpc_avg = 0;
            int rpc_count = 0;
            for (auto& s : all) {
                if (s.name.find("RPC") != std::string::npos && s.name.find("8192") == std::string::npos) {
                    rpc_avg += s.avg();
                    ++rpc_count;
                }
            }
            rpc_avg /= rpc_count;

            double topic_avg = 0;
            int topic_count = 0;
            for (auto& s : all) {
                if (s.name.find("Topic") != std::string::npos) {
                    topic_avg += s.avg();
                    ++topic_count;
                }
            }
            topic_avg /= topic_count;

            ofs << "- 0~4096 byte 常见 RPC 在当前机器上平均值约为 " << std::fixed << std::setprecision(1) << rpc_avg << " us\n"
                << "- topic pub/sub 在当前机器上平均值约为 " << topic_avg << " us\n";

            printf("\n[OK] Report written: %s\n", report_path.c_str());
        }
    }

    stopProcess(srv_pid);
    stopProcess(sm_pid);
    g_srv_pid = -1;
    g_sm_pid = -1;
    printf("\n=== Benchmark Complete ===\n");
    return 0;
}
