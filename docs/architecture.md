# OmniBinder 架构说明

面向实现者的内部细节请参考 `docs/architecture-internals.md`。

## 1. 项目定位

OmniBinder 是一个运行在用户态的微服务通信框架，面向嵌入式和分布式进程场景，支持：

- 同机进程间低延迟通信
- 跨板 / 跨机器通信
- 服务注册与发现
- 同步 RPC、单向调用、发布订阅、服务死亡通知
- C / C++ 双语言接口与 IDL 驱动开发

框架的核心思想是：

- **控制面集中，数据面直连**
- **同机优先 SHM，跨机使用 TCP**
- **对外 API 保持简单，对内 runtime 按职责拆分**

## 2. 总体架构

### 2.1 分层模型

```
+-------------------------------------------------------------------+
|                          应用 / 工具层                              |
|  ServiceA / ServiceB / HMI / omni-cli / 生成的 Proxy / Stub       |
+-------------------------------------------------------------------+
|                         libomnibinder                               |
|                                                                   |
|  OmniRuntime (Facade / 协调层)                                      |
|    |-- SmControlChannel                                            |
|    |-- RpcRuntime                                                  |
|    |-- TopicRuntime                                                |
|    |-- ServiceHostRuntime                                          |
|    `-- ConnectionManager                                           |
|                                                                   |
|  EventLoop                                                         |
|  Transport Layer: TCP / SHM                                        |
+-------------------------------------------------------------------+
|                     ServiceManager（独立进程）                      |
|     服务注册 / 服务发现 / 心跳 / 死亡通知 / 话题关系管理            |
+-------------------------------------------------------------------+
```

### 2.2 控制面与数据面

#### 控制面

控制面由 `ServiceManager` 统一负责：

- 服务注册 / 注销
- 服务发现 / 列表 / 接口查询
- 心跳与在线状态
- 服务死亡通知
- topic 发布者 / 订阅者关系维护

控制面消息始终走 TCP。

#### 数据面

数据面不经过 `ServiceManager` 转发，由服务间直连：

- 同机：优先 SHM
- 跨机：TCP
- SHM 不可用或连接失败：自动降级到 TCP

## 3. 核心运行时组件

### 3.1 OmniRuntime

`OmniRuntime` 是对外的统一入口，负责：

- 初始化 runtime
- 连接 `ServiceManager`
- 注册本地服务
- 查询远程服务
- 发起 RPC / one-way 调用
- 订阅 / 发布 topic
- 订阅服务死亡通知

`OmniRuntime` 主要承担 facade 和协调职责，本身不承载大部分底层细节。

### 3.2 SmControlChannel

`SmControlChannel` 负责控制面通道：

- 与 `ServiceManager` 的 TCP 连接
- 控制消息发送
- 控制消息接收与拆包
- pending reply 管理

所有注册、发现、订阅、心跳等控制消息都经过这一层。

### 3.3 RpcRuntime

`RpcRuntime` 负责同步 RPC 的核心运行时语义：

- sequence 分配
- 默认超时策略
- wait state
- deadline 管理
- `waitForReply()` 主循环委托

同步 RPC 的“等待回复”语义由这一层统一管理。

### 3.4 TopicRuntime

`TopicRuntime` 负责 topic 相关本地状态：

- topic callback 管理
- topic name / topic id 映射
- topic owner 状态
- publisher / subscriber 本地状态
- 本地 callback dispatch

topic 数据面仍然走 publisher 与 subscriber 之间的直连通道。

### 3.5 ServiceHostRuntime

`ServiceHostRuntime` 负责本地服务托管侧的运行时：

- service accept
- client data 接收
- service client message dispatch skeleton
- SHM request dispatch skeleton

它负责把来自远端 client 的请求交给本地服务执行路径。

### 3.6 ConnectionManager

`ConnectionManager` 负责数据面连接管理：

- 维护服务连接池
- 避免重复建立直连
- 根据 `host_id`、`shm_name` 和本地环境选择 transport
- SHM 失败后降级 TCP
- 处理直连消息与断开回调

## 4. Transport 选择策略

### 4.1 主机标识

每个节点启动时都会获得本机 `host_id`，用于判断服务是否与调用方同机。

### 4.2 自动选择规则

当客户端通过 `ServiceManager` 拿到 `ServiceInfo` 后：

1. 比较本地 `host_id` 与目标服务的 `host_id`
2. 如果相同且服务提供了 `shm_name`
   - 优先尝试 SHM
3. 如果不同
   - 直接使用 TCP
4. 如果 SHM 建立失败
   - 自动回退到 TCP

这一策略由 `TransportFactory` 和 `ConnectionManager` 共同执行。

## 5. SHM 架构

### 5.1 设计目标

SHM 用于同机低延迟通信，设计原则是：

- 低延迟
- 多客户端共享
- 内存占用可控
- 支持 eventfd 事件驱动

### 5.2 当前布局

每个服务创建一块主 SHM 区域，布局为：

```
+---------------------------------------------------------------+
| ShmControlBlock                                               |
| - magic / version / max_clients                               |
| - active_clients                                               |
| - req_ring_capacity / resp_ring_capacity                       |
| - response_bitmap                                              |
| - req_lock                                                     |
| - slots[0..31] => active / response_offset / response_cap      |
+---------------------------------------------------------------+
| Global RequestQueue                                            |
| - 所有客户端共享写入                                           |
| - 服务端读取                                                   |
+---------------------------------------------------------------+
| ResponseArena                                                  |
| - 每个活跃 slot 占用一个响应 block                             |
| - 通过 bitmap 管理使用状态                                     |
+---------------------------------------------------------------+
```

### 5.3 SHM 通信模型

- 请求方向：
  - 多客户端共享一个 `RequestQueue`
  - 服务端统一消费
- 响应方向：
  - 每个活跃客户端占用一个 response block
  - 服务端按 `slot` 写回
- 客户端断开后：
  - `slot` 可复用
  - response block 重新回到空闲状态

### 5.4 SHM 默认容量

当前默认 SHM 配置：

- `req_ring_capacity = 4KB`
- `resp_ring_capacity = 4KB`
- `max_clients = 32`

对 payload 稍大的服务，可通过服务级配置显式放大：

```cpp
setShmConfig(ShmConfig(8 * 1024, 16 * 1024));
```

该配置会通过 `ServiceInfo.shm_config` 传播到客户端，保证 client/server 看到一致的 ring 容量。

### 5.5 SHM 事件驱动

当前 SHM 已使用 `eventfd + EventLoop`：

- client 写 request 后通知服务端 `req_eventfd`
- server 写 response 后通知客户端 `resp_eventfd`

因此当前 SHM 路径不是轮询模型，而是事件驱动模型。

## 6. 服务生命周期

### 6.1 服务注册

服务启动后：

1. 建立 TCP 监听
2. 建立 SHM（如果启用）
3. 生成 `ServiceInfo`
4. 向 `ServiceManager` 注册：
   - name
   - host
   - port
   - host_id
   - shm_name
   - shm_config
   - interface metadata

### 6.2 服务发现

客户端查询服务时，`ServiceManager` 返回 `ServiceInfo`，包括：

- 服务地址
- 端口
- `host_id`
- `shm_name`
- `shm_config`
- 接口信息

### 6.3 服务死亡通知

客户端可订阅目标服务死亡：

1. 向 `ServiceManager` 发送订阅请求
2. 只有收到 reply 成功后，才更新本地 death callback 状态
3. 服务死亡后，`ServiceManager` 统一广播死亡通知

## 7. RPC 模型

### 7.1 同步 RPC

同步 RPC 的路径是：

1. client 查找服务
2. 选择 SHM 或 TCP
3. 发送 `MSG_INVOKE`
4. server 执行业务方法
5. 返回 `MSG_INVOKE_REPLY`
6. `RpcRuntime` 等待并交付结果

### 7.2 One-way 调用

one-way 调用发送 `MSG_INVOKE_ONEWAY`，服务端执行后不返回 reply。

### 7.3 interface 校验

当前实现中，`interface_id` 不匹配会被**硬拒绝**：

- 不再进入业务实现
- 同步调用会返回 `ERR_INTERFACE_NOT_FOUND`
- one-way 调用直接丢弃

这一规则在 TCP 和 SHM 两条路径中都一致生效。

## 8. 发布订阅模型

### 8.1 控制关系

topic 的发布者 / 订阅者关系由 `ServiceManager` 维护：

- 发布者通过 `publishTopic()` 声明发布
- 订阅者通过 `subscribeTopic()` 注册订阅
- 相关本地状态均在收到 SM reply 成功后再更新

### 8.2 数据路径

topic 数据不经过 `ServiceManager` 中转。

路径是：

1. 发布者向 SM 声明 topic
2. 订阅者向 SM 订阅 topic
3. SM 通知订阅者 publisher 信息
4. 订阅者直连 publisher
5. publisher 广播数据直送订阅者

### 8.3 topic owner

当前 topic 发布状态显式记录 owner service。  
当某个 service 注销时，只会清理它 own 的 topic 状态，不会误伤同进程内其他 service 的 topic。

## 9. IDL / 代码生成 / 工具

### 9.1 omni-idlc

`omni-idlc` 根据 `.bidl` 文件生成：

- 结构体定义
- 序列化 / 反序列化代码
- Stub
- Proxy

当前外部 API 和 IDL 语义保持稳定，没有因内部 runtime 重构而破坏生成模型。

### 9.2 omni-cli

`omni-cli` 通过控制面与服务交互，支持：

- 服务列表查询
- 服务详情查询
- 接口查询
- 方法调用

在指定 `--idl` 时，可进行更友好的结构化输入输出。

## 10. 当前架构特征总结

当前 OmniBinder 的最终形态可以概括为：

- 控制面集中，数据面直连
- 同机优先 SHM，跨机使用 TCP
- SHM 为全局 request ring + session response block 模型
- 支持同步 RPC、one-way、发布订阅和服务死亡通知
- 支持服务级 SHM 容量配置与自动传输选择

对使用该库的开发者来说，可以直接理解为：

- 通过 `OmniRuntime` 使用统一 API 完成服务注册、发现、调用与订阅
- 通过 IDL 与 `omni-idlc` 自动生成 Stub / Proxy，减少样板代码
- 通过 `omni-cli` 查询服务状态、接口信息并直接发起调用

如果需要查看更细的内部运行时实现、模块边界和调用路径，请参考 `docs/architecture-internals.md`。
