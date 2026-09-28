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
// 内存分配入口（C 侧）
// ============================================================

/* @brief 分配内存（C 代码用；C++ 请直接用 new）
 * @param[in] size 请求字节数
 * @return 分配到的内存指针；失败返回 NULL
 * @note 本函数是 ::operator new 的薄封装。库内所有堆分配（C++ 的 new、C 生成代码的
 *       omni_malloc）最终都走同一个全局 operator new。**想让库内分配使用自定义堆，
 *       请在你的程序中重载全局 operator new/delete 即可**——C 与 C++ 一次覆盖。 */
extern "C" void* omni_malloc(size_t size);

/* @brief 释放由 omni_malloc 分配的内存
 * @param[in] ptr 待释放指针（可为 NULL，此时为空操作）
 * @note 本函数是 ::operator delete 的薄封装。 */
extern "C" void  omni_free(void* ptr);

#endif // OMNIBINDER_H
