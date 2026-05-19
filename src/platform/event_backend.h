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
 * Copyright (c) 2026 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
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
    
    // 初始化后端，返回是否成功
    virtual bool init() = 0;
    
    // 销毁后端
    virtual void destroy() = 0;
    
    // 添加 fd 监听
    // fd: 文件描述符
    // events: 监听的事件类型（EventFlags 组合）
    // 返回: 是否成功
    virtual bool addFd(int fd, uint32_t events) = 0;
    
    // 修改 fd 监听的事件
    // fd: 文件描述符
    // events: 新的事件类型
    // 返回: 是否成功
    virtual bool modifyFd(int fd, uint32_t events) = 0;
    
    // 移除 fd 监听
    // fd: 文件描述符
    // 返回: 是否成功
    virtual bool removeFd(int fd) = 0;
    
    // 等待事件
    // events: 输出就绪事件数组
    // max_events: 数组容量
    // timeout_ms: 超时时间（-1 表示无限等待）
    // 返回: 就绪事件数量，-1 表示错误
    virtual int poll(ReadyEvent* events, int max_events, int timeout_ms) = 0;
};

// ============================================================
// 工厂函数
// ============================================================
// 创建当前平台的事件后端（调用者负责 delete）
EventBackend* createEventBackend();

} // namespace platform
} // namespace omnibinder

#endif // OMNIBINDER_EVENT_BACKEND_H
