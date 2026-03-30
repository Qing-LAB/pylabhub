# Config Single Truth — Role-Level Policy Unification

**Status**: Draft (2026-03-29)
**Scope**: Timing config, checksum config, single-truth propagation
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
parse_checksum_config() → ChecksumParams { bool slot, bool flexzone }
    ↓ (single struct, passed once)
Role host propagates to all owned queues:
    ├─ ShmQueue: set_checksum_options(slot, fz) + DataBlock policy derived
    ├─ ZmqQueue: checksum always-on (wire integrity)
    └─ InboxQueue: checksum always-on (wire integrity)
DataBlock ChecksumPolicy: derived from ChecksumParams, not hardcoded
    - slot=true → Enforced (DataBlock handles automatically)
    - slot=false → None (no checksums)
    - Manual: reserved for advanced C API users only
```

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
- ZMQ/Inbox wire checksum (always-on, independent of policy)
- ContextMetrics (timing measurement, checksum_error_count)
- QueueMetrics reporting structure
- Hierarchical metrics output
