# DataHub Naming Conventions

**Purpose:** Authoritative reference for names used across the DataHub system — hub identity,
channels, shared memory segments, process roles, and lifecycle module identifiers. All new
code and configuration must follow these conventions. Names are designed to be **decodable**:
reading a name tells you who owns it, what role it plays, and what channel it carries.

**Related documents:**
- `docs/DATAHUB_PROTOCOL_AND_POLICY.md` — slot-level protocol
- `docs/IMPLEMENTATION_GUIDANCE.md` — implementation rules
- `docs/todo/MESSAGEHUB_TODO.md` — Messenger/hub protocol backlog

---

## 1. Entity Roles

The DataHub system has three distinct roles. The naming system encodes role explicitly.

| Role | Description | Lifecycle position |
|------|-------------|-------------------|
| **Hub** | The central broker process (hubshell). Owns shared memory. Authoritative registry. | Runs as a standalone process |
| **Source** | A process that writes to a DataBlock channel (formerly "producer"). Attaches to broker-created shared memory. | Connects to hub |
| **Terminal** | A process that reads from a DataBlock channel (formerly "consumer"). Attaches read-only. | Connects to hub |

> **Terminology note:** "Source" and "Terminal" are the semantic roles.
> In code, `DataBlockProducer` and `DataBlockConsumer` remain as the C++ class names
> for backward compatibility. The logical roles are Source/Terminal in documentation and
> naming conventions.

---

## 2. Hub Identity

A hub is identified by three pieces of information:

```
hub_name   : logical name, lowercase, underscores, e.g. "lab_hub", "production_hub"
endpoint   : ZMQ endpoint, e.g. "tcp://192.168.1.100:5570"
public_key : CurveZMQ server public key, z85-encoded (40 chars), e.g. "rq:rM>}"
```

Hub names:
- Lowercase, alphanumeric, underscores only: `[a-z0-9_]+`
- Globally unique within a deployment
- Used as a namespace prefix for channels in federation contexts

---

## 3. Channel (DataBlock) Naming

A channel name identifies one DataBlock shared memory segment within a hub.

### 3.1 Local channel name (within a hub)

```
<source_id>/<channel_function>
```

| Component | Rules | Examples |
|-----------|-------|---------|
| `source_id` | Identifies the source process/device. Lowercase, alphanumeric, underscores, hyphens. | `camera_0`, `lidar_1`, `ctrl_main`, `imu_front` |
| `channel_function` | Describes the data type or purpose. Lowercase, alphanumeric, underscores. | `raw_frames`, `point_cloud`, `setpoints`, `diagnostics` |

**Examples:**
```
camera_0/raw_frames
lidar_1/point_cloud
ctrl_main/setpoints
imu_front/attitude
sensor_array/temperature
```

### 3.2 Fully-qualified channel name (for federation / hub-to-hub routing)

```
<hub_name>::<source_id>/<channel_function>
```

**Examples:**
```
lab_hub::camera_0/raw_frames
production_hub::ctrl_main/setpoints
```

The `::` separator distinguishes the hub namespace from the local path.
Local names are used within a hub; fully-qualified names are used in broker-to-broker routing.

### 3.3 Channel naming rules

- All components: lowercase, `[a-z0-9_\-]+`
- Separator between source and function: forward slash `/`
- Separator between hub and channel: double colon `::`
- No spaces, no dots (dots are reserved for shared memory segment names)
- Maximum total length: 128 characters (fits in `SharedMemoryHeader`)
- Must be unique within a hub (the hub enforces this at registration)

---

## 4. Shared Memory Segment Naming

POSIX shared memory names must start with `/` and must not contain further slashes.
The segment name is derived from the fully-qualified channel name by transforming separators:

```
/<hub_name>.<source_id>.<channel_function>
```

Transformation rules:
- Prepend `/`
- Replace `::` with `.`
- Replace `/` with `.`

**Examples:**

| Channel name | Shared memory segment name |
|-------------|--------------------------|
| `camera_0/raw_frames` (hub: `lab_hub`) | `/lab_hub.camera_0.raw_frames` |
| `ctrl_main/setpoints` (hub: `lab_hub`) | `/lab_hub.ctrl_main.setpoints` |

The segment name is computed by the hub when it creates the segment. Sources and Terminals
receive the segment name from the hub as part of registration. They do not construct it
independently.

> **Platform note:** On Windows, shared memory uses named file mappings. The same naming
> convention applies with backslashes replaced by underscores and the leading `/` dropped:
> `lab_hub.camera_0.raw_frames`.

---

## 5. Lifecycle Module Names

All modules registered with the lifecycle system follow these conventions:

### 5.1 Static modules (registered before `InitializeApp()`)

| Module | Lifecycle name | Notes |
|--------|----------------|-------|
| Lifecycle itself | `"Lifecycle"` | Self-registered; root dependency |
| Logger | `"pylabhub::utils::Logger"` | Existing |
| CryptoUtils | `"CryptoUtils"` | Existing |
| DataExchangeHub | `"pylabhub::hub::DataExchangeHub"` | Existing (renamed to Hub in future) |
| ZMQContext | `"ZMQContext"` | New; one `zmq::context_t` per process |

### 5.2 Dynamic modules (registered and loaded/unloaded at runtime)

#### Messenger modules

Each `Messenger` instance is a dynamic module. Its lifecycle name encodes the channel
and the role of this process on that channel:

```
"Messenger[<channel_name>:<role>]"
```

| Role value | Meaning |
|------------|---------|
| `source` | This process is a Source (writer) on this channel |
| `terminal` | This process is a Terminal (reader) on this channel |
| `hub` | This is the hub's own management context for this channel |

**Examples:**
```
Messenger[camera_0/raw_frames:source]      # camera process, writing frames
Messenger[camera_0/raw_frames:terminal]    # vision process, reading frames
Messenger[camera_0/raw_frames:hub]         # hub's management context for this channel
```

#### Rules for dynamic module names

- Name must be globally unique within a process's lifecycle
- Bracket syntax `[...]` is reserved for dynamic module parameter encoding
- The channel name within brackets follows §3.1 (local name, no hub prefix)
- The role suffix follows a colon `:` with no spaces

---

## 6. Process Identification

Each source or terminal process registers with the hub using:

```
process_id  : OS PID (uint64, for liveness detection)
instance_id : stable identifier across restarts (optional, e.g. hostname + role)
```

The hub uses `process_id` for heartbeat monitoring and zombie detection.
The `instance_id` is informational (for logs and diagnostics).

---

## 7. Decoding Summary

Given a name, you can decode its meaning:

| What you see | What it tells you |
|-------------|-------------------|
| `lab_hub` | A hub named "lab_hub" |
| `camera_0/raw_frames` | A channel where `camera_0` is the source, publishing `raw_frames` |
| `lab_hub::camera_0/raw_frames` | Fully-qualified: hub `lab_hub`, channel `camera_0/raw_frames` |
| `/lab_hub.camera_0.raw_frames` | The POSIX shared memory segment for that channel |
| `Messenger[camera_0/raw_frames:source]` | Lifecycle module: Messenger on the source side of `camera_0/raw_frames` |
| `Messenger[camera_0/raw_frames:hub]` | Hub's management Messenger for that channel |

---

## 8. Reserved Names and Prefixes

| Name / Prefix | Reserved for |
|--------------|-------------|
| `pylabhub.*` | All shared memory segments created by this library |
| `hub` | Role identifier in Messenger module names |
| `source` | Role identifier in Messenger module names |
| `terminal` | Role identifier in Messenger module names |
| `Lifecycle` | Root lifecycle module |
| `ZMQContext` | Shared ZMQ context static module |
| `::` | Hub-to-channel separator in fully-qualified names |

---

*Last updated: 2026-02-17*
*Owner: core architecture*
