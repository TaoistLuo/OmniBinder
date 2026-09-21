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

struct NameValue {
    string name;
    int32  value;
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
mkdir -p generated/

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

### 3.1 CMakeLists.txt

```cmake
# examples/example_cpp/CMakeLists.txt

add_executable(example_cpp_sensor_server sensor_server.cpp)
target_link_libraries(example_cpp_sensor_server PRIVATE omnibinder_static)

# IDL code generation (C++)
if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_cpp_sensor_server
        LANGUAGE cpp
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
endif()
```

### 3.2 `sensor_server.cpp`

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

### 4.1 CMakeLists.txt

```cmake
# examples/example_cpp/CMakeLists.txt

add_executable(example_cpp_sensor_client sensor_client.cpp)
target_link_libraries(example_cpp_sensor_client PRIVATE omnibinder_static)

# The client uses the Proxy generated from the same IDL
if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_cpp_sensor_client
        LANGUAGE cpp
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
endif()
```

### 4.2 `sensor_client.cpp`

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

bool bool_out = false;
proxy.EchoBool(false, bool_out);

int32_t int_out = 0;
proxy.EchoInt32(32, int_out);

common::StatusResponse status_out;
proxy.EchoStatus(status, status_out);

demo::SensorEnvelope envelope_out;
proxy.EchoEnvelope(envelope, envelope_out);

std::vector<int32_t> ids_out;
proxy.EchoIdArray(ids, ids_out);

demo::SensorArrayBundle bundle_out;
proxy.EchoBundle(bundle, bundle_out);

proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) { ... });
proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& msg) { ... });

demo::AsyncRequest req;
req.request_id = 42;
req.client_tag = "cpp-client";
common::StatusResponse ack;
proxy.RequestLatestDataAsync(req, ack);
```

Use `examples/example_cpp/sensor_client.cpp` as the authoritative source for the full implementation.

---

## 5. C server

### 5.1 CMakeLists.txt

```cmake
# examples/example_c/CMakeLists.txt

add_executable(example_c_sensor_server sensor_server.c)
target_link_libraries(example_c_sensor_server PRIVATE omnibinder_static)

if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_c_sensor_server
        LANGUAGE c
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
endif()
```

### 5.2 `sensor_server.c`

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

`list` enumerates service **instances** (`(service_name, host_id, instance_id)` per row):

```bash
./build/target/bin/omni-cli list
```

### 7.2 Show service details

**Basic mode:**

```bash
./build/target/bin/omni-cli info SensorService
```

**Select a specific instance:**

```bash
./build/target/bin/omni-cli info SensorService --instance-id ttyUSB1
```

**IDL-aware mode:**

```bash
./build/target/bin/omni-cli --idl examples/sensor_service.bidl info SensorService
```

### 7.3 Invoke methods

Current `omni-cli` rules:

- with `--idl`, `call` uses **JSON input**
- without `--idl`, parameters must be passed as raw hex payload
- the target instance is selected with `--instance-id` / `--host-id`; without a selector `call` uses the deterministic first instance and reports it
- JSON I/O supports:
  - primitive types
  - string
  - regular structs
  - nested structs
  - no-arg methods returning structs
  - arrays and nested complex structs that do not contain `bytes`
- the current JSON codec does not support `bytes`, including arrays/structs containing it; every declared struct field is required

#### 7.3.1 Generic form

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService <Method> <param>
```

#### 7.3.2 Methods that can be called directly with `omni-cli`

**No-arg RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetLatestData
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetSensorCount
```

**Primitive RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoBool false
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt8 7
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt8 7
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt16 16
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt16 16
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt32 32
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt32 32
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoInt64 64
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoUInt64 64
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat32 1.5
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoFloat64 2.5
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoString hello
```

> String scalar parameters are passed as raw CLI text. For example, use `EchoString hello`; writing `EchoString '"hello"'` includes the quote characters in the string.

**Regular struct RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoStatus '{"code":7,"message":"demo"}'
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoConfig '{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"}'
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService SetThreshold '{"command_type":1,"target":"temperature","value":30}'
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService RequestLatestDataAsync '{"request_id":42,"client_tag":"cli"}'
```

**Nested struct RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService EchoEnvelope '{"data":{"sensor_id":10,"temperature":18.5,"humidity":45.5,"timestamp":123456789,"location":"Lab-1"},"config":{"enabled":true,"sample_rate_hz":25,"label":"sensor-main"},"captured_at":{"seconds":123456789,"nanos":321}}'
```

**Oneway RPC**

```bash
./build/target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService ResetSensor 1
```

#### 7.3.3 Methods not currently recommended through `omni-cli`

The following methods contain `bytes` and are rejected by the current JSON codec. Other arrays can be supplied as JSON beginning with `[`. Affected methods:

- `EchoBytes(bytes value)`
- `EchoBundle(SensorArrayBundle value)`

Use `examples/example_cpp/sensor_client.cpp` or `examples/example_c/sensor_client.c` to validate those methods.

#### 7.3.4 Service inspection

`list` and `info` are covered in sections 7.1 and 7.2 above.

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
./target/bin/omni-cli --idl ./examples/sensor_service.bidl call SensorService GetLatestData
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
./target/test/test_transport
./target/test/test_shm_transport
./target/test/test_service_registry
./target/test/test_heartbeat_death
./target/test/test_topic_manager
./target/test/test_idl_compiler

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
- independent per-client SHM regions (3 clients concurrent + 5 clients sequential)
- broadcast / subscribe delivery over SHM
- death notifications after service crash
- service lifecycle changes after unregistering

### 8.4 Validate death notifications

If you stop the C++ server with `Ctrl+C`, the client should print a death-notification message, and ServiceManager should log that the service timed out and subscribers were notified.

Typical client-side output:

```text
[SensorUpdate] id=1 temp=25.15 humidity=59.78

!!! [DeathNotify] SensorService has DIED !!!

```

Typical ServiceManager-side output:

```text
[WARN][HeartbeatMonitor] Service SensorService timed out (missed 1 heartbeats)
[WARN][ServiceManager] Service timed out: SensorService (host_id=<host_id>, instance_id=)
[INFO][DeathNotifier] Instance SensorService (host_id=<host_id>, instance_id=) died, name_gone=1, notifying 1 subscribers
```

### 8.5 Run two same-name service instances on one board

To run several **same-name** services on one board, each must declare a **distinct `instance_id`**: an instance is identified by `(service_name, host_id, instance_id)`, and an exactly identical identity is treated as the same instance and taken over (newer wins). The C++ example below runs two `SensorService` instances.

**Terminal 1: start ServiceManager**
```bash
$ ./target/bin/service_manager --port 9900
```

**Terminal 2: start instance A (`--instance-id ttyUSB0`)**
```bash
$ ./target/example/example_cpp_sensor_server --instance-id ttyUSB0
=== SensorService (C++ Server) Starting ===
ServiceManager: 127.0.0.1:9900
InstanceId: ttyUSB0
SensorService registered on port <A> (host_id=<host>, instance_id=ttyUSB0)
```

**Terminal 3: start instance B (`--instance-id ttyUSB1`)**
```bash
$ ./target/example/example_cpp_sensor_server --instance-id ttyUSB1
...
SensorService registered on port <B> (host_id=<host>, instance_id=ttyUSB1)
```

ServiceManager (terminal 1) then logs two registrations, with both instances coexisting:

```
Registered service: SensorService (handle=..., fd=..., host=127.0.0.1, port=<A>, host_id=<host>, instance_id=ttyUSB0)
Registered service: SensorService (handle=..., fd=..., host=127.0.0.1, port=<B>, host_id=<host>, instance_id=ttyUSB1)
```

> If both instances use the default empty `instance_id`, the second log line becomes
> `Service instance taken over by newer registration: SensorService (... instance_id=)`,
> meaning the newer one takes over the older and only one instance remains in the registry.
>
> **Known boundary (takeover churn)**: takeover applies to an **exactly identical identity** `(service_name, host_id, instance_id)`. If two **live** processes declare the same identity, ServiceManager lets the newer one take over and closes the older control connection; the displaced live instance reconnects and re-registers automatically, so the two can repeatedly take each other over (takeover churn). This is an accepted boundary, not a defect: operators must give same-name services on one board **distinct** `instance_id` values and must never run two live instances with the exact same identity.

**Terminal 4: client lists instances, then binds to one**
```bash
$ ./target/example/example_cpp_sensor_client --instance ttyUSB1
=== SensorClient (C++ Client) Starting ===
ServiceManager: 127.0.0.1:9900
Discovered 2 SensorService instance(s):
  [0] host_id=<host> instance_id=ttyUSB0 host=127.0.0.1 port=<A>
  [1] host_id=<host> instance_id=ttyUSB1 host=127.0.0.1 port=<B>
Connected to SensorService instance_id=ttyUSB1
...
```

Without `--instance`, the client connects by service name, hitting the deterministic first instance by registry instance key.

The C example takes the same arguments (server `--instance-id`, client `--instance`) and uses the runtime instance APIs underneath:

```c
/* server: declare the instance id before registering */
omni_service_t* svc = demo_SensorService_stub_create(runtime);
omni_service_set_instance_id(svc, "ttyUSB0");
omni_runtime_register_service(runtime, svc);

/* client: enumerate instances, then bind to one */
omni_runtime_list_instances(runtime, "SensorService", on_instance, &lookup);
demo_SensorService_proxy_connect_instance(&proxy, lookup.found_host_id, lookup.found_instance_id);
```

> For cross-board deployment, `host_id` is part of instance addressing, so make each board's `host_id` unique: cloned images share `/etc/machine-id`, so set each device's unique id (MAC / chip id / serial) at startup via `setHostId()`.

---

## 9. Cross-board communication example

When the service and client run on different machines, point both sides to the same ServiceManager and register a host address that the client can reach.

Machine 1 runs ServiceManager and the server.
Machine 2 runs the client.

OmniBinder will detect that the two endpoints have different `host_id` values and will use TCP automatically instead of SHM.

If both run on the same machine, each client creates its own independent SHM region and exchanges eventfds with the server through UDS.

The per-client SHM model uses the name format `/binder_<service name prefix>_<FNV-1a hash>`; `host_id` and `instance_id` participate in the hash but do not appear in the name (so same-name instances and different boards get different handshake paths). The client sends its SHM name to the server via UDS, and the server returns two eventfds: one notifies the client when a response is ready, the other lets the client signal a new request to the server.

Each client has its own independent SHM ring with no shared resources, eliminating contention on a single shared memory segment. This is already covered in `test_full_integration`.

Typical commands:

**Machine 1 (ServiceManager + server)**

```bash
./target/bin/service_manager --host 0.0.0.0 --port 9900
./target/example/example_cpp_sensor_server --sm-host 127.0.0.1 --sm-port 9900 --register-host 192.168.1.10
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
mkdir -p generated/
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

### 10.5 Full C++ example code

#### server.cpp

```cpp
#include "my_service.bidl.h"
#include <cstdio>
#include <csignal>
#include <ctime>

static volatile bool g_running = true;
void signalHandler(int) { g_running = false; }

class MyServiceImpl : public myapp::MyServiceStub {
public:
    myapp::Response HandleRequest(const myapp::Request& req) override {
        printf("[Server] HandleRequest: id=%d name=%s\n", req.id, req.name.c_str());
        myapp::Response resp;
        resp.code = 0;
        resp.message = "OK";
        return resp;
    }

    void Notify(int32_t event_id) override {
        printf("[Server] Notify: event_id=%d\n", event_id);
    }
};

int main() {
    signal(SIGINT, signalHandler);

    omnibinder::OmniRuntime runtime;
    if (runtime.init("127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }

    MyServiceImpl service;
    runtime.registerService(&service);
    service.PublishStatusUpdate();

    printf("Server running (Ctrl+C to stop)...\n");
    while (g_running) {
        runtime.pollOnce(100);
    }

    runtime.unregisterService(&service);
    runtime.stop();
    return 0;
}
```

#### client.cpp

```cpp
#include "my_service.bidl.h"
#include <cstdio>
#include <csignal>

static volatile bool g_running = true;
void signalHandler(int) { g_running = false; }

int main() {
    signal(SIGINT, signalHandler);

    omnibinder::OmniRuntime runtime;
    if (runtime.init("127.0.0.1", 9900) != 0) {
        fprintf(stderr, "Failed to connect to ServiceManager\n");
        return 1;
    }

    myapp::MyServiceProxy proxy(runtime);
    if (proxy.connect() != 0) {
        fprintf(stderr, "MyService not found\n");
        return 1;
    }

    // RPC call
    myapp::Request req;
    req.id = 1;
    req.name = "hello";
    myapp::Response resp;
    proxy.HandleRequest(req, resp);
    printf("Response: code=%d message=%s\n", resp.code, resp.message.c_str());

    // Subscribe to broadcast
    proxy.SubscribeStatusUpdate([](const myapp::StatusUpdate& msg) {
        printf("[Broadcast] id=%d status=%s\n", msg.id, msg.status.c_str());
    });

    // Subscribe to death notification
    proxy.OnServiceDied([]() {
        printf("!!! MyService DIED !!!\n");
    });

    while (g_running) {
        runtime.pollOnce(100);
    }

    runtime.stop();
    return 0;
}
```

#### Build and run

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$OMNIBINDER_DIR
make -j$(nproc)

# Run (start ServiceManager first)
# Use bin_host for a normal host install; use bin_cross when deploying to a target board.
$OMNIBINDER_DIR/bin_host/service_manager &
./my_server &
./my_client
```

### 10.6 Practical guidance

For most projects, evolve the interface incrementally:

1. start with one query-style RPC such as `GetStatus()`
2. add one control-style RPC with a request struct
3. add one topic such as `StatusUpdate`
4. expand only after the chain is proven stable

This is the easiest way to validate your service path early.

### 10.7 C++ vs C quick reference

| Feature | C++ | C |
|---|---|---|
| Generated header | `xxx.bidl.h` | `xxx.bidl_c.h` |
| Service implementation | inherit `XxxStub` and override methods | implement generated `xxx_impl_*` functions and call `xxx_stub_create(user_data)` |
| Proxy creation | `XxxProxy proxy(runtime)` | `xxx_proxy proxy; xxx_proxy_init(&proxy, runtime)` |
| RPC return style | output references | output pointers |
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
- [Downstream Sensor/HMI Example](../examples/artifact_examples/README.en.md)
