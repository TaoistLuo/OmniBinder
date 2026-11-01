#include "app_thread.h"

#include <chrono>
#include <new>
#include <thread>

struct app_thread_t {
    std::thread thread;
};

extern "C" app_thread_t* app_thread_create(void (*entry)(void*), void* arg) {
    if (!entry) return nullptr;
    app_thread_t* thread = new (std::nothrow) app_thread_t();
    if (!thread) return nullptr;
    try {
        thread->thread = std::thread(entry, arg);
    } catch (...) {
        delete thread;
        return nullptr;
    }
    return thread;
}

extern "C" void app_thread_join(app_thread_t* thread) {
    if (!thread) return;
    if (thread->thread.joinable()) {
        thread->thread.join();
    }
    delete thread;
}

extern "C" void app_sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
