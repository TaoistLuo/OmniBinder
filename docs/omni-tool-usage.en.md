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
| `watch --pid <pid> --idl <file.bidl> [--filter <method\|topic>]` | Watch business interface I/O from a runtime PID |

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
12345    service  I     pid-12345            SensorService
12346    client   I     pid-12346            -
```

---

### 3. `info`

Shows interface and method definitions for a service.

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

---

### 4. `call`

Invokes a service method.

#### 4.1 Hex mode

Use a hex string as input and get hex output.

```bash
omni-cli call <service> <method> [hex_params]
```

#### 4.2 JSON mode

Use JSON input and get formatted JSON output.

```bash
omni-cli --idl <file.bidl> call <service> <method> [json_params]
```

Examples:

```bash
omni-cli --idl sensor_service.bidl call SensorService GetLatestData

omni-cli --idl sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"sensor1","value":100}'

omni-cli --idl sensor_service.bidl call SensorService ResetSensor 1
```

Notes:

- JSON input is auto-detected when the argument starts with `{`
- even when `--idl` is specified, hex input is still supported for backward compatibility
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
omni-cli watch --pid <pid> --idl <file.bidl> [--filter <method|topic>]
```

Behavior:

- Service PID: observes RPC input and topic publish output.
- Client PID: observes RPC output and subscription input.
- ServiceManager control messages, lookup, heartbeat, and other internal control flows are not printed.
- `--filter` matches decoded method/topic names, for example `GetLatestData` or `broadcast`.

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
| `string` | string | `"hello"` |
| `struct` | object | `{"field": value}` |
| `array<T>` | array | `[1, 2, 3]` |

Example inputs:

```json
42
```

```json
"hello world"
```

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
3. the response type is a cross-package type and the complete IDL chain is not available

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
