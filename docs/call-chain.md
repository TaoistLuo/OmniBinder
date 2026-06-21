# 调用链路详解

本文档描述 OmniBinder 两条核心数据通路的完整调用链路：**同步 RPC** 和 **发布/订阅（Topic）**。

文档覆盖客户端、服务端、ServiceManager 三侧的代码级调用流程，以及传输层（SHM / TCP）的选择时机。

---

## 目录

- [全局概览](#全局概览)
- [同步 RPC 调用链路](#同步-rpc-调用链路)
  - [客户端侧：发起调用](#客户端侧发起调用)
  - [服务端侧：处理请求](#服务端侧处理请求)
  - [客户端侧：收到回复](#客户端侧收到回复)
- [One-Way RPC 调用链路](#one-way-rpc-调用链路)
- [发布/订阅调用链路](#发布订阅调用链路)
  - [Phase 1：发布者注册 Topic](#phase-1发布者注册-topic)
  - [Phase 2：订阅者订阅 Topic](#phase-2订阅者订阅-topic)
  - [Phase 3：订阅者收到通知并建立直连](#phase-3订阅者收到通知并建立直连)
  - [Phase 4：发布者广播数据](#phase-4发布者广播数据)
  - [订阅者收到广播](#订阅者收到广播)
- [传输层选择机制](#传输层选择机制)
- [关键代码文件索引](#关键代码文件索引)

---

## 全局概览

```
                    ┌─────────────────────────────────────────────────────────┐
                    │                    ServiceManager                        │
                    │          注册/发现/心跳/死亡通知/Topic 关系管理            │
                    └───┬──────────▲──────────────┬──────────▲────────────────┘
                        │          │              │          │
              TCP 控制面 │          │    TCP 控制面 │          │
            (注册/查找)  │          │  (注册/查找)  │          │
                        │          │              │          │
┌───────────────────────┴──┐     ┌──┴───────────────────────┴──────────────┐
│      客户端进程           │     │           服务端进程                      │
│                          │     │                                          │
│  invoke()                │     │  registerService()                       │
│    │                     │     │    │ listen TCP + 创建 SHM               │
│    ├─ lookup → SM ───────┼────►│    │ 注册到 SM                            │
│    │  (拿到 host/port/   │     │    ▼                                      │
  │    │   host_id) │     │  等待连接                                 │
│    │                     │     │    │                                      │
│    ├─ 同机? SHM 直连 ────┼────►│    │◄── accept TCP / SHM eventfd         │
│    │  跨机? TCP 直连 ────┼────►│    │                                      │
│    │                     │     │    ▼                                      │
│    ├─ send MSG_INVOKE ───┼────►│  onServiceClientData / SHM eventfd       │
│    │                     │     │    │                                      │
│    ├─ waitReply ─────────┼─────┼───┤  decode → service->onInvoke()         │
│    │                     │     │    │  encode reply → sendOnFd ─────────┐   │
│    ◄── reply ◄───────────┼─────┼───┘                                  │   │
│                          │     │                                       │   │
│  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ │     │  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│   │
│                          │     │                                       │   │
│  publishTopic() ─────────┼──► SM ── 记住 publisher ──►               │   │
│                          │     │                                       │   │
│  subscribeTopic() ───────┼──► SM ── 记住 subscriber ──►              │   │
│                          │     │   SM 回推 PUBLISHER_NOTIFY            │   │
│  ◄── PUBLISHER_NOTIFY ◄─┼────┼─── 给订阅者                            │   │
│                          │     │                                       │   │
│  建直连到 publisher ──────┼────►│ accept                               │   │
│  发 SUBSCRIBE_BROADCAST ─┼────►│ 记住 fd 要收哪个 topic                │   │
│                          │     │                                       │   │
│                          │     │  broadcast()                          │   │
│                          │     │    ├─ TCP: 遍历 fd → send ──────────►│   │
│  ◄── MSG_BROADCAST ◄────┼─────┼────┘                                  │   │
│  dispatch → callback()   │     │  SHM: serverBroadcast()               │   │
│                          │     │                                       ▼   ▼
└──────────────────────────┘     └───────────────────────────────────────────┘
```

**核心设计**：ServiceManager 只负责控制面（"告诉你在哪"和"建立订阅关系"），所有实际数据流量（RPC request/response、topic broadcast）都走客户端到服务端的**数据面直连**（SHM 或 TCP）。一旦连接建立，数据流量完全不经 ServiceManager。

---

## 同步 RPC 调用链路

### 客户端侧：发起调用

```
用户代码
  │
  ▼
proxy.GetLatestData(request, &response)             // 生成代码 Proxy 方法
  │  序列化 request → Buffer
  ▼
OmniRuntime::invoke(service_name, iface, method, request, response, timeout)
  │
  ▼
callSerialized(lambda)                               // 串行化入口（保证线程安全）
  │
  ├── 无 owner thread
  │     lock_guard<mutex> lock(api_mutex_)
  │     return func()                                // 直接在当前线程执行
  │
  └── 有 owner thread
        │  SyncCallState<int> state                  // 跨线程结果信箱
        │  loop_->post(lambda) ──────► owner event loop 执行
        │  state.wait()  ←────────── 阻塞等结果（mutex + condvar）
        ▼
invokeInternal(service_name, iface, method, request, response, timeout)
  │
  ├── 1. lookupServiceInfo(service_name, info)
  │     │
  │     │  查本地缓存 service_cache_
  │     │  ├── 缓存命中 → 直接返回 ServiceInfo
  │     │  │   {host, port, host_id, shm_config}
  │     │  │
  │     │  └── 缓存未命中
  │     │       发 MSG_LOOKUP 给 ServiceManager
  │     │       等待 MSG_LOOKUP_REPLY
  │     │       反序列化 ServiceInfo
  │     │       写入 service_cache_
  │     │       返回 ServiceInfo
  │     ▼
  │
  ├── 2. acquireInvokeConnection(service_name, info, attempt, conn)
  │     │
  │     │  conn_mgr_->getOrCreateConnection(name, host, port, host_id, shm_name, shm_config)
  │     │    │
  │     │    │  已有连接且 connected → 直接复用
  │     │    │
  │     │    │  新建连接
  │     │    │    TransportFactory::selectPreferredTransport(local_host_id, remote_host_id, shm_name)
  │     │    │      isSameMachine(local, remote) → 相等 → SHM
  │     │    │      isSameMachine(local, remote) → 不等 → TCP
  │     │    │
  │     │    │    SHM 路径：new ShmTransport(generateShmName(service_name)) → connect() → 创建独立 SHM + UDS 握手
  │     │    │      失败 → 降级 TCP
  │     │    │
  │     │    │    TCP 路径：new TcpTransport() → connect(host, port)
  │     │    ▼
  │     │
  │     │  返回 ServiceConnection{transport, connected, host_id, ...}
  │     │  注册到 event loop（fd 可读回调 → onDirectMessage）
  │     ▼
  │
  ├── 3. 构造 MSG_INVOKE 消息
  │     │  Message header: type=MSG_INVOKE, sequence=seq
  │     │  Payload: interface_id + method_id + request_length + request_data
  │     ▼
  │
  ├── 4. 发送请求
  │     │
  │     │  sendInvokeMessageWithTimeout(service_name, msg, info, conn, timeout)
  │     │    conn_mgr_->sendMessageWithinTimeout(name, msg, timeout, &elapsed)
  │     │      transport->send(serialized_data)
  │     │    发送失败 + attempt==0 → refreshServiceConnection → 重试一次
  │     ▼
  │
  └── 5. 等待回复
        │
        │  waitForReply(seq, reply_timeout, reply)
        │    rpc_runtime_.waitForReply(...)
        │      pollOnceWithoutFunctors() 循环
        │      期间只处理 fd 读取和 timer，不执行新的 API functor
        │      等 MSG_INVOKE_REPLY 匹配 seq
        │
        ▼
      decodeInvokeReplyPayload(reply, status, response)
        status == 0 → 成功，response 包含返回数据
        status != 0 → 返回错误码（ERR_DESERIALIZE / ERR_INVOKE_FAILED / ...）
```

**涉及代码位置**：

| 函数 | 文件 |
|------|------|
| `invoke()` | `omni_runtime.cpp:919` |
| `callSerialized()` | `omni_runtime.h:322` |
| `invokeInternal()` | `omni_runtime.cpp:927` |
| `lookupServiceInfo()` | `omni_runtime.cpp:771` |
| `acquireInvokeConnection()` | `omni_runtime.cpp:1852` |
| `populateInvokeMessage()` | `omni_runtime.cpp:1863` |
| `sendInvokeMessageWithTimeout()` | `omni_runtime.cpp:1895` |
| `waitForReply()` | `omni_runtime.cpp:500` |

### 服务端侧：处理请求

```
EventLoop epoll 触发 fd 可读
  │
  ├── TCP 路径
  │     listen fd 可读 → onServiceAccept
  │       │  accept() 新连接
  │       │  注册 client fd 到 event loop
  │       ▼
  │     client fd 可读 → onServiceClientData
  │       │  transport->recv() → 反序列化 → 得到 Message
  │       │  根据 MessageType 分派
  │       ▼
  │
  └── SHM 路径
        per-client eventfd 可读
          │  eventFdConsume(fd)
          │  shm_server->serverRecv(buf, sizeof(buf), client_id)
          │    从 client_id 对应的 per-client request ring 读取数据
          ▼

handleInvokeRequest(service_name, client_fd, msg, "TCP")    // TCP
handleShmRequest(service_name, entry, client_id, data, len)  // SHM
  │
  ├── 1. decodeInvokePayload(msg, interface_id, method_id, request)
  │     反序列化：读 interface_id、method_id、request data
  │     失败 → 构造错误 reply 直接返回
  │
  ├── 2. 校验 interface_id 是否匹配已注册服务
  │     不匹配 → 返回 ERR_INTERFACE_NOT_FOUND
  │
  ├── 3. service->onInvoke(method_id, request, response)     // ★ 核心调用
  │     │  进入用户 Service 子类（如 MySensorService）
  │     │  生成代码 Stub 内部：
  │     │    switch(method_id)
  │     │      case METHOD_GET_LATEST_DATA:
  │     │        反序列化 request 参数
  │     │        调用业务方法 GetLatestData(sensor_id)
  │     │        序列化返回值到 response
  │     │        return 0  (成功)
  │     │      default:
  │     │        return ERR_METHOD_NOT_FOUND
  │     ▼
  │
  ├── 4. 构造回复
  │     invoke_status == 0 (成功):
  │       MSG_INVOKE_REPLY(seq, status=0, response_length, response_data)
  │     invoke_status != 0 (失败):
  │       MSG_INVOKE_REPLY(seq, status=invoke_status)
  │     失败:
  │       MSG_INVOKE_REPLY(seq, status=ERR_INVOKE_FAILED)
  │
  └── 5. 发送回复
        TCP → sendOnFd(transport, reply)
              transport->send(serialized_reply)
        SHM → shm_server->serverSend(client_id, serialized_reply)
              写入 per-client response ring + eventfd 通知客户端
```

**涉及代码位置**：

| 函数 | 文件 |
|------|------|
| `onServiceAccept()` | `omni_runtime.cpp:1305` |
| `onServiceClientData()` | `omni_runtime.cpp:1334` |
| `handleInvokeRequest()` | `omni_runtime.cpp:1373` |
| `handleShmRequest()` | `omni_runtime.cpp:1626` |
| SHM eventfd 回调注册 | `omni_runtime.cpp:616` |

### 客户端侧：收到回复

```
EventLoop epoll 触发 fd 可读
  │
  ├── TCP 路径
  │     ConnectionManager 的 fd 回调
  │       │  transport->recv() → 反序列化 → 得到 Message
  │       ▼
  │     onDirectMessage(service_name, msg)
  │       │  type == MSG_INVOKE_REPLY
  │       │  storePendingReply(seq, msg)
  │       │    写入 sm_channel_ 的 pending_replies_
  │       │    唤醒 waitForReply 中阻塞的线程
  │       ▼
  │
  └── SHM 路径
        ShmTransport 的 eventfd 回调
          │  clientRecv() 读 response ring buffer
          │  反序列化 → 得到 Message
          │  走同样的 reply 匹配逻辑
          ▼

waitForReply 返回 → decodeInvokeReplyPayload
  │  解析 status 和 response data
  ▼
invokeInternal 返回结果给调用方
```

**涉及代码位置**：

| 函数 | 文件 |
|------|------|
| `onDirectMessage()` | `omni_runtime.cpp:1567` |
| `storePendingReply()` | `omni_runtime.cpp:509` |
| `waitForReply()` | `omni_runtime.cpp:500` |

---

## One-Way RPC 调用链路

与同步 RPC 几乎完全一致，区别仅在于：

**客户端**：

```
invokeOneWayInternal(service_name, iface, method, request)
  │
  ├── lookupServiceInfo(...)          // 同步 RPC 一样
  ├── acquireInvokeConnection(...)    // 同步 RPC 一样
  ├── 构造 MSG_INVOKE_ONEWAY 消息
  ├── sendInvokeMessage(...)          // 发送
  │
  └── return 0                        // ★ 不等回复，直接返回
```

**服务端**：

```
handleInvokeOneWayRequest(service_name, msg, "TCP")
  │
  ├── decodeInvokePayload(...)
  ├── service->onInvoke(method_id, request, response)   // 执行业务逻辑
  │
  └── return                                            // ★ 不构造回复，不发回任何东西
```

**涉及代码位置**：

| 函数 | 文件 |
|------|------|
| `invokeOneWay()` | `omni_runtime.cpp:1003` |
| `invokeOneWayInternal()` | `omni_runtime.cpp:1010` |
| `handleInvokeOneWayRequest()` | `omni_runtime.cpp:1474` |

---

## 发布/订阅调用链路

发布/订阅的完整流程分四个阶段，涉及 ServiceManager 的控制面协调和客户端到发布者的数据面直连。

### Phase 1：发布者注册 Topic

```
发布者进程
  │
  ▼
runtime.publishTopic("SensorUpdate")
  │
  ▼
publishTopicInternal("SensorUpdate")
  │
  ├── 1. 构造 MSG_PUBLISH_TOPIC 消息
  │     payload = topic_name + ServiceInfo
  │     ServiceInfo 包含：
  │       host     = 第一个本地注册服务的监听地址
  │       port     = 第一个本地注册服务的监听端口
  │       host_id  = 本机 machine-id
  │       shm_config = SHM ring 容量
  │
  ├── 2. sendToSM(msg) → 等待 MSG_PUBLISH_TOPIC_REPLY → accepted=true
  │
  └── 3. topic_runtime_.rememberPublishedTopic(topic, topic_id, service_name)
         本地记录 topic_id 与服务的映射关系

        │
        │    ServiceManager 侧
        │
        ▼
handlePublishTopic(conn, msg)
  │
  ├── registry_.registerPublisher(topic, pub_info, conn->fd)
  │     记录 topic → publisher 的 ServiceInfo
  │
  ├── 回复 MSG_PUBLISH_TOPIC_REPLY(accepted=true) 给发布者
  │
  └── 遍历该 topic 的已有订阅者
        给每个订阅者发送 MSG_TOPIC_PUBLISHER_NOTIFY
        包含：topic_name + publisher 的 ServiceInfo
```

### Phase 2：订阅者订阅 Topic

```
订阅者进程
  │
  ▼
runtime.subscribeTopic("SensorUpdate", callback)
  │
  ▼
subscribeTopicInternal("SensorUpdate", callback)
  │
  ├── 1. 发 MSG_SUBSCRIBE_TOPIC 给 ServiceManager
  │
  ├── 2. 等待 MSG_SUBSCRIBE_TOPIC_REPLY → accepted=true
  │
  └── 3. topic_runtime_.rememberSubscription(topic, callback)
         本地注册用户的回调函数

        │
        │    ServiceManager 侧
        │
        ▼
handleSubscribeTopic(conn, msg)
  │
  ├── topic_manager_.addSubscriber(topic, conn->fd)
  │     记录订阅关系
  │
  ├── 回复 MSG_SUBSCRIBE_TOPIC_REPLY(accepted=true) 给订阅者
  │
  └── 如果该 topic 已有 publisher
        │  取出 publisher 的 ServiceInfo
        │  立即给这个新订阅者发 MSG_TOPIC_PUBLISHER_NOTIFY
        ▼  （包含 publisher 的 host/port/host_id）
```

### Phase 3：订阅者收到通知并建立直连

```
订阅者进程 — onSMData → handleSMMessage
  │
  ▼
case MSG_TOPIC_PUBLISHER_NOTIFY:
  │  反序列化 → topic_name + pub_info{host, port, host_id}
  │
  ▼
ensureTopicPublisherConnection(topic, pub_info)
  │
  ├── 1. conn_mgr_->getOrCreateConnection(pub_name, host, port, host_id, generateShmName(pub_name))
  │     │  同机 → SHM 直连到发布者进程
  │     │  跨机 → TCP 直连到发布者进程
  │     ▼
  │
  └── 2. conn_mgr_->sendMessage(pub_name, MSG_SUBSCRIBE_BROADCAST{topic_id, topic_name})
        │  告诉发布者："我要订阅这个 topic"
        │  这条消息走数据面直连，不经 ServiceManager
        ▼

        │
        │    发布者进程 — 收到 MSG_SUBSCRIBE_BROADCAST
        │
        ▼
handleSubscribeBroadcast(client_fd, msg)
  │
  └── topic_runtime_.addTcpSubscriber(topic_id, client_fd)
       或 addShmSubscriber(topic_id, service_name)
       记住"这个 fd / shm 客户端需要接收该 topic 的广播"
```

### Phase 4：发布者广播数据

```
发布者进程
  │
  ▼
service.BroadcastSensorUpdate(data)                  // 生成代码
  │  序列化 data → Buffer
  ▼
runtime.broadcast(topic_id, data)
  │
  ▼
broadcastInternal(topic_id, data)
  │
  ├── 1. 构造 MSG_BROADCAST 消息
  │     payload = topic_id + data_length + data
  │     msg.serialize(send_buf)
  │
  ├── 2. TCP 订阅者
  │     │  遍历 topic_runtime_.tcpSubscribers(topic_id)
  │     │  对每个 fd：
  │     │    通过 client_fd_to_service_ 找到对应 service
  │     │    通过 service 的 client_transports 找到 transport
  │     │    sendOnFd(transport, broadcast_msg)
  │     ▼
  │
  └── 3. SHM 订阅者
        │  遍历 topic_runtime_.shmSubscriberServices(topic_id)
        │  对每个 service_name：
        │    找到对应 LocalServiceEntry
        │    shm_server->serverBroadcast(send_buf.data(), send_buf.size())
        │    写入所有客户端共享的 response ring buffer
        ▼
```

### 订阅者收到广播

```
EventLoop epoll 触发 fd 可读
  │
  ├── TCP 路径
  │     ConnectionManager → onDirectMessage
  │       │  type == MSG_BROADCAST
  │       │  decodeBroadcastPayload → topic_id + data
  │       ▼
  │
  └── SHM 路径
        ShmTransport client eventfd 可读
          │  clientRecv() → 反序列化 → Message
          ▼

topic_runtime_.dispatch(topic_id, data)
  │  找到 topic_name 对应的用户回调
  ▼
callback(data)                                       // 用户回调
  反序列化 data → 业务结构体 → 处理
```

**涉及代码位置**：

| 函数 | 文件 |
|------|------|
| `publishTopicInternal()` | `omni_runtime.cpp:1111` |
| `subscribeTopicInternal()` | `omni_runtime.cpp:1255` |
| `handleSMMessage()` → `MSG_TOPIC_PUBLISHER_NOTIFY` | `omni_runtime.cpp:465` |
| `ensureTopicPublisherConnection()` | `omni_runtime.cpp:1938` |
| `handleSubscribeBroadcast()` | `omni_runtime.cpp:1513` |
| `broadcastInternal()` | `omni_runtime.cpp:1178` |
| `onDirectMessage()` → `MSG_BROADCAST` | `omni_runtime.cpp:1584` |
| `handleTopicBroadcastMessage()` | `omni_runtime.cpp:1236` |
| SM `handlePublishTopic()` | `service_manager/main.cpp:584` |
| SM `handleSubscribeTopic()` | `service_manager/main.cpp:644` |
| SM `sendTopicPublisherNotify()` | `service_manager/main.cpp:685` |

---

## 传输层选择机制

### host_id 的来源

```
platform::getMachineId()                              // platform.cpp:636
  │
  ├── 1. 读 /etc/machine-id                           // Linux 系统唯一标识
  ├── 2. 读 /var/lib/dbus/machine-id                  // 备选
  └── 3. hostname 的 FNV1a 哈希                       // 兜底
```

### 同机/跨机判断

```
服务端注册时：
  svc_info.host_id = host_id_                         // 塞入 ServiceInfo 发给 SM

客户端查找时：
  lookupServiceInfo → 从 SM 拿到 ServiceInfo（含 remote host_id）

客户端建连接时：
  ConnectionManager::getOrCreateConnection(local_host_id, remote_host_id, ...)
    │
    ▼
  TransportFactory::selectPreferredTransport(local_host_id, remote_host_id, shm_name)
    │
    ▼
  TransportFactory::isSameMachine(local_host_id, remote_host_id)
    return !local_host_id.empty()
        && !remote_host_id.empty()
        && local_host_id == remote_host_id            // 字符串相等 = 同机
    │
    ├── 相等 → TransportType::SHM
    └── 不等 → TransportType::TCP
```

### SHM 降级 TCP

```
ConnectionManager::getOrCreateConnection()
  │
  │  首选 SHM
  │  new ShmTransport(generateShmName(service_name)) → connect()
  │
  ├── connect() == 0 → 使用 SHM
  └── connect() != 0 → 日志 data_connect_fallback
                        delete shm
                        new TcpTransport() → connect(host, port)   // 降级 TCP
```

---

## 关键代码文件索引

| 文件 | 职责 |
|------|------|
| `src/core/omni_runtime.h` | OmniRuntime::Impl 定义，`callSerialized` 模板实现 |
| `src/core/omni_runtime.cpp` | 核心运行时：init、invoke、topic、服务端请求处理 |
| `src/core/connection_manager.cpp` | 连接管理：创建/复用/删除连接，传输层选择 |
| `src/core/owner_thread_executor.h` | 跨线程调度：`SyncCallState`、`invokeOnOwner*` |
| `src/core/service_host_runtime.cpp` | 服务端骨架：accept、消息分派 |
| `src/transport/transport_factory.cpp` | 传输层工厂：同机/跨机判断 |
| `src/transport/shm_transport.h/cpp` | SHM 传输实现：ring buffer、eventfd |
| `src/transport/tcp_transport.h/cpp` | TCP 传输实现 |
| `src/platform/platform.cpp` | 平台层：`getMachineId()`、`getHostName()` |
| `service_manager/main.cpp` | ServiceManager：服务注册、查找、topic 关系管理 |
| `src/core/message.cpp` | Message 序列化/反序列化 |
| `include/omnibinder/types.h` | `ServiceInfo` 定义（含 host_id） |
| `include/omnibinder/proxy_base.h` | ServiceProxyBase 基类 |
| `src/core/service_host_runtime.cpp` | 服务端消息分发，含心跳自动响应 |

---

## 连接管理调用链路

### 主动连接（Proxy::connect）

```
Proxy::connect()
    │
    └── ServiceProxyBase::connect()
        │
        ├── runtime_.connectService(service_name)
        │       │
        │       ├── lookupServiceInfo()     // 查询 SM 获取 ServiceInfo
        │       │       ├── 检查 service_cache_
        │       │       └── 未命中则查询 SM
        │       │
        │       └── conn_mgr_->getOrCreateConnection()  // 建立 SHM/TCP 连接
        │
        └── runtime_.subscribeServiceDeath()  // 注册死亡通知
```

### 自动重连

```
服务死亡 / 心跳超时
    │
    ├── service_cache_.erase(service_name)
    ├── conn_mgr_->removeConnection(service_name)
    │
    └── scheduleReconnect(interval_ms)
        │
        └── 定时器触发
            │
            └── tryReconnectService(service_name)
                │
                └── connectServiceInternal()
                    ├── lookupServiceInfo()   // 查询 SM 获取最新信息
                    └── getOrCreateConnection()  // 建立新连接
```

### 心跳流程

```
startHeartbeat(interval, timeout)
    │
    └── 启动定时器
        │
        ├── sendHeartbeatToService()
        │       └── MSG_HEARTBEAT → 服务端
        │               │
        │               └── ServiceHostRuntime 自动响应 MSG_HEARTBEAT_ACK
        │
        └── checkHeartbeatTimeout()
            ├── 更新 last_ack_time
            └── 超时: 清除缓存 + 移除连接 + 触发重连
```
