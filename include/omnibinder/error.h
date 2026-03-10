/**************************************************************************************************
 * @file        error.h
 * @brief       全局错误码定义
 * @details     定义 OmniBinder 框架中所有模块共用的错误码枚举（ErrorCode），
 *              按功能分段：通用错误(1-99)、网络错误(100-199)、服务错误(200-299)、
 *              话题错误(300-399)、传输层错误(400-499)、序列化错误(500-599)。
 *              同时提供错误码转字符串和成功判断的辅助函数。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
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
#ifndef OMNIBINDER_ERROR_H
#define OMNIBINDER_ERROR_H

#include <stdint.h>

namespace omnibinder {

// ============================================================
// 错误码定义
// ============================================================
enum class ErrorCode : int32_t {
    // 成功
    OK                      = 0,

    // 通用错误 (1-99)
    ERR_UNKNOWN             = -1,
    ERR_INVALID_PARAM       = -2,
    ERR_OUT_OF_MEMORY       = -3,
    ERR_TIMEOUT             = -4,
    ERR_NOT_INITIALIZED     = -5,
    ERR_ALREADY_INITIALIZED = -6,
    ERR_NOT_SUPPORTED       = -7,
    ERR_INTERNAL            = -8,

    // 网络错误 (100-199)
    ERR_CONNECT_FAILED      = -100,
    ERR_CONNECTION_CLOSED   = -101,
    ERR_SEND_FAILED         = -102,
    ERR_RECV_FAILED         = -103,
    ERR_PROTOCOL_ERROR      = -104,
    ERR_SM_UNREACHABLE      = -105,
    ERR_BIND_FAILED         = -106,
    ERR_LISTEN_FAILED       = -107,
    ERR_ACCEPT_FAILED       = -108,

    // 服务错误 (200-299)
    ERR_SERVICE_NOT_FOUND   = -200,
    ERR_SERVICE_EXISTS      = -201,
    ERR_SERVICE_OFFLINE     = -202,
    ERR_INTERFACE_NOT_FOUND = -203,
    ERR_METHOD_NOT_FOUND    = -204,
    ERR_INVOKE_FAILED       = -205,
    ERR_REGISTER_FAILED     = -206,
    ERR_UNREGISTER_FAILED   = -207,

    // 话题错误 (300-399)
    ERR_TOPIC_NOT_FOUND     = -300,
    ERR_TOPIC_EXISTS        = -301,
    ERR_NOT_SUBSCRIBED      = -302,
    ERR_NOT_PUBLISHER       = -303,

    // 传输层错误 (400-499)
    ERR_TRANSPORT_INIT      = -400,
    ERR_SHM_CREATE          = -401,
    ERR_SHM_ATTACH          = -402,
    ERR_SHM_FULL            = -403,
    ERR_SHM_TIMEOUT         = -404,

    // 序列化错误 (500-599)
    ERR_SERIALIZE           = -500,
    ERR_DESERIALIZE         = -501,
    ERR_BUFFER_OVERFLOW     = -502,
    ERR_BUFFER_UNDERFLOW    = -503,
};

// 错误码转字符串
const char* errorCodeToString(ErrorCode code);

// 判断是否成功
inline bool isSuccess(ErrorCode code) {
    return code == ErrorCode::OK;
}

inline bool isSuccess(int code) {
    return code == 0;
}

} // namespace omnibinder

#endif // OMNIBINDER_ERROR_H
