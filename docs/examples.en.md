# OmniBinder Examples Guide

[中文](examples.md) | English

This document shows end-to-end OmniBinder usage examples, including:

- a C++ service (`SensorService`) that exposes sensor data and periodically broadcasts updates
- a C++ client (`SensorClient`) that invokes RPCs, subscribes to topics, and listens for death notifications
- pure C server/client examples equivalent to the C++ version
- a downstream Sensor/HMI example that consumes installed OmniBinder artifacts
- `omni-cli` usage for service inspection and invocation

> If you want to verify **how a business project consumes OmniBinder artifacts**, start with
> `examples/artifact_examples/`. That directory is a standalone CMake project that uses
> `find_package(OmniBinder REQUIRED)` and an installed `omni-idlc`, instead of depending on internal repo targets.

---

## 1. Overview

The built-in examples demonstrate how to:

- define interfaces in `.bidl`
- generate C and C++ Stub/Proxy code
- implement services and clients
- invoke RPC methods
- broadcast and subscribe to topics
- receive service death notifications
- run the same model both in-tree and from downstream projects

The main example uses two IDL files to demonstrate cross-file `import` resolution.

---

## 2. IDL interface definition

The example uses two IDL files.

### 2.1 `common_types.bidl`

```protobuf
package common;

struct Timestamp {
    int64   seconds;
    int32   nanos;
}

struct StatusResponse {
    int32   code;
    string  message;
}
```

### 2.2 `sensor_service.bidl`

The current demo `examples/sensor_service.bidl` has been expanded into a fuller capability matrix. It includes:

- primitive RPC methods: `EchoBool` / `EchoInt8` / `EchoUInt8` / ... / `EchoFloat64` / `EchoString` / `EchoBytes`
- custom-struct RPC: `EchoStatus` / `EchoConfig`
- nested-struct RPC: `EchoEnvelope`
- array RPC: `EchoIdArray` / `EchoLabelArray` / `EchoSensorArray` / `EchoBundle`
- regular service methods: `GetLatestData` / `SetThreshold` / `ResetSensor` / `GetSensorCount`
- callback-like asynchronous method: `RequestLatestDataAsync`
- topics: `SensorUpdate` / `AsyncResultReady`

Key shapes:

```protobuf
package demo;

import "common_types.bidl";

struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    int64   timestamp;
    string  location;
}

struct SensorConfig {
    bool    enabled;
    int32   sample_rate_hz;
    string  label;
}

struct SensorEnvelope {
    SensorData        data;
    SensorConfig      config;
    common.Timestamp  captured_at;
}

struct SensorArrayBundle {
    array<int32>      ids;
    array<string>     labels;
    array<bytes>      blobs;
    array<SensorData> samples;
}

struct AsyncRequest {
    int32   request_id;
    string  client_tag;
}

struct AsyncResult {
    int32                 request_id;
    string                client_tag;
    common.StatusResponse status;
    SensorData            data;
}

topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic AsyncResultReady {
    AsyncResult result;
    int64       publish_time;
}
```

Use the checked-in `examples/sensor_service.bidl` as the authoritative source for the complete method list.

Key points:

- `import "common_types.bidl"` imports common type definitions
- use `common.StatusResponse` to reference imported package types
- make sure imported files are in the same directory or a configured search path

### 2.3 Code generation

```bash
# Generate C++ code
omni-idlc --lang=cpp --output=generated/ sensor_service.bidl

# Generate C code
omni-idlc --lang=c --output=generated/ sensor_service.bidl
```

The compiler resolves `import` dependencies automatically and generates all required files.

For `--lang=c`, the generated file names are:

- `common_types.bidl_c.h`
- `common_types.bidl.c`
- `sensor_service.bidl_c.h`
- `sensor_service.bidl.c`

---

## 3. C++ server: `SensorService`

The current `examples/example_cpp/sensor_server.cpp` has been expanded into a comprehensive demo server. It covers:

- all primitive RPC methods (`EchoBool` ~ `EchoFloat64`, `EchoString`, `EchoBytes`)
- custom struct RPC (`EchoStatus`, `EchoConfig`)
- nested struct RPC (`EchoEnvelope`)
- array RPC (`EchoIdArray`, `EchoLabelArray`, `EchoSensorArray`, `EchoBundle`)
- regular service methods (`GetLatestData`, `SetThreshold`, `ResetSensor`, `GetSensorCount`)
- callback-like asynchronous behavior (`RequestLatestDataAsync` followed by `AsyncResultReady` topic push)
- regular pub/sub (`SensorUpdate`)

Core shape:

```cpp
class MySensorService : public demo::SensorServiceStub {
public:
    bool EchoBool(bool value) override { return !value; }
    int32_t EchoInt32(int32_t value) override { return value + 3; }
    std::string EchoString(const std::string& value) override { return value + "_echo"; }
    common::StatusResponse EchoStatus(const common::StatusResponse& value) override { ... }
    demo::SensorEnvelope EchoEnvelope(const demo::SensorEnvelope& value) override { ... }
    std::vector<int32_t> EchoIdArray(const std::vector<int32_t>& value) override { ... }
    demo::SensorArrayBundle EchoBundle(const demo::SensorArrayBundle& value) override { ... }

    demo::SensorData GetLatestData() override { ... }
    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override { ... }
    void ResetSensor(int32_t sensor_id) override { ... }
    int32_t GetSensorCount() override { return 3; }

    common::StatusResponse RequestLatestDataAsync(const demo::AsyncRequest& request) override {
        demo::AsyncResultReady ready;
        ready.result.request_id = request.request_id;
        ready.result.client_tag = request.client_tag;
        ...
        BroadcastAsyncResultReady(ready);
        ...
    }
};
```

Use `examples/example_cpp/sensor_server.cpp` as the authoritative source for the full implementation.

---

## 4. C++ client: `SensorClient`

The current `examples/example_cpp/sensor_client.cpp` has also been expanded into a comprehensive demo client. It covers:

- all primitive RPC calls
- custom and nested struct RPC calls
- array RPC calls
- regular service calls (`GetLatestData`, `SetThreshold`, `ResetSensor`, `GetSensorCount`)
- topic subscriptions (`SensorUpdate`)
- callback-like asynchronous result reception (`AsyncResultReady`)
- death notification handling

Core shape:

```cpp
omnibinder::OmniRuntime runtime;
runtime.init("127.0.0.1", 9900);

demo::SensorServiceProxy proxy(runtime);
proxy.connect();

uint8_t bool_out = 0;
proxy.EchoBool(false, &bool_out);

int32_t int_out = 0;
proxy.EchoInt32(32, &int_out);

StatusResponse status_out;
proxy.EchoStatus(status, &status_out);

SensorEnvelope envelope_out;
proxy.EchoEnvelope(envelope, &envelope_out);

std::vector<int32_t> ids_out;
proxy.EchoIdArray(ids, &ids_out);

SensorArrayBundle bundle_out;
proxy.EchoBundle(bundle, &bundle_out);

proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) { ... });
proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& msg) { ... });

demo::AsyncRequest req;
req.request_id = 42;
req.client_tag = "cpp-client";
common::StatusResponse ack;
proxy.RequestLatestDataAsync(req, &ack);
```

Use `examples/example_cpp/sensor_client.cpp` as the authoritative source for the full implementation.

---

## 5. C server

The C server is functionally equivalent to the C++ server and uses code generated by `omni-idlc --lang=c`.

Main differences from C++:

- instead of inheriting a Stub base class, implement the generated `demo_SensorService_impl_xxx(...)` functions directly
- `demo_SensorService_stub_create(user_data)` automatically binds those generated implementations
- use generated helper functions for broadcasting, for example `demo_SensorService_broadcast_sensor_update(...)`
- strings and byte buffers are represented with pointer + length fields
- generated structs require `_init()` / `_destroy()` lifecycle management

This gives C users a stronger workflow: after the IDL changes, newly required service methods appear directly in the generated header, so missing implementations surface at compile/link time instead of being silently omitted from a manually filled callback table.

---

## 6. C client

The current `examples/example_c/sensor_client.c` is also a comprehensive demo client. It covers:

- all primitive RPC calls
- custom and nested struct RPC calls
- array RPC calls
- regular pub/sub (`SensorUpdate`)
- callback-like asynchronous result reception (`AsyncResultReady`)
- death notification callbacks

Core shape:

```c
demo_SensorService_proxy proxy;
demo_SensorService_proxy_init(&proxy, runtime);
demo_SensorService_proxy_connect(&proxy);

uint8_t b = 0;
demo_SensorService_proxy_echo_bool(&proxy, 0, &b);

demo_SensorEnvelope env_in;
demo_SensorEnvelope env_out;
demo_SensorService_proxy_echo_envelope(&proxy, &env_in, &env_out);

demo_int32_t_array ids_in;
demo_int32_t_array ids_out;
demo_SensorService_proxy_echo_id_array(&proxy, &ids_in, &ids_out);

demo_SensorService_proxy_subscribe_sensor_update(&proxy, on_sensor_update, NULL);
demo_SensorService_proxy_subscribe_async_result_ready(&proxy, on_async_result, NULL);

demo_AsyncRequest async_req;
demo_SensorService_proxy_request_latest_data_async(&proxy, &async_req, &ack);
```

Use `examples/example_c/sensor_client.c` as the authoritative source for the full implementation.

Compared with C++:

- methods return through output pointers rather than direct return values
- all structs need explicit init/destroy calls
- callbacks use function pointers and optional `user_data`

This C client now demonstrates the same feature matrix as the C++ client:

- primitive RPC calls
- custom and nested struct RPC calls
- array RPC calls
- regular topic subscription
- callback-like asynchronous result reception via `AsyncResultReady`
- death notification handling

---

## 7. `omni-cli` usage examples

### 7.1 List services

```bash
./target/bin/omni-cli list
```

### 7.2 Show service details

**Basic mode:**

```bash
./target/bin/omni-cli info SensorService
```

**IDL-aware mode:**

```bash
./target/bin/omni-cli --idl examples/sensor_service.bidl info SensorService
```

### 7.3 Invoke methods

Current `omni-cli` rules:

- with `--idl`, `call` uses **JSON input**
- without `--idl`, parameters must be passed as raw hex payload
- JSON I/O is currently verified to work for:
  - primitive types
  - string
  - regular structs
  - nested structs
  - no-arg methods returning structs
- JSON I/O is **not currently verified** for:
  - `bytes`
  - `array<...>`
  - complex structs containing arrays

#### 7.3.1 Generic form

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService <Method> <JSON-param>
```

#### 7.3.2 Methods that can be called directly with `omni-cli`

**No-arg RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetLatestData
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetSensorCount
```

**Primitive RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoBool false
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt8 7
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt8 7
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt16 16
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt16 16
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt32 32
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt32 32
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt64 64
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt64 64
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat32 1.5
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat64 2.5
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoString '"hello"'
```

> String parameters must be passed as JSON strings, so shell quoting must preserve the inner JSON quotes, for example `'
"hello"'`.

**Regular struct RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoStatus '{"code":7,"message":"demo"}'
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoConfig '{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"}'
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService SetThreshold '{"command_type":1,"target":"temperature","value":30}'
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService RequestLatestDataAsync '{"request_id":42,"client_tag":"cli"}'
```

**Nested struct RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoEnvelope '{"data":{"sensor_id":10,"temperature":18.5,"humidity":45.5,"timestamp":123456789,"location":"Lab-1"},"config":{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"},"captured_at":{"seconds":123456789,"nanos":321}}'
```

**Oneway RPC**

```bash
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService ResetSensor 1
```

#### 7.3.3 Methods not currently recommended through `omni-cli`

The following methods are currently **not verified** through `omni-cli` JSON I/O and may fail with JSON encoding/type resolution errors:

- `EchoBytes(bytes value)`
- `EchoIdArray(array<int32> value)`
- `EchoLabelArray(array<string> value)`
- `EchoSensorArray(array<SensorData> value)`
- `EchoBundle(SensorArrayBundle value)`

Use `examples/example_cpp/sensor_client.cpp` or `examples/example_c/sensor_client.c` to validate those methods.

#### 7.3.4 Service inspection

```bash
./build-host/target/bin/omni-cli list
./build-host/target/bin/omni-cli info SensorService
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl info SensorService
```

See the full guide in [omni-cli Usage Guide](omni-tool-usage.en.md).

---

## 8. End-to-end run flow

### 8.1 Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 8.2 Run

Open four terminals:

1. start ServiceManager
2. start the C++ server
3. start the C++ client
4. inspect services with `omni-cli`

The client will keep waiting for broadcasts until you stop it manually.
For automated verification you can use `timeout 10s ...`.

Typical sequence:

- Terminal 1 starts `service_manager`
- Terminal 2 starts `example_cpp_sensor_server`
- Terminal 3 starts `example_cpp_sensor_client`
- Terminal 4 uses `omni-cli` to inspect the generated service contract

With the current demos, the server side will:

- expose the full RPC matrix
- periodically publish `SensorUpdate`
- publish `AsyncResultReady` when `RequestLatestDataAsync` is triggered

The client side will:

- call primitive / struct / nested / array RPCs
- subscribe to `SensorUpdate`
- subscribe to `AsyncResultReady`
- wait for death notification callbacks until stopped

Example commands:

**Terminal 1: start ServiceManager**

```bash
./target/bin/service_manager --port 9900
```

**Terminal 2: start the C++ server**

```bash
./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900
```

**Terminal 3: start the C++ client**

```bash
./target/example/example_cpp_sensor_client --sm-host 127.0.0.1 --sm-port 9900
```

**Terminal 4: inspect with `omni-cli`**

```bash
./target/bin/omni-cli list
./target/bin/omni-cli info SensorService
./build-host/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetLatestData
```

In steady state you should see:

- the server broadcasting `SensorUpdate`
- the client printing both synchronous RPC results and async topic deliveries
- `AsyncResultReady` published after `RequestLatestDataAsync`

### 8.3 Run tests

```bash
cd build

# unit tests
./target/test/test_buffer
./target/test/test_message
./target/test/test_event_loop

# integration tests
./target/test/test_integration
./target/test/test_full_integration

# or run everything
ctest --output-on-failure
```

Integration tests automatically fork a ServiceManager child process and clean it up afterward.

`test_full_integration` covers scenarios such as:

- dual-channel initialization (TCP + SHM)
- automatic SHM selection for same-host clients
- multi-client SHM sharing
- broadcast / subscribe delivery over SHM
- death notifications after service crash
- service lifecycle changes after unregistering

### 8.4 Validate death notifications

If you stop the C++ server with `Ctrl+C`, the client should print a death-notification message, and ServiceManager should log that the service timed out and subscribers were notified.

Typical client-side output:

```text
!!! [DeathNotify] SensorService has DIED !!!
```

Typical ServiceManager-side output:

```text
[ServiceManager] Service 'SensorService' heartbeat timeout, marking as dead
[ServiceManager] Sending death notification to 1 subscriber(s)
```

---

## 9. Cross-board communication example

When the service and client run on different machines, just point both sides to the same ServiceManager address.

Machine 1 runs ServiceManager and the server.
Machine 2 runs the client.

OmniBinder will detect that the two endpoints have different `host_id` values and will use TCP automatically instead of SHM.

If both run on the same machine, each client creates its own independent SHM region and exchanges eventfds with the server through UDS.

The per-client SHM model uses a unique name format: `/binder_<ServiceName>_cli_<PID>_<N>`. The client sends its SHM name to the server via UDS, and the server replies with `resp_eventfd` (to notify the client when a response is ready) and `master_eventfd` (for the client to signal a new request to the server).

There is no slot allocation or 32-client hard limit. Each client has its own independent SHM ring with no shared resources, eliminating contention on a single shared memory segment. This is already covered in `test_full_integration`.

Typical commands:

**Machine 1 (ServiceManager + server)**

```bash
./target/bin/service_manager --host 0.0.0.0 --port 9900
./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900
```

**Machine 2 (client)**

```bash
./target/example/example_cpp_sensor_client --sm-host 192.168.1.10 --sm-port 9900
```

Expected transport behavior:

- different `host_id` values → TCP transport
- same `host_id` values → SHM transport + UDS eventfd exchange

---

## 10. Building an independent project on top of OmniBinder

This section describes how to build an independent service/client project after OmniBinder has been installed.

### 10.1 Prerequisites

Assume OmniBinder has been installed to `build/install`:

```bash
cd omnibinder
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
make install
```

Install layout typically includes:

```text
build/install/
├── bin_host/
├── include/
└── lib/
```

Then define:

```bash
export OMNIBINDER_DIR=/path/to/omnibinder/build/install
```

### 10.2 Write the IDL

Define your `.bidl` file first, then generate code:

```protobuf
package myapp;

struct Request {
    int32  id;
    string name;
}

struct Response {
    int32  code;
    string message;
}

topic StatusUpdate {
    int32  id;
    string status;
    int64  timestamp;
}

service MyService {
    Response HandleRequest(Request req);
    void     Notify(int32 event_id);

    publishes StatusUpdate;
}
```

```bash
$OMNIBINDER_DIR/bin_host/omni-idlc --lang=cpp --output=generated/ my_service.bidl
$OMNIBINDER_DIR/bin_host/omni-idlc --lang=c   --output=generated/ my_service.bidl
```

The generated files will typically include:

```text
generated/my_service.bidl.h
generated/my_service.bidl.cpp
generated/my_service.bidl_c.h
generated/my_service.bidl.c
```

### 10.3 C++ project layout

```text
my_cpp_project/
├── CMakeLists.txt
├── my_service.bidl
├── server.cpp
└── client.cpp
```

Use `find_package(OmniBinder REQUIRED)` and `omnic_generate(...)` in your `CMakeLists.txt`.

### 10.4 CMake integration

Typical pattern:

```cmake
find_package(OmniBinder REQUIRED)

add_executable(my_server server.cpp)
target_link_libraries(my_server PRIVATE OmniBinder::omnibinder_static)

omnic_generate(
    TARGET my_server
    LANGUAGE cpp
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/my_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_executable(my_client client.cpp)
target_link_libraries(my_client PRIVATE OmniBinder::omnibinder_static)

omnic_generate(
    TARGET my_client
    LANGUAGE cpp
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/my_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)
```

For C projects, the same pattern applies, but generated code will come from `--lang=c` and the service implementation style changes to generated `xxx_impl_*` functions plus `xxx_stub_create(user_data)`.

### 10.5 Practical guidance

For most projects, evolve the interface incrementally:

1. start with one query-style RPC such as `GetStatus()`
2. add one control-style RPC with a request struct
3. add one topic such as `StatusUpdate`
4. expand only after the chain is proven stable

This is the easiest way to validate your service path early.

### 10.6 C++ vs C quick reference

| Feature | C++ | C |
|---|---|---|
| Generated header | `xxx.bidl.h` | `xxx.bidl_c.h` |
| Service implementation | inherit `XxxStub` and override methods | implement generated `xxx_impl_*` functions and call `xxx_stub_create(user_data)` |
| Proxy creation | `XxxProxy proxy(runtime)` | `xxx_proxy proxy; xxx_proxy_init(&proxy, runtime)` |
| RPC return style | direct return values | output pointers |
| Topic subscription | lambdas / functors | function pointers + `user_data` |
| Struct lifecycle | RAII | explicit `_init()` / `_destroy()` |

Practical rule of thumb:

- if you want the most ergonomic service implementation flow, use C++
- if you need a pure C integration boundary, use the generated C API and implement the generated `xxx_impl_*` functions directly
- for both languages, keep your first demo small and only expand after end-to-end service registration, RPC, and topic delivery are confirmed

---

## Related documents

- [Top-level README](../README.en.md)
- [IDL Specification](idl-specification.en.md)
- [omni-cli Usage Guide](omni-tool-usage.en.md)
- [Downstream Sensor/HMI Example](../examples/artifact_sensor_hmi/README.en.md)
