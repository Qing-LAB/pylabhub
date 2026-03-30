# Config Single Truth — Role-Level Policy Unification

**Status**: Implemented (2026-03-30)
**Scope**: Timing config, checksum config, single-truth propagation, config validation
**Depends on**: ContextMetrics unification (done), LoopTimingParams (done)

---

## 1. Problem

Configuration values are consumed at multiple levels independently, with some
paths hardcoded instead of derived from config.

### 1.1 Timing

The same `tc.period_us` is read in THREE places independently:

```
config_.timing().period_us
    ├─ opts.timing = tc.timing_params()           → establish_channel → queue ContextMetrics
    ├─ core_.set_configured_period(tc.period_us)  → RoleHostCore → LoopMetricsSnapshot
    └─ const double period_us = tc.period_us      → main loop (reads config directly)
```

No single derived value flows to all consumers.

### 1.2 Checksum

`ChecksumPolicy::Manual` is hardcoded at the producer role host, not derived from config:

```
config: update_checksum = true  (boolean)
    ├─ opts.update_checksum = true                → ShmQueue flag → explicit checksum
    └─ opts.shm_config.checksum_policy = Manual   → DataBlock → skips auto-checksum (HARDCODED)
```

Config has booleans (`update_checksum`, `verify_checksum`). DataBlock has a 3-way
policy enum (`None`/`Manual`/`Enforced`). The mapping is hardcoded, not derived.

### 1.3 Per-direction config that should be per-role

Current config has per-direction prefixes for checksum:
- `out_update_checksum` (producer/processor output)
- `in_verify_checksum` (consumer/processor input)

But checksum policy is a role-level decision: either the role enforces checksums
on all data streams or it doesn't. Fine-grained per-direction control adds
complexity without benefit.

Timing is already per-role (one `loop_timing`, one `target_period_ms`). But the
processor applies it only to `out_opts` instead of uniformly.

---

## 2. Target Design

### 2.1 Role-level config truth

| Config field | Scope | Description |
|---|---|---|
| `loop_timing` | Per-role | `"max_rate"`, `"fixed_rate"`, `"fixed_rate_with_compensation"` |
| `target_period_ms` or `target_rate_hz` | Per-role | Loop period (required for fixed_rate) |
| `checksum` | Per-role | `true`/`false` — all data streams |
| `flexzone_checksum` | Per-role, SHM-only | `true`/`false` — flexzone region |

### 2.2 Single propagation path

Each config value flows through ONE path from config to execution:

**Timing:**
```
parse_timing_config() → LoopTimingParams
    ↓ (single struct, passed once)
RoleHostCore::set_timing_params(params)
    ├─ Stores params for loop_metrics() reporting
    ├─ Main loop reads from stored params (not config directly)
    └─ Queue: set_configured_period(params.period_us) for ContextMetrics
```

**Checksum:**
```
JSON: "checksum": "enforced" | "manual" | "none"
      "flexzone_checksum": true | false
    ↓
parse_checksum_config() → { ChecksumPolicy policy, bool flexzone_checksum }
    ↓
Role host propagates to all owned queues via unified interface:
    queue->set_checksum_policy(policy)
    queue->set_flexzone_checksum(flexzone_enabled)
    ↓
ShmQueue: passes policy to DataBlockConfig; DataBlock executes
ZmqQueue: stores policy; skips compute/verify when None
InboxQueue: same as ZmqQueue
```

**Unified checksum API on QueueWriter/QueueReader base class:**
```
set_checksum_policy(ChecksumPolicy)     — all transports
set_flexzone_checksum(bool)             — SHM acts, ZMQ/Inbox no-op
update_checksum()                       — compute slot checksum (writer)
update_flexzone_checksum()              — compute flexzone checksum (writer)
verify_checksum() → bool                — verify slot checksum (reader)
verify_flexzone_checksum() → bool       — verify flexzone checksum (reader)
```

ShmQueue delegates to DataBlock SlotWriteHandle/SlotConsumeHandle.
ZmqQueue/InboxQueue compute/verify BLAKE2b on raw payload data.
Flexzone variants are no-ops on ZMQ/Inbox (no flexzone concept).

When Enforced: queue calls update/verify automatically in commit/release.
When Manual: caller calls explicitly via script API. If caller doesn't call
  update_checksum(), checksum bytes are zero.
When None: all checksum operations skipped. Checksum bytes are zero.

**Checksum enforcement:**
The receiver's policy is authoritative. If the receiver requires checksum
(Enforced or Manual), verification is mandatory — zero checksum bytes from
a sender that didn't compute will fail verification and the frame is dropped.

- Receiver None: skip all verification
- Receiver Enforced/Manual: verify BLAKE2b unconditionally. Zero = fail.
- Sender None: sends zero checksum bytes. Receiver with Enforced will reject.
- Sender Enforced: sends valid checksum. Receiver with None will ignore.

This means sender and receiver should use the same policy. For SHM, the
producer writes the policy to SharedMemoryHeader and the consumer reads it.
For ZMQ/Inbox, both sides should be configured with the same role-level policy.
Mismatched policies (sender=None, receiver=Enforced) result in all frames
being rejected — this is correct behavior (configuration error, not silent failure).

Note: DataBlock SHM uses a separate zero-detection mechanism in
verify_checksum_slot_impl (checks for all-zero hash before computing) because
the SHM slot_cks_is_valid flag provides the authoritative validity signal.
ZMQ/Inbox do not have this flag — they rely on BLAKE2b verification directly.

### 2.3 Per-role, not per-direction

Each role has one main loop, one timing policy, one checksum policy:
- **Producer**: writes to output queue. Checksum = compute on write.
- **Consumer**: reads from input queue. Checksum = verify on read.
- **Processor**: reads from input, writes to output. Both sides obey the same policy.

The role decides. The queue executes. No per-direction config.

---

## 3. Specific Changes

### 3.1 Config parser

Replace per-direction checksum booleans with role-level:
- Remove: `out_update_checksum`, `in_verify_checksum`
- Add: `checksum: true/false`, `flexzone_checksum: true/false`

### 3.2 DataBlock ChecksumPolicy derivation

Currently hardcoded `Manual`. After:
- `checksum: true` → `ChecksumPolicy::Enforced` (DataBlock auto)
- `checksum: false` → `ChecksumPolicy::None`
- `Manual`: not exposed in config; reserved for C API direct users

If `Enforced`, ShmQueue does NOT need to call `update_checksum_slot()` /
`verify_checksum_slot()` explicitly — DataBlock handles it in release.
ShmQueue's `checksum_slot`/`verify_slot` flags become unnecessary for the
`Enforced` path.

### 3.3 Timing single path

Main loop reads from `RoleHostCore` stored params instead of `config_.timing()`
directly. All three consumers (queue, metrics, loop) read from one source.

### 3.4 Processor dual-queue

Processor applies the same timing and checksum to both input and output queues.
No separate `in_opts` / `out_opts` for these parameters.

---

## 4. Migration

### Phase 1: Checksum config unification
- New config fields: `checksum`, `flexzone_checksum`
- Derive `ChecksumPolicy` from config (not hardcode)
- Remove per-direction checksum from processor config
- Update test configs

### Phase 2: Timing single path
- `RoleHostCore` stores `LoopTimingParams`
- Main loop reads from `RoleHostCore` instead of config
- Remove direct config reads from main loop

### Phase 3: Remove ShmQueue explicit checksum path
- If `Enforced`, ShmQueue delegates entirely to DataBlock
- ShmQueue `checksum_slot`/`verify_slot` flags remain for `Manual` (C API)
- Queue factories no longer need checksum flag parameters

---

## 5. What Does NOT Change

- DataBlock checksum implementation (compute, verify, invalidate, caching)
- ZMQ/Inbox wire checksum (conditional on policy, not always-on)
- ContextMetrics (timing measurement, checksum_error_count)
- QueueMetrics reporting structure
- Hierarchical metrics output

---

## 6. Implementation Summary (2026-03-30)

### 6.1 Config layer architecture

```
JSON config file
    ↓  parse_timing_config()     → TimingConfig { loop_timing, period_us, io_wait_ratio }
    ↓  parse_checksum_config()   → ChecksumConfig { policy, flexzone }
    ↓  parse_shm_config()        → ShmConfig { enabled, secret, slot_count, sync_policy }
    ↓  parse_transport_config()  → TransportConfig { transport, zmq_* fields }
    ↓  ... (identity, auth, script, inbox, startup, monitoring)
    ↓
    ↓  validate_known_keys()     → reject unknown JSON keys (whitelist)
    ↓
RoleConfig (unified config class)
    ├── timing()      → LoopTimingParams via timing_params()
    ├── checksum()    → ChecksumConfig { policy, flexzone }
    ├── in_shm()      → ShmConfig (per-direction: slot_count, sync_policy — no checksum)
    ├── out_shm()     → ShmConfig
    └── ... other accessors
```

**Config levels:**

| Level | Parameters | Scope |
|-------|-----------|-------|
| **Role** | loop_timing, target_period_ms/target_rate_hz, checksum, flexzone_checksum, script, identity, auth, inbox, startup, monitoring | One per role instance |
| **Per-direction** | hub_dir, channel, transport, zmq_*, shm_enabled/secret/slot_count/sync_policy | Separate in_ and out_ |
| **Role-specific** | slot_schema, flexzone_schema | Varies by role type |

### 6.2 Config → role host → execution

**Timing:**
```
config_.timing()
    → opts.timing = tc.timing_params()              (role host)
    → establish_channel():
        queue->set_configured_period(period_us)      (ContextMetrics storage)
    → core_.set_configured_period(period_us)         (LoopMetricsSnapshot reporting)
    → main loop reads config_.timing() directly      (deadline computation)
```

**Checksum:**
```
config_.checksum()
    → opts.checksum_policy = config_.checksum().policy    (role host)
    → opts.shm_config.checksum_policy = config_.checksum().policy
    → establish_channel():
        DataBlock created with policy from DataBlockConfig
        queue->set_checksum_policy(policy)
        queue->set_flexzone_checksum(flexzone)
    → inbox_queue_->set_checksum_policy(policy)           (inbox receiver)
    → ctx.checksum_policy = policy                        (RoleContext for InboxClient)
```

### 6.3 Config key whitelist

`validate_known_keys()` in `role_config.cpp` checks all top-level JSON keys against
`kAllowedKeys`. Unknown keys → `std::runtime_error` at parse time.

This prevents:
- Typos (`"cheksum"` instead of `"checksum"`)
- Obsolete keys (`"out_update_checksum"` — removed)
- Ambiguous config (keys that look valid but are ignored)

Every new config parameter must be added to the whitelist.

### 6.4 Role registration and inbox discovery

**Registration** (role host startup):

All roles register with the broker. Inbox info is carried in the registration message:

| Role | Registration | Inbox stored on |
|------|-------------|-----------------|
| Producer | REG_REQ (creates channel) | ChannelEntry |
| Consumer | CONSUMER_REG_REQ (subscribes) | ConsumerEntry |
| Processor | REG_REQ (output) + CONSUMER_REG_REQ (input) | ChannelEntry (via output REG_REQ) |

Registration payload includes: `inbox_endpoint`, `inbox_schema_json`, `inbox_packing`,
`inbox_checksum`. All optional — empty = no inbox.

**Discovery** (api.open_inbox):

```
Script: api.open_inbox("TARGET-UID")
    → ScriptEngine → RoleHostCore cache (check for existing client)
    → Cache miss: Messenger.query_role_info("TARGET-UID")
    → ROLE_INFO_REQ to broker
    → Broker searches:
        1. ChannelEntry::producer_role_uid (producers + processors)
        2. ConsumerEntry::role_uid (consumers)
    → ROLE_INFO_ACK: endpoint, schema, packing, checksum_policy
    → InboxClient::connect_to(endpoint, schema, packing)
    → client->set_checksum_policy(owner's policy)
    → Cached in RoleHostCore for reuse
```

The inbox **owner** dictates the checksum policy. The sender reads it from
ROLE_INFO_ACK and adopts it. Mismatched policies (sender computes, receiver
expects none, or vice versa) work correctly: the receiver enforces its own policy.

### 6.5 Future: role-level identity separation

Currently role identity (UID, name, role_type, inbox) is embedded in data channel
registration (REG_REQ / CONSUMER_REG_REQ). A future `ROLE_REG_REQ` would separate
role identity from data channel participation:

```
ROLE_REG_REQ: { uid, name, role_type, inbox_endpoint, inbox_schema, inbox_packing, inbox_checksum }
    → Broker stores in global role table (HEP-0019 Phase 2)
    → ROLE_INFO_REQ queries this table directly

REG_REQ / CONSUMER_REG_REQ: data channel operations only
    → No inbox/identity fields (already in role table)
```

This cleanly supports the processor which has one identity but participates in
two data channels (input + output).
