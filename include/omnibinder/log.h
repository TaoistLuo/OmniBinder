/**************************************************************************************************
 * @file        log.h
 * @brief       简单的日志实现
 * @details     提供轻量级的日志输出功能，支持 DEBUG/INFO/WARN/ERROR 四个级别，
 *              可在运行时通过 setLogLevel() 动态调整。支持可选的时间戳输出。
 *              通过 OMNI_LOG_DEBUG/INFO/WARN/ERROR 宏简化调用，输出到 stderr。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-02-11
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
#ifndef OMNIOMNI_LOG_H
#define OMNIOMNI_LOG_H

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <sys/time.h>

namespace omnibinder {

enum class LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
    LOG_NONE  = 4,
};

// 全局日志级别（可在运行时修改）
inline LogLevel& globalLogLevel() {
    static LogLevel level = LogLevel::LOG_INFO;
    return level;
}

inline void setLogLevel(LogLevel level) {
    globalLogLevel() = level;
}

// 全局时间戳开关（默认关闭，保持向后兼容）
inline bool& globalTimestampEnabled() {
    static bool enabled = true;
    return enabled;
}

inline void enableTimestamp(bool enable) {
    globalTimestampEnabled() = enable;
}

inline const char* logLevelStr(LogLevel level) {
    switch (level) {
    case LogLevel::LOG_DEBUG: return "D";
    case LogLevel::LOG_INFO:  return "I";
    case LogLevel::LOG_WARN:  return "W";
    case LogLevel::LOG_ERROR: return "E";
    default:                  return "?????";
    }
}

inline void logPrint(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level < globalLogLevel()) return;

    if (globalTimestampEnabled()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_buf;
        localtime_r(&tv.tv_sec, &tm_buf);
        fprintf(stderr, "[%04d-%02d-%02d %02d:%02d:%02d.%03d][%s][%s] ",
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                static_cast<int>(tv.tv_usec / 1000),
                logLevelStr(level), tag);
    } else {
        fprintf(stderr, "[%s][%s] ", logLevelStr(level), tag);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

} // namespace omnibinder

#define OMNI_LOG_DEBUG(tag, ...) \
    omnibinder::logPrint(omnibinder::LogLevel::LOG_DEBUG, tag, __VA_ARGS__)
#define OMNI_LOG_INFO(tag, ...)  \
    omnibinder::logPrint(omnibinder::LogLevel::LOG_INFO,  tag, __VA_ARGS__)
#define OMNI_LOG_WARN(tag, ...)  \
    omnibinder::logPrint(omnibinder::LogLevel::LOG_WARN,  tag, __VA_ARGS__)
#define OMNI_LOG_ERROR(tag, ...) \
    omnibinder::logPrint(omnibinder::LogLevel::LOG_ERROR, tag, __VA_ARGS__)

#endif // OMNIOMNI_LOG_H
