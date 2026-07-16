# OmniBinder

[中文](README.md) | English

**A service communication middleware for embedded Linux and distributed service systems**

OmniBinder is a service communication middleware designed for embedded Linux, distributed multi-board deployments, and multi-service collaboration scenarios.
Its goal is not just to move bytes between processes, but to provide a unified way for services to **register, discover, invoke, broadcast, and observe lifecycle state**.
With a unified service communication and management entry point plus automatic SHM/TCP data path selection, OmniBinder enables cross-process, cross-board, and cross-device collaboration through one consistent programming model.

In one sentence: **OmniBinder acts as a communication bridge between distributed services, standardizing service connectivity, invocation, and data distribution.**

---

## Background

In embedded, industrial, and edge-computing systems, a complete solution is rarely a monolith. It is usually composed of multiple cooperating services: sensor acquisition, algorithm processing, device control, gateway forwarding, HMI, logging, and monitoring.
These modules often run in different processes, on different boards, or even on different devices.

The hard part is usually not a single socket send operation. The real challenge is how to connect those services into a stable communication bridge that is:

- discoverable after startup
- callable through a unified interface model
- able to broadcast state and events to multiple consumers
- capable of notifying peers when a service exits unexpectedly
- independent from deployment topology changes such as same-host vs. cross-host

---

## Where OmniBinder fits compared with common alternatives

OmniBinder is not trying to replace every existing communication framework. It targets a specific engineering problem:
**when a system is composed of multiple services distributed across the same host, multiple boards, or multiple devices, how do you unify service management and service-to-service communication under one model?**

| Category | Representative solutions | Best suited for | How OmniBinder differs |
|---|---|---|---|
| Local IPC / service bus | Android Binder, D-Bus | Same-host service communication, system service orchestration, local process collaboration | These focus more on local system communication. Additional mechanisms are typically needed for cross-board or cross-device scenarios. Application teams often still need to standardize interface contracts and client/server integration on their own. |
| RPC frameworks | gRPC, Thrift, Cap'n Proto RPC | Standardized interface definitions, cross-language RPC, general distributed systems | These frameworks have strong IDL and code generation capabilities. OmniBinder differs by unifying **IDL, Stub/Proxy generation, same-host IPC, and cross-device communication** within one runtime model tailored to embedded/distributed service systems. |
| Message middleware / brokers | MQTT, RabbitMQ, NATS, Kafka | Event distribution, async decoupling, routing, telemetry, and status reporting | These focus more on asynchronous messaging. If you also need service discovery, synchronous RPC, and service lifecycle awareness, they usually need to be combined with other mechanisms. |
| Messaging libraries | ZeroMQ and similar libraries | Flexible point-to-point, pub/sub, and pipeline topologies | These provide lower-level communication primitives. Service registration, contract management, Stub/Proxy generation, and runtime management are still typically built on top. |
| Domain-specific distributed buses | ROS 2, DDS, SOME-IP | Robotics, automotive, real-time control, data-distribution-heavy systems | These are powerful in their target domains but often come with stronger domain assumptions, QoS models, or ecosystem constraints. |
| Service mesh / cloud-native infrastructure | Istio, Linkerd, Envoy | Cloud-native traffic governance, observability, and security | These overlap with some distributed concerns but do not directly replace a unified process-level and service-level communication runtime. |
| Custom protocols | Socket + custom protocol | Fully custom communication chains with maximum control | Very flexible, but as the system evolves into a service-oriented architecture, discovery, interface management, broadcasts, and lifecycle signaling still need to be built separately. |

OmniBinder is especially suitable when:

- you want to unify **same-host process communication** and **cross-device service invocation** under one interface model
- you need more than just data transport, including **service registration/discovery, event broadcast, death notifications, and runtime debugging**
- you want **unified IDL, generated Stub/Proxy code, and a consistent integration model** to reduce repetitive protocol adaptation work
- your deployment targets Linux / embedded systems and you want a relatively lightweight runtime with both **C and C++** support
- you want business code to focus on *which service to call*, not on transport details, connection state, or serialization plumbing

OmniBinder is designed to provide **service registration and discovery, service invocation, event broadcasting, and service state awareness** for distributed service systems while keeping **zero external dependencies, C++11 compatibility, same-host SHM high-speed communication, cross-board TCP communication, and dual C/C++ APIs**.

In short, OmniBinder solves **how services communicate efficiently, consistently, and in a manageable way**, not just how raw packets are transferred.

---

## Core capabilities

OmniBinder covers both **service management** and **service-to-service data paths**:

- **Service registration and discovery** — centralized ServiceManager for service lifecycle tracking
- **Remote method invocation (RPC)** — synchronous request/response and one-way calls
- **Publish/subscribe broadcast** — topic-based broadcast with real-time subscribers
- **Death notification** — automatic notification when a service exits unexpectedly
- **Proactive connection management** — Proxy provides `connect()`/`disconnect()` for explicit service connections
- **Auto-reconnect** — automatic reconnection on service recovery with configurable exponential backoff
- **Heartbeat detection** — periodic heartbeat for service liveness detection, auto-triggers reconnect on timeout
- **Automatic transport selection** — SHM for same-host communication, TCP for cross-host communication
- **Per-service SHM configuration** — default small SHM rings (`4KB / 4KB`) with optional per-service enlargement
- **IDL code generation** — generate Stub/Proxy code from `.bidl` files
- **Zero external dependencies** — only POSIX APIs and standard C++11
- **CLI tooling** — `omni-cli` for service inspection and invocation, with JSON I/O support

### Threading model notes

- `OmniRuntime` exposes thread-safe public APIs
- the internal runtime is serialized by a single owner event loop
- callbacks (such as topic callbacks and death callbacks) run on the owner event-loop thread by default
- synchronous reply wait only processes fd/timer events, not new pending API functors
- see the [threading model document](docs/threading-model.md) for `run()` / `pollOnce()` / `stop()` concurrency details

### Error handling model

- Buffer write APIs return `bool`
- Buffer read APIs use `tryReadXxx(out)` returning `bool`
- server-side `onInvoke()` returns `int` error codes
- parameter deserialization failure typically returns `ERR_DESERIALIZE`
- response serialization failure typically returns `ERR_SERIALIZE`
- the runtime main path no longer relies on C++ exceptions to propagate ordinary protocol errors

---

## Performance Snapshot

The following data highlights typical OmniBinder latency on the **same-host SHM path**.

**Test environment summary:**

- Transport: automatic TCP + SHM selection (SHM used for same-host traffic)
- RPC warmup rounds: 50; RPC measured rounds: 1000 per case
- Topic mode: server publishes rotating topics every 500us; client uses `pollOnce(0)` busy-spin receive loop
- Test service SHM ring: 64KB / 64KB
- Topic warmup rounds: 100; topic measured rounds: 1000 per case

| Test Item | Samples | Average | 95% Case | 99% Case | Notes |
|-----------|---------|---------|----------|----------|-------|
| RPC EchoBytes (0 bytes) | 1000 | 9.9 us | 17 us | 54 us | Empty payload, protocol overhead |
| RPC EchoBytes (64 bytes) | 1000 | 13.4 us | 45 us | 55 us | Common small payload RPC |
| RPC EchoBytes (256 bytes) | 1000 | 11.9 us | 45 us | 60 us | Common small payload RPC |
| RPC EchoBytes (1024 bytes) | 1000 | 19.1 us | 48 us | 66 us | 1KB payload |
| RPC EchoBytes (4096 bytes) | 1000 | 44.9 us | 67 us | 97 us | 4KB payload, enlarged SHM ring |
| RPC EchoBytes (8192 bytes) | 1000 | 61.2 us | 84 us | 119 us | 8KB payload, enlarged SHM ring |
| RPC EchoInt32 | 1000 | 8.9 us | 9 us | 52 us | Small primitive RPC |
| RPC EchoStruct | 1000 | 25.1 us | 58 us | 77 us | Struct RPC |
| RPC Add (2 x int32) | 1000 | 9.6 us | 13 us | 54 us | Small compute RPC |
| Topic pub/sub (0 bytes) | 1000 | 5.6 us | 10 us | 26 us | Empty broadcast payload |
| Topic pub/sub (64 bytes) | 1000 | 4.9 us | 9 us | 15 us | Small broadcast data |
| Topic pub/sub (256 bytes) | 1000 | 5.8 us | 11 us | 25 us | Common small broadcast data |
| Topic pub/sub (1024 bytes) | 1000 | 5.5 us | 10 us | 20 us | 1KB broadcast data |
| Topic pub/sub (4096 bytes) | 1000 | 10.8 us | 18 us | 24 us | 4KB broadcast data |
| Topic pub/sub (8192 bytes) | 1000 | 16.0 us | 26 us | 31 us | 8KB broadcast data |

Based on the full report:

- **Common 0~1024 byte RPC** averages around **9.9~19.1 us**
- **4096~8192 byte RPC payloads** average around **44.9~61.2 us** under enlarged SHM-ring configuration
- **Topic pub/sub** averages around **4.9~16.0 us**

> **Performance note:** the current SHM path uses an `eventfd + EventLoop` event-driven model.
> The latency numbers mainly reflect serialization, shared-memory copies, eventfd wakeups, epoll scheduling, and application-side handling.
> The current performance report uses a service-level `64KB / 64KB` SHM ring rather than the default small SHM-ring configuration.

---

## Typical use cases

| Scenario | Description |
|---|---|
| **Single-board multi-process systems** | Multiple services on one Linux device communicate through a unified local service bus, automatically using SHM for low latency |
| **LAN-scale distributed service systems** | Multiple devices cooperate over Ethernet while preserving one unified service invocation and management model |
| **Embedded gateways / edge nodes** | A gateway aggregates multiple board or peripheral services, bridges internal services, and exposes unified capabilities outward |
| **Robotics / autonomous systems** | Sensors, perception, planning, and control modules run on different compute units and require stable RPC + event distribution |
| **Industrial control** | PLC, HMI, acquisition, and control strategy services need real-time communication and synchronized status updates |

If your system is fundamentally built from **multiple services that collaborate to complete one job**, and you need a consistent communication bridge between them, OmniBinder is designed for that purpose.

---

## Quick start

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

For a normal host-only build, no cross-compilation is needed:

```bash
cmake --install build
```

The install tree looks like:

```text
install/
├── bin_host/
├── include/
└── lib/
```

For cross-compilation, the recommended workflow is a dual-stage build script that produces both host tools and target runtime in one step:

```bash
CC=/path/to/target-gcc CXX=/path/to/target-g++ ./scripts/build_dual_stage_install.sh
```

If the toolchain requires an explicit toolchain file or sysroot, add:

```bash
CC=/path/to/target-gcc CXX=/path/to/target-g++ \
TOOLCHAIN_FILE=/path/to/toolchain.cmake \
./scripts/build_dual_stage_install.sh
```

> **Important**: pass the target compilers through `CC/CXX` when invoking the dual-stage script. The script clears them for its host stage and reuses them for the cross stage.
>
> For detailed toolchain configuration, environment variables, install layout, and manual build steps, see the [ARM Cross-Compilation Guide](docs/cross-compiling-arm.md).

Build options:

| Option | Default | Description |
|---|---|---|
| `OMNIBINDER_BUILD_TESTS` | ON | Build unit and integration tests |
| `OMNIBINDER_BUILD_EXAMPLES` | ON | Build example programs |
| `OMNIBINDER_BUILD_TOOLS` | ON | Build tool targets |
| `OMNIBINDER_BUILD_HOST_IDLC` | ON on host / OFF when cross-compiling | Build the host-side `omni-idlc` |

### Run the built-in example

```bash
# 1. Start ServiceManager
./build/target/bin/service_manager

# 2. Start the service provider
./build/target/example/example_cpp_sensor_server

# 3. Start the service consumer
./build/target/example/example_cpp_sensor_client
```

If you want to verify **how a downstream business project consumes OmniBinder build artifacts**, use the downstream example:

```bash
cmake --install build

# C++ examples
cmake -S examples/artifact_examples/cpp -B build/example_artifacts_cpp \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_cpp -j$(nproc)

# C examples
cmake -S examples/artifact_examples/c -B build/example_artifacts_c \
  -DCMAKE_PREFIX_PATH="$(pwd)/build/install"
cmake --build build/example_artifacts_c -j$(nproc)
```

That example provides both C and CPP versions.
It demonstrates service-to-service RPC calls, topic subscription, and death-notification handling when the sensor service exits.

The built-in demo in `examples/sensor_service.bidl` / `examples/example_cpp/*` / `examples/example_c/*` has also been expanded into a fuller capability matrix covering:

- primitive RPCs (`EchoBool` ~ `EchoFloat64`, `EchoString`, `EchoBytes`)
- custom / nested struct RPCs (`EchoStatus`, `EchoConfig`, `EchoEnvelope`)
- array RPCs (`EchoIdArray`, `EchoLabelArray`, `EchoSensorArray`, `EchoBundle`)
- regular service methods (`GetLatestData`, `SetThreshold`, `ResetSensor`, `GetSensorCount`)
- topic broadcast (`SensorUpdate`)
- async request + topic result push (`RequestLatestDataAsync` + `AsyncResultReady`)
- client-side death notification

For the full example flow, refer to [examples/](examples/) and the [Examples Guide](docs/examples.en.md).

### Use omni-cli for inspection and debugging

**Basic mode (hex I/O):**

```bash
./build/target/bin/omni-cli list
./build/target/bin/omni-cli info SensorService
./build/target/bin/omni-cli call SensorService GetLatestData
```

**IDL-aware mode (JSON I/O):**

```bash
./build/target/bin/omni-cli --idl examples/sensor_service.bidl info SensorService
./build/target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData
./build/target/bin/omni-cli --idl examples/sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"sensor1","value":100}'
```

See the full guide in [omni-cli Usage Guide](docs/omni-tool-usage.en.md).

---

## Programming example

### Define an interface (IDL)

The complete, self-contained generation input is [`examples/sensor_service.bidl`](examples/sensor_service.bidl), together with its imported
[`examples/common_types.bidl`](examples/common_types.bidl). Run the command below from the repository root. Prefer these checked-in examples
over copying an abbreviated snippet that can drift from the generated API.

Generate C++ Stub/Proxy code with `omni-idlc`:

```bash
mkdir -p generated/
./build/target/bin/omni-idlc --lang=cpp --output=generated/ examples/sensor_service.bidl
```

### Server

```cpp
#include "sensor_service.bidl.h"

class MySensorService : public demo::SensorServiceStub {
public:
    bool EchoBool(bool value) override { return !value; }
    int32_t EchoInt32(int32_t value) override { return value + 3; }
    std::string EchoString(const std::string& value) override { return value + "_echo"; }

    demo::SensorData GetLatestData() override { ... }
    common::StatusResponse SetThreshold(const demo::ControlCommand& cmd) override { ... }
    int32_t GetSensorCount() override { return 3; }

    common::StatusResponse RequestLatestDataAsync(const demo::AsyncRequest& request) override {
        demo::AsyncResultReady ready;
        ready.result.request_id = request.request_id;
        ready.result.client_tag = request.client_tag;
        ...
        BroadcastAsyncResultReady(ready);

        common::StatusResponse ack;
        ack.code = 0;
        ack.message = "accepted";
        return ack;
    }
};

int main() {
    omnibinder::OmniRuntime runtime;
    runtime.setRegisterHost("192.168.1.10");
    runtime.init("127.0.0.1", 9900);

    MySensorService service;
    runtime.registerService(&service);
    runtime.publishTopic("SensorUpdate");
    runtime.publishTopic("AsyncResultReady");

    while (running) {
        runtime.pollOnce(100);
        demo::SensorUpdate msg;
        ...
        service.BroadcastSensorUpdate(msg);
    }
}
```

### Client

```cpp
#include "sensor_service.bidl.h"

int main() {
    omnibinder::OmniRuntime runtime;
    runtime.init("127.0.0.1", 9900);

    demo::SensorServiceProxy proxy(runtime);
    proxy.connect();

    bool echo_bool = false;
    proxy.EchoBool(false, echo_bool);

    int32_t echo_i32 = 0;
    proxy.EchoInt32(32, echo_i32);

    std::string echo_string;
    proxy.EchoString("hello", echo_string);

    demo::SensorData latest;
    proxy.GetLatestData(latest);

    demo::ControlCommand cmd;
    cmd.command_type = 1;
    cmd.target = "temperature";
    cmd.value = 30;
    common::StatusResponse resp;
    proxy.SetThreshold(cmd, resp);

    int32_t sensor_count = 0;
    proxy.GetSensorCount(sensor_count);

    proxy.SubscribeSensorUpdate([](const demo::SensorUpdate& msg) {
        // regular topic broadcast
    });

    proxy.SubscribeAsyncResultReady([](const demo::AsyncResultReady& msg) {
        // async result pushed back through topic
    });

    demo::AsyncRequest req;
    req.request_id = 42;
    req.client_tag = "cpp-client";
    common::StatusResponse ack;
    proxy.RequestLatestDataAsync(req, ack);

    proxy.OnServiceDied([]() {
        // service death notification
    });

    while (running) {
        runtime.pollOnce(100);
    }
}
```

See the complete syntax in [IDL Specification](docs/idl-specification.en.md) and the full examples in [Examples Guide](docs/examples.en.md).

---

## Architecture overview

From a responsibility perspective, OmniBinder can be viewed as a **communication bridge middleware** between business services:

- **Upward**: exposes a unified service-oriented programming model
- **Downward**: hides the differences between same-host IPC and cross-host RPC
- **Sideways**: connects services into a discoverable, callable, and broadcast-capable distributed collaboration network

```text
┌─────────────────────────────────────────────────────┐
│                   ServiceManager                    │
│   (registration / discovery / heartbeat / topics)  │
└──────────┬──────────────────────────┬───────────────┘
           │ TCP control channel      │ TCP control channel
     ┌─────┴──────┐            ┌──────┴─────┐
     │ Service A   │◄──────────►│ Service B   │
     │ (provider)  │  SHM/TCP   │ (consumer)  │
     └─────────────┘  data path └─────────────┘
```

- **Control path**: all services talk to ServiceManager over TCP for registration, discovery, heartbeat, and control-plane operations
- **Data path**: services communicate directly; same-host traffic uses SHM automatically, cross-host traffic uses TCP
- **Automatic selection**: `host_id` is used to determine whether SHM can be used

See [Architecture Design](docs/architecture.md) for more details.

---

## Runtime and observability

Current runtime capabilities include:

- thread-safe access to `OmniRuntime` public APIs
- basic ServiceManager reconnect and control-plane recovery
- lightweight runtime statistics: `RuntimeStats / getStats / resetStats`
- standardized error-log keywords for easy grep and log-platform search

Useful log keywords include:

- `sm_connect_failed`
- `sm_connect_timeout`
- `sm_connection_lost`
- `sm_reconnect_begin`
- `sm_reconnect_success`
- `rpc_timeout`
- `data_connect_failed`
- `data_connect_fallback`

---

## Documentation

| Document | Description |
|---|---|
| [Architecture Design](docs/architecture.md) | System architecture, component descriptions, communication flows, threading model |
| [Internal Architecture](docs/architecture-internals.md) | Internal runtime details for secondary developers, data flows, memory model |
| [Communication Protocol](docs/protocol.md) | Binary protocol format, message types, serialization rules, communication sequences |
| [API Reference](docs/api-reference.md) | Complete API for OmniRuntime / Service / Buffer and other core classes |
| [IDL Specification](docs/idl-specification.md) | `.bidl` syntax, type system, and code-generation rules |
| [IDL Specification (EN)](docs/idl-specification.en.md) | English version of the IDL specification |
| [ARM Cross-Compilation Guide](docs/cross-compiling-arm.md) | Host/Target separated build, ARM toolchain, deployment recommendations |
| [omni-cli Usage Guide](docs/omni-tool-usage.md) | CLI usage, JSON mode, and service inspection examples |
| [omni-cli Usage Guide (EN)](docs/omni-tool-usage.en.md) | English version of the omni-cli guide |
| [Testing Guide](docs/testing-guide.md) | Test case descriptions, startup methods, recommended execution |
| [Examples Guide](docs/examples.md) | Complete server/client examples, cross-board communication examples |
| [Examples Guide (EN)](docs/examples.en.md) | English version of the examples guide |
| [Downstream Example](examples/artifact_examples/README.en.md) | Building downstream projects with installed OmniBinder artifacts |
| [Performance Report](docs/performance-report.md) | RPC and topic latency test data (microsecond level) |

The Chinese documentation remains available in the original `.md` files.

---

## Project layout

```text
omnibinder/
├── include/omnibinder/            # Public headers
├── src/                           # Core library implementation
├── service_manager/               # ServiceManager process
├── tools/                         # omni-idlc and omni-cli sources
├── examples/                      # Built-in and downstream examples
├── tests/                         # Unit, integration, performance, and longevity tests
├── docs/                          # Project documentation
└── CMakeLists.txt
```

---

## Environment requirements

- **OS**: Linux (epoll / eventfd / POSIX SHM) or Windows (MinGW / MSVC)
- **Compiler**: GCC 4.8+, Clang 3.4+, MinGW 7.3+, or MSVC 2017+ with C++11 support
- **CMake**: 3.10+
- **External dependencies**: none

---

## Running tests

```bash
cd build

ctest --output-on-failure

# Run the performance benchmark and regenerate the report
./target/test/test_performance -o ../docs/performance-report.md
```

---

## License

MIT
