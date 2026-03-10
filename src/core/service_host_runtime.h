#ifndef OMNIBINDER_CORE_SERVICE_HOST_RUNTIME_H
#define OMNIBINDER_CORE_SERVICE_HOST_RUNTIME_H

#include "core/event_loop.h"
#include "core/topic_runtime.h"
#include "omnibinder/message.h"
#include "omnibinder/transport.h"
#include <functional>
#include <map>
#include <string>

namespace omnibinder {

struct LocalServiceEntry;

class ServiceHostRuntime {
public:
    typedef std::function<void(const std::string&, int, uint32_t)> ClientDataHandler;
    typedef std::function<void(const std::string&, int, const Message&)> InvokeHandler;
    typedef std::function<void(const std::string&, const Message&)> InvokeOneWayHandler;
    typedef std::function<void(int, const Message&)> SubscribeBroadcastHandler;
    typedef std::function<void(const std::string&, uint32_t, const Message&)> ShmInvokeHandler;
    typedef std::function<void(const std::string&, const Message&)> ShmInvokeOneWayHandler;
    typedef std::function<void(const std::string&, uint32_t, const Message&)> ShmSubscribeBroadcastHandler;
    typedef std::function<void(const std::string&, int)> DisconnectHandler;
    typedef std::function<void(LocalServiceEntry*, int)> ClientConnectedHook;
    typedef std::function<void(LocalServiceEntry*, int)> ClientDisconnectedHook;

    void onServiceAccept(const std::string& service_name,
                         LocalServiceEntry* entry,
                         EventLoop* loop,
                         std::map<int, std::string>& client_fd_to_service,
                         const ClientDataHandler& on_client_data,
                         const ClientConnectedHook& on_client_connected) const;

    void onServiceClientData(const std::string& service_name,
                             LocalServiceEntry* entry,
                             int client_fd,
                             TopicRuntime& topic_runtime,
                             EventLoop* loop,
                             std::map<int, std::string>& client_fd_to_service,
                             const InvokeHandler& on_invoke,
                             const InvokeOneWayHandler& on_invoke_oneway,
                             const SubscribeBroadcastHandler& on_subscribe_broadcast,
                             const DisconnectHandler& on_disconnect,
                             const ClientDisconnectedHook& on_client_disconnected) const;

    void onShmRequest(const std::string& service_name,
                      uint32_t client_id,
                      const uint8_t* data,
                      size_t length,
                      TopicRuntime& topic_runtime,
                      const ShmInvokeHandler& on_invoke,
                      const ShmInvokeOneWayHandler& on_invoke_oneway,
                      const ShmSubscribeBroadcastHandler& on_subscribe_broadcast) const;

private:
    void processServiceClientMessages(const std::string& service_name,
                                      LocalServiceEntry* entry,
                                      int client_fd,
                                      TopicRuntime& topic_runtime,
                                      const InvokeHandler& on_invoke,
                                      const InvokeOneWayHandler& on_invoke_oneway,
                                      const SubscribeBroadcastHandler& on_subscribe_broadcast) const;
};

} // namespace omnibinder

#endif
