/**************************************************************************************************
 * @file        result.h
 * @brief       C++11 调用结果与错误码
 * @details     用于生成 Proxy 的非 void 方法：返回值与错误码绑定，避免依赖 per-proxy lastError。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-10-31
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 * MIT License
 *************************************************************************************************/
#ifndef OMNIBINDER_RESULT_H
#define OMNIBINDER_RESULT_H

#include "omnibinder/error.h"
#include <utility>

namespace omnibinder {

/* @brief 调用结果：成功时 value 有效，失败时 error() 返回错误码
 * @tparam T 返回值类型（需可默认构造）
 * @note 失败时 value() 返回默认值，调用方应先判断 ok()/operator bool。 */
template <typename T>
class Result {
public:
    Result() : error_(static_cast<int>(ErrorCode::ERR_UNKNOWN)), value_() {}
    explicit Result(int error) : error_(error), value_() {}
    Result(int error, T value) : error_(error), value_(std::move(value)) {}

    bool ok() const { return error_ == 0; }
    explicit operator bool() const { return ok(); }
    int error() const { return error_; }

    T& value() { return value_; }
    const T& value() const { return value_; }

private:
    int error_;
    T value_;
};

} // namespace omnibinder

#endif // OMNIBINDER_RESULT_H
