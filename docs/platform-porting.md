# OmniBinder 平台适配指南

## 概述

OmniBinder 设计为跨平台 IPC/RPC 框架。当前支持 Linux 和 Windows，未来计划支持 Android、鸿蒙、FreeRTOS、ESP32 等。

适配新平台的核心工作是**实现平台抽象层**（`platform.h` 声明的 53 个函数）和**事件驱动后端**（`EventBackend` 接口）。传输层（TCP/SHM）、序列化、协议栈、运行时逻辑均为平台无关的 C++11 代码，不需要修改。

## 适配级别

| 级别 | 支持功能 | 需实现的平台函数 | 工作量 |
|------|---------|----------------|--------|
| **TCP 客户端** | 连接服务、RPC 调用、订阅 topic、死亡通知 | ~20 个网络 + ~5 个系统 | 2-3 天 |
| **TCP 服务端** | + 注册服务、接受连接、TCP 数据通道 | + bind/listen/accept | +1 天 |
| **完整支持（含 SHM）** | + 同机 SHM 高速通信 | + ~15 个 SHM/eventfd/handshake | +3-5 天 |

> **注意**：绝大多数 RTOS/MCU 平台只需要适配到"TCP 客户端"或"TCP 服务端"级别。
> SHM 需要内核级共享内存支持（如 `/dev/shm`），仅在 Linux/类 Unix 上可用。

## 平台 API 清单

### 第一组：网络（必须实现，TCP 路径依赖）

| 函数 | 说明 | MCU/RTOS 实现建议 |
|------|------|------------------|
| `netInit()` | 初始化网络栈 | lwIP: `lwip_init()` 或空（SDK 已初始化） |
| `netCleanup()` | 清理网络 | 通常空实现 |
| `createTcpSocket()` | 创建 TCP socket | `lwip_socket(AF_INET, SOCK_STREAM, 0)` |
| `setNonBlocking(fd)` | 设为非阻塞 | `fcntl(fd, F_SETFL, O_NONBLOCK)` |
| `setReuseAddr(fd)` | 端口复用 | `setsockopt(SO_REUSEADDR)` |
| `setTcpNoDelay(fd)` | 禁用 Nagle | `setsockopt(TCP_NODELAY)` |
| `setKeepAlive(fd)` | TCP keepalive | `setsockopt(SO_KEEPALIVE)` |
| `bindSocket(fd, host, port)` | 绑定地址 | `lwip_bind(fd, ...)` |
| `listenSocket(fd, backlog)` | 开始监听 | `lwip_listen(fd, backlog)` |
| `acceptSocket(fd, &host, &port)` | 接受连接 | `lwip_accept(fd, ...)` |
| `connectSocket(fd, host, port)` | 发起连接 | `lwip_connect(fd, ...)` |
| `socketSend(fd, data, len)` | 发送数据 | `lwip_send(fd, data, len, 0)` |
| `socketSendAll(fd, data, len, timeout)` | 发送全部数据（带超时） | 循环 send + `select` 检查可写 |
| `socketRecv(fd, buf, size)` | 接收数据 | `lwip_recv(fd, buf, size, 0)` |
| `closeSocket(fd)` | 关闭 socket | `lwip_close(fd)` |
| `getSocketPort(fd)` | 获取本地端口 | `getsockname(fd, ...)` |
| `getSocketAddress(fd)` | 获取本地地址 | `getsockname(fd, ...)` |
| `getSocketError()` | 获取最后一次 socket 错误 | `errno` 或 `lwip_errno` |
| `isWouldBlock(err)` | 判断是否为 EAGAIN/EWOULDBLOCK | `err == EAGAIN \|\| err == EWOULDBLOCK` |
| `isConnectionReset(err)` | 判断是否为连接重置 | `err == ECONNRESET` |
| `waitSocketWritable(fd, timeout)` | 等待 socket 可写 | `select` 检查 `writefds` |
| `checkSocketConnected(fd)` | 检查异步 connect 结果 | `getsockopt(SO_ERROR)` |

### 第二组：事件通知（SHM 依赖，可空返回）

| 函数 | 空返回值 | 说明 |
|------|---------|------|
| `createEventFd()` | `-1` | 仅 SHM 使用 |
| `createNamedEventFd(name)` | `-1` | 仅 SHM 使用 |
| `openNamedEventFd(name)` | `-1` | 仅 SHM 使用 |
| `eventFdNotify(efd)` | `false` | 仅 SHM 使用 |
| `eventFdConsume(efd)` | `true` | 仅 SHM 使用 |
| `closeEventFd(efd)` | — | 空函数 |

### 第三组：共享内存（SHM 依赖，可空返回）

| 函数 | 空返回值 |
|------|---------|
| `shmCreate(name, size, create, &mapped)` | `NULL` |
| `shmDetach(addr, size)` | — |
| `shmUnlink(name)` | — |

### 第四组：进程间握手机制（SHM 依赖，可空返回）

| 函数 | 空返回值 |
|------|---------|
| `handshakeListen(name)` | `NULL` |
| `handshakeAccept(listener)` | `NULL` |
| `handshakeConnect(name)` | `NULL` |
| `handshakeCloseListener(listener)` | — |
| `handshakeSend(ch, data, len, fds, n)` | `false` |
| `handshakeRecv(ch, buf, ...)` | `false` |
| `handshakeTakeLocalNotifyFd(ch)` | `-1` |
| `handshakeClose(ch)` | — |
| `handshakeGetFd(ch)` | `-1` |
| `handshakeGetListenerFd(listener)` | `-1` |
| `isShmHandshakeAvailable()` | `false` |

### 第五组：系统/时间（必须实现）

| 函数 | MCU/RTOS 实现 |
|------|-------------|
| `getMachineId()` | MAC 地址或芯片 ID |
| `currentTimeMs()` | `xTaskGetTickCount() * portTICK_PERIOD_MS` |
| `currentTimeUs()` | 同上 × 1000（或硬件定时器微秒读） |
| `getHostName()` | 固定字符串如 `"mcu-node"` |
| `getPid()` | `xPortGetCoreID() << 16 \| uxTaskGetTaskNumber()` |
| `getProcessName()` | `pcTaskGetName(NULL)` |
| `getLocalTime(out_tm, out_ms)` | 填充本地时间与毫秒（无 RTC 时返回启动以来时间） |
| `sleepMs(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| `setupSignalHandlers(handler)` | 空函数（MCU 无信号） |
| `memoryBarrier()` | `__sync_synchronize()` 或 `asm volatile("":::"memory")` |

### 第六组：测试辅助（可选）

| 函数 | MCU 实现 |
|------|---------|
| `waitFdReadable(fd, timeout)` | `select` 读检查 |

## EventBackend 实现

`EventLoop` 依赖 `EventBackend` 抽象接口处理 I/O 多路复用。当前有 `EpollBackend`（Linux）和 `IocpBackend`（Windows）。

RTOS/MCU 平台需要实现一个轻量替代：

```
class SelectBackend : public EventBackend {
    fd_set read_fds_, write_fds_;
    int    max_fd_;
    
    void addFd(int fd, int events) override;   // FD_SET
    void removeFd(int fd) override;            // FD_CLR
    int  poll(int timeout_ms) override;        // select + 检查可读/可写
    void wakeup() override;                    // xSemaphoreGive
    // ...
};
```

**核心逻辑**：
- `addFd`/`removeFd`：维护 `fd_set`
- `poll(timeout_ms)`：调用 `select(max_fd+1, &read_fds_, &write_fds_, NULL, &tv)`，返回就绪 fd 数量
- `wakeup()`：通过一个内部 socket pair 或 FreeRTOS 信号量唤醒阻塞的 `poll()`

## 完整示例：FreeRTOS + lwIP

### 文件清单

```
src/platform/
├── platform.h                    # 接口声明（不动）
├── platform_freertos_lwip.cpp    # 新文件：适配实现
└── event_backend_freertos.cpp    # 新文件：SelectBackend
```

### platform_freertos_lwip.cpp 核心实现

```cpp
#include "platform/platform.h"
#include "lwip/sockets.h"
#include "FreeRTOS.h"
#include "task.h"

// ─── 网络（直转 lwIP）───
bool netInit() { return true; /* SDK 已初始化 lwIP */ }
SocketFd createTcpSocket() { return lwip_socket(AF_INET, SOCK_STREAM, 0); }
int connectSocket(SocketFd fd, const std::string& host, uint16_t port) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    lwip_inet_aton(host.c_str(), &addr.sin_addr);
    return lwip_connect(fd, (struct sockaddr*)&addr, sizeof(addr));
}
// ... 其余 socket 函数类似，全部直转 lwip_*

// ─── 时间 / 系统 ───
int64_t currentTimeMs() { return xTaskGetTickCount() * portTICK_PERIOD_MS; }
void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
int getPid() { return (int)(xPortGetCoreID() << 16 | (uint32_t)uxTaskGetTaskNumber(NULL)); }
std::string getMachineId() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);  // ESP32 示例
    char buf[13];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return buf;
}

// ─── SHM / eventfd / handshake（全部空返回）───
void* shmCreate(...) { return NULL; }
int createEventFd() { return -1; }
bool isShmHandshakeAvailable() { return false; }
// ... 其他空函数 ...
```

### event_backend_freertos.cpp 核心

```cpp
#include "platform/event_backend.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lwip/sockets.h"

class FreertosSelectBackend : public EventBackend {
    SemaphoreHandle_t wake_sem_;
    // fd_set + select 逻辑
public:
    FreertosSelectBackend() { wake_sem_ = xSemaphoreCreateBinary(); }
    void wakeup() override { xSemaphoreGive(wake_sem_); }
    int poll(int timeout_ms) override {
        // 用 select 监听所有 fd，配合信号量超时
    }
};
```

## 验证

完成适配后，运行以下最小验证：

```cpp
// 1. 确保编译通过
// 2. 连接 ServiceManager
OmniRuntime rt;
assert(rt.init("192.168.1.100", 9900) == 0);  // SM 在 Linux 主机上

// 3. 连接服务并发起 RPC
assert(rt.connectService("EchoService") == 0);
Buffer req, resp;
req.writeString("hello");
assert(rt.invoke("EchoService", echo_iface_id, echo_method_id, 0, req, resp) == 0);
assert(resp.size() > 0);

// 4. 验证自动重连（断开 SM 再重连）
```

## 相关文件

- `src/platform/platform.h` — 完整平台接口声明
- `src/platform/platform_linux.cpp` — Linux 参考实现
- `src/platform/platform_win.cpp` — Windows 参考实现
- `src/platform/event_backend.h` — EventBackend 接口
- `src/platform/event_backend_linux.cpp` — Epoll 参考实现
- `src/core/event_loop.cpp` — EventLoop（平台无关）
- `src/transport/transport_selector.cpp` — 传输选择（自动回退 TCP）
