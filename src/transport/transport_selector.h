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

class IClientTransport;

enum class TransportSelectionPolicy {
    PREFER_SHM,
    USE_TCP
};

TransportSelectionPolicy chooseTransportPolicy(
    const std::string& local_host_id,
    const std::string& remote_host_id);

/*
 * @brief  传输选择 — 扩展点
 * @param[in]  service_name   目标服务名
 * @param[in]  host           目标主机
 * @param[in]  port           目标端口
 * @param[in]  local_host_id  本机 host_id
 * @param[in]  remote_host_id 远端 host_id
 * @param[in]  shm_config     SHM 容量配置
 * @return 客户端传输实例
 * @note   当前：同机优先 SHM，失败/跨机使用 TCP；如需添加新传输（如 I2C/UDP/RDMA），在此函数中扩展
 */
IClientTransport* createClientTransport(const std::string& service_name,
                            const std::string& host, uint16_t port,
                            const std::string& local_host_id,
                            const std::string& remote_host_id,
                            const ShmConfig& shm_config);

/*
 * @brief  服务端端点选择 — 扩展点
 * @param[in]  service_name 服务名
 * @param[in]  type         传输类型
 * @param[in]  config       端点容量配置
 * @return 未启动的 IServerTransport；调用方负责 start()/close()/delete；NULL 表示该传输不可用
 * @note   TCP：service_name/config 忽略，host/port 由 start() 提供；
 *         SHM：service_name 派生握手路径，config 提供默认 ring 容量
 */
IServerTransport* createServerTransport(const std::string& service_name,
                            TransportType type,
                            const TransportConfig& config);

/*
 * @brief  控制面（ServiceManager）通道专用传输：始终 TCP，不做 SHM 选择
 * @param[in]  host    目标 SM 地址
 * @param[in]  port    目标 SM 端口
 * @param[out] out_err 失败时的错误码（ERR_SM_UNREACHABLE / ERR_TIMEOUT）
 * @return 连接成功的传输实例；失败返回 NULL
 * @note   保留 sm_connect_failed / sm_connect_timeout 日志关键词与错误码语义
 */
IClientTransport* createControlTransport(const std::string& host, uint16_t port,
                                         int& out_err);

} // namespace omnibinder

#endif
