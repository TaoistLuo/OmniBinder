# omni-cli Usage Guide

[中文](omni-tool-usage.md) | English

> **Version**: 1.0.0  
> **Updated**: 2026-03-05

## Overview

`omni-cli` is the command-line debugging tool for OmniBinder. It can be used to:

- list online services
- list online runtimes / PIDs
- inspect service interfaces and method signatures
- invoke service methods
- display call latency statistics
- set runtime log levels by PID
- watch IDL business interface input/output by PID

It supports both:

- **hex mode** for raw requests/responses
- **JSON mode** when an IDL file is provided

---

## Basic usage

### Command format

```bash
omni-cli [options] <command> [args]
```

### Global options

| Option | Description | Default |
|---|---|---|
| `-h, --host <addr>` | ServiceManager address | `127.0.0.1` |
| `-p, --port <port>` | ServiceManager port | `9900` |
| `--idl <file.bidl>` | IDL file path (enables JSON support) | none |
| `--help` | Show help | - |

### Available commands

| Command | Description |
|---|---|
| `list` | List online services |
| `ps` | List online runtime PIDs, roles, log levels, and services |
| `info <service>` | Show service details |
| `call <service> <method> [params]` | Invoke a service method |
| `log set --pid <pid> --level <F\|E\|W\|I\|D\|V\|O>` | Set a runtime log level by PID |
| `watch --pid <pid> --idl <file.bidl> [--filter <name>]` | Watch business interface I/O from a runtime PID |

---

## Command reference

### 1. `list`

Lists all online services and their basic information.

```bash
omni-cli list
```

Example:

```bash
$ omni-cli list
NAME                     HOST             PORT     STATUS
----                     ----             ----     ------
SensorService            127.0.0.1        35451    ONLINE
NavigationService        192.168.1.100    8002     ONLINE

Total: 2 services online
```

---

### 2. `ps`

Lists runtime PIDs currently connected to ServiceManager. Hidden diagnostic services are not shown in the service list.

```bash
omni-cli ps
```

Example:

```bash
$ omni-cli ps
PID      ROLE     LOG   PROCESS              SERVICES
-------- -------- ----- -------------------- ----------------
12345    service  I     example_cpp_sensor_server    SensorService
12346    client   I     example_cpp_sensor_client    -
```

The `PROCESS` column is the executable name reported by the runtime at startup. It has a minimum display width of 20 characters; longer names are printed in full and move the `SERVICES` column to the right. A process registered by an older runtime must be restarted before this field is refreshed to the real process name.

---

### 3. `info`

Shows a service's published topics, interface, and method definitions. The Published Topics section is always printed before interfaces:

```text
  Published Topics:
    - SensorUpdate
    - AsyncResultReady
```

It shows `(none)` when the service publishes no topics. If service lookup succeeds but the dedicated topic query fails, it shows `(unavailable: <reason>)` and continues with interfaces; a missing service instead returns an error before this section.

#### 3.1 Basic mode (without IDL)

Displays type names only:

```bash
omni-cli info <service_name>
```

#### 3.2 IDL-aware mode

Expands full field definitions for structs:

```bash
omni-cli --idl <file.bidl> info <service_name>
```

Notes:

- imported IDL dependencies are loaded recursively
- cross-package types such as `common.StatusResponse` are expanded correctly
- `--idl` runs the same strict semantic validation as `omni-idlc` and can fail before connecting to ServiceManager

---

### 4. `call`

Invokes a service method.

#### 4.1 Hex mode

Use a hex string as input and get hex output.

```bash
omni-cli call <service> <method> [hex_params]
```

#### 4.2 JSON mode

Use IDL type information to encode input. Decodable non-`void` responses are formatted as JSON; otherwise output falls back to hex (with a warning when decoding fails).

```bash
omni-cli --idl <file.bidl> call <service> <method> [json_params]
```

Examples:

```bash
omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData

omni-cli --idl examples/sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"sensor1","value":100}'

omni-cli --idl examples/sensor_service.bidl call SensorService ResetSensor 1
```

Notes:

- objects beginning with `{` and arrays beginning with `[` are parsed as JSON
- known scalar parameters are parsed from unwrapped CLI text; strings are raw arguments (`EchoString hello`), so `EchoString '"hello"'` includes the quotes
- non-scalar input beginning with neither `{` nor `[` is parsed as hex for backward compatibility
- every declared struct field is required; the JSON codec does not currently support `bytes`, including nested uses
- JSON numbers use a lightweight number representation, so `int64`/`uint64` values outside the IEEE-754 exact integer range are not guaranteed to remain exact
- response time is displayed in milliseconds

---

### 5. `log set`

Sets the log level of a runtime by PID. Use `omni-cli ps` to find the PID.

```bash
omni-cli log set --pid <pid> --level <F|E|W|I|D|V|O>
```

Levels: `F` fatal, `E` error, `W` warn, `I` info, `D` debug, `V` verbose, `O` off.

Example:

```bash
omni-cli log set --pid 12345 --level D
```

---

### 6. `watch`

Watches IDL business interface input/output from a runtime PID. The watch data path reuses OmniBinder's normal topic data channel: same-machine traffic uses SHM automatically, while cross-machine traffic uses TCP. ServiceManager is only used for start/stop control.

```bash
omni-cli watch --pid <pid> --idl <file.bidl> [--filter <name>]
```

Behavior:

- Service PID: observes RPC input and topic publish output.
- Client PID: observes RPC output and subscription input.
- ServiceManager control messages, lookup, heartbeat, and other internal control flows are not printed.
- `--filter` matches decoded RPC method names or the generic direction names `broadcast` and `subscribe`. Filtering happens before topic payload decoding, so actual topic names such as `SensorUpdate` do not match; events still named `?` bypass this filter.

Example:

```bash
omni-cli watch --pid 12345 --idl examples/sensor_service.bidl --filter GetLatestData
```

---

## JSON format notes

### Supported mappings

| IDL type | JSON type | Example |
|---|---|---|
| `bool` | boolean | `true`, `false` |
| integer types | number | `42`, `-10` |
| float types | number | `3.14`, `25.5` |
| `string` | raw CLI string | `hello` |
| `struct` | object | `{"field": value}` |
| `array<T>` | array | `[1, 2, 3]` |

Example inputs:

```json
42
```

For a scalar string, pass one raw shell argument such as `'hello world'`; do not preserve extra JSON quote characters.

```json
{
  "command_type": 1,
  "target": "sensor1",
  "value": 100
}
```

```json
[1, 2, 3, 4, 5]
```

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

## Advanced usage

### Connect to a remote ServiceManager

```bash
omni-cli -h 192.168.1.100 -p 9900 list
```

### Use a relative IDL path

```bash
cd examples
omni-cli --idl sensor_service.bidl info SensorService
```

### Use an absolute IDL path

```bash
omni-cli --idl /path/to/sensor_service.bidl call SensorService GetLatestData
```

### Piping and scripting

```bash
omni-cli list | grep Sensor

RESULT=$(omni-cli --idl sensor.bidl call SensorService GetLatestData)
echo "Result: $RESULT"
```

---

## FAQ

### Q1: Why do I still see hex output instead of JSON?

Possible reasons:

1. `--idl` was not provided
2. the IDL path is wrong or cannot be resolved
3. the response type cannot be resolved from the complete IDL/import chain, or decoding failed
4. the type contains `bytes`, which the current JSON codec does not support

Cross-package structs decode normally when the complete import chain is available.

### Q2: How do I know what parameters a method expects?

Use detailed `info` mode:

```bash
omni-cli --idl sensor_service.bidl info SensorService
```

### Q3: What if my JSON input is invalid?

Common issues:

- field names must use double quotes
- string values must use double quotes
- numbers should not be quoted unless they are actually strings
- commas must be placed correctly between fields

### Q4: What does the reported call latency include?

Included:

- transport time (TCP or SHM)
- server-side handling time
- serialization and deserialization time

Not included:

- `omni-cli` JSON parsing time
- initial connection setup time for the very first call

### Q5: Why are same-host calls so fast?

When `omni-cli` and the target service run on the same machine, OmniBinder automatically uses SHM, so latency is often below 1 ms.

---

## Performance reference

### Local calls (SHM)

| Operation type | Typical latency |
|---|---|
| No-arg call | 0.5-1.0 ms |
| Primitive-parameter call | 0.4-0.8 ms |
| Struct-parameter call | 0.5-1.2 ms |

### Remote calls (TCP)

| Network environment | Typical latency |
|---|---|
| LAN | 1-5 ms |
| Cross-site / WAN-like conditions | 10-50 ms |

---

## Related documents

- [Top-level README](../README.en.md)
- [IDL Specification](idl-specification.en.md)
- [Protocol](protocol.md)
- [Examples Guide](examples.en.md)

---

**Document version**: 1.0.0  
**Last updated**: 2026-03-05
