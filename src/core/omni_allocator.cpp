/**************************************************************************************************
 * @file        omni_allocator.cpp
 * @brief       自定义内存分配器钩子
 * @details     提供 omniSetAllocator() 注册自定义 malloc/free 函数指针。
 *              同时提供全局 operator new/delete 的弱符号重载，使得注册钩子后
 *              所有 C++ 堆分配（new/delete、STL 容器、std::function、std::string 等）
 *              自动走用户自定义分配器。
 *
 *              钩子采用一次性锁定策略：一旦设置非 NULL 钩子，后续调用 omniSetAllocator()
 *              将被忽略。这从根本上防止了"分配时用一种堆、释放时用另一种堆"的错配。
 *              嵌入式场景下，自定义堆通常是唯一的堆（如 FreeRTOS heap_4 静态数组），
 *              一次性设定后永不改变正是标准实践。
 *
 *              不调用 omniSetAllocator() → 回退到系统 malloc/free（透明）。
 *              用户定义自己的强符号 operator new/delete → 库的弱符号被覆盖（不冲突）。
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
#include <new>
#include <atomic>

#if defined(__GNUC__) || defined(__clang__)
  #define OMNI_WEAK  __attribute__((weak))
#else
  #define OMNI_WEAK
#endif

namespace omnibinder {
namespace {

static OmniMallocFn      g_malloc_fn = nullptr;
static OmniFreeFn        g_free_fn   = nullptr;
static std::atomic<bool> g_locked{false};

}
}

extern "C" void omniSetAllocator(OmniMallocFn malloc_fn, OmniFreeFn free_fn) {
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

extern "C" void omni_ensure_allocator_linked() {}

// ============================================================
// operator new / delete（弱符号）
// ============================================================

OMNI_WEAK void* operator new(std::size_t size) {
    if (omnibinder::g_malloc_fn) {
        if (void* p = omnibinder::g_malloc_fn(size))
            return p;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size))
        return p;
    throw std::bad_alloc();
}

OMNI_WEAK void* operator new[](std::size_t size) {
    return ::operator new(size);
}

OMNI_WEAK void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

OMNI_WEAK void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

OMNI_WEAK void operator delete(void* ptr) noexcept {
    if (!ptr) return;
    if (omnibinder::g_free_fn) {
        omnibinder::g_free_fn(ptr);
    } else {
        std::free(ptr);
    }
}

OMNI_WEAK void operator delete[](void* ptr) noexcept {
    ::operator delete(ptr);
}

OMNI_WEAK void operator delete(void* ptr, std::size_t /*size*/) noexcept {
    ::operator delete(ptr);
}

OMNI_WEAK void operator delete[](void* ptr, std::size_t /*size*/) noexcept {
    ::operator delete(ptr);
}
