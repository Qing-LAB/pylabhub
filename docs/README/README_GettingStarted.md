# Getting Started — pyLabHub Data Pipeline

This guide explains how to set up roles, connect them through a broker, configure
policies, and discover peers. For detailed field references, see `README_Deployment.md`.

---

## 1. Concepts

A pyLabHub data pipeline consists of:

- **Hub** (`pylabhub-hubshell`): The broker that coordinates all roles. Manages
  channel registration, consumer discovery, peer notifications, and metadata storage.
- **Producer** (`pylabhub-producer`): Creates a data channel and writes typed slots.
- **Consumer** (`pylabhub-consumer`): Subscribes to a channel and reads typed slots.
- **Processor** (`pylabhub-processor`): Reads from one channel, transforms, writes to another.

Roles communicate through two planes:
- **Data plane**: Shared memory (SHM) or ZMQ for slot data transfer.
- **Control plane**: ZMQ DEALER/ROUTER via the broker for registration, heartbeat,
  shutdown notifications, and peer discovery.

---

## 2. Setting Up a Role

### 2.1 Initialize a role directory

```bash
pylabhub-producer --init /path/to/my-producer
```

This creates the directory with a `producer.json` config template and a
`script/python/__init__.py` skeleton. Same for consumer and processor.

### 2.2 Configure the role

Edit `producer.json`. Key sections:

```json
{
    "producer": {
        "uid": "PROD-MySensor-12345678",
        "name": "MySensor"
    },
    "out_hub_dir": "/var/pylabhub/my_hub",
    "out_channel": "sensor.temperature",

    "loop_timing": "fixed_rate",
    "target_period_ms": 10.0,

    "checksum": "enforced",

    "out_transport": "shm",
    "out_shm_enabled": true,
    "out_shm_slot_count": 8,

    "out_slot_schema": {
        "fields": [
            {"name": "timestamp", "type": "float64"},
            {"name": "value", "type": "float32"}
        ]
    },

    "script": {"type": "python", "path": "."}
}
```

### 2.3 Config parameter levels

Parameters are organized by scope:

| Level | Examples | Description |
|-------|---------|-------------|
| **Role** | `loop_timing`, `checksum`, `script`, identity | One value for the entire role. Applies to all data streams. |
| **Per-direction** | `out_hub_dir`, `out_channel`, `out_transport`, `out_shm_*` | Separate for input (in_) and output (out_). |
| **Role-specific** | `out_slot_schema`, `in_slot_schema` | Data schema definitions. |

**Important**: Checksum policy and loop timing are per-role, not per-direction.
A role that reads and writes (processor) uses the same checksum and timing policy
for both sides.

### 2.4 Config validation

All JSON keys are validated against a whitelist at parse time. Unknown keys cause
a hard error — prevents typos and obsolete configuration from being silently ignored.

The `null` value is treated as "absent" — use it to comment out a parameter:
```json
"target_rate_hz": null
```

---

## 3. Connecting Roles Through the Broker

### 3.1 Start the hub

```bash
pylabhub-hubshell /path/to/hub_dir
```

The hub creates `hub.json` with its broker endpoint (e.g., `tcp://127.0.0.1:5570`).

### 3.2 Point roles to the hub

Each role's config specifies the hub directory:
- Producer: `"out_hub_dir": "/path/to/hub_dir"`
- Consumer: `"in_hub_dir": "/path/to/hub_dir"`
- Processor: `"in_hub_dir": "..."` and `"out_hub_dir": "..."`

The role reads `hub.json` from the hub directory to discover the broker endpoint.

### 3.3 Automatic channel establishment

When a role starts:

1. **Producer** sends `REG_REQ` to the broker → creates the channel.
   The broker stores: channel name, SHM name, schema hash, transport type,
   inbox info, and the producer's identity.

2. **Consumer** sends `DISC_REQ` to discover the channel, then `CONSUMER_REG_REQ`
   to register. The broker returns connection info (SHM name, ZMQ endpoint, etc.).
   If the consumer has an inbox, its inbox info is also registered.

3. **Processor** does both: `REG_REQ` for its output channel, `CONSUMER_REG_REQ`
   for its input channel. Its inbox (if configured) is registered via the output REG_REQ.

All of this is automatic — the role host handles registration based on the config.
No manual wiring required.

### 3.4 Peer notifications

The broker sends notifications to connected roles:
- `ROLE_REGISTERED_NOTIFY`: a new role joined a channel
- `ROLE_DEREGISTERED_NOTIFY`: a role left
- `CHANNEL_CLOSING_NOTIFY`: channel is shutting down

Scripts receive these as messages in the `messages` parameter of their callbacks.

---

## 4. Discovering Roles and Inboxes

### 4.1 What is an inbox?

An inbox is an optional point-to-point messaging channel. Any role can have one.
It allows targeted messages between specific roles (e.g., a control role sending
calibration parameters to a sensor producer).

See `HEP-CORE-0027-Inbox-Messaging.md` for the full specification.

### 4.2 Configuring an inbox

Add inbox fields to the role's JSON config:

```json
"inbox_schema": {
    "fields": [
        {"name": "cmd", "type": "int32"},
        {"name": "value", "type": "float64"}
    ]
},
"inbox_endpoint": "tcp://0.0.0.0:0"
```

The role host creates an InboxQueue (ZMQ ROUTER) at startup and registers
its endpoint with the broker.

### 4.3 Discovering and connecting to a peer's inbox

From a Python script:

```python
def on_produce(out_slot, fz, msgs, api):
    handle = api.open_inbox("CONS-Display-AABBCCDD")
    if handle is None:
        api.log("Target not found or has no inbox")
        return True

    handle.acquire()
    handle.slot.cmd = 1
    handle.slot.value = 3.14
    ack = handle.send(1000)  # 1s timeout
```

**How discovery works:**

1. `api.open_inbox(uid)` → ScriptEngine queries broker via `ROLE_INFO_REQ`
2. Broker searches all registered roles (producers, consumers, processors)
   by UID and returns the inbox endpoint, schema, packing, and checksum policy.
3. An InboxClient (ZMQ DEALER) connects to the inbox endpoint.
4. The client adopts the inbox **owner's** checksum policy (not the sender's).
5. The connection is cached — subsequent calls return the cached handle.

### 4.4 Checksum policy for inbox

The inbox owner's `"checksum"` config dictates the checksum policy for all
senders. If the owner sets `"checksum": "enforced"`, all senders must compute
BLAKE2b checksums. If `"none"`, no checksums are expected.

The sender learns the policy from the broker's `ROLE_INFO_ACK` response and
configures its InboxClient accordingly. No manual coordination needed.

---

## 5. Policy Configuration

### 5.1 Checksum policy

```json
"checksum": "enforced"
```

| Value | Meaning |
|-------|---------|
| `"enforced"` | Auto-checksum on write, auto-verify on read. Default. |
| `"manual"` | Caller decides when to checksum/verify (advanced). |
| `"none"` | No checksums. Maximum throughput. |

Applies uniformly to all data streams and inbox for the role.

Flexzone checksum is separate (SHM-specific):
```json
"flexzone_checksum": true
```

### 5.2 Loop timing policy

```json
"loop_timing": "fixed_rate",
"target_period_ms": 10.0
```

| Policy | Meaning |
|--------|---------|
| `"max_rate"` | No sleep. Run as fast as possible. No period allowed. |
| `"fixed_rate"` | Sleep to maintain target rate. Reset on overrun. |
| `"fixed_rate_with_compensation"` | Advance deadline from previous, catch up after overrun. |

`loop_timing` is **required**. Exactly one of `target_period_ms` or `target_rate_hz`
for fixed-rate policies. Neither for max_rate.

---

## 6. Typical Pipeline Example

```
┌─────────────┐     SHM      ┌─────────────┐     SHM      ┌─────────────┐
│  Producer    │─────────────→│  Processor   │─────────────→│  Consumer   │
│  (sensor)    │              │  (filter)    │              │  (display)  │
└──────┬──────┘              └──────┬──────┘              └──────┬──────┘
       │                            │                            │
       └────────────────────────────┴────────────────────────────┘
                    Control plane (via Broker)
```

1. Start hub: `pylabhub-hubshell /hub`
2. Start producer: `pylabhub-producer --config /producer/producer.json`
3. Start processor: `pylabhub-processor --config /processor/processor.json`
4. Start consumer: `pylabhub-consumer --config /consumer/consumer.json`

Order doesn't matter — the broker coordinates startup. Consumers retry discovery
until the producer registers the channel. Use `"startup": {"wait_for_roles": [...]}`
for explicit ordering (HEP-0023).

---

## 7. Cross-References

| Topic | Document |
|-------|----------|
| Full config field reference | `README_Deployment.md` §5-7 |
| Inbox architecture | `HEP-CORE-0027-Inbox-Messaging.md` |
| Broker protocol | `HEP-CORE-0007-DataHub-Protocol-and-Policy.md` |
| Checksum policy design | `HEP-CORE-0009-Policy-Reference.md` §2.6.3 |
| Loop timing design | `HEP-CORE-0008-LoopPolicy-and-IterationMetrics.md` |
| Config architecture | `docs/tech_draft/config_single_truth.md` |
| Embedded C++ API | `README_EmbeddedAPI.md` |
| Python script API | `README_Deployment.md` §8 |
