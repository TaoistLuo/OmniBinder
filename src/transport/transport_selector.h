/**************************************************************************************************
 * @file        transport_selector.h
 * @brief       传输策略选择器
 * @details     替换 TransportFactory 的轻量级传输选择逻辑。同机优先 SHM，
 *              跨机回退 TCP。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-10-15
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
#ifndef OMNIBINDER_TRANSPORT_SELECTOR_H
#define OMNIBINDER_TRANSPORT_SELECTOR_H

#include "omnibinder/transport.h"
#include "omnibinder/types.h"
#include <string>
#include <stdint.h>

namespace omnibinder {

class ITransport;

enum class TransportSelectionPolicy {
    PREFER_SHM,
    USE_TCP
};

TransportSelectionPolicy chooseTransportPolicy(
    const std::string& local_host_id,
    const std::string& remote_host_id);

// 传输选择 — 扩展点。
// 当前: 同机优先 SHM，失败/跨机使用 TCP。
// 如需添加新传输 (如 I2C/UDP/RDMA)，在此函数中扩展。
ITransport* selectTransport(const std::string& service_name,
                            const std::string& host, uint16_t port,
                            const std::string& local_host_id,
                            const std::string& remote_host_id,
                            const ShmConfig& shm_config);

} // namespace omnibinder

#endif
