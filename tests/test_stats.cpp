/**
 * @file test_stats.cpp
 * @brief 运行时统计测试
 */

#include <omnibinder/omnibinder.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>

using namespace omnibinder;

const uint16_t SM_PORT = 9900;

// 简单的测试服务
class TestService : public Service {
public:
    TestService() : Service("TestService") {}
    
    const char* serviceName() const override {
        return "TestService";
    }
    
    const InterfaceInfo& interfaceInfo() const override {
        static InterfaceInfo info;
        static bool initialized = false;
        if (!initialized) {
            info.interface_id = 0x12345678;
            info.name = "ITestService";
            
            MethodInfo method;
            method.method_id = 0x00000001;
            method.name = "echo";
            info.methods.push_back(method);
            initialized = true;
        }
        return info;
    }
    
    void onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) override {
        if (method_id == 0x00000001) {
            // 从 const Buffer 读取需要创建临时 Buffer
            Buffer temp(request.data(), request.size());
            int32_t value = temp.readInt32();
            response.writeInt32(value);
        }
    }
};

int main() {
    printf("=== Runtime Stats Test ===\n\n");
    
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
    OmniRuntime server_client;
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
    pid_t server_pid = fork();
    if (server_pid == 0) {
        server_client.run();
        exit(0);
    }
    
    sleep(1);
    
    // 创建客户端
    printf("Creating client...\n");
    OmniRuntime runtime;
    if (runtime.init("127.0.0.1", SM_PORT) != 0) {
        printf("Failed to init client\n");
        kill(server_pid, SIGTERM);
        kill(sm_pid, SIGTERM);
        return 1;
    }
    
    // 在单独进程运行客户端事件循环
    pid_t client_pid = fork();
    if (client_pid == 0) {
        client.run();
        exit(0);
    }
    
    sleep(1);
    
    // 测试：获取初始统计
    printf("\n[Test 1] Initial stats\n");
    RuntimeStats stats;
    runtime.getStats(stats);
    printf("  RPC calls: %lu\n", stats.total_rpc_calls);
    printf("  RPC success: %lu\n", stats.total_rpc_success);
    printf("  RPC failures: %lu\n", stats.total_rpc_failures);
    printf("  Active connections: %u\n", stats.active_connections);
    
    // 测试：执行 RPC 调用
    printf("\n[Test 2] After 10 RPC calls\n");
    for (int i = 0; i < 10; i++) {
        Buffer request, response;
        request.writeInt32(i);
        runtime.invoke("TestService", 0x12345678, 0x00000001,
                     request, response, 5000);
    }
    
    sleep(1);
    runtime.getStats(stats);
    printf("  RPC calls: %lu\n", stats.total_rpc_calls);
    printf("  RPC success: %lu\n", stats.total_rpc_success);
    printf("  RPC failures: %lu\n", stats.total_rpc_failures);
    printf("  Active connections: %u\n", stats.active_connections);
    
    if (stats.total_rpc_calls != 10) {
        printf("  FAILED: Expected 10 RPC calls, got %lu\n", stats.total_rpc_calls);
        goto cleanup;
    }
    if (stats.total_rpc_success != 10) {
        printf("  FAILED: Expected 10 successful RPCs, got %lu\n", stats.total_rpc_success);
        goto cleanup;
    }
    printf("  PASSED\n");
    
    // 测试：重置统计
    printf("\n[Test 3] After reset\n");
    runtime.resetStats();
    runtime.getStats(stats);
    printf("  RPC calls: %lu\n", stats.total_rpc_calls);
    printf("  RPC success: %lu\n", stats.total_rpc_success);
    
    if (stats.total_rpc_calls != 0) {
        printf("  FAILED: Expected 0 RPC calls after reset, got %lu\n", stats.total_rpc_calls);
        goto cleanup;
    }
    printf("  PASSED\n");
    
    printf("\n=== All stats tests PASSED ===\n");
    
cleanup:
    // 清理
    printf("\nCleaning up...\n");
    runtime.stop();
    server_runtime.stop();
    kill(client_pid, SIGTERM);
    kill(server_pid, SIGTERM);
    kill(sm_pid, SIGTERM);
    waitpid(client_pid, NULL, 0);
    waitpid(server_pid, NULL, 0);
    waitpid(sm_pid, NULL, 0);
    
    return 0;
}
