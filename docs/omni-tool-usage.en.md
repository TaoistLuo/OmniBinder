# omni-cli Usage Guide

[中文](omni-tool-usage.md) | English

> **Version**: 1.1.0  
> **Updated**: 2026-09-21

## Overview

`omni-cli` is the command-line debugging tool for OmniBinder. It can be used to:

- list all online service instances (service name / `instance_id` / `host_id` / `host:port`)
- list online runtimes / PIDs
- enumerate every instance of a service name and inspect its interfaces and method signatures
- invoke a method on a specific instance by `(host_id, instance_id)`
- display call latency statistics
- set runtime log levels by PID
- watch IDL business interface input/output by PID

**Service identity model**: one service name may have several live instances at once, identified by `(service_name, host_id, instance_id)`. An empty `instance_id` is the **default instance** of that name on that `host_id`. `list` / `info` / `call` all work at instance granularity: a single instance is simply the "one row for this name" special case.

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
| `--instance-id <id>` | Target instance id (`info` / `call`); `--instance-id ''` selects the default instance | none (see each command) |
| `--host-id <id>` | Board-level host id of the target instance, combined with `--instance-id` for exact selection | none (see each command) |
| `--help` | Show help | - |

> `--instance-id` / `--host-id` are **instance selectors** with uniform semantics:
> - only `--instance-id`: match the unique `instance_id` under that service name; if several `host_id` values match, it errors and asks for `--host-id`.
> - only `--host-id`: match the unique instance on that `host_id`; if that host has more than one same-name instance, it errors and asks for `--instance-id`.
> - both: exact match on `(host_id, instance_id)`.
> - neither: `call` uses the **deterministic first instance** and says so in the output; `info` only enumerates when there are multiple instances and never picks one silently.

### Available commands

| Command | Description |
|---|---|
| `list` | List all online service instances |
| `ps` | List online runtime PIDs, roles, log levels, and services |
| `info <service> [--instance-id <id>] [--host-id <id>]` | Enumerate instances and show the selected instance's interfaces |
| `call <service> <method> [params] [--instance-id <id>] [--host-id <id>]` | Invoke a method on the selected instance |
| `log set --pid <pid> --level <F\|E\|W\|I\|D\|V\|O>` | Set a runtime log level by PID |
| `watch --pid <pid> --idl <file.bidl> [--filter <name>]` | Watch business interface I/O from a runtime PID |

---

## Command reference

### 1. `list`

Lists all online service **instances**. Every `(service_name, host_id, instance_id)` in the registry is one row; when a name has only one instance it is simply one row among the same-name rows.

```bash
omni-cli list
```

Columns:

| Column | Meaning |
|---|---|
| `NAME` | Service name |
| `INSTANCE_ID` | Instance id; an empty `instance_id` is shown as `(default)` |
| `HOST_ID` | Board-level host id (part of the instance identity and of same-host detection) |
| `ENDPOINT` | Data-plane direct address `host:port` |
| `STATUS` | Currently always `ONLINE` |

Example:

```bash
$ omni-cli list
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            ttyUSB0              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35451       ONLINE
SensorService            ttyUSB1              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35452       ONLINE
NavigationService        (default)            0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:8002        ONLINE

Total: 3 service instances online
```

> Same-name instances with different `instance_id` are listed side by side; an empty `instance_id` shows as `(default)` and can be selected exactly with `--instance-id ''`.

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

The `PROCESS` column is the executable name reported by the runtime at startup. It has a minimum display width of 20 characters; longer names are printed in full and move the `SERVICES` column to the right.

---

### 3. `info`

Enumerates every instance of a service name, then shows the interfaces and method definitions of the selected instance.

```bash
omni-cli info <service_name> [--instance-id <id>] [--host-id <id>]
```

Behavior:

1. First enumerate all instances of the name (same columns as `list`).
2. If `--instance-id` / `--host-id` is given, resolve the unique target instance and show its interfaces.
3. If no selector is given:
   - one instance: show its interfaces directly and label it on the `Instance:` line.
   - several instances: **only enumerate, never pick silently**, and hint to use `--instance-id` / `--host-id`.

The Published Topics section is always printed before interfaces:

```text
  Published Topics:
    - SensorUpdate
    - AsyncResultReady
```

It shows `(none)` when the service publishes no topics. If service lookup succeeds but the dedicated topic query fails, it shows `(unavailable: <reason>)` and continues with interfaces; a missing service instead returns an error before this section.

#### 3.1 Basic mode (without IDL), single instance

Displays type names only:

```bash
$ omni-cli info SensorService
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            (default)            0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35451       ONLINE

Instance: instance_id="" (default) host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35451

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x61564c6c)
      - SetThreshold(ControlCommand) -> common.StatusResponse  (id=0x2214744e)
      - ResetSensor(int32) -> void  (id=0x7f2b95ee)
```

#### 3.2 Several same-name instances, no selector (enumerate only)

```bash
$ omni-cli info SensorService
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            ttyUSB0              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35451       ONLINE
SensorService            ttyUSB1              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35452       ONLINE

Multiple instances found; not selecting one automatically. Use --instance-id <id> [--host-id <id>] to inspect a specific instance.
```

#### 3.3 IDL-aware mode with a selected instance

Expands full field definitions for structs:

```bash
$ omni-cli --idl examples/sensor_service.bidl info SensorService --instance-id ttyUSB1
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            ttyUSB0              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35451       ONLINE
SensorService            ttyUSB1              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35452       ONLINE

Instance: instance_id="ttyUSB1" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35452

  Published Topics:
    - SensorUpdate
    - AsyncResultReady

  Interface: SensorService (id=0xa112646d)
    Methods:
      - GetLatestData() -> SensorData  (id=0x61564c6c)
          return: {
            sensor_id: int32
            temperature: float64
            humidity: float64
            timestamp: int64
            location: string
          }
      - SetThreshold(ControlCommand) -> common.StatusResponse  (id=0x2214744e)
          param: {
            command_type: int32
            target: string
            value: int32
          }
      - ResetSensor(int32) -> void  (id=0x7f2b95ee)
```

Notes:

- the identity line always prints the explicitly selected `(instance_id, host_id, endpoint)`; there is no silent hit on some instance
- imported IDL dependencies are loaded recursively
- cross-package types such as `common.StatusResponse` are expanded correctly
- `--idl` runs the same strict semantic validation as `omni-idlc` and can fail before connecting to ServiceManager

---

### 4. `call`

Invokes a method on a service instance. The instance selector is a first-class argument: `omni-cli` enumerates the name's instances, resolves the target by `(host_id, instance_id)`, then establishes a data-plane direct connection with `connectServiceInstance`.

```bash
omni-cli call <service> <method> [params] [--instance-id <id>] [--host-id <id>]
```

Instance selection semantics:

- with `--instance-id` (optionally `--host-id`): invoke the exactly matched instance; a missing or ambiguous match (same name across hosts) is reported as an error.
- with no selector: invoke the **deterministic first instance** (the same one as the first `list` row) and print which instance was actually hit on the first output line.

#### 4.1 Hex mode

Use a hex string as input and get hex output.

```bash
# explicit instance (no-arg method)
$ omni-cli call SensorService GetLatestData --instance-id ttyUSB1
Target instance: instance_id="ttyUSB1" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35452
Calling SensorService.GetLatestData() ...
  interface_id = 0xa112646d
  method_id    = 0x61564c6c
Response (status=OK, 38 bytes, 0.75 ms):
  Hex: 01 00 00 00 00 00 00 00 80 39 40 ...

# no selector: deterministic first instance, reported explicitly
$ omni-cli call SensorService ResetSensor 01000000
No instance selector given; using deterministic first instance: instance_id="ttyUSB0" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35451
  Hint: use --instance-id <id> [--host-id <id>] to target another instance.
Calling SensorService.ResetSensor() ...
  interface_id = 0xa112646d
  method_id    = 0x7f2b95ee
  request (4 bytes)
Response (status=OK, 0 bytes, 0.61 ms):
```

#### 4.2 JSON mode

Use IDL type information to encode input. Decodable non-`void` responses are formatted as JSON; otherwise output falls back to hex (with a warning when decoding fails).

```bash
omni-cli --idl <file.bidl> call <service> <method> [json_params] [--instance-id <id>] [--host-id <id>]
```

Examples:

**Example 1: no-argument method**
```bash
$ omni-cli --idl examples/sensor_service.bidl call SensorService GetLatestData --instance-id ttyUSB1
Target instance: instance_id="ttyUSB1" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35452
Calling SensorService.GetLatestData() ...
  interface_id = 0xa112646d
  method_id    = 0x61564c6c
Response (status=OK, 38 bytes, 0.81 ms):
  {
    "humidity": 61.95,
    "location": "Room-A",
    "sensor_id": 1,
    "temperature": 23.99,
    "timestamp": 1772645628
  }
```

**Example 2: struct parameter**
```bash
$ omni-cli --idl examples/sensor_service.bidl call SensorService SetThreshold \
  '{"command_type":1,"target":"sensor1","value":100}' --instance-id ttyUSB1
Target instance: instance_id="ttyUSB1" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35452
Calling SensorService.SetThreshold() ...
  interface_id = 0xa112646d
  method_id    = 0x2214744e
  request (19 bytes)
Response (status=OK, 10 bytes, 0.47 ms):
  {
    "code": 0,
    "message": "OK"
  }
```

**Example 3: scalar parameter**
```bash
$ omni-cli --idl examples/sensor_service.bidl call SensorService ResetSensor 1 --instance-id ttyUSB0
Target instance: instance_id="ttyUSB0" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35451
Calling SensorService.ResetSensor() ...
  interface_id = 0xa112646d
  method_id    = 0x7f2b95ee
  request (4 bytes)
Response (status=OK, 0 bytes, 0.58 ms):
```

Notes:

- objects beginning with `{` and arrays beginning with `[` are parsed as JSON
- known scalar parameters are parsed from unwrapped CLI text; strings are raw arguments (`EchoString hello`), so `EchoString '"hello"'` includes the quotes
- non-scalar input beginning with neither `{` nor `[` is parsed as hex
- every declared struct field is required; the JSON codec does not currently support `bytes`, including nested uses
- JSON numbers use a lightweight number representation, so `int64`/`uint64` values outside the IEEE-754 exact integer range are not guaranteed to remain exact
- response time is displayed in milliseconds
- the target instance's interface list comes from the instance discovery result, so single-instance and multi-instance calls share one method-resolution path

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

## Multi-instance addressing walkthrough

Scenario: two same-name `SensorService` instances run on one board, bound to `ttyUSB0` and `ttyUSB1` (the server calls `Service::setInstanceId("ttyUSB0")` / `setInstanceId("ttyUSB1")` before registering). They share a `host_id` but differ in `instance_id`, and coexist in ServiceManager.

**1. Start ServiceManager and the two instances**

```bash
./build/target/bin/service_manager -p 9900
# two instance processes (pseudo-code; real services call setInstanceId)
#   SensorService(instance_id="ttyUSB0")  -> register + broadcast
#   SensorService(instance_id="ttyUSB1")  -> register + broadcast
```

**2. `list` shows both instances**

```bash
$ omni-cli list
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            ttyUSB0              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35451       ONLINE
SensorService            ttyUSB1              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35452       ONLINE

Total: 2 service instances online
```

**3. `info` enumerates and selects by `instance_id`**

```bash
$ omni-cli info SensorService
Service: SensorService
NAME                     INSTANCE_ID          HOST_ID                          ENDPOINT              STATUS
------------------------ -------------------- -------------------------------- --------------------- ------
SensorService            ttyUSB0              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35451       ONLINE
SensorService            ttyUSB1              0cee7b0ae232461ba25df5376b29b46b 127.0.0.1:35452       ONLINE

Multiple instances found; not selecting one automatically. Use --instance-id <id> [--host-id <id>] to inspect a specific instance.

$ omni-cli info SensorService --instance-id ttyUSB1
# (same table)
Instance: instance_id="ttyUSB1" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35452
  ...interfaces and method definitions...
```

**4. Call each instance separately**

```bash
# only ttyUSB0
$ omni-cli call SensorService GetLatestData --instance-id ttyUSB0
Target instance: instance_id="ttyUSB0" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35451
Calling SensorService.GetLatestData() ...
Response (status=OK, 38 bytes, 0.75 ms):
  Hex: 01 00 00 00 ...

# only ttyUSB1 (add --host-id when the same instance_id exists on another host)
$ omni-cli call SensorService GetLatestData --instance-id ttyUSB1
Target instance: instance_id="ttyUSB1" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35452
Calling SensorService.GetLatestData() ...
Response (status=OK, 38 bytes, 0.74 ms):
  Hex: 02 00 00 00 ...
```

**5. No selector hits the deterministic first instance and says so**

```bash
$ omni-cli call SensorService GetLatestData
No instance selector given; using deterministic first instance: instance_id="ttyUSB0" host_id="0cee7b0ae232461ba25df5376b29b46b" endpoint=127.0.0.1:35451
  Hint: use --instance-id <id> [--host-id <id>] to target another instance.
Calling SensorService.GetLatestData() ...
```

> The "deterministic first instance" matches the first row of the `list` / `info` table (ServiceManager returns instances in instance-key order), so the same command reproduces the same target instead of hitting a random instance.

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

How to fix:

- make sure `--idl` is passed with the correct IDL file path
- check that the IDL file contains the required type definitions
- cross-package structs decode when the complete import chain is available; use hex mode when needed to inspect the actual type or payload

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

- [IDL Specification](idl-specification.en.md)
- [Protocol](protocol.md)

---

**Document version**: 1.1.0  
**Last updated**: 2026-09-21
