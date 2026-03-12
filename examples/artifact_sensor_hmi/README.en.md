# Sensor ↔ HMI Downstream Integration Example

[中文](README.md) | English

This document is not just a “how to run the demo” note. It explains `examples/artifact_sensor_hmi/` as a **template for downstream business projects**.

It shows:

- how your own project can depend on OmniBinder build artifacts
- how to write `.bidl` interface definitions
- how to generate C / C++ Stub / Proxy code
- how to implement the sensor-side service
- how to implement the HMI-side client
- how to perform RPC calls
- how to implement publish / subscribe flows
- how to handle death notifications
- how to reason about threading model and callback-thread semantics

If you follow this README, you should be able to turn the example into your own business project quickly.

---

## 1. What problem this example solves

This example demonstrates a **downstream consumption build model**.

It is different from the in-tree examples under `examples/example_cpp` and `examples/example_c`:

- the built-in examples depend directly on in-repo CMake targets
- this example behaves like a **real downstream project**, depending on:
  - the exported OmniBinder package (`find_package(OmniBinder REQUIRED)`)
  - the built / installed `omni-idlc` compiler

That makes this directory a better template for your own product codebase.

---

## 2. Example scenario

The scenario models a typical industrial / embedded system:

- `sensor_*`: the sensor service process, acting as the service provider
- `hmi_*`: the HMI / upper-computer process, acting as the service consumer

Business relationship:

1. the HMI invokes control RPCs on the sensor service
2. the sensor service periodically broadcasts real-time status
3. the HMI subscribes to those broadcasts to refresh its view
4. when the sensor process exits, the HMI receives a death notification and enters an offline-waiting state
5. when the sensor comes back, the HMI continues probing and relies on the library recovery path to rebuild the call chain

The example provides both:

- a C++ stack: `sensor_cpp` / `hmi_cpp`
- a C stack: `sensor_c` / `hmi_c`

---

## 3. Directory structure

```text
examples/artifact_sensor_hmi/
├── CMakeLists.txt
├── README.md
├── README.en.md
├── idl/
│   ├── sensor_hmi_common.bidl
│   └── sensor_hmi_service.bidl
├── sensor_cpp/
│   └── sensor_service.cpp
├── hmi_cpp/
│   └── hmi_client.cpp
├── sensor_c/
│   └── sensor_service.c
├── hmi_c/
│   └── hmi_client.c
└── scripts/
    ├── build_downstream_example.sh
    ├── run_cpp_stack.sh
    └── run_c_stack.sh
```

Responsibilities:

- `idl/`: business interfaces and data structures
- `sensor_cpp/`, `sensor_c/`: server implementations
- `hmi_cpp/`, `hmi_c/`: client implementations
- `CMakeLists.txt`: how a downstream project references OmniBinder
- `scripts/`: one-command build and one-command launch helpers

---

## 4. Recommended project organization for your own codebase

Suggested layout:

```text
my_app/
├── CMakeLists.txt
├── idl/
│   ├── common_types.bidl
│   └── my_service.bidl
├── service/
│   └── my_service_impl.cpp
├── client/
│   └── my_client.cpp
└── tools/
```

Recommended layers:

1. **IDL layer** — external interface and data model definitions
2. **Service layer** — implementations of generated Stub interfaces
3. **Client layer** — Proxy-based RPC, topic subscription, and death-notification handling

Why this structure works well:

- interface definitions stay decoupled from implementations
- C and C++ can share the same `.bidl` files
- adding new clients later becomes cheaper

---

## 5. Build prerequisites

First build and install OmniBinder from the repository root:

```bash
cmake -S . -B build
cmake --build build -j4
cmake --install build
```

Make sure `omni-idlc` is discoverable:

```bash
export PATH="$(pwd)/build/install/bin:$PATH"
```

This matters because `omnic_generate()` in downstream projects finds `omni-idlc` through `PATH`.

---

## 6. Building the downstream example

### 6.1 One-command build

```bash
./examples/artifact_sensor_hmi/scripts/build_downstream_example.sh
```

The script will:

1. rebuild the OmniBinder main project
2. install it to `build/install`
3. configure the downstream project at `build/example_sensor_hmi`
4. build:
   - `sensor_cpp`
   - `hmi_cpp`
   - `sensor_c`
   - `hmi_c`

### 6.2 Manual build

```bash
cmake -S examples/artifact_sensor_hmi -B build/example_sensor_hmi \
  -DOmniBinder_DIR="$(pwd)/build/install/lib/cmake/OmniBinder"

cmake --build build/example_sensor_hmi -j4
```

Key points:

- `OmniBinder_DIR` points to the installed CMake package directory
- the downstream project gets exported library targets through `find_package(OmniBinder REQUIRED)`
- `omnic_generate()` invokes `omni-idlc` to generate code

---

## 7. How the downstream CMake should look

Core idea:

```cmake
find_package(OmniBinder REQUIRED)

add_executable(sensor_cpp sensor_cpp/sensor_service.cpp)
target_link_libraries(sensor_cpp PRIVATE OmniBinder::omnibinder_static)

omnic_generate(
    TARGET sensor_cpp
    LANGUAGE cpp
    FILES ${BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp
)
```

In your own project you usually only replace:

- target names
- source file paths
- `.bidl` file list

More details:

- [IDL Specification](../../docs/idl-specification.en.md)
- [Examples Guide](../../docs/examples.en.md)

---

## 8. How to design the IDL

This example uses two IDL files:

- `idl/sensor_hmi_common.bidl`
- `idl/sensor_hmi_service.bidl`

### 8.1 Common types

```bidl
package hmi_common;

struct OperationResult {
    int32 code;
    string message;
}
```

Good candidates for this kind of file:

- common return types
- shared error models
- common structs reused across services

### 8.2 Service IDL

```bidl
package sensor_hmi;

import "sensor_hmi_common.bidl";

struct SensorSnapshot {
    int32 sensor_id;
    bool enabled;
    int32 sampling_interval_ms;
    float64 current_value;
    string mode;
    int64 timestamp_ms;
}

struct SensorControlRequest {
    bool enabled;
    int32 sampling_interval_ms;
    string mode;
}

topic SensorStatusTopic {
    SensorSnapshot snapshot;
    int64 publish_time_ms;
}

service SensorControlService {
    SensorSnapshot GetSnapshot();
    hmi_common.OperationResult ApplyControl(SensorControlRequest request);
    void TriggerCalibration(int32 sensor_id);

    publishes SensorStatusTopic;
}
```

This IDL covers the most common capabilities:

1. RPC with return value — `GetSnapshot()`
2. RPC with request parameters — `ApplyControl(...)`
3. one-way invocation — `TriggerCalibration(...)`
4. publish/subscribe — `publishes SensorStatusTopic`

Practical recommendations:

- use query-style RPCs such as `GetStatus()` for full snapshots
- use request structs for control-style RPCs such as `ApplyControl(ControlRequest)`
- define topics separately for status or alarm broadcasts
- use common response structs like `OperationResult` / `StatusResponse`

See full syntax rules in [IDL Specification](../../docs/idl-specification.en.md).

---

## 9. What code generation produces

After running `omni-idlc`, you get two generated API sets.

### 9.1 C++ side

Typical generated files:

- `sensor_hmi_service.bidl.h`
- `sensor_hmi_service.bidl.cpp`

Typical generated symbols:

- `sensor_hmi::SensorControlServiceStub`
- `sensor_hmi::SensorControlServiceProxy`
- `sensor_hmi::SensorStatusTopic`
- `sensor_hmi::SensorSnapshot`
- `sensor_hmi::SensorControlRequest`

### 9.2 C side

Typical generated files:

- `sensor_hmi_service.bidl_c.h`
- `sensor_hmi_service.bidl.c`

Typical generated symbols:

- `sensor_hmi_SensorControlService_callbacks`
- `sensor_hmi_SensorControlService_stub_create(...)`
- `sensor_hmi_SensorControlService_proxy_*`
- `sensor_hmi_SensorControlService_broadcast_sensor_status_topic(...)`

If you want to inspect the exact generated API shape, check:

- `build/example_sensor_hmi/generated/cpp/`
- `build/example_sensor_hmi/generated/c/`

---

## 10. How to implement the service (`sensor`)

### 10.1 C++ service idea

Reference:

- `sensor_cpp/sensor_service.cpp`

Core steps:

1. create `OmniRuntime`
2. connect to `ServiceManager` with `init()`
3. inherit from the generated `SensorControlServiceStub`
4. implement IDL-defined methods
5. call `registerService()`
6. call `publishTopic()`
7. in the main loop, `pollOnce()` and broadcast data

### 10.2 C service idea

Reference:

- `sensor_c/sensor_service.c`

Differences:

- no class inheritance in C
- implement service behavior through `sensor_hmi_SensorControlService_callbacks`
- create service objects with `sensor_hmi_SensorControlService_stub_create()`

### 10.3 Design advice

Split server code into two layers:

- **IDL interface layer** — unpack inputs and wrap outputs
- **business state layer** — maintain actual business state, hardware state, and configuration

This avoids putting too much business logic directly into callback handlers.

---

## 11. How to implement the client (`hmi`)

### 11.1 C++ client idea

Reference:

- `hmi_cpp/hmi_client.cpp`

Core steps:

1. create `OmniRuntime`
2. connect to `ServiceManager`
3. create the generated Proxy
4. `connect()` to the service
5. invoke RPCs
6. subscribe to topics
7. register death notifications
8. handle callbacks inside the event loop

### 11.2 C client idea

Reference:

- `hmi_c/hmi_client.c`

The C version follows the same logic but uses generated init/connect/invoke/subscribe function families.

---

## 12. RPC patterns

### 12.1 Query-style RPC

Example: `GetSnapshot()`

Useful when the HMI needs a full state snapshot before switching to subscription-driven updates.

### 12.2 Control-style RPC

Example: `ApplyControl(SensorControlRequest request)`

Prefer wrapping control parameters into one request struct instead of exposing many loose parameters.

### 12.3 One-way invocation

Example: `TriggerCalibration(int32 sensor_id)`

Useful for fire-and-forget actions where no return value is needed.

---

## 13. Publish / subscribe patterns

In this example:

- the service publishes `SensorStatusTopic`
- the client subscribes through `SubscribeSensorStatusTopic(...)`

Service side:

1. call `runtime.publishTopic("SensorStatusTopic")`
2. assemble the topic struct
3. publish through the generated helper

Client side:

- subscribe with the generated helper and update UI / cache in the callback

Recommendations:

- make topic payloads directly consumable by the receiving side
- prefer complete state snapshots over too many tiny topics when appropriate
- keep callbacks lightweight if message frequency is high

---

## 14. Death notification

Death notification tells you whether a service is still alive.

In this example, the HMI registers:

- C++: `proxy.OnServiceDied(...)`
- C: `sensor_hmi_SensorControlService_proxy_on_service_died(...)`

Typical uses:

- switch UI state to offline
- stop sending control commands
- notify the operator that the peer process exited
- trigger reconnect or retry strategies

Important mindset:

- do **not** treat death notification as “automatic reconnection is already done”
- it is primarily a **state change signal**
- your business logic still decides whether to enter offline mode, wait for restart, retry connect, or rebuild subscriptions

This example keeps the HMI alive and continues probing instead of implementing a separate heavyweight application-layer reconnection state machine.

---

## 15. Threading model and callback-thread semantics

Current external semantics:

- `OmniRuntime` public APIs are thread-safe
- the internal runtime is serialized by a single owner event loop
- callbacks (topic / death callback) run on the owner event-loop thread by default

You can think of it as:

- multiple threads may safely call the same `OmniRuntime`
- internal event handling is still advanced by one owner loop
- callbacks do not jump to arbitrary threads by default

Practical usage patterns:

- **Dedicated loop thread** with `run()` for more complex systems
- **Periodic `pollOnce()`** inside an existing main loop for simpler processes

Important constraints:

- do not call `run()` / `pollOnce()` concurrently on the same runtime from multiple threads
- avoid issuing synchronous blocking RPCs to the same runtime from inside topic or death callbacks

See also:

- [Threading Model](../../docs/threading-model.md)
- [API Reference](../../docs/api-reference.md)

---

## 16. How to turn this into your own project

Practical migration path:

1. **Copy the directory skeleton** to your own project directory
2. **Change the IDL first**
3. **Then update service/client implementations** to match new generated APIs
4. **Finally update scripts and docs**

Recommended evolution path:

1. add one `GetStatus()` RPC
2. add one `ApplyControl(request)` RPC
3. add one `StatusUpdate` topic
4. then expand with more methods and richer structs

This is usually the fastest way to validate the communication chain correctly.

---

## 17. Running the example

### 17.1 Start the C++ stack manually

Terminal 1:

```bash
./build/target/bin/service_manager
```

Terminal 2:

```bash
./build/example_sensor_hmi/bin/sensor_cpp
```

Terminal 3:

```bash
./build/example_sensor_hmi/bin/hmi_cpp
```

### 17.2 Start the C stack manually

Terminal 1:

```bash
./build/target/bin/service_manager
```

Terminal 2:

```bash
./build/example_sensor_hmi/bin/sensor_c
```

Terminal 3:

```bash
./build/example_sensor_hmi/bin/hmi_c
```

### 17.3 One-command startup

```bash
./examples/artifact_sensor_hmi/scripts/run_cpp_stack.sh
./examples/artifact_sensor_hmi/scripts/run_c_stack.sh
```

Those scripts will:

- build the main project
- install OmniBinder
- build the downstream example
- start `service_manager + sensor + hmi`

Notes:

- the default ServiceManager port `9900` must not already be in use
- on interactive terminals, the scripts attach to a tmux session automatically
- if you want background startup, use `--detach`

---

## 18. How to verify that the example works

Expected behavior for the C++ version:

- `hmi_cpp` fetches an initial snapshot successfully
- `ApplyControl` succeeds
- `TriggerCalibration` succeeds
- RPC latency is printed periodically
- broadcasts keep arriving and print `pubsub_latency=...`
- after stopping `sensor_cpp`, `hmi_cpp` prints a death notification and stays alive
- after restarting `sensor_cpp`, `hmi_cpp` prints a recovery message once probing succeeds again

Expected behavior for the C version is equivalent.

---

## 19. Recommended follow-up reading

- [IDL Specification](../../docs/idl-specification.en.md)
- [API Reference](../../docs/api-reference.md)
- [Threading Model](../../docs/threading-model.md)
- [Examples Guide](../../docs/examples.en.md)
- [Architecture](../../docs/architecture.md)

---

## 20. One-sentence summary

> First define service interfaces, control requests, and broadcast messages in `.bidl`, then let the downstream project use `find_package(OmniBinder)` + `omnic_generate()` to generate Stub / Proxy code, and finally implement the service and client independently.

That is the most practical path for integrating OmniBinder into a real business project.
