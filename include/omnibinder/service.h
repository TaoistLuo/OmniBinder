/**************************************************************************************************
 * @file        service.h
 * @brief       服务定义
 * @details     定义服务端基类 Service，所有由 omni-idlc 生成的 Stub 类均继承自此类。
 *              提供服务名称、端口、接口信息等元数据访问，以及 onInvoke() 方法分发、
 *              onStart/onStop 生命周期回调和客户端连接/断开通知等虚函数接口。
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
#ifndef OMNIBINDER_SERVICE_H
#define OMNIBINDER_SERVICE_H

#include "omnibinder/types.h"
#include "omnibinder/buffer.h"
#include <string>

namespace omnibinder {

class OmniRuntime;

class Service {
public:
    explicit Service(const std::string& name);
    virtual ~Service();

    const std::string& name() const;
    uint16_t port() const;
    void setPort(uint16_t p);
    void setShmConfig(const ShmConfig& config);
    ShmConfig shmConfig() const;

    virtual const char* serviceName() const = 0;
    virtual const InterfaceInfo& interfaceInfo() const = 0;

protected:
    virtual void onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) = 0;
    virtual void onStart();
    virtual void onStop();
    virtual void onClientConnected(const std::string& client_info);
    virtual void onClientDisconnected(const std::string& client_info);

    OmniRuntime* runtime() const;

    friend class OmniRuntime;

private:
    Service(const Service&);
    Service& operator=(const Service&);

    std::string name_;
    uint16_t port_;
    ShmConfig shm_config_;
    OmniRuntime* runtime_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SERVICE_H
