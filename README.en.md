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
- **Automatic transport selection** — SHM for same-host communication, TCP for cross-host communication
- **Per-service SHM configuration** — default small SHM rings (`4KB / 4KB`) with optional per-service enlargement
- **IDL code generation** — generate Stub/Proxy code from `.bidl` files
- **Zero external dependencies** — only POSIX APIs and standard C++11
- **CLI tooling** — `omni-cli` for service inspection and invocation, with JSON I/O support

### Threading model notes

- `OmniRuntime` exposes thread-safe public APIs
- the internal runtime is serialized by a single owner event loop
- callbacks (such as topic callbacks and death callbacks) run on the owner event-loop thread by default
- see the [threading model document](docs/threading-model.md) for concurrency details

---

## Performance Snapshot

The following data highlights typical OmniBinder latency on the **same-host SHM path**.

**Test environment summary:**

- Transport: automatic TCP + SHM selection (SHM used for same-host traffic)
- RPC warmup rounds: 50; RPC measured rounds: 1000 per case
- Topic warmup rounds: 10; topic measured rounds: 1000 per case

| Test case | Samples | Average | 95% case | 99% case | Notes |
|---|---:|---:|---:|---:|---|
| RPC Echo (0 bytes) | 1000 | 67.0 us | 99 us | 123 us | Empty payload, mostly protocol and scheduling overhead |
| RPC Echo (256 bytes) | 1000 | 66.4 us | 96 us | 130 us | Common small-payload RPC |
| RPC Echo (1024 bytes) | 1000 | 67.1 us | 94 us | 121 us | 1 KB payload |
| RPC Echo (4096 bytes) | 1000 | 81.1 us | 107 us | 126 us | 4 KB payload, measured with enlarged SHM ring |
| RPC Echo (8192 bytes) | 1000 | 93.3 us | 127 us | 168 us | 8 KB payload, measured with enlarged SHM ring |
| RPC Add (2 x int32) | 1000 | 68.8 us | 95 us | 127 us | Small compute-style RPC |
| Topic pub/sub (64 bytes) | 1000 | 58.0 us | 91 us | 113 us | Small broadcast payload |
| Topic pub/sub (256 bytes) | 1000 | 53.4 us | 83 us | 107 us | Common small broadcast payload |
| Topic pub/sub (1024 bytes) | 1000 | 56.2 us | 87 us | 103 us | 1 KB broadcast payload |
| Topic pub/sub (8192 bytes) | 1000 | 74.3 us | 109 us | 134 us | 8 KB broadcast payload |

Based on the full report:

- **Common 0~1024 byte RPC** averages around **65.9~67.1 us**
- **4096~8192 byte RPC payloads** average around **81.1~93.3 us** under enlarged SHM-ring configuration
- **Topic pub/sub** averages around **53.4~74.3 us**

> **Performance note:** the current SHM path uses an `eventfd + EventLoop` event-driven model.
> The latency numbers mainly reflect serialization, shared-memory copies, eventfd wakeups, epoll scheduling, and application-side handling.
> The `4096 / 8192 bytes` cases use a service-level `16KB / 16KB` SHM ring rather than the default `4KB / 4KB` configuration.

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

Build options:

| Option | Default | Description |
|---|---|---|
| `OMNIBINDER_BUILD_TESTS` | ON | Build unit and integration tests |
| `OMNIBINDER_BUILD_EXAMPLES` | ON | Build example programs |
| `OMNIBINDER_BUILD_TOOLS` | ON | Build `omni-idlc` and `omni-cli` |

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

./examples/artifact_sensor_hmi/scripts/build_downstream_example.sh
./examples/artifact_sensor_hmi/scripts/run_cpp_stack.sh
```

That example provides both `sensor_cpp` / `hmi_cpp` and `sensor_c` / `hmi_c` stacks.
It demonstrates HMI-side RPC calls, topic subscription, and death-notification handling when the sensor service exits.

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

```bidl
package demo;

struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    string  location;
}

topic SensorUpdate {
    SensorData data;
}

service SensorService {
    SensorData GetLatestData();
    void       ResetSensor();
}
```

Generate C++ Stub/Proxy code with `omni-idlc`:

```bash
./build/target/bin/omni-idlc sensor_service.bidl --lang cpp --outdir generated/
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
| [Examples Guide](docs/examples.en.md) | Full server/client examples, cross-board communication examples, and downstream build examples |
| [IDL Specification](docs/idl-specification.en.md) | `.bidl` syntax, type system, and code-generation rules |
| [omni-cli Usage Guide](docs/omni-tool-usage.en.md) | CLI usage, JSON mode, and service inspection examples |
| [Downstream Sensor/HMI Example](examples/artifact_sensor_hmi/README.en.md) | How to build a standalone downstream business project with installed OmniBinder artifacts |
| [Performance Report](docs/performance-report.md) | Detailed RPC and topic latency data |

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

- **OS**: Linux (requires epoll, eventfd, POSIX SHM)
- **Compiler**: GCC 4.8+ or Clang 3.4+ with C++11 support
- **CMake**: 3.10+
- **External dependencies**: none

---

## Running tests

```bash
cd build

ctest --output-on-failure

# Run the performance benchmark and regenerate the report
./target/test/test_performance --report ../docs/performance-report.md
```

---

## License

MIT
