#ifndef OMNIBINDER_EXAMPLES_APP_THREAD_H
#define OMNIBINDER_EXAMPLES_APP_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 示例/应用侧线程辅助。
 *
 * 物理位置：examples/common，仅用于示例，不安装、不进入 include/，不进入核心 ABI。
 * 核心 runtime 不创建线程；业务必须自行提供线程执行 omni_runtime_start()。
 */
typedef struct app_thread_t app_thread_t;

app_thread_t* app_thread_create(void (*entry)(void*), void* arg);
void app_thread_join(app_thread_t* thread);
void app_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
