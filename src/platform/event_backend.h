/**************************************************************************************************
 * @file        event_backend.h
 * @brief       事件循环后端抽象接口
 * @details     定义事件循环后端的平台无关接口，用于替代 EventLoop 中直接使用 epoll/select。
 *              各平台提供具体实现：
 *              - Linux: epoll
 *              - Windows: select
 *              - macOS/BSD: kqueue（未来扩展）
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-05-19
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
#ifndef OMNIBINDER_EVENT_BACKEND_H
#define OMNIBINDER_EVENT_BACKEND_H

#include <stdint.h>

namespace omnibinder {
namespace platform {

// ============================================================
// 事件标志（平台无关）
// ============================================================
enum EventFlags : uint32_t {
    EVENT_READ  = 0x01,
    EVENT_WRITE = 0x02,
    EVENT_ERROR = 0x04,
};

// ============================================================
// 就绪事件
// ============================================================
struct ReadyEvent {
    int      fd;
    uint32_t events;  // EventFlags 组合
};

// ============================================================
// 事件循环后端接口
// ============================================================
class EventBackend {
public:
    virtual ~EventBackend() {}
    
    /*
     * @brief  初始化后端
     * @return true 成功；false 失败
     */
    virtual bool init() = 0;
    
    /*
     * @brief  销毁后端
     */
    virtual void destroy() = 0;
    
    /*
     * @brief  添加 fd 监听
     * @param[in]  fd     文件描述符
     * @param[in]  events 监听的事件类型（EventFlags 组合）
     * @return true 成功；false 失败
     */
    virtual bool addFd(int fd, uint32_t events) = 0;
    
    /*
     * @brief  修改 fd 监听的事件
     * @param[in]  fd     文件描述符
     * @param[in]  events 新的事件类型
     * @return true 成功；false 失败
     */
    virtual bool modifyFd(int fd, uint32_t events) = 0;
    
    /*
     * @brief  移除 fd 监听
     * @param[in]  fd 文件描述符
     * @return true 成功；false 失败
     */
    virtual bool removeFd(int fd) = 0;
    
    /*
     * @brief  等待事件
     * @param[out] events     输出就绪事件数组
     * @param[in]  max_events 数组容量
     * @param[in]  timeout_ms 超时时间（-1 表示无限等待）
     * @return 就绪事件数量，-1 表示错误
     */
    virtual int poll(ReadyEvent* events, int max_events, int timeout_ms) = 0;
};

// ============================================================
// 工厂函数
// ============================================================
/*
 * @brief  创建当前平台的事件后端
 * @return EventBackend 实例（调用者负责 delete）
 */
EventBackend* createEventBackend();

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_EVENT_BACKEND_H
