/**************************************************************************************************
 * @file        omni_allocator.cpp
 * @brief       自定义内存分配器钩子
 * @details     提供 omniSetAllocator() 注册自定义 malloc/free 函数指针，并据此提供
 *              omni_malloc / omni_free / omni_realloc* —— 库内经这些接口完成的分配
 *              （Buffer、C API、生成代码）都走用户注册的分配器。
 *
 *              本单元不重载全局 operator new/delete：全局 new/delete 是进程级
 *              interpose，无法限定在库内，与"库只接管自己"的目标相悖，且在静态
 *              链接下不可靠。C++ 侧对象/容器的接管由源码级显式分配接口负责
 *              （omni_new/omni_delete + 分配器感知别名），不依赖全局重载。
 *
 *              钩子采用一次性锁定策略：一旦设置非 NULL 钩子，后续调用 omniSetAllocator()
 *              将被忽略。这从根本上防止了"分配时用一种堆、释放时用另一种堆"的错配。
 *              嵌入式场景下，自定义堆通常是唯一的堆（如 FreeRTOS heap_4 静态数组），
 *              一次性设定后永不改变正是标准实践。
 *
 *              不调用 omniSetAllocator() → 回退到系统 malloc/free（透明）。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-06-21
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
 *************************************************************************************************/

#include "omnibinder/omnibinder.h"
#include <cstdlib>
#include <cstring>
#include <atomic>

namespace omnibinder {
namespace {

static OmniMallocFn      g_malloc_fn = nullptr;
static OmniFreeFn        g_free_fn   = nullptr;
static std::atomic<bool> g_locked{false};

}
}

extern "C" void omniSetAllocator(OmniMallocFn malloc_fn, OmniFreeFn free_fn) {
    // 必须成对提供：单边非空会导致分配用一个堆、释放用另一个堆的错配
    if ((malloc_fn == nullptr) != (free_fn == nullptr)) {
        return;
    }
    if (malloc_fn && free_fn) {
        bool expected = false;
        if (!omnibinder::g_locked.compare_exchange_strong(expected, true)) {
            return;
        }
    } else {
        if (omnibinder::g_locked.load(std::memory_order_acquire)) {
            return;
        }
    }
    omnibinder::g_malloc_fn = malloc_fn;
    omnibinder::g_free_fn   = free_fn;
}

extern "C" void* omni_malloc(size_t size) {
    if (omnibinder::g_malloc_fn) return omnibinder::g_malloc_fn(size);
    return std::malloc(size);
}

extern "C" void omni_free(void* ptr) {
    if (!ptr) return;
    if (omnibinder::g_free_fn) { omnibinder::g_free_fn(ptr); return; }
    std::free(ptr);
}

extern "C" void* omni_realloc_sized(void* ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) {
        omni_free(ptr);
        return nullptr;
    }
    if (!ptr) {
        return omni_malloc(new_size);
    }
    if (!omnibinder::g_malloc_fn) {
        return std::realloc(ptr, new_size);
    }

    void* new_ptr = omnibinder::g_malloc_fn(new_size);
    if (!new_ptr) return nullptr;
    std::memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    omnibinder::g_free_fn(ptr);
    return new_ptr;
}

extern "C" void* omni_realloc(void* ptr, size_t new_size) {
    if (new_size == 0) {
        omni_free(ptr);
        return nullptr;
    }
    if (!ptr) {
        return omni_malloc(new_size);
    }
    if (omnibinder::g_malloc_fn) {
        return nullptr;
    }
    return std::realloc(ptr, new_size);
}
