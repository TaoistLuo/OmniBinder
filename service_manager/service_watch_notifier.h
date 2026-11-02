/**************************************************************************************************
 * @file        service_watch_notifier.h
 * @brief       服务状态关注关系管理器
 * @details     管理客户端对服务实例上线/离线事件的持久订阅。订阅按 watch_id 分组，
 *              每组可包含多个 (service_name, instance_id) 逻辑过滤条件；空
 *              instance_id 表示通配。host_id 仅为兼容字段，不参与匹配。
 *              仅 ServiceManager owner 线程访问。
 *************************************************************************************************/
#ifndef OMNIBINDER_SERVICE_WATCH_NOTIFIER_H
#define OMNIBINDER_SERVICE_WATCH_NOTIFIER_H

#include "omnibinder/types.h"

#include <stdint.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace omnibinder {

struct ServiceWatchFilter {
    std::string service_name;
    std::string host_id;      // 兼容字段，不参与逻辑匹配
    std::string instance_id;  // 空串表示该服务下全部实例

    ServiceWatchFilter() {}
    ServiceWatchFilter(const std::string& name, const std::string& host,
                       const std::string& instance)
        : service_name(name), host_id(host), instance_id(instance) {}
};

struct ServiceWatchGroup {
    int subscriber_fd;
    uint64_t watch_id;
    std::vector<ServiceWatchFilter> filters;

    ServiceWatchGroup() : subscriber_fd(-1), watch_id(0) {}
};

class ServiceWatchNotifier {
public:
    ServiceWatchNotifier();
    ~ServiceWatchNotifier();

    ServiceWatchNotifier(const ServiceWatchNotifier&) = delete;
    ServiceWatchNotifier& operator=(const ServiceWatchNotifier&) = delete;

    bool subscribe(int subscriber_fd, uint64_t watch_id,
                   const std::vector<ServiceWatchFilter>& filters);
    bool unsubscribe(int subscriber_fd, uint64_t watch_id);
    void removeSubscriber(int subscriber_fd);

    /*
     * @brief  返回所有匹配该实例事件的 watch group
     * @note   同一 group 内多个 filter 重叠时只返回一次；调用方负责按 fd 重新
     *         校验连接存活（发送失败可能重入 closeClient）。
     */
    std::vector<ServiceWatchGroup> matching(const ServiceIdentity& identity) const;

    static bool matches(const ServiceWatchFilter& filter, const ServiceIdentity& identity);
    static bool overlaps(const ServiceWatchFilter& a, const ServiceWatchFilter& b);

private:
    struct WatchKey {
        int fd;
        uint64_t watch_id;

        WatchKey() : fd(-1), watch_id(0) {}
        WatchKey(int f, uint64_t id) : fd(f), watch_id(id) {}

        bool operator<(const WatchKey& other) const {
            if (fd != other.fd) return fd < other.fd;
            return watch_id < other.watch_id;
        }
    };

    std::map<WatchKey, ServiceWatchGroup> groups_;
    std::map<std::string, std::set<WatchKey> > service_index_;
    std::map<int, std::set<uint64_t> > subscriber_index_;

    void eraseGroup(std::map<WatchKey, ServiceWatchGroup>::iterator it);
};

} // namespace omnibinder

#endif // OMNIBINDER_SERVICE_WATCH_NOTIFIER_H
