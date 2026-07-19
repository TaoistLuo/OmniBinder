/**************************************************************************************************
 * @file        log.h
 * @brief       轻量级 C/C++ 双兼容日志
 * @details     C 和 C++ 均可用。性能关键：级别检查在宏中展开为直接全局变量比较，过滤日志零开销。
 *              支持 FATAL/ERROR/WARN/INFO/DEBUG/VERBOSE 六个级别，运行时动态调整。
 *
 * @author      taoist.luo
 * @version     2.0.0
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
#ifndef OMNIBINDER_LOG_H
#define OMNIBINDER_LOG_H

#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 日志级别 (C enum, C++ 也有对应的常量)
 * ============================================================ */
typedef enum {
    OMNI_LOG_FATAL   = 0,  /* 致命错误 */
    OMNI_LOG_ERROR   = 1,  /* 功能错误 */
    OMNI_LOG_WARN    = 2,  /* 告警 */
    OMNI_LOG_INFO    = 3,  /* 重要信息 */
    OMNI_LOG_DEBUG   = 4,  /* 调试信息 */
    OMNI_LOG_VERBOSE = 5,  /* 详细日志 */
    OMNI_LOG_OFF     = 6   /* 关闭日志 (仅 set, 不用于打印) */
} omni_log_level_t;

/* ============================================================
 * 全局状态 — 直接可读，宏内零开销过滤
 * ============================================================ */
extern omni_log_level_t g_omni_log_level;       /* 当前日志级别 */
extern int              g_omni_log_timestamp;    /* 时间戳开关 (1=开 0=关) */

/* ============================================================
 * C API (C++ 也通过宏调用这些函数)
 * ============================================================ */
void omni_log_set_level(omni_log_level_t level);
void omni_log_enable_timestamp(int enable);
const char* omni_log_level_str(omni_log_level_t level);

/* 格式化打印（变参版本，供上层封装） */
void omni_log_vprint(omni_log_level_t level, const char* tag,
                     const char* fmt, va_list args);

/* 格式化打印 */
void omni_log_print(omni_log_level_t level, const char* tag,
                    const char* fmt, ...);

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ============================================================
 * 日志宏 — C 和 C++ 均可使用
 *
 * 性能关键：级别检查展开为直接全局变量比较，无函数调用开销。
 * 仅当日志级别足够低时才进入 omni_log_print() 进行格式化。
 * ============================================================ */
/* ##__VA_ARGS__ (GNU extension): 当 __VA_ARGS__ 为空时移除前置逗号，
 * 允许 OMNI_LOG_INFO("tag", "fmt") 无额外变参的写法。 */
#define OMNI_LOG_FATAL(tag, ...) \
    do { if (OMNI_LOG_FATAL <= g_omni_log_level) \
        omni_log_print(OMNI_LOG_FATAL, tag, ##__VA_ARGS__); } while(0)

#define OMNI_LOG_ERROR(tag, ...) \
    do { if (OMNI_LOG_ERROR <= g_omni_log_level) \
        omni_log_print(OMNI_LOG_ERROR, tag, ##__VA_ARGS__); } while(0)

#define OMNI_LOG_WARN(tag, ...) \
    do { if (OMNI_LOG_WARN <= g_omni_log_level) \
        omni_log_print(OMNI_LOG_WARN,  tag, ##__VA_ARGS__); } while(0)

#define OMNI_LOG_INFO(tag, ...) \
    do { if (OMNI_LOG_INFO <= g_omni_log_level) \
        omni_log_print(OMNI_LOG_INFO,  tag, ##__VA_ARGS__); } while(0)

#define OMNI_LOG_DEBUG(tag, ...) \
    do { if (OMNI_LOG_DEBUG <= g_omni_log_level) \
        omni_log_print(OMNI_LOG_DEBUG, tag, ##__VA_ARGS__); } while(0)

#define OMNI_LOG_VERBOSE(tag, ...) \
    do { if (OMNI_LOG_VERBOSE <= g_omni_log_level) \
        omni_log_print(OMNI_LOG_VERBOSE, tag, ##__VA_ARGS__); } while(0)

/* ============================================================
 * C++ 兼容层 — 保留原有 namespace + enum class 风格
 *
 * 以下代码仅在 C++ 编译时生效。所有包装函数均为 inline，编译后
 * 与直接使用 C API 完全一致（零额外开销）。
 * ============================================================ */
#ifdef __cplusplus

namespace omnibinder {

/* 类型别名，与旧代码兼容 */
using LogLevel = omni_log_level_t;

/* 常量别名 */
constexpr LogLevel LOG_FATAL   = OMNI_LOG_FATAL;
constexpr LogLevel LOG_ERROR   = OMNI_LOG_ERROR;
constexpr LogLevel LOG_WARN    = OMNI_LOG_WARN;
constexpr LogLevel LOG_INFO    = OMNI_LOG_INFO;
constexpr LogLevel LOG_DEBUG   = OMNI_LOG_DEBUG;
constexpr LogLevel LOG_VERBOSE = OMNI_LOG_VERBOSE;
constexpr LogLevel LOG_OFF     = OMNI_LOG_OFF;

/* 与旧实现兼容的静态级别访问器 */
inline LogLevel& globalLogLevel() {
    return g_omni_log_level;
}

inline const char* logLevelStr(LogLevel level) {
    return omni_log_level_str(level);
}

/* 包装函数 — 编译后与直接调用 C API 无差异 */
inline void setLogLevel(LogLevel level) {
    omni_log_set_level(level);
}

inline void enableTimestamp(bool enable) {
    omni_log_enable_timestamp(enable ? 1 : 0);
}

void logPrint(LogLevel level, const char* tag, const char* fmt, ...);

} /* namespace omnibinder */

#endif /* __cplusplus */

#endif /* OMNIBINDER_LOG_H */
