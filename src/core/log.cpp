/**
 * @file        log.c
 * @brief       日志实现 (C++ 编译, C 链接)
 * @details     omni_log_print / omni_log_vprint 的实现。
 *              使用 POSIX gettimeofday 获取时间戳，避免 C++ chrono 依赖。
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
 */
#include "omnibinder/log.h"

#include <time.h>
#include <sys/time.h>

/* ============================================================
 * 全局状态 — 定义
 * ============================================================ */
omni_log_level_t g_omni_log_level   = OMNI_LOG_INFO;
int              g_omni_log_timestamp = 1;

/* ============================================================
 * 设置函数
 * ============================================================ */
void omni_log_set_level(omni_log_level_t level) {
    g_omni_log_level = level;
}

void omni_log_enable_timestamp(int enable) {
    g_omni_log_timestamp = enable ? 1 : 0;
}

/* ============================================================
 * 级别字符串
 * ============================================================ */
const char* omni_log_level_str(omni_log_level_t level) {
    switch (level) {
    case OMNI_LOG_FATAL:   return "F";
    case OMNI_LOG_ERROR:   return "E";
    case OMNI_LOG_WARN:    return "W";
    case OMNI_LOG_INFO:    return "I";
    case OMNI_LOG_DEBUG:   return "D";
    case OMNI_LOG_VERBOSE: return "V";
    default:               return "?";
    }
}

/* ============================================================
 * 变参打印 — 实际输出函数
 * ============================================================ */
void omni_log_vprint(omni_log_level_t level, const char* tag,
                     const char* fmt, va_list args) {
    if (level > g_omni_log_level) return;

    if (g_omni_log_timestamp) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_buf;
        localtime_r(&tv.tv_sec, &tm_buf);
        fprintf(stderr, "[%04d-%02d-%02d %02d:%02d:%02d.%03d][%s][%s] ",
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                (int)(tv.tv_usec / 1000),
                omni_log_level_str(level), tag);
    } else {
        fprintf(stderr, "[%s][%s] ", omni_log_level_str(level), tag);
    }

    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ============================================================
 * 格式化打印 — 宏调用的入口
 * ============================================================ */
void omni_log_print(omni_log_level_t level, const char* tag,
                    const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    omni_log_vprint(level, tag, fmt, args);
    va_end(args);
}
