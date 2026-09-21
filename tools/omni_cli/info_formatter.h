/**************************************************************************************************
 * @file        info_formatter.h
 * @brief       服务信息格式化输出与实例选择
 * @details     omni-cli list / info / call 命令的辅助。将 ServiceInfo 结构体渲染为
 *              可读的终端文本输出（支持字段名转义），并按 (host_id, instance_id)
 *              从同名多实例中解析唯一目标实例。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-03-15
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
#ifndef OMNI_CLI_INFO_FORMATTER_H
#define OMNI_CLI_INFO_FORMATTER_H

#include <stdint.h>
#include <string>
#include <vector>

namespace omni_cli {

std::string escapeForTerminal(const std::string& input);

std::string formatPublishedTopicsSection(const std::vector<std::string>& topics,
                                         const std::string& unavailable_reason);

// ============================================================
// 多实例服务身份：list / info / call 的展示与选择
// ============================================================
// 服务身份 = (service_name, host_id, instance_id)。同名服务可存在多个实例；
// instance_id 为空串表示该 host_id 上该服务的默认（单）实例，仅是多实例模型
// 的一个取值，不是独立格式。以下辅助函数与 omnibinder::ServiceInfo 解耦，
// 便于脱离 ServiceManager 做确定性单元测试。

/* @brief 单个服务实例的可展示字段 */
struct ServiceInstanceView {
    std::string name;         // 服务名
    std::string host_id;      // 板级主机身份
    std::string instance_id;  // 实例标识（空串 = 默认实例）
    std::string host;         // 数据面直连地址
    uint16_t    port;         // 数据面直连端口

    ServiceInstanceView() : port(0) {}
};

/* @brief 实例选择器：对应命令行 --instance-id / --host-id 的显式选择
 * @note  记录字段是否被显式提供，以区分"未指定实例"与"指定空 instance_id"（默认实例）。 */
struct InstanceSelector {
    bool        host_id_set;      // 是否显式提供 --host-id
    bool        instance_id_set;  // 是否显式提供 --instance-id
    std::string host_id;
    std::string instance_id;

    InstanceSelector() : host_id_set(false), instance_id_set(false) {}
};

/* @brief 渲染实例字段：空 instance_id 显示为 (default) */
std::string displayInstanceId(const std::string& instance_id);

/* @brief 渲染实例列表表头（含列名与分隔线）
 * @return 以换行结尾的表头文本 */
std::string formatInstanceTableHeader();

/* @brief 渲染一个实例列表数据行
 * @param[in] instance 实例视图
 * @return 以换行结尾的一行；字段经终端转义，列对齐 */
std::string formatInstanceListRow(const ServiceInstanceView& instance);

/* @brief 渲染实例列表（表头 + 各行）
 * @param[in] instances 实例列表（可为空，此时仅返回表头）
 * @return 完整表格文本 */
std::string formatInstanceTable(const std::vector<ServiceInstanceView>& instances);

/* @brief 从同名多实例中解析唯一目标实例
 * @param[in]  instances 候选实例列表（调用方保证非空）
 * @param[in]  selector  显式选择；未指定任何字段时选中确定性第一实例
 * @param[out] selected  解析出的目标实例
 * @param[out] error     失败时的中文错误说明
 * @return 0 成功；1 无匹配；2 匹配到多个（需补充选择条件）
 * @note   指定 instance_id 而未指定 host_id 且跨 host_id 命中多个实例时视为歧义；
 *         未指定任何字段时返回 instances[0]，由调用方在输出中显式说明。 */
int selectInstance(const std::vector<ServiceInstanceView>& instances,
                   const InstanceSelector& selector,
                   ServiceInstanceView& selected,
                   std::string& error);

/* @brief 渲染 info 详情中的实例身份行
 * @return 形如 "Instance: instance_id=\"...\" host_id=\"...\" endpoint=host:port" */
std::string formatInstanceIdentity(const ServiceInstanceView& instance);

/* @brief 渲染 info 在"多实例但未选择"时的提示
 * @return 提示文本（以换行结尾），要求用户用 --instance-id / --host-id 指定实例 */
std::string formatInstanceSelectionHint();

/* @brief 渲染 call 命令的目标实例说明
 * @param[in] instance          实际调用的目标实例
 * @param[in] explicit_selection 是否由 --instance-id/--host-id 显式指定
 * @return 显式时以 "Target instance:" 开头；未指定时说明将使用确定性第一实例并给出提示 */
std::string formatCallTargetNotice(const ServiceInstanceView& instance,
                                   bool explicit_selection);

} // namespace omni_cli

#endif // OMNI_CLI_INFO_FORMATTER_H
