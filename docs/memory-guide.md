# OmniBinder 内存使用说明（业务侧）

本文面向**使用 OmniBinder 开发业务**的开发者，说明哪些内存由业务申请、哪些由框架释放、
回调参数能保存多久、哪些释放函数不能混用。规则以当前实现为准，涉及代码生成的地方同时给出
C 与 C++ 两套写法。

---

## 1. 总原则

判断一块内存归谁管，看四句话：

1. **框架返回给你的 → 业务释放**（仅 C API 需要手动做；C++ API 的返回值/出参是标准容器，自动管理）。
2. **框架只读取的 → 所有权仍是业务的**：RPC 请求参数、广播发布对象，框架只在调用期间读取，
   不接管、不释放。
3. **框架借给你读的 → 业务不要释放、不要跨回调保存**：handler 入参、话题订阅回调参数、
   实例枚举/死亡通知回调里的指针，只在本次处理/回调执行期间有效。
4. **你交给框架的 → 框架释放**：C 侧 handler 的返回数据由 stub 序列化后自动释放；
   handler 入参也由 stub 在处理结束后自动释放。

C++ API 基于 RAII，绝大多数场景**完全不需要手动申请/释放**；需要严格配对的只有 C API。

---

## 2. C++ 业务代码怎么用

| 场景 | 内存归属 | 业务要不要释放 |
|------|----------|----------------|
| `proxy.Method(req, resp)` 的 `req` | 归业务；框架只读 | 不需要 |
| `proxy.Method(req, resp)` 的 `resp` | 归业务；`std::string` / `std::vector` 等字段自动管理 | 不需要 |
| 话题回调 `[](const SensorUpdate& msg)` | **借用**：回调返回后即失效 | 不需要；要留用请拷贝成值 |
| 广播发布 `stub.BroadcastTopic(msg)` | 归业务；框架只读 | 不需要（按业务自身规则管理 msg） |
| `omnibinder::Buffer` | 拥有自己的数据，析构自动释放 | 不需要 |
| `omnibinder::BufferView` | **借用**：不拥有，视图不得超出源 Buffer 生命周期 | 不需要 |

要点：

- 回调里的 `const T&` 是框架在回调前反序列化出来的临时对象，**回调返回后立即销毁**；
  跨回调保存必须自行拷贝（`auto copy = msg;`）。
- 同步 RPC 的输出对象只应在返回值为 0 时使用；失败时不要读取其中的动态字段。
- `OmniRuntime` / `Service` / `Proxy` 对象按正常 C++ 生命周期使用即可；服务注销请先调用
  `unregisterService()` 再析构（详见 [API 参考](api-reference.md)）。

```cpp
demo::SensorServiceProxy proxy(runtime);
proxy.connect();

demo::SensorData data;
if (proxy.GetLatestData(data) == 0) {          // resp 归业务，字段自动管理
    std::printf("%s\n", data.location.c_str());
}

proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
    // 仅在本次回调内有效；要留用：auto saved = msg;
});
```

---

## 3. C 业务代码怎么用

### 3.1 分配器入口

- 所有跨 OmniBinder 边界的内存统一用 `omni_malloc()` / `omni_free()`
  （`omnibinder_c.h`；二者是全局 `::operator new` / `::operator delete` 的薄封装）。
- **禁止混用系统 `malloc()` / `free()`**：库内分配走 `new`，用 `free()` 释放是未定义行为。
- 想整体接管库内分配：在**自己的程序**里重载全局 `operator new` / `operator delete`
  （C 与 C++ 一次覆盖），不要再混入第二套分配器。
- `omni_free(NULL)` 是安全的。

### 3.2 RPC 调用（同步 / oneway）

| 侧 | 环节 | 基础类型 | string / bytes | struct / 数组 |
|----|------|----------|----------------|----------------|
| 客户端 | 组装并发送请求 | 不用申请/释放 | 业务持有，框架只读 | 业务持有，框架只读 |
| 客户端 | 接收返回值 | 不用申请/释放 | proxy 已分配，业务 `omni_free()` | proxy 已 `_init` + `_deserialize`，业务 `xxx_destroy()` |
| 服务端 | handler 入参 | 值拷贝 | **框架分配、框架释放**：禁止 `free`、禁止跨请求保存 | 同上（stub 在 handler 返回后 `xxx_destroy`） |
| 服务端 | handler 返回 | 不用申请/释放 | **必须用 `omni_malloc()` 申请**（字符串可直接用 `omni_strdup()` 拷贝，返回的内存同样由 stub `omni_free()`）；业务不释放，stub 发完自动 `omni_free()` | 动态字段必须 `omni_malloc()`；业务不释放，stub 序列化后自动 `xxx_destroy()` |

补充：

- 生成代码在每次调用内部创建并销毁 `req` / `resp` 缓冲，业务不需要接触 `omni_buffer_t`。
- 调用失败（返回非 0）时无需释放任何返回值，框架内部已清理。
- handler 中途失败需要提前清理返回值时，调用 `xxx_destroy(&resp)`（幂等、字段会置空），
  不要逐个裸 `omni_free`。

### 3.3 广播（发布 / 订阅）

| 侧 | 环节 | 说明 |
|----|------|------|
| 发布端 | 构造并发送广播对象 | 框架只读；对象何时申请、何时释放完全归业务自管（用 `omni_malloc` 申请、`xxx_destroy` 释放） |
| 订阅端 | 回调参数 `const xxx* msg` | 框架在回调前 `_init` + `_deserialize`，回调返回后立即 `xxx_destroy`；**禁止释放、禁止保存**，留用请自行深拷贝 |

### 3.4 生命周期与缓冲区

| 对象 | 创建 | 释放 | 说明 |
|------|------|------|------|
| `omni_runtime_t` | `omni_runtime_create()` + `omni_runtime_init()` | `omni_runtime_stop()` → `omni_runtime_destroy()` | 先停业务线程再销毁 |
| 服务（stub） | `xxx_stub_create(runtime)` | `omni_runtime_unregister_service()` → `xxx_stub_destroy()` | 注册状态下直接 destroy 会留下悬空指针 |
| proxy | `xxx_proxy_init(&proxy, runtime)` | `xxx_proxy_destroy(&proxy)` | 栈上结构体也要 destroy（内部含订阅上下文） |
| `omni_buffer_t` | `omni_buffer_create()` / `omni_buffer_create_from()` | `omni_buffer_destroy()` | 必须配对；`create_from` 会**拷贝**输入，业务数据随后即可释放 |
| `omni_buffer_read_string()` / `omni_buffer_read_bytes()` | 框架分配 | 业务 `omni_free()` | 返回堆内存；读失败返回 NULL |
| `omni_buffer_data()` | — | 不释放 | 借用指针，buffer 销毁后失效 |
| `xxx_init()` / `xxx_destroy()`（struct / 数组 / topic） | `xxx_init()` | `xxx_destroy()` | `_destroy` 递归释放嵌套动态字段，且可重复调用（字段会置空） |
| 回调的 `user_data` | 业务自管 | 业务自管 | 生命周期必须覆盖注册期间 |

### 3.5 释放对照速查表

| 你拿到的东西 | 释放方式 |
|--------------|----------|
| proxy 返回的 `string` / `bytes` | `omni_free()` |
| proxy 返回的 struct / 数组 | `xxx_destroy(&obj)` |
| 回调参数（topic / 实例枚举 / 死亡通知） | **不要释放** |
| handler 入参 | **不要释放**（stub 自动释放） |
| `omni_buffer_create*` 出来的 buffer | `omni_buffer_destroy()` |
| `omni_buffer_read_string/read_bytes` 的返回 | `omni_free()` |
| `omni_buffer_data()` 的返回 | 不释放（借用） |
| 业务自己 `omni_malloc()` 的内存 | 业务 `omni_free()`（或交给 `xxx_destroy()` 递归释放） |
| 业务自己 malloc/new 的内存 | 按业务规则释放，**不要**用 `omni_free()` |

---

## 4. 常见错误

1. **用 `free()` 释放库返回的内存**：库内分配走 `new`，必须 `omni_free()`。
2. **把回调参数存起来跨回调使用**：回调返回后即失效，需深拷贝。
3. **在 handler 里释放入参或保存入参指针**：入参由 stub 管理，handler 返回后失效。
4. **handler 返回含动态字段的 struct 时忘记用 `omni_malloc()` 分配**：stub 会按字段指针释放，
   若指向栈/静态内存会在序列化后释放出问题；反之，业务**不能**自行释放返回值。
5. **忘记销毁 proxy 返回的 struct / 继承的嵌套字段**：泄漏；用 `xxx_destroy()` 递归释放。
6. **对回调参数调用 `xxx_destroy()`**：双重释放（框架已在回调后销毁）。
7. **混用分配器**：如自定义 `malloc` + `omni_free`、或系统 `free` + 框架内 `new` 分配，
   都会造成 alloc/dealloc 失配。
8. **销毁顺序错误**：先 `destroy` runtime 再停业务线程，会导致运行期回调访问已释放对象；
   按 3.4 的顺序收尾。

---

## 5. 相关文档

- [API 参考](api-reference.md)：`omni_malloc/omni_free`、Buffer、Runtime/Service 接口声明。
- [IDL 规范](idl-specification.md)：C 生成代码的 `_init` / `_destroy` / `_deserialize` 语义。
- [使用示例](examples.md)：C 与 C++ 的完整示例代码。
