/**************************************************************************************************
 * @file        omnibinder.h
 * @brief       公共头文件
 * @details     OmniBinder 框架的统一入口头文件，包含所有公共 API 头文件
 *              （types、error、log、buffer、message、transport、service、runtime），
 *              使用者只需 #include "omnibinder/omnibinder.h" 即可访问全部功能。
 *              同时提供 version() 和 versionNumbers() 版本查询接口。
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
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include "omnibinder/service.h"
#include "omnibinder/runtime.h"

namespace omnibinder {

inline const char* version() { return "1.0.0"; }

inline void versionNumbers(int& major, int& minor, int& patch) {
    major = 1; minor = 0; patch = 0;
}

} // namespace omnibinder

// ============================================================
// 自定义内存分配器
// ============================================================

typedef void* (*OmniMallocFn)(size_t size);
typedef void  (*OmniFreeFn)(void* ptr);

/// 注册自定义内存分配器。一次性锁定：一旦设置非 NULL 钩子，后续调用被忽略。
/// 必须在任何 OmniRuntime API 调用之前设置（包括 init/registerService）。
///
/// 不调用 → 透明回退到系统 malloc/free（开发机零配置）。
/// 设置后永不重置 → 从根本上杜绝分配/释放走不同堆导致的堆损坏。
///
/// @param malloc_fn  自定义 malloc（如 pvPortMalloc / tlsf_malloc）
/// @param free_fn    自定义 free（如 vPortFree / tlsf_free）
///
/// 用法:
///   int main() {
///       omniSetAllocator(pvPortMalloc, vPortFree);  // 必须最先调用
///       OmniRuntime runtime;
///       runtime.init(...);
///       ...
///   }
extern "C" void omniSetAllocator(OmniMallocFn malloc_fn, OmniFreeFn free_fn);

extern "C" void* omni_malloc(size_t size);
extern "C" void  omni_free(void* ptr);
extern "C" void* omni_realloc(void* ptr, size_t new_size);

#endif // OMNIBINDER_H
