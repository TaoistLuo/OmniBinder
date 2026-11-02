# OmniBinder 内存管理（C / C++ 业务开发者视角）

本文从**业务开发者**的角度说明：你写 C 或 C++ 程序时，什么时候要申请内存、什么时候要释放、哪些东西绝对不能动。

先选你的语言：

- **C++ 开发者**：看第 1 部分。核心是 RAII，正常情况下你几乎不需要 `new/delete`。
- **C 开发者**：看第 2 部分。核心是显式配对：谁创建、谁销毁、回调参数不能动。
- **两者都建议看**：第 3 部分的回调借用、`user_data` 生命周期、分配器规则和常见错误。

---

# 第 1 部分：C++ 开发者

## 1.1 四句话

1. **不用手动 `new/delete`**：`std::string`、`std::vector`、`omnibinder::Buffer` 都由 RAII 自动管理。
2. **你传进去的对象归你**：请求参数、广播对象，框架只在调用期间读取。
3. **回调参数是借用**：`const T&` 只在本次回调内有效，要留用请拷贝成值。
4. **生命周期顺序**：先 `proxy.disconnect()` / 销毁 proxy，再 `unregisterService()` / 销毁 Service，最后 `runtime.stop()` / 析构 runtime。

### 1.1.1 一张表：C++ 内存归谁

| 场景 | 谁分配 | 谁释放 | 业务侧规则 |
|---|---|---|---|
| Proxy 请求对象 `const T&` | 业务 | 业务 | 栈/RAII；框架只借用，不释放 |
| Proxy 输出 `Result<T>` | 框架反序列化 | `Result` 自身 | `if (result)` 后使用 `result.value()`；不要手动 free |
| Handler 入参 `const T&` | 框架 | 框架 | 只读；不要保存引用 |
| Handler 返回值 `T` | 业务构造 | 框架序列化后自动析构 | 按值返回；不要手动 free |
| Topic 回调 `const T&` | 框架 | 框架 | 回调内只读；保留请拷贝 |
| `omnibinder::Buffer` | 业务/框架 | RAII | move-only；不要拷贝 |
| `BufferView` | 不分配 | 不释放 | 借用 Buffer；Buffer 必须比 view 活得久 |


## 1.2 客户端：用生成的 Proxy

```cpp
omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

demo::SensorServiceProxy proxy(runtime);
std::thread runtime_thread([&runtime]() { runtime.start(); });
while (!proxy.isConnected()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
if (!proxy.isConnected()) {
    return -1;
}

demo::ControlCommand cmd;
cmd.command_type = 1;
cmd.target = "temperature";      // std::string：自动管理
cmd.value = 30;

auto result = proxy.SetThreshold(cmd);
if (!result) {
    std::printf("RPC failed: %d\n", result.error());
    return -1;
}

const common::StatusResponse& resp = result.value();
std::printf("%s\n", resp.message.c_str());
// resp 是普通 C++ 对象：不要 delete，不要 free

proxy.disconnect();              // 或让 proxy 析构自动 disconnect
```

规则：

| 东西 | 谁拥有 | 你要做什么 |
|---|---|---|
| `cmd` / 请求对象 | 你 | 正常构造；离开作用域自动析构 |
| `result.value()` | 你 | 只在 `result.ok()` / `if (result)` 成功后使用；不要手动释放 |
| `std::string` / `std::vector` 字段 | 对象自身 | 不需要 `new/delete` |
| Proxy 本身 | 你 | `connect()` 后使用；析构/`disconnect()` 会退订并断开 |

失败时：

- `!result`：不要读取 `result.value()`；失败时值是默认对象，可能没有被填充。
- 你传入的 `cmd` 仍然归你，正常析构即可。

## 1.3 客户端：裸 runtime + Buffer

只有不用生成代码、或需要手工拼二进制协议时才这样写。

```cpp
omnibinder::Buffer req;
req.writeInt32(42);

omnibinder::Buffer resp;
int rc = runtime.invoke("CalcService", IFACE_ID, METHOD_ADD, IDL_HASH,
                        req, resp, 3000);

if (rc == 0) {
    omnibinder::BufferView view(resp.data(), resp.size());
    int32_t sum = 0;
    if (!view.tryReadInt32(sum)) {
        // 解码失败
    }
}
```

规则：

| 类型 | 所有权 | 说明 |
|---|---|---|
| `omnibinder::Buffer` | 自己拥有数据 | 析构自动释放；**move-only，不能拷贝**；需要转移用 `std::move` |
| `omnibinder::BufferView` | **不拥有** | 只借用 `Buffer` 的内存；源 `Buffer` 必须比 view 活得久 |
| `Buffer::data()` | 借用 | `Buffer` 再次写入/扩容/析构后失效 |
| `runtime.invoke()` 的 `req` / `resp` | 你 | 框架只读 `req`、写入 `resp`；不需要手动释放 |

## 1.4 服务端：实现生成的方法

```cpp
class MySensorService : public demo::SensorServiceStub {
public:
    demo::SensorData GetLatestData() override {
        demo::SensorData data;
        data.sensor_id = 1;
        data.temperature = 25.5;
        data.location = "Room-A";    // std::string：自动管理
        return data;                 // 按值返回；框架负责序列化
    }

    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override {
        // cmd 是借用引用：只在本次调用内有效。
        // 不要保存 &cmd，也不要保存 cmd.target.c_str()。
        common::StatusResponse resp;
        resp.code = 0;
        resp.message = "ok";
        return resp;
    }
};
```

规则：

| 东西 | 所有权 | 你要做什么 |
|---|---|---|
| 方法入参 `const T&` | 框架临时对象 | **只读**；不要保存引用/指针；要留用请拷贝成值 |
| 方法返回值 `T` | 你构造，框架序列化 | 按值返回；`std::string/vector` 自动管理；不要 `delete` |
| `Service` 对象 | 你 | 注册前构造；先 `unregisterService()` 再析构 |

注意：

- 方法在 owner event-loop 线程执行；不要在回调里做长时间阻塞操作。
- 入参引用在方法返回后失效；需要异步使用时必须拷贝。

## 1.5 Topic：发布与订阅

### 发布

```cpp
demo::SensorUpdate msg;
msg.data.temperature = 25.5;
service.BroadcastSensorUpdate(msg);   // 框架只读 msg
// msg 离开作用域自动析构；不需要 delete
```

### 订阅

```cpp
proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
    // msg 和它的 std::string/vector 只在本次回调内有效。
    // 不要保存 &msg；要留用：auto saved = msg;
    std::printf("%f\n", msg.data.temperature);
});
```

规则：

- 回调参数 `const T&` 是框架反序列化出的临时对象，回调返回后销毁。
- 要跨回调保存，必须**拷贝成值**。
- 回调的 `std::function` 捕获对象生命周期由你负责；proxy 销毁时会退订。

## 1.6 对象生命周期与销毁顺序

| 对象 | 创建 | 销毁 | 注意 |
|---|---|---|---|
| `OmniRuntime` | 栈对象或 `new` | 析构 | 必须最后销毁 |
| `Service` / 生成的 Stub | 栈对象或 `new` | 析构 | 先 `unregisterService()` |
| 生成的 Proxy | 栈对象或 `new` | 析构 | 析构会 `disconnect()` / 退订 |
| `Buffer` | 栈对象 | 析构 | move-only；不要拷贝 |
| `BufferView` | 栈对象 | 不拥有 | 源 Buffer 必须活得更久 |

推荐顺序：

```cpp
proxy.disconnect();                 // 或让 proxy 先离开作用域
runtime.unregisterService(&service);
runtime.stop();
// runtime 最后析构
```

`OmniRuntime` 析构不会替你销毁 `Service`、Proxy 或 `Buffer`；它们必须按上面的顺序先处理。

## 1.7 嵌入式自定义堆

如果目标环境需要确定性内存：

- 在**程序启动时**重载全局 `operator new` / `operator delete`（C 与 C++ 一次覆盖）。
- 重载后，库内和业务侧的 C++ 堆分配都会走你的分配器；`std::string/vector` 也包括在内。
- 运行期不要切换分配器。
- 普通应用不要重载。

```cpp
void* operator new(std::size_t size) {
    void* p = my_pool_alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    my_pool_free(p);
}
```

## 1.8 C++ 常见问题

**Q：我需要 `new/delete` 吗？**

A：不需要。用栈对象、`std::string`、`std::vector`、`Buffer` 即可。

**Q：回调里的 `const T&` 能保存吗？**

A：不能。回调返回后失效；要留用请拷贝成值。

**Q：调用失败时，输出对象还能用吗？**

A：不要用。只在返回值为 0 时读取输出对象。

**Q：`Buffer` 能拷贝吗？**

A：不能，它是 move-only。需要转移用 `std::move`。

**Q：`BufferView` 能长期保存吗？**

A：不能。它不拥有内存；源 `Buffer` 失效后 view 悬垂。

**Q：Service / Proxy / Runtime 谁先销毁？**

A：Proxy 先 disconnect/析构，然后 unregister Service，最后 stop/析构 Runtime。

**Q：C++ 里要用 `omni_malloc/omni_free` 吗？**

A：一般不需要。它们用于 C API 或 C 生成代码；C++ 用 `new/delete` 和 RAII。

---

# 第 2 部分：C 开发者

## 2.1 六句话

1. **你创建的 `omni_buffer_t`，你负责 `omni_buffer_destroy()`。**
2. **生成 Proxy 返回的字符串/结构体，你负责释放**：字符串 `omni_free()`，结构体/数组 `xxx_destroy()`。
3. **回调参数只在本次回调内有效**：不要释放，不要保存；要留用就深拷贝。
4. **你作为服务端返回给框架的结构体，动态字段由你申请，但不要释放**：框架序列化后释放。
5. **所有框架分配的内存都用 `omni_free()` 释放**，不要混系统 `free()`。
6. **销毁顺序**：先销毁 proxy / 注销 service，再 `omni_runtime_stop()`，最后 `omni_runtime_destroy()`。

### 2.1.1 一张表：C 内存归谁

| 场景 | 谁分配 | 谁释放 | 业务侧规则 |
|---|---|---|---|
| Proxy 请求 `const T*` | 业务 | 业务 | 直接指向字面量/栈；框架只读；**不要 destroy** |
| Proxy 输出 `T*` | 框架反序列化 | 业务 | `init` → 调用 → `destroy` |
| Handler 入参 `const T*` | 框架 | 框架 | 回调期间只读；不要 free/保存 |
| Handler 返回值 `T*` | 业务填动态字段 | 框架序列化后释放 | 动态字段用 `omni_malloc`；不要手动 free |
| Topic 回调 `const T*` | 框架 | 框架 | 回调期间只读；保留请 `clone` |
| `omni_buffer_t` | 业务/框架 | 创建者 | `omni_buffer_destroy`；NULL 安全 |
| `omni_buffer_read_string/bytes` | 框架 | 业务 | `omni_free()` |
| thread-local 字符串 | 框架 | 不释放 | 立即拷贝；下次调用前有效 |


## 2.2 最小 C 程序的内存时间线

```c
omni_runtime_t* rt = omni_runtime_create();
omni_runtime_init(rt, "127.0.0.1", 9900);

/* 生成头文件只声明，函数体由业务实现；缺少任一实现会在链接期报错。 */
omni_impl demo_SensorService_impl_get_latest_data(
    struct demo_SensorData* result, void* user_data) {
    (void)user_data;
    demo_SensorData_init(result);
    result->sensor_id = 1;
    result->location = omni_strdup("Room-A");
}
/* 其余生成的 demo_SensorService_impl_* 也必须实现；不要手动赋值回调表。 */
omni_service_t* svc = demo_SensorService_stub_create(my_context);
omni_runtime_register_service(rt, svc);

demo_SensorService_proxy proxy;
demo_SensorService_proxy_init(&proxy, rt);
demo_SensorService_proxy_set_instance_id(&proxy, instance_id);

demo_SensorData result;
demo_SensorData_init(&result);
if (demo_SensorService_proxy_get_latest_data(&proxy, &result) == 0) {
    printf("%s\n", result.location);
}
demo_SensorData_destroy(&result);           /* 成功/失败都安全 */

demo_SensorService_proxy_destroy(&proxy);
omni_runtime_unregister_service(rt, svc);
demo_SensorService_stub_destroy(svc);
omni_runtime_stop(rt);
omni_runtime_destroy(rt);
```

`omni_runtime_destroy()` **不会**释放 proxy、service、buffer；必须按顺序自己销毁。

## 2.3 客户端：用生成的 Proxy

```c
demo_SensorService_proxy proxy;
demo_SensorService_proxy_init(&proxy, rt);
demo_SensorService_proxy_set_instance_id(&proxy, instance_id);

demo_ControlCommand cmd;
demo_ControlCommand_init(&cmd);
cmd.command_type = 1;
cmd.target = (char*)"temperature";         /* 借用：直接指向字面量即可 */
cmd.value = 30;

common_StatusResponse resp;
common_StatusResponse_init(&resp);
int rc = demo_SensorService_proxy_set_threshold(&proxy, &cmd, &resp);

/* cmd 是借用输入：框架只读，不接管；不要对借用输入调用 _destroy()。 */

if (rc == 0) {
    printf("%s\n", resp.message);
}
common_StatusResponse_destroy(&resp);      /* 输出归你；失败也可安全 destroy */

demo_SensorService_proxy_destroy(&proxy);
```

| 你传进去的东西 | 你要做什么 |
|---|---|
| 基础类型 | 不需要申请 |
| string | 直接指向 NUL 结尾字面量/栈字符串；调用期间有效即可，**不要为输入 `omni_strdup()`** |
| bytes | 直接指向数据并设置 `_len`；调用期间有效即可 |
| struct/数组 | 可直接用栈对象/栈数组填值；输入是借用语义，调用后**不要** `xxx_destroy()`。只有你自己 `omni_malloc` 的输入才由你释放 |

| Proxy 返回的东西 | 你怎么释放 |
|---|---|
| 基础类型 | 不需要释放 |
| `char*` / `uint8_t*` | `omni_free()` |
| struct / 数组 | `xxx_destroy(&result)` |

失败时：`rc != 0` 不要使用结果；借用输入按你的构造方式处理（栈对象自然离开作用域，不要 `_destroy()` 字面量）。

## 2.4 客户端：不用 Proxy，直接调 runtime

```c
omni_buffer_t* req = omni_buffer_create();      /* 你创建 */
omni_buffer_write_int32(req, 42);

omni_buffer_t* resp = omni_buffer_create();     /* 你创建 */
int rc = omni_runtime_invoke(rt, "CalcService",
                             IFACE_ID, METHOD_ADD, IDL_HASH,
                             req, resp, 3000);

if (rc == 0) {
    int32_t sum = omni_buffer_read_int32(resp);
    if (!omni_buffer_read_ok(resp)) {
        /* 解码失败，不要使用 sum */
    }
}

/* 无论成功失败：两个 buffer 都由你销毁 */
omni_buffer_destroy(req);
omni_buffer_destroy(resp);
```

| 东西 | 谁创建 | 谁释放 | 说明 |
|---|---|---|---|
| `omni_buffer_create()` | 你 | 你 | 空 buffer |
| `omni_buffer_create_from(data,len)` | 你 | 你 | **会拷贝 `data`**；外部数据随后可释放 |
| `omni_runtime_invoke` 的 `request` | 你 | 你 | 框架只读 |
| `omni_runtime_invoke` 的 `response` | 你 | 你 | 框架写入/覆盖；失败时不要读 |
| `omni_buffer_data(buf)` | 框架内部 | **不要释放** | 借用指针；buffer 写/重置/销毁后失效 |
| `omni_buffer_read_string/read_bytes` | 框架分配 | 你 `omni_free()` | 新堆内存 |

补充：

- `omni_buffer_read_bytes()` 长度为 0 时返回 `NULL` 且 `out_len=0`，这是合法空数组。
- `omni_buffer_reset()` 只重置读写位置，不释放底层内存。
- `omni_buffer_destroy(NULL)`、`omni_free(NULL)` 安全。

## 2.5 服务端：用生成的 callback 实现方法

### 入参：只读，不要释放

```c
void on_set_threshold(const demo_ControlCommand* cmd,
                      common_StatusResponse* result,
                      void* user_data) {
    /* cmd 和它里面的字符串/数组只在本次回调内有效。
       不要 free、不要 destroy、不要保存指针。 */
    printf("target=%s value=%d\n", cmd->target, cmd->value);

    result->code = 0;
    result->message = omni_strdup("ok");   /* 返回值动态字段：你申请 */
}
```

字符串入参：

```c
void on_echo_string(const char* value, char** result, void* user_data) {
    /* value 是 NUL 结尾字符串，只在本次回调内有效；不要 free，不要保存。
       要留用：omni_strdup(value)。 */
}
```

**handler 入参（基础类型、字符串、struct、数组）全部由框架管理，业务侧一律不要释放。**

### 返回值：填值 + 申请动态字段，但不要释放

```c
void on_get_latest_data(demo_SensorData* result, void* user_data) {
    result->sensor_id = 1;
    result->location = omni_strdup("Room-A");   /* 你申请 */
    /* 不要 free(result->location)；
       框架序列化完成后会 demo_SensorData_destroy(&result)。 */
}
```

返回字符串/bytes：

```c
void on_echo_string(const char* value, char** result, void* user_data) {
    *result = omni_strdup("pong");   /* 你申请 */
    /* 不要 omni_free(*result)；框架发完响应后释放 */
}
```

**为什么返回 struct 也由框架释放？**

```text
你填 result -> 你返回 -> 框架序列化 result -> 框架 xxx_destroy(&result)
```

如果业务提前释放，框架序列化时就会读到已释放内存。

所以返回值生命周期由框架托管到序列化完成；业务只负责填值 + 申请动态字段。

### 错误怎么返回

生成的 typed callback 返回 `void`，不能通过返回值传错误码。需要错误码时：

1. 在 IDL 响应结构里放 `status`/`code` 字段；或
2. 改用非 typed 的 `omni_service_create()` + `omni_invoke_callback_t`（返回 `int`）。

## 2.6 服务端：不用生成 callback，直接注册

```c
int on_invoke(uint32_t method_id, const omni_buffer_t* request,
              omni_buffer_t* response, void* user_data) {
    /* request/response 都是框架提供的 buffer：
       request 只读；response 你写入；
       不要 omni_buffer_destroy，不要保存到回调之外。 */
    int32_t a = omni_buffer_read_int32((omni_buffer_t*)request);
    omni_buffer_write_int32(response, a + 1);
    return 0;   /* 非 0 作为业务错误码 */
}
```

- `request` / `response` 归框架，不要 destroy。
- `user_data` 归你，活到服务销毁。

## 2.7 Topic：发布与订阅

### 发布

```c
demo_SensorUpdate msg;
demo_SensorUpdate_init(&msg);
msg.data.temperature = 25.5;

/* 你拥有 msg；broadcast helper 内部创建/销毁传输 buffer */
demo_SensorService_broadcast_sensor_update(rt, &msg);

demo_SensorUpdate_destroy(&msg);
```

### 订阅

```c
void on_sensor_update(const demo_SensorUpdate* msg, void* user_data) {
    /* msg 和它的字符串/数组只在本次回调内有效。
       不要 free、不要 destroy、不要保存；要留用请深拷贝。 */
    printf("%f\n", msg->data.temperature);
}
```

- 生成 Proxy 的 topic 回调 `const T* msg`：框架反序列化，回调后 `_destroy`；业务不要释放。
- `omni_runtime_subscribe_topic` 的 `data` buffer 是借用的：不要 destroy、不要保存。
- `user_data` 归你，活到 `unsubscribe`。

## 2.8 对象生命周期与销毁顺序

| 对象 | 创建 | 释放 | 注意 |
|---|---|---|---|
| `omni_runtime_t` | `omni_runtime_create()` | `omni_runtime_stop()` → `omni_runtime_destroy()` | 不要在业务线程调用中销毁 |
| C service | `xxx_stub_create()` | `xxx_stub_destroy()` | 业务实现全部 `xxx_impl_*`；先 `omni_runtime_unregister_service()` |
| C proxy | `xxx_proxy_init()` | `xxx_proxy_destroy()` | 先 destroy proxy |
| `omni_buffer_t` | `omni_buffer_create/create_from` | `omni_buffer_destroy()` | 每个成功创建的都要销毁 |
| 生成 struct/数组/topic | `xxx_init()` | `xxx_destroy()` | 递归释放字段，可重复调用 |
| 你 `omni_malloc()` 的内存 | 你 | `omni_free()` 或 `xxx_destroy()` | 不要混系统 `free()` |

推荐顺序：

```c
demo_SensorService_proxy_destroy(&proxy);
omni_runtime_unregister_service(rt, svc);
demo_SensorService_stub_destroy(svc);
omni_runtime_stop(rt);
omni_runtime_destroy(rt);
```

## 2.9 这些指针不要释放

| 你拿到的东西 | 为什么不能释放 |
|---|---|
| topic 回调的 `data` / `const T* msg` | 框架借用，回调结束即失效 |
| service watch 的 identity 字段 | 同上；要留用请拷贝 |
| `omni_runtime_list_instances` 的 `info` 字段 | 同上 |
| handler 入参（字符串/struct/数组） | 框架反序列化，handler 返回后框架释放 |
| handler 返回 struct 的动态字段 | 框架序列化后释放；提前释放会 UAF/double free |
| `omni_buffer_data()` 返回值 | 借用内部内存 |
| `omni_runtime_host_id/runtime_id/get_register_host/service_connection_key` | 线程局部缓冲区，下次调用同一函数即失效；要留用请立即拷贝 |
| 回调的 `user_data` | 框架不拥有，由你按业务规则管理 |

## 2.10 C 常见问题

**Q：handler 里收到的 struct 参数，要不要 `xxx_destroy()`？**

A：不要。框架给你读的临时对象，handler 返回后框架销毁。

**Q：handler 返回的 struct，要不要 `xxx_destroy()`？**

A：不要。你填值、用 `omni_malloc` 申请动态字段；框架序列化后销毁。

**Q：Proxy 返回的 struct，要不要 `xxx_destroy()`？**

A：要。它是给你的结果，用完必须销毁。

**Q：Proxy 返回的字符串，要不要释放？**

A：要，用 `omni_free()`。

**Q：调用失败时，结果还要释放吗？**

A：不要。`rc != 0` 时不要读、不要销毁结果；请求对象仍由你释放。

**Q：topic 回调里的 msg 能保存吗？**

A：不能。回调返回后失效；要留用请深拷贝。

**Q：`omni_buffer_data()` 能长期保存吗？**

A：不能。buffer 写入/重置/销毁后失效；不要释放。

**Q：`omni_runtime_destroy()` 会释放我的 buffer/proxy/service 吗？**

A：不会。按第 2.8 节顺序自己销毁。

**Q：可以用系统 `free()` 释放框架内存吗？**

A：不行。库内走 C++ `new`，必须 `omni_free()`。

---

# 第 3 部分：共用规则（C / C++ 都适用）

## 3.1 回调参数都是借用

| 回调 | 参数 | 有效期 | 能否保存 |
|---|---|---|---|
| topic | `const T&` / `const T*` / `data` buffer | 仅本次回调 | 不能；要留用请拷贝 |
| service watch | `identity` 字段 | 仅本次回调 | 不能；要留用请拷贝 |
| 实例枚举 | `omni_service_instance_t` 字段 | 仅本次回调 | 不能；要留用请拷贝 |
| 服务端方法 | 入参 `const T&` / `const T*` | 仅本次调用 | 不能；要留用请拷贝 |

## 3.2 `user_data` 归你

- 框架只保存指针，不复制、不释放。
- 生命周期必须覆盖：topic 订阅期间、service watch 期间、服务注册期间。
- C++ 的 `std::function` 捕获对象同理：proxy/service 活着时捕获对象必须有效。

## 3.3 分配器规则

| 语言 | 规则 |
|---|---|
| C | 框架分配的内存用 `omni_free()`；C 侧 buffer 用 `omni_buffer_destroy()`；生成 struct 用 `xxx_destroy()` |
| C++ | 用 `new/delete` 和 RAII；一般不用 `omni_malloc/omni_free` |
| 嵌入式自定义堆 | 启动时重载全局 `operator new/delete`，不要运行期切换，不要混第二套分配器 |

**禁止**：

- 用系统 `free()` 释放 `omni_malloc` / `omni_strdup` / `omni_buffer_read_*` 返回的内存；
- 用 `omni_free()` 释放系统 `malloc()` 或 C++ `new` 的内存；
- 对回调参数调用 `free` / `xxx_destroy` / `omni_buffer_destroy`。

## 3.4 线程与回调

- 所有回调（topic / service watch / onInvoke / onClientConnected）默认在 owner event-loop 线程执行。
- 不要在回调里做长时间阻塞操作；回调是重入点，回调内注销服务/取消订阅属于已接受的边界场景（见 AGENTS.md）。
- 回调里申请的内存，业务自己负责释放（C++ RAII 自动；C 按第 2 部分规则）。

## 3.5 一页速查

| 场景 | C++ | C |
|---|---|---|
| RPC 请求对象 | 栈对象，自动管理 | 你 `xxx_init` / `xxx_destroy` |
| RPC 输出对象 | 栈对象，自动管理 | Proxy 分配；你 `xxx_destroy` / `omni_free` |
| 服务端入参 | `const T&`，只读，不能保存 | 框架管理，不要释放/保存 |
| 服务端返回值 | 按值返回，自动管理 | 动态字段 `omni_malloc`，框架释放 |
| topic 回调 | `const T&`，只读，不能保存 | `const T*` / buffer，只读，不能释放 |
| 普通 buffer | `Buffer` 自动释放 | `omni_buffer_destroy` |
| 只读视图 | `BufferView` 借用 | `omni_buffer_data` 借用 |
| 对象销毁 | Proxy → Service → Runtime | Proxy → Service → Runtime |

---
