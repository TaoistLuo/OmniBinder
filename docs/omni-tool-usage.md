# omni-cli 使用指南

中文 | [English](omni-tool-usage.en.md)

> **版本**：1.0.0  
> **更新日期**：2026-03-05

## 概述

`omni-cli` 是 OmniBinder 的命令行调试工具，用于查询在线服务和运行时、查看接口定义、调用服务方法、调整运行时日志级别，以及按 PID 观察业务接口输入/输出。

**主要功能**：
- 列出所有在线服务
- 列出所有在线 runtime / PID
- 查看服务接口和方法签名
- 调用服务方法（支持 hex 和 JSON 格式）
- 显示调用耗时统计
- 按 PID 设置运行时日志级别
- 按 PID watch IDL 业务接口输入/输出

---

## 基本用法

### 命令格式

```bash
omni-cli [options] <command> [args]
```

### 全局选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `-h, --host <addr>` | ServiceManager 地址 | 127.0.0.1 |
| `-p, --port <port>` | ServiceManager 端口 | 9900 |
| `--idl <file.bidl>` | IDL 文件路径（启用 JSON 支持） | 无 |
| `--help` | 显示帮助信息 | - |

### 可用命令

| 命令 | 说明 |
|------|------|
| `list` | 列出所有在线服务 |
| `ps` | 列出在线 runtime PID、角色、日志级别和服务 |
| `info <service>` | 查看服务详细信息 |
| `call <service> <method> [params]` | 调用服务方法 |
| `log set --pid <pid> --level <F\|E\|W\|I\|D\|V\|O>` | 设置指定 runtime 日志级别 |
| `watch --pid <pid> --idl <file.bidl> [--filter <name>]` | 观察指定 PID 的业务接口 I/O |

---

## 命令详解

### 1. list - 列出服务

列出所有在线服务及其基本信息。

**语法**：
```bash
omni-cli list
```

**示例**：
```bash
$ omni-cli list
NAME                     HOST             PORT     STATUS
----                     ----             ----     ------
SensorService            127.0.0.1        35451    ONLINE
NavigationService        192.168.1.100    8002     ONLINE

Total: 2 services online
```

---

### 2. ps - 列出运行时

列出当前连接到 ServiceManager 的 runtime PID。隐藏诊断服务不会出现在服务列表中。

**语法**：
```bash
omni-cli ps
```

**示例**：
```bash
$ omni-cli ps
PID      ROLE     LOG   PROCESS              SERVICES
-------- -------- ----- -------------------- ----------------
12345    service  I     example_cpp_sensor_server    SensorService
12346    client   I     example_cpp_sensor_client    -
```

`PROCESS` 列来自 runtime 启动时上报的可执行文件名。该列的格式宽度为最小 20 字符；更长的名称会完整输出，不会截断，因此后面的 `SERVICES` 列会向右移动。如果进程是在旧版本 runtime 下注册的，需要重启该进程后才会刷新为真实进程名。

---

### 3. info - 查看服务信息

查看服务的接口和方法定义。支持两种模式：

#### 2.1 基础模式（不指定 IDL）

显示方法签名的类型名称。

**语法**：
```bash
omni-cli info <service_name>
```

**示例**：
```bash
$ omni-cli info SensorService
Service: SensorService
  Host:    127.0.0.1
  Port:    35451
  HostID:  0cee7b0ae232461ba25df5376b29b46b
  Status:  ONLINE

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x61564c6c)
      - SetThreshold(ControlCommand) -> common.StatusResponse  (id=0x2214744e)
      - ResetSensor(int32) -> void  (id=0x7f2b95ee)
```

#### 2.2 详细模式（指定 IDL）

展开结构体的完整字段定义。

**语法**：
```bash
omni-cli --idl <file.bidl> info <service_name>
```

**示例**：
```bash
$ omni-cli --idl examples/sensor_service.bidl info SensorService
Service: SensorService
  Host:    127.0.0.1
  Port:    35451
  HostID:  0cee7b0ae232461ba25df5376b29b46b
  Status:  ONLINE

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x61564c6c)
          return:           {
            sensor_id: int32
            temperature: float64
            humidity: float64
            timestamp: int64
            location: string
          }
      - SetThreshold(ControlCommand) -> common.StatusResponse  (id=0x2214744e)
          param:           {
            command_type: int32
            target: string
            value: int32
          }
      - ResetSensor(int32) -> void  (id=0x7f2b95ee)
```

**说明**：
- IDL 文件中的 `import` 依赖会自动递归加载
- 跨包类型（如 `common.StatusResponse`）也能正确展开
- `Published Topics` 来自独立的运行时查询：没有发布话题时显示 `(none)`；服务查询成功但话题查询失败时显示 `(unavailable: <原因>)`，随后仍继续显示接口。服务本身不存在时则直接报错。
- `--idl` 使用与 `omni-idlc` 相同的严格语义校验，校验失败时会在连接 ServiceManager 前退出。

---

### 4. call - 调用服务方法

调用服务的指定方法。支持两种输入格式：

#### 3.1 Hex 模式（不指定 IDL）

使用十六进制字符串作为输入，输出也是十六进制。

**语法**：
```bash
omni-cli call <service> <method> [hex_params]
```

**示例**：
```bash
# 带参数调用
$ omni-cli call SensorService ResetSensor 01000000
Calling SensorService.ResetSensor() ...
  interface_id = 0xa112646d
  method_id    = 0x7f2b95ee
  request (4 bytes)
Response (status=OK, 0 bytes, 0.61 ms):

# 无参数调用
$ omni-cli call SensorService GetLatestData
Calling SensorService.GetLatestData() ...
  interface_id = 0xa112646d
  method_id    = 0x61564c6c
Response (status=OK, 38 bytes, 0.75 ms):
  Hex: 01 00 00 00 00 00 00 00 80 39 40 ...
```

#### 3.2 JSON 模式（指定 IDL）

使用 IDL 类型信息编码参数；可解码的非 `void` 响应显示为格式化 JSON，否则回退为 hex（解码失败时同时显示警告）。

**语法**：
```bash
omni-cli --idl <file.bidl> call <service> <method> [json_params]
```

**示例 1：无参数方法**
```bash
$ omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData
Calling SensorService.GetLatestData() ...
  interface_id = 0xa112646d
  method_id    = 0x61564c6c
Response (status=OK, 38 bytes, 0.81 ms):
  {
    "humidity": 61.95,
    "location": "Room-A",
    "sensor_id": 1,
    "temperature": 23.99,
    "timestamp": 1772645628
  }
```

**示例 2：结构体参数**
```bash
$ omni-cli --idl examples/sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"sensor1","value":100}'
Calling SensorService.SetThreshold() ...
  interface_id = 0xa112646d
  method_id    = 0x2214744e
  request (19 bytes)
Response (status=OK, 10 bytes, 0.47 ms):
  {
    "code": 0,
    "message": "OK"
  }
```

**示例 3：基础类型参数**
```bash
$ omni-cli --idl examples/sensor_service.bidl call SensorService ResetSensor 1
Calling SensorService.ResetSensor() ...
  interface_id = 0xa112646d
  method_id    = 0x7f2b95ee
  request (4 bytes)
Response (status=OK, 0 bytes, 0.58 ms):
```

**说明**：
- `{` 开头的对象或 `[` 开头的数组按 JSON 解析；已知基础类型参数按 IDL 类型解析裸命令行文本
- 字符串基础类型使用原始参数，例如 `EchoString hello`；`EchoString '"hello"'` 会把双引号也作为字符串内容
- 非基础类型参数如果不以 `{` 或 `[` 开头，则按 hex 解析（向后兼容）
- struct 输入必须提供所有声明字段；当前 JSON codec 不支持 `bytes`（包括包含 `bytes` 的数组/结构体）
- JSON 数字经轻量 number 表示处理，超出 IEEE-754 精确整数范围的 `int64`/`uint64` 不保证精确
- 响应时间以毫秒为单位显示

---

### 5. log set - 设置运行时日志级别

按 PID 调整指定 runtime 的日志级别。PID 可通过 `omni-cli ps` 获取。

**语法**：
```bash
omni-cli log set --pid <pid> --level <F|E|W|I|D|V|O>
```

日志级别含义：

| 级别 | 含义 |
|------|------|
| `F` | fatal |
| `E` | error |
| `W` | warn |
| `I` | info |
| `D` | debug |
| `V` | verbose |
| `O` | off |

**示例**：
```bash
omni-cli log set --pid 12345 --level D
```

---

### 6. watch - 观察业务接口 I/O

按 PID 观察 runtime 的 IDL 业务接口输入/输出。watch 数据面复用 OmniBinder 既有 topic 数据通道：同机自动使用 SHM，跨机自动使用 TCP；ServiceManager 只负责控制面启动/停止。

**语法**：
```bash
omni-cli watch --pid <pid> --idl <file.bidl> [--filter <name>]
```

**行为说明**：

- 服务端 PID：观察 RPC 输入和 topic 发布输出。
- 客户端 PID：观察 RPC 输出和订阅输入。
- 不输出 ServiceManager 控制消息、lookup、heartbeat 等内部控制流。
- `--filter` 可匹配已解码的 RPC 方法名，也可使用通用方向名 `broadcast` 或 `subscribe`。过滤发生在话题 payload 解码之前，因此当前不能按实际话题名（例如 `SensorUpdate`）过滤；名称仍为 `?` 的事件不会被该过滤器排除。

**示例**：
```bash
omni-cli watch --pid 12345 --idl examples/sensor_service.bidl --filter GetLatestData
```

---

## JSON 格式说明

### 支持的类型映射

| IDL 类型 | JSON 类型 | 示例 |
|----------|-----------|------|
| `bool` | boolean | `true`, `false` |
| `int8`, `uint8`, `int16`, `uint16` | number | `42`, `-10` |
| `int32`, `uint32`, `int64`, `uint64` | number | `1000`, `9999999` |
| `float32`, `float64` | number | `3.14`, `25.5` |
| `string` | 原始命令行字符串 | `hello`, `Room-A` |
| `struct` | object | `{"field1": value1, "field2": value2}` |
| `array<T>` | array | `[1, 2, 3]`, `["a", "b"]` |

### JSON 输入示例

**基础类型**：
```json
42
```

**字符串基础类型**：在 shell 中作为一个原始参数传递，例如 `'hello world'`，不要额外保留 JSON 双引号。

**结构体**：
```json
{
  "command_type": 1,
  "target": "sensor1",
  "value": 100
}
```

**数组**：
```json
[1, 2, 3, 4, 5]
```

**嵌套结构体**：
```json
{
  "sensor_id": 1,
  "location": "Room-A",
  "readings": [
    {"temperature": 25.5, "humidity": 60.2},
    {"temperature": 26.0, "humidity": 58.5}
  ]
}
```

---

## 高级用法

### 连接远程 ServiceManager

```bash
omni-cli -h 192.168.1.100 -p 9900 list
```

### 使用相对路径的 IDL 文件

```bash
cd examples
omni-cli --idl sensor_service.bidl info SensorService
```

### 使用绝对路径的 IDL 文件

```bash
omni-cli --idl /path/to/sensor_service.bidl call SensorService GetLatestData
```

### 管道和脚本集成

```bash
# 获取服务列表并过滤
omni-cli list | grep Sensor

# 在脚本中使用
#!/bin/bash
RESULT=$(omni-cli --idl sensor.bidl call SensorService GetLatestData)
echo "Result: $RESULT"
```

---

## 常见问题

### Q1: 为什么 JSON 输出显示为 hex？

**A**: 可能的原因：
1. 未指定 `--idl` 参数（默认使用 hex 模式）
2. IDL 文件路径错误或无法解析
3. 返回类型无法从完整 IDL/import 链解析，或响应解码失败
4. 返回类型含当前 JSON codec 不支持的 `bytes`

**解决方法**：
- 确保使用 `--idl` 参数并指定正确的 IDL 文件路径
- 检查 IDL 文件是否包含所需的类型定义
- 跨包 struct 在完整 import 链可用时可以解码；必要时可使用 hex 模式辅助排查实际类型或 payload

### Q2: 如何知道方法需要什么参数？

**A**: 使用 `info` 命令的详细模式：
```bash
omni-cli --idl sensor_service.bidl info SensorService
```
这会展开所有方法的参数和返回值的完整字段定义。

### Q3: JSON 输入格式错误怎么办？

**A**: 常见错误：
- 字段名必须用双引号：`{"name": "value"}` ✅，`{name: "value"}` ❌
- 字符串值必须用双引号：`"hello"` ✅，`'hello'` ❌
- 数字不要加引号：`42` ✅，`"42"` ❌
- 注意逗号分隔：`{"a": 1, "b": 2}` ✅，`{"a": 1 "b": 2}` ❌

### Q4: 调用耗时包括哪些部分？

**A**: 耗时统计包括：
- 网络传输时间（TCP 或 SHM）
- 服务端处理时间
- 序列化/反序列化时间

**不包括**：
- omni-cli 的 JSON 解析时间
- 连接建立时间（首次调用会建立连接）

### Q5: 为什么本地调用这么快？

**A**: 当 omni-cli 和服务在同一台机器上时，会自动使用共享内存（SHM）传输，延迟通常在 1 毫秒以内。

---

## 性能参考

### 本地调用（SHM）

| 操作类型 | 典型耗时 |
|---------|---------|
| 无参数调用 | 0.5-1.0 ms |
| 基础类型参数 | 0.4-0.8 ms |
| 结构体参数 | 0.5-1.2 ms |

### 远程调用（TCP）

| 网络环境 | 典型耗时 |
|---------|---------|
| 局域网 | 1-5 ms |
| 跨机房 | 10-50 ms |

---

## 相关文档

- [IDL 语法规范](idl-specification.md)
- [通信协议规范](protocol.md)

---

**文档版本**：1.0.0  
**最后更新**：2026-03-05
