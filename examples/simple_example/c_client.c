/* 最简单的 C 客户端示例：调用一个 RPC，并订阅一个 topic。 */

#include "example.bidl_c.h"
#include "app_thread.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void runtime_entry(void* arg) {
    omni_runtime_start((omni_runtime_t*)arg);
}

static void on_update(const demo_Update* update, void* user_data) {
    (void)user_data;
    printf("Update => value=%d text=%s\n", update->value, update->text);
}

int main(int argc, char* argv[]) {
    const char* sm_host = "127.0.0.1";
    uint16_t sm_port = 9900;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sm-host") == 0 && i + 1 < argc) {
            sm_host = argv[++i];
        } else if (strcmp(argv[i], "--sm-port") == 0 && i + 1 < argc) {
            sm_port = (uint16_t)atoi(argv[++i]);
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    omni_runtime_t* runtime = omni_runtime_create();
    if (!runtime || omni_runtime_init(runtime, sm_host, sm_port) != 0) {
        fprintf(stderr, "init failed\n");
        omni_runtime_destroy(runtime);
        return 1;
    }

    demo_ExampleService_proxy proxy;
    demo_ExampleService_proxy_init(&proxy, runtime);

    app_thread_t* thread = app_thread_create(runtime_entry, runtime);
    if (!thread) {
        demo_ExampleService_proxy_destroy(&proxy);
        omni_runtime_destroy(runtime);
        return 1;
    }

    while (g_running && !omni_runtime_is_ready(runtime)) {
        app_sleep_ms(50);
    }
    while (g_running && !omni_runtime_is_service_connected(runtime, "ExampleService")) {
        omni_runtime_connect_service(runtime, "ExampleService");
        app_sleep_ms(100);
    }
    if (!g_running) {
        omni_runtime_stop(runtime);
        app_thread_join(thread);
        demo_ExampleService_proxy_destroy(&proxy);
        omni_runtime_destroy(runtime);
        return 0;
    }

    common_Input input;
    common_Output output;
    common_Input_init(&input);
    common_Output_init(&output);
    input.value = 42;
    input.text = (char*)"world";
    demo_ExampleService_proxy_echo(&proxy, &input, &output);
    printf("Echo => code=%d message=%s\n", output.code, output.message);
    common_Output_destroy(&output);

    demo_ExampleService_proxy_subscribe_update(&proxy, on_update, NULL);
    printf("waiting for Update...\n");

    while (g_running) {
        app_sleep_ms(100);
    }

    omni_runtime_stop(runtime);
    app_thread_join(thread);
    demo_ExampleService_proxy_destroy(&proxy);
    omni_runtime_destroy(runtime);
    return 0;
}
