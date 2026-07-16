# Per-Client SHM 最终方案

## 一、模型

```
服务端: 为每个连接的客户端单独创建一块 SHM
  每块 SHM = [req ring(客户端写,服务端读)] + [resp ring(服务端写,客户端读)]
  服务端同时创建 req_eventfd + resp_eventfd，打包通过 UDS 发给客户端

客户端: SHM 只包含 resp ring（客户端读,服务端写）+ req ring（客户端写,服务端读）
  从 UDS 接收 SHM name + 两个 eventfd
```

```
                     服务端
                        │
           ┌────────────┼────────────┐
           ▼            ▼            ▼
      客户端 A 的    客户端 B 的    客户端 C 的
      SHM (8KB)     SHM (8KB)     SHM (8KB)
           │            │            │
       req fd=5      req fd=8     req fd=11
       resp fd=6     resp fd=9    resp fd=12
           │            │            │
       服务端 EventLoop 分别注册 5,8,11 → 各有一个回调
```

## 二、连接流程

```
客户端 connectService():
  → TransportFactory::isSameMachine(host_id) → true → 走 SHM
  → new ShmTransport(service_name, false)
  → connect()

ShmTransport::initClient():
  1. UDS connect(server_path)
  2. udsRecvFds → 收到 [resp_eventfd, req_eventfd]  ← 两个 fd 都由服务端创建
  3. 收到 SHM name
  4. shm_open(name) → mmap
  5. event_fd_ = resp_eventfd (服务端通知客户端用)
  6. req_eventfd_ = req_eventfd (客户端通知服务端用)

服务端 EventLoop → UDS accept → onUdsClientConnect():
  1. shm_open + ftruncate + mmap → 独立 SHM
  2. createEventFd → req_eventfd
  3. createEventFd → resp_eventfd
  4. loop_->addFd(req_eventfd, callback)  ← 直接注册，零嵌套
  5. udsSendFds → 发 [resp_eventfd, req_eventfd] + SHM name
  6. 完成 — 客户端收到后即可开始收发
```

## 三、事件通知

```
客户端 send():
  写 req ring → eventFdNotify(req_eventfd_)
  → 服务端 epoll 检测到 req fd 可读 → 回调:
      eventFdConsume → serverRecv → onShmRequest → serverSend

服务端 serverSend():
  写 resp ring → eventFdNotify(resp_fd)
  → 客户端 epoll 检测到 event_fd_ 可读 → 回调:
      eventFdConsume → clientRecv → 存 reply → invoke() 返回
```

## 四、生命周期

```
客户端连接:   服务端 UDS accept → shm_open + createEventFds → addFd → 发客户端
正常断开:     客户端 close → 服务端检测 req_eventfd POLLHUP 或心跳超时
异常崩溃:     SM 心跳超时 → OmniRuntime::onClientHeartbeatTimeout
              → removeShmClient(client_id):
                removeFd(req_eventfd)
                closeEventFd(req_eventfd)
                closeEventFd(resp_eventfd)
                shmUnlink(name)
                erase client_context
服务端关闭:   unregisterService → removeShmFromLoop → 遍历所有客户端清理
```

## 五、对比

| | 老模型 | 当前（客户端创建） | 最终（服务端创建） |
|---|---|---|---|
| SHM owner | 服务端(1 块共享) | 客户端 | 服务端(N 块 per-client) |
| eventfd owner | 服务端(1 个共享 req) | 客户端(各自创建) | 服务端(各自创建) |
| EventLoop addFd | 1 次 | N 次(需回调发现) | N 次(直接注册) |
| Lambda 层数 | 1 | 3 | 1 |
| 新增 API | — | 3 个 | 0 |
| 自旋锁 | 有 | 无 | 无 |
| 客户端上限 | 32 | 无 | 无 |
| 崩溃清理 | 服务端单点 | 客户端残留 | 服务端单点 |
| 代码量 | 300 行 | 500 行 | 350 行 |

## 六、须删除的

- `OnNewClientCallback` 类型和 `setOnNewClientCallback()` 方法
- `getClientReqEventFd()` 方法
- `ClientShmContext` 中的 `req_eventfd` 字段（改为 `resp_eventfd` 由服务端管理）
- 客户端 `initClient()` 中 `shmCreate()` 调用

## 七、改动范围

| 文件 | 改动 |
|---|---|
| `shm_transport.h` | 删除 callback 机制，简化 ClientShmContext |
| `shm_transport.cpp` | 重写 initClient(打开 SHM, 收 eventfds)，改写 onUdsClientConnect(创建 SHM, 创建 fds, addFd) |
| `omni_runtime.cpp` | 简化 initializeServiceShm(去掉 callback)，增加心跳驱动的移除逻辑 |
| 其他文件 | **0 行** |
