/**************************************************************************************************
 * @file        transport_factory.h
 * @brief       传输层工厂
 * @details     根据通信双方的 host_id 自动选择最优传输方式：同机使用 SHM（共享内存），
 *              跨机使用 TCP。提供单例接口，支持创建客户端传输、服务端传输，
 *              以及强制指定传输类型。
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
#ifndef OMNIBINDER_TRANSPORT_FACTORY_H
#define OMNIBINDER_TRANSPORT_FACTORY_H

#include "omnibinder/transport.h"
#include <string>
#include <map>

namespace omnibinder {

// ============================================================
// 传输层工厂 - 自动选择合适的传输方式
// ============================================================
class TransportFactory {
public:
    // 获取单例
    static TransportFactory& instance();

    // 创建客户端传输
    // 根据 host_id 自动选择：同机用 SHM，跨机用 TCP
    ITransport* createTransport(const std::string& local_host_id,
                                const std::string& remote_host_id);

    // 选择优先传输类型：同机且存在 shm_name 时优先 SHM，否则 TCP
    static TransportType selectPreferredTransport(const std::string& local_host_id,
                                                  const std::string& remote_host_id,
                                                  const std::string& shm_name);

    // 创建服务端传输
    ITransportServer* createServer(TransportType type);

    // 强制创建指定类型的传输
    ITransport* createTransport(TransportType type);

    // 判断是否同机
    static bool isSameMachine(const std::string& local_host_id,
                              const std::string& remote_host_id);

private:
    TransportFactory();
    ~TransportFactory();

    // 禁止拷贝
    TransportFactory(const TransportFactory&);
    TransportFactory& operator=(const TransportFactory&);
};

} // namespace omnibinder

#endif // OMNIBINDER_TRANSPORT_FACTORY_H
