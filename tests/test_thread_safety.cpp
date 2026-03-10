/**
 * @file test_thread_safety.cpp
 * @brief 线程安全测试
 */

#include <omnibinder/omnibinder.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace omnibinder;

const uint16_t SM_PORT = 19930;
std::atomic<int> success_count(0);
std::atomic<int> failure_count(0);

// 简单的测试服务
class TestService : public Service {
public:
    TestService() : Service("TestService") {}
    
    const char* serviceName() const override {
        return "TestService";
    }
    
    InterfaceInfo interfaceInfo() const override {
        InterfaceInfo info;
        info.interface_id = 0x12345678;
        info.interface_name = "ITestService";
        
        MethodInfo method;
        method.method_id = 0x00000001;
        method.method_name = "echo";
        info.methods.push_back(method);
        
        return info;
    }
    
    int onInvoke(uint32_t interface_id, uint32_t method_id,
                 const Buffer& request, Buffer& response) override {
        if (interface_id == 0x12345678 && method_id == 0x00000001) {
            // Echo: 返回接收到的数据
            int32_t value = request.readInt32();
            response.writeInt32(value);
            return 0;
        }
        return -1;
    }
};

// 测试1: 多线程同时调用 lookupService
void testConcurrentLookup(OmniRuntime& runtime, int thread_id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        ServiceInfo info;
        int ret = runtime.lookupService("TestService", info);
        if (ret == 0) {
            success_count++;
        } else {
            failure_count++;
        }
        
        // 随机延迟
        std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
    }
}

// 测试2: 多线程同时调用 invoke
void testConcurrentInvoke(OmniRuntime& runtime, int thread_id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        Buffer request, response;
        request.writeInt32(thread_id * 1000 + i);
        
        int ret = runtime.invoke("TestService", 0x12345678, 0x00000001,
                               request, response, 5000);
        if (ret == 0) {
            int32_t result = response.readInt32();
            if (result == thread_id * 1000 + i) {
                success_count++;
            } else {
                failure_count++;
                printf("Thread %d: Expected %d, got %d\n", 
                       thread_id, thread_id * 1000 + i, result);
            }
        } else {
            failure_count++;
        }
        
        std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
    }
}

// 测试3: 多线程同时注册/注销服务
void testConcurrentRegister(OmniRuntime& runtime, int thread_id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        TestService service;
        
        int ret = runtime.registerService(&service);
        if (ret == 0) {
            success_count++;
            
            // 短暂等待
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            ret = runtime.unregisterService(&service);
            if (ret == 0) {
                success_count++;
            } else {
                failure_count++;
            }
        } else {
            failure_count++;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 50));
    }
}

int main() {
    printf("=== OmniBinder Thread Safety Test ===\n\n");
    
    // 启动 ServiceManager
    printf("Starting ServiceManager...\n");
    pid_t sm_pid = fork();
    if (sm_pid == 0) {
        execl("./target/bin/service_manager", "service_manager", NULL);
        exit(1);
    }
    sleep(1);
    
    // 创建服务端
    printf("Starting test service...\n");
    OmniRuntime server_runtime;
    if (server_runtime.init("127.0.0.1", SM_PORT) != 0) {
        printf("Failed to init server client\n");
        kill(sm_pid, SIGTERM);
        return 1;
    }
    
    TestService test_service;
    if (server_runtime.registerService(&test_service) != 0) {
        printf("Failed to register service\n");
        kill(sm_pid, SIGTERM);
        return 1;
    }
    
    // 在单独线程运行服务端事件循环
    std::thread server_thread([&server_runtime]() {
        server_runtime.run();
    });
    
    sleep(1);
    
    // 创建客户端
    printf("Creating client...\n");
    OmniRuntime runtime;
    if (runtime.init("127.0.0.1", SM_PORT) != 0) {
        printf("Failed to init client\n");
        server_runtime.stop();
        server_thread.join();
        kill(sm_pid, SIGTERM);
        return 1;
    }
    
    // 在单独线程运行客户端事件循环
    std::thread runtime_thread([&runtime]() {
        runtime.run();
    });
    
    sleep(1);
    
    // 测试1: 多线程并发查询
    printf("\n[Test 1] Concurrent lookupService (10 threads x 100 iterations)\n");
    success_count = 0;
    failure_count = 0;
    {
        std::vector<std::thread> threads;
        auto start = std::chrono::steady_clock::now();
        
        for (int i = 0; i < 10; i++) {
            threads.emplace_back(testConcurrentLookup, std::ref(runtime), i, 100);
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        printf("  Success: %d, Failure: %d\n", success_count.load(), failure_count.load());
        printf("  Duration: %ld ms\n", duration.count());
        
        if (failure_count > 0) {
            printf("  FAILED!\n");
            runtime.stop();
            server_runtime.stop();
            runtime_thread.join();
            server_thread.join();
            kill(sm_pid, SIGTERM);
            return 1;
        }
        printf("  PASSED\n");
    }
    
    // 测试2: 多线程并发 RPC 调用
    printf("\n[Test 2] Concurrent invoke (10 threads x 50 iterations)\n");
    success_count = 0;
    failure_count = 0;
    {
        std::vector<std::thread> threads;
        auto start = std::chrono::steady_clock::now();
        
        for (int i = 0; i < 10; i++) {
            threads.emplace_back(testConcurrentInvoke, std::ref(runtime), i, 50);
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        printf("  Success: %d, Failure: %d\n", success_count.load(), failure_count.load());
        printf("  Duration: %ld ms\n", duration.count());
        
        if (failure_count > 0) {
            printf("  FAILED!\n");
            runtime.stop();
            server_runtime.stop();
            runtime_thread.join();
            server_thread.join();
            kill(sm_pid, SIGTERM);
            return 1;
        }
        printf("  PASSED\n");
    }
    
    // 清理
    printf("\nCleaning up...\n");
    runtime.stop();
    server_runtime.stop();
    runtime_thread.join();
    server_thread.join();
    kill(sm_pid, SIGTERM);
    waitpid(sm_pid, NULL, 0);
    
    printf("\n=== All thread safety tests PASSED ===\n");
    return 0;
}
