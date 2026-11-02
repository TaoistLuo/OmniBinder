/* 最简单的 C 服务端示例：注册服务、实现一个 RPC、周期发布一个 topic。 */

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

/* 输入 common_Input，输出 common_Output。 */
omni_impl demo_ExampleService_impl_echo(const common_Input* input,
                                         common_Output* output,
                                         void* user_data) {
    (void)user_data;
    output->code = 0;
    output->message = omni_strdup(input->text);
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

    omni_service_t* service = demo_ExampleService_stub_create(runtime);
    if (!service || omni_runtime_register_service(runtime, service) != 0) {
        fprintf(stderr, "register failed\n");
        demo_ExampleService_stub_destroy(service);
        omni_runtime_destroy(runtime);
        return 1;
    }

    app_thread_t* thread = app_thread_create(runtime_entry, runtime);
    if (!thread) {
        demo_ExampleService_stub_destroy(service);
        omni_runtime_destroy(runtime);
        return 1;
    }

    while (g_running && !omni_runtime_is_ready(runtime)) {
        app_sleep_ms(50);
    }

    printf("ExampleService ready on port %u\n", omni_service_port(service));
    int counter = 0;
    while (g_running) {
        app_sleep_ms(1000);
        demo_Update update;
        demo_Update_init(&update);
        update.value = ++counter;
        update.text = omni_strdup("tick");
        demo_ExampleService_broadcast_update(runtime, &update);
        printf("publish Update value=%d text=%s\n", update.value, update.text);
        demo_Update_destroy(&update);
    }

    omni_runtime_stop(runtime);
    app_thread_join(thread);
    demo_ExampleService_stub_destroy(service);
    omni_runtime_destroy(runtime);
    return 0;
}
