/**************************************************************************************************
 * @file        omnibinder_c_view.h
 * @brief       C 零拷贝只读视图（内部/辅助接口）
 * @details     提供 omni_buffer_view_t 及其读取函数。该接口属于运行时协议解析辅助能力，
 *              不属于生成的业务 API；生成的业务头文件不会暴露它。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-10-31
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *************************************************************************************************/
#ifndef OMNIBINDER_C_VIEW_H
#define OMNIBINDER_C_VIEW_H

#include <omnibinder/omnibinder_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/* @brief 非拥有只读字节视图；用于从 message payload 直接解析字段。
 * @note  view 只保存 data/size/pos，不分配、不释放；data 生命周期由调用方保证。 */
typedef struct omni_buffer_view_t {
    const uint8_t* data;
    size_t         size;
    size_t         pos;
    int            ok;
} omni_buffer_view_t;

/* @brief 从外部字节初始化 view（不拷贝） */
void omni_buffer_view_init(omni_buffer_view_t* view, const uint8_t* data, size_t len);
/* @brief 从 message/buffer 初始化 view（不拷贝） */
void omni_buffer_view_from_buffer(omni_buffer_view_t* view, const omni_buffer_t* buf);

/* @brief 读取基础类型；失败时 ok=0，pos 不前进 */
uint8_t  omni_buffer_view_read_bool(omni_buffer_view_t* view);
int8_t   omni_buffer_view_read_int8(omni_buffer_view_t* view);
uint8_t  omni_buffer_view_read_uint8(omni_buffer_view_t* view);
int16_t  omni_buffer_view_read_int16(omni_buffer_view_t* view);
uint16_t omni_buffer_view_read_uint16(omni_buffer_view_t* view);
int32_t  omni_buffer_view_read_int32(omni_buffer_view_t* view);
uint32_t omni_buffer_view_read_uint32(omni_buffer_view_t* view);
int64_t  omni_buffer_view_read_int64(omni_buffer_view_t* view);
uint64_t omni_buffer_view_read_uint64(omni_buffer_view_t* view);
float    omni_buffer_view_read_float32(omni_buffer_view_t* view);
double   omni_buffer_view_read_float64(omni_buffer_view_t* view);

/* @brief 读取 uint32 长度前缀 + 字符串；返回 payload 内指针（不分配）
 * @param[out] out_len 字符串长度；可为 NULL
 * @return 指向 payload 的指针；失败返回 NULL（pos 不前进）。长度 0 时返回当前位置指针。 */
const char* omni_buffer_view_read_string(omni_buffer_view_t* view, uint32_t* out_len);
/* @brief 读取 uint32 长度前缀 + bytes；返回 payload 内指针（不分配）
 * @param[out] out_len 字节长度；可为 NULL
 * @return 指向 payload 的指针；失败返回 NULL（pos 不前进）。长度 0 时返回当前位置指针。 */
const uint8_t* omni_buffer_view_read_bytes(omni_buffer_view_t* view, uint32_t* out_len);

int    omni_buffer_view_ok(const omni_buffer_view_t* view);
size_t omni_buffer_view_position(const omni_buffer_view_t* view);
int    omni_buffer_view_set_position(omni_buffer_view_t* view, size_t pos);


#ifdef __cplusplus
}
#endif

#endif /* OMNIBINDER_C_VIEW_H */
