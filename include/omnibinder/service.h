/**************************************************************************************************
 * @file        service.h
 * @brief       服务定义
 * @details     定义服务端基类 Service，所有由 omni-idlc 生成的 Stub 类均继承自此类。
 *              提供服务名称、端口、接口信息等元数据访问，以及 onInvoke() 方法分发、
 *              onStart/onStop 生命周期回调和客户端连接/断开通知等虚函数接口。
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

    /* @brief 服务名称（构造时传入）
     * @return 服务名字符串引用 */
    const std::string& name() const;
    /* @brief 服务数据面监听端口（注册成功后由 runtime 回填）
     * @return 监听端口，未注册时为 0 */
    uint16_t port() const;
    /* @brief 设置服务注册到 ServiceManager 时对外通告的可达地址
     * @param[in] host 通告地址（IP 或域名） */
    void setRegisterHost(const std::string& host);
    /* @brief 获取服务注册的对外通告地址
     * @return 通告地址字符串引用 */
    const std::string& getRegisterHost() const;
    /* @brief 设置服务级 SHM ring 容量
     * @param[in] config request / response 两段 ring 的容量 */
    void setShmConfig(const ShmConfig& config);
    /* @brief 获取服务级 SHM ring 容量
     * @return SHM 容量配置 */
    ShmConfig shmConfig() const;
    /* @brief 设置实例标识（同名服务多实例时用于区分）
     * @param[in]  instance_id 实例标识；空串表示该板上该服务的单实例（默认）
     * @note   需在 registerService() 之前调用方生效；实例身份参与注册与 SHM 命名派生 */
    void setInstanceId(const std::string& instance_id);
    /* @brief 获取实例标识
     * @return 实例标识；未设置时返回空串 */
    const std::string& instanceId() const;
    /* @brief 服务名称（默认取自构造时传入的名称，子类可覆盖）
     * @return 服务的 C 字符串名称
     * @note   运行时注册/发现统一使用 name()；本方法为兼容与外部识别保留，
     *         默认实现即返回 name()，子类无需再重复实现 */
    virtual const char* serviceName() const;
    /* @brief 返回本服务接口元信息（由 IDL 生成的 Stub 子类提供）
     * @return 接口信息（接口 ID、名称、方法列表） */
    virtual const InterfaceInfo& interfaceInfo() const = 0;

protected:
    virtual int onInvoke(uint32_t method_id, const Buffer& request, Buffer& response) = 0;
    virtual void onStart();
    virtual void onStop();
    virtual void onClientConnected(const std::string& client_info);
    virtual void onClientDisconnected(const std::string& client_info);

    OmniRuntime* runtime() const;

    friend class OmniRuntime;

private:
    Service(const Service&);
    Service& operator=(const Service&);

    // 由 runtime 注册成功后回填实际监听端口（内部行为，不属于用户 API）
    void setPort(uint16_t p);

    std::string name_;
    uint16_t port_;
    std::string register_host_;
    std::string instance_id_;
    ShmConfig shm_config_;
    OmniRuntime* runtime_;
};

} // namespace omnibinder

#endif // OMNIBINDER_SERVICE_H
