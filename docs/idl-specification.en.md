# OmniBinder IDL Specification

[中文](idl-specification.md) | English

## 1. Overview

OmniBinder IDL (Interface Definition Language) uses a protobuf-like syntax to define:

- service interfaces
- data structures
- broadcast topics

IDL files use the `.bidl` extension and are compiled by `omni-idlc` to generate C and C++ code.

---

## 2. File structure

Basic structure of a `.bidl` file:

```text
// file comments

package <package_name>;

import "path/to/dependency.bidl";   // optional

struct <Name> {
    <type> <field_name>;
}

topic <Name> {
    <type> <field_name>;
}

service <Name> {
    <return_type> <Method>(<param_type> <param_name>);
    publishes <TopicName>;
}
```

### 2.1 Import declaration

An `import` statement must appear after the `package` declaration and before any type definitions.

Syntax:

```text
import "<path>";
```

Path resolution rules:

- **relative path**: resolved against the current `.bidl` directory
- **absolute path**: resolved as-is

Cross-package type references use `package_name.TypeName`, for example:

```text
package demo;

import "common_types.bidl";

struct MapData {
    common.Point center;
    float32 zoom_level;
}

service MapService {
    common.StatusResponse UpdateLocation(common.Point p);
}
```

Generated code mapping:

| IDL type | Generated C++ | Generated C |
|---|---|---|
| `common.Point` | `common::Point` | `common_Point` |
| `common.StatusResponse` | `common::StatusResponse` | `common_StatusResponse` |

The generated headers automatically include imported headers.

Rules and limitations:

- cyclic imports are not allowed
- the same imported file is only loaded once
- package names must be unique across files
- all imported files are also generated when compiling the main file

---

## 3. Lexical rules

### 3.1 Comments

```text
// single-line comment

/* multi-line
   comment */
```

### 3.2 Identifiers

- letters, digits, and underscores are allowed
- must begin with a letter or underscore
- case-sensitive
- recommended styles:
  - structs / services / topics: `PascalCase`
  - fields / methods: `snake_case` or `camelCase`

### 3.3 Keywords

Reserved keywords:

```text
package  import   struct  service  topic  publishes
bool     int8     uint8   int16    uint16
int32    uint32   int64   uint64
float32  float64  string  bytes
array
```

---

## 4. Type system

### 4.1 Primitive types

| IDL type | Meaning | C type | C++ type | Size |
|---|---|---|---|---|
| `bool` | boolean | `uint8_t` | `bool` | 1 byte |
| `int8` | signed 8-bit integer | `int8_t` | `int8_t` | 1 byte |
| `uint8` | unsigned 8-bit integer | `uint8_t` | `uint8_t` | 1 byte |
| `int16` | signed 16-bit integer | `int16_t` | `int16_t` | 2 bytes |
| `uint16` | unsigned 16-bit integer | `uint16_t` | `uint16_t` | 2 bytes |
| `int32` | signed 32-bit integer | `int32_t` | `int32_t` | 4 bytes |
| `uint32` | unsigned 32-bit integer | `uint32_t` | `uint32_t` | 4 bytes |
| `int64` | signed 64-bit integer | `int64_t` | `int64_t` | 8 bytes |
| `uint64` | unsigned 64-bit integer | `uint64_t` | `uint64_t` | 8 bytes |
| `float32` | 32-bit floating point | `float` | `float` | 4 bytes |
| `float64` | 64-bit floating point | `double` | `double` | 8 bytes |
| `string` | string | `char*` + `uint32_t` | `std::string` | variable |
| `bytes` | byte array | `uint8_t*` + `uint32_t` | `std::vector<uint8_t>` | variable |

### 4.2 Array types

Use `array<T>` syntax:

```text
struct Example {
    array<int32>      ids;
    array<string>     names;
    array<SensorData> sensors;
}
```

| IDL type | C type | C++ type |
|---|---|---|
| `array<T>` | `T*` + `uint32_t count` | `std::vector<T>` |

### 4.3 User-defined struct types

Structs defined in the same package can be used as field types.

Circular references are not supported.

---

## 5. Struct definition (`struct`)

Syntax:

```text
struct <Name> {
    <type> <field_name>;
    ...
}
```

Example:

```text
struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    int64   timestamp;
    string  location;
    bytes   raw_data;
}
```

Generated C++ shape:

```cpp
namespace demo {

struct SensorData {
    int32_t sensor_id;
    double temperature;
    double humidity;
    int64_t timestamp;
    std::string location;
    std::vector<uint8_t> raw_data;

    SensorData();
    bool serialize(omnibinder::Buffer& buf) const;
    bool deserialize(omnibinder::Buffer& buf);
    size_t serializedSize() const;
};

}
```

Generated C shape:

```c
typedef struct demo_SensorData {
    int32_t sensor_id;
    double temperature;
    double humidity;
    int64_t timestamp;
    char* location;
    uint32_t location_len;
    uint8_t* raw_data;
    uint32_t raw_data_len;
} demo_SensorData;

void demo_SensorData_init(demo_SensorData* self);
void demo_SensorData_destroy(demo_SensorData* self);
void demo_SensorData_serialize(const demo_SensorData* self, omni_buffer_t* buf);
int demo_SensorData_deserialize(demo_SensorData* self, omni_buffer_t* buf);
```

---

## 6. Topic definition (`topic`)

Topics define the payload used for publish/subscribe broadcast.

Syntax:

```text
topic <Name> {
    <type> <field_name>;
    ...
}
```

Example:

```text
topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic SystemAlert {
    int32  alert_level;
    string message;
    string source;
}
```

Notes:

- at the serialization layer, a `topic` is equivalent to a `struct`
- the semantic difference is that a topic is declared through `publishes` inside a `service`
- every topic gets a unique `topic_id` derived from package + topic name

---

## 7. Service definition (`service`)

Syntax:

```text
service <Name> {
    <return_type> <Method>(<param_type> <param_name>);
    <return_type> <Method>();
    void <Method>(<param_type> <param_name>);
    publishes <TopicName>;
}
```

Example:

```text
service SensorService {
    StatusResponse SetThreshold(ControlCommand cmd);
    SensorData     GetLatestData();
    void           ResetSensor(int32 sensor_id);

    publishes SensorUpdate;
    publishes SystemAlert;
}
```

Method rules:

- each method has at most one parameter; wrap multiple fields in a struct if needed
- return type may be `void` for one-way invocation
- parameter and return types may be primitive or user-defined struct types

Generated C++ Stub shape:

```cpp
class SensorServiceStub : public omnibinder::Service {
public:
    virtual StatusResponse SetThreshold(const ControlCommand& cmd) = 0;
    virtual SensorData GetLatestData() = 0;
    virtual void ResetSensor(int32_t sensor_id) = 0;

    void BroadcastSensorUpdate(const SensorUpdate& msg);
    void BroadcastSystemAlert(const SystemAlert& msg);
};
```

Generated C++ Proxy shape:

```cpp
class SensorServiceProxy {
public:
    explicit SensorServiceProxy(omnibinder::OmniRuntime& runtime);
    int connect();
    void disconnect();
    bool isConnected() const;

    int SetThreshold(const ControlCommand& cmd, StatusResponse& out);
    int GetLatestData(SensorData& out);
    void ResetSensor(int32_t sensor_id);

    void SubscribeSensorUpdate(const std::function<void(const SensorUpdate&)>& callback);
    void OnServiceDied(const std::function<void()>& callback);
};
```

Generated C API shape:

```c
typedef struct demo_SensorService_proxy {
    omni_runtime_t* runtime;
    uint8_t connected;
} demo_SensorService_proxy;

void demo_SensorService_proxy_init(demo_SensorService_proxy* p, omni_runtime_t* runtime);
int  demo_SensorService_proxy_connect(demo_SensorService_proxy* p);
int  demo_SensorService_proxy_SetThreshold(demo_SensorService_proxy* p,
                                           const demo_ControlCommand* cmd,
                                           common_StatusResponse* result);
```

---

## 7. Runtime boundary contract

The generated Stub/Proxy code now follows an explicit-status runtime contract:

- business methods still keep their IDL return type (`void`, primitive, struct, array, ...)
- a generated C++ Proxy method for an IDL `void` method also returns `void` and calls `runtime_.invokeOneWay(...)`; it neither creates nor waits for a response, and cannot return serialization/submission errors through that method
- the generated `onInvoke(...)` itself returns `int`
- `0` means success
- non-zero means an `ErrorCode` such as `ERR_DESERIALIZE` or `ERR_SERIALIZE`

This lets the runtime propagate decode/encode failures explicitly instead of relying on hidden side channels.

`omni-cli call` is a diagnostic path and currently uses synchronous `invoke(...)`; an empty response printed by the CLI is distinct from the generated C++ Proxy's one-way contract.

The generated C runtime callback contract is also explicit-status based:

```c
typedef int (*omni_invoke_callback_t)(uint32_t method_id,
    const omni_buffer_t* request, omni_buffer_t* response, void* user_data);
```

## 8. Strict semantic validation

`omni-idlc` and `omni-cli --idl` run the same semantic validator after parsing. Validation errors prevent generation, and the CLI exits before connecting to ServiceManager. Current rules include:

- struct, topic, and service declaration names cannot collide in one package namespace; a file has at most one `package`, before imports and declarations
- imports require a package and must precede declarations; every custom value type must resolve to a struct, not a topic or service
- `void` is allowed only as a method return type, never as a field, topic field, parameter, or array element
- method names and `publishes` entries cannot repeat within a service; method overloading by parameter type is not supported
- `publishes` must name an existing topic in the service's package, not a struct or service
- by-value struct cycles are rejected, including qualified cross-package cycles; `array<T>` is a container edge and does not itself form a by-value cycle
- valid forward struct dependencies are ordered before code generation

## 9. ID generation rules

### 9.1 `interface_id`

Generated from service name using FNV-1a 32-bit hash:

```text
interface_id = fnv1a_32(package_name + "." + service_name)
```

### 9.2 `method_id`

Generated from method name:

```text
method_id = fnv1a_32(method_name)
```

### 9.3 `topic_id`

Generated from topic name:

```text
topic_id = fnv1a_32(package_name + "." + topic_name)
```

### 9.4 FNV-1a example

```cpp
uint32_t fnv1a_32(const char* str) {
    uint32_t hash = 0x811c9dc5;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 0x01000193;
    }
    return hash;
}
```

---

## 10. Complete IDL example

Base type file:

```text
package common;

struct Timestamp {
    int64 seconds;
    int32 nanos;
}

struct StatusResponse {
    int32  code;
    string message;
}
```

Service file using `import`:

```text
package sensor;

import "common_types.bidl";

struct SensorData {
    int32   sensor_id;
    float64 temperature;
    float64 humidity;
    int64   timestamp;
    string  location;
}

struct ControlCommand {
    int32   command_type;
    string  target;
    int32   value;
}

topic SensorUpdate {
    SensorData data;
    int64      publish_time;
}

topic SensorAlert {
    int32   sensor_id;
    int32   alert_level;
    string  description;
    int64   timestamp;
}

service SensorService {
    common.StatusResponse SetThreshold(ControlCommand cmd);
    SensorData            GetLatestData();
    void                  ResetSensor(int32 sensor_id);

    publishes SensorUpdate;
    publishes SensorAlert;
}
```

---

## 11. Using the `omni-idlc` compiler

### 11.1 Command-line usage

```bash
mkdir -p ./generated

omni-idlc --lang=cpp --output=./generated sensor_service.bidl
omni-idlc --lang=c   --output=./generated sensor_service.bidl
omni-idlc --lang=all --output=./generated sensor_service.bidl
```

### 11.2 Parameters

| Parameter | Description | Default |
|---|---|---|
| `--lang=<cpp\|c\|all>` | target language | `cpp` |
| `--output=<dir>` | output directory; it must already exist | current directory |
| `--dep-file=<file>` | generate Makefile-style dependency file | disabled |

### 11.3 Generated files

```text
Input: sensor_service.bidl

--lang=cpp:
  sensor_service.bidl.h
  sensor_service.bidl.cpp

--lang=c:
  sensor_service.bidl_c.h
  sensor_service.bidl.c
```

### 11.4 CMake integration

```cmake
find_package(OmniBinder REQUIRED)

add_executable(my_service main.cpp)

omnic_generate(
    TARGET my_service
    LANGUAGE cpp
    FILES sensor_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(my_service omnibinder)
```

The `omnic_generate` helper will:

1. invoke `omni-idlc` automatically at build time
2. add generated source files to the target
3. add generated include directories to the target include path

---

## Related documents

- [Top-level README](../README.en.md)
- [Examples Guide](examples.en.md)
- [omni-cli Usage Guide](omni-tool-usage.en.md)
