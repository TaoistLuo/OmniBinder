/**************************************************************************************************
 * @file        omni_allocator.cpp
 * @brief       C 侧内存分配入口
 * @details     为 C 代码（C API 及 omni-idlc 生成的 C 源码）提供 omni_malloc/omni_free/omni_strdup。
 *              二者是 ::operator new / ::operator delete 的薄封装：库内所有堆分配——
 *              无论 C++ 直接 new 还是 C 经 omni_malloc——最终都汇聚到同一个可重载的
 *              全局 operator new/delete 上。使用者只需重载全局 new/delete，即可接管
 *              库内（C 与 C++）的全部堆分配。
 *
 *              不再提供 omniSetAllocator 之类的分配器注册钩子：那类钩子只能覆盖部分
 *              分配路径，名实不符。自定义堆请通过重载全局 new/delete 实现。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-06-21
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
 *************************************************************************************************/

#include "omnibinder/omnibinder.h"
#include <new>
#include <cstring>

extern "C" void* omni_malloc(size_t size) {
    // nothrow：C 侧无异常语义，分配失败返回 NULL
    return ::operator new(size, std::nothrow);
}

extern "C" void omni_free(void* ptr) {
    ::operator delete(ptr);
}

extern "C" char* omni_strdup(const char* str) {
    if (str == NULL) {
        return NULL;
    }
    const size_t len = std::strlen(str);
    char* copy = static_cast<char*>(::operator new(len + 1, std::nothrow));
    if (copy == NULL) {
        return NULL;
    }
    std::memcpy(copy, str, len + 1);
    return copy;
}
