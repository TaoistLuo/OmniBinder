/**************************************************************************************************
 * @file        omnibinder.h
 * @brief       公共头文件
 * @details     OmniBinder 框架的 C++ 统一入口头文件，聚合常用公共 API：
 *              types、error、log、buffer、service、runtime。
 *              内部协议/传输头（message、transport）与 Proxy 基类（proxy_base.h）
 *              不在此聚合，需要时请直接 #include 对应头文件。
 *              同时提供 version()/versionNumbers() 版本查询接口及自定义内存分配器声明。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-02-11
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef OMNIBINDER_H
#define OMNIBINDER_H

#include "omnibinder/types.h"
#include "omnibinder/error.h"
#include "omnibinder/log.h"
#include "omnibinder/buffer.h"
#include "omnibinder/service.h"
#include "omnibinder/runtime.h"

namespace omnibinder {

/* @brief 返回版本字符串
 * @note 版本号由 CMakeLists.txt 的 project(VERSION ...) 注入，编译时通过 add_compile_definitions 传递；
 *       升级版本只需改 CMakeLists.txt 一处 */
inline const char* version() { return OMNIBINDER_VERSION; }

/* @brief 返回主版本号、次版本号和修订号
 * @param[out] major 主版本号
 * @param[out] minor 次版本号
 * @param[out] patch 修订号 */
inline void versionNumbers(int& major, int& minor, int& patch) {
    major = OMNIBINDER_VERSION_MAJOR;
    minor = OMNIBINDER_VERSION_MINOR;
    patch = OMNIBINDER_VERSION_PATCH;
}

} // namespace omnibinder

// ============================================================
// 自定义内存分配器
// ============================================================

typedef void* (*OmniMallocFn)(size_t size);
typedef void  (*OmniFreeFn)(void* ptr);

/* @brief 注册自定义内存分配器
 * @param[in] malloc_fn 自定义 malloc（如 pvPortMalloc / tlsf_malloc）
 * @param[in] free_fn   自定义 free（如 vPortFree / tlsf_free）
 * @note 一次性锁定：一旦设置非 NULL 钩子，后续调用被忽略。
 *       必须在任何 OmniRuntime API 调用之前设置（包括 init/registerService）。
 *       不调用 → 透明回退到系统 malloc/free（开发机零配置）。
 *       设置后永不重置 → 从根本上杜绝分配/释放走不同堆导致的堆损坏。
 *       用法:
 *       int main() {
 *           omniSetAllocator(pvPortMalloc, vPortFree);  // 必须最先调用
 *           OmniRuntime runtime;
 *           runtime.init(...);
 *           ...
 *       } */
extern "C" void omniSetAllocator(OmniMallocFn malloc_fn, OmniFreeFn free_fn);

/* @brief 从注册的自定义 allocator（或系统 malloc）分配内存
 * @param[in] size 请求字节数
 * @return 分配到的内存指针；失败返回 NULL */
extern "C" void* omni_malloc(size_t size);
/* @brief 释放由 omni_malloc / omni_realloc* 分配的内存
 * @param[in] ptr 待释放指针（可为 NULL，此时为空操作） */
extern "C" void  omni_free(void* ptr);
/* @brief 安全 realloc 变体
 * @param[in] old_size 旧分配大小
 * @param[in] new_size 新分配大小
 * @note 使用自定义 allocator 时，调用方必须提供旧分配大小，这样实现才能只复制
 *       min(old_size, new_size) 字节，避免越界读取旧块。 */
extern "C" void* omni_realloc_sized(void* ptr, size_t old_size, size_t new_size);
/* @brief 兼容系统 realloc 的包装
 * @param[in] new_size 新分配大小
 * @note 未注册自定义 allocator 时直接走 std::realloc；注册自定义 allocator 后，
 *       无法得知旧块大小，因此仅支持 ptr == NULL 或 new_size == 0。
 *       需要扩容并保留内容时请使用 omni_realloc_sized()。 */
extern "C" void* omni_realloc(void* ptr, size_t new_size);

#endif // OMNIBINDER_H
