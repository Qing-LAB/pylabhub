# DataBlock / Queue Ownership Redesign

**Status**: Design (2026-04-02)
**Scope**: ShmQueue owns DataBlock, symmetric with ZmqQueue, strict schema validation

---

## 1. Design Principle

Each queue type is a **self-contained abstraction** that owns its transport:

- **ZmqQueue**: creates and owns ZMQ sockets. Receives schema + packing +
  transport params. Computes layout internally. Caller never sees ZMQ internals.
- **ShmQueue**: creates and owns DataBlock. Receives schema + packing +
  SHM params. Computes sizes internally. Caller never sees DataBlockConfig.

Both queue types expose the same `QueueReader` / `QueueWriter` interface.
The role API operates identically on both — no transport-specific code above
the queue layer.

**Layer isolation:**
```
Role API layer    →  schema + high-level params
Queue layer       →  owns transport, computes layout, validates schema
DataBlock layer   →  raw SHM allocation and access (untouched)
ZMQ layer         →  raw socket management (untouched)
```

---

## 2. ZmqQueue Pattern (reference)

```cpp
// Writer factory — self-contained
unique_ptr<QueueWriter> ZmqQueue::push_to(
    endpoint,             // transport address
    schema,               // field list (SchemaFieldDesc vector)
    packing,              // "aligned" or "packed"
    bind,                 // transport-specific
    schema_tag,           // validation tag
    sndhwm,               // transport-specific
    send_buffer_depth,    // queue-level
    overflow_policy);     // queue-level

// Reader factory — self-contained
unique_ptr<QueueReader> ZmqQueue::pull_from(
    endpoint,             // transport address
    schema,               // field list
    packing,              // layout
    bind,                 // transport-specific
    max_buffer_depth,     // queue-level
    schema_tag);          // validation tag
```

**Internally**: calls `compute_field_layout(schema, packing)` → gets `item_sz`,
creates ZMQ socket, allocates buffers. Caller never provides pre-computed sizes.

---

## 3. ShmQueue Factory Design (new)

### 3.1 Writer (producer — creates SHM)

```cpp
unique_ptr<QueueWriter> ShmQueue::create_writer(
    const std::string &channel_name,
    const std::vector<SchemaFieldDesc> &slot_schema,
    const std::string &slot_packing,
    const std::vector<SchemaFieldDesc> &fz_schema,    // empty = no flexzone
    const std::string &fz_packing,
    // SHM allocation params:
    uint32_t ring_buffer_capacity,
    DataBlockPageSize page_size,
    uint64_t shared_secret,
    DataBlockPolicy policy,
    ConsumerSyncPolicy sync_policy,
    ChecksumPolicy checksum_policy,
    // Checksum behavior:
    bool checksum_slot,
    bool checksum_fz,
    bool always_clear_slot,
    // Identity (stored in SHM header):
    const std::string &hub_uid = {},
    const std::string &hub_name = {},
    // Schema hashes (stored in SHM header for consumer validation):
    const SchemaInfo *slot_schema_info = nullptr,
    const SchemaInfo *fz_schema_info = nullptr,
    // Producer identity (stored in SHM header):
    const std::string &producer_uid = {},
    const std::string &producer_name = {});
```

**Internally:**
1. `compute_field_layout(slot_schema, slot_packing)` → `item_size`
2. `compute_field_layout(fz_schema, fz_packing)` → `fz_size`, round to 4KB
3. Build `DataBlockConfig` with computed sizes + caller's SHM params
4. `create_datablock_producer_impl(channel_name, config, fz_info, slot_info)`
5. Own the `DataBlockProducer`
6. Return `QueueWriter`

### 3.2 Reader (consumer — attaches to existing SHM)

```cpp
unique_ptr<QueueReader> ShmQueue::create_reader(
    const std::string &shm_name,               // from broker discovery
    uint64_t shared_secret,
    const std::vector<SchemaFieldDesc> &expected_slot_schema,
    const std::string &expected_packing,
    const std::string &channel_name,
    // Validation:
    bool verify_slot_checksum,
    bool verify_fz_checksum,
    // Consumer identity:
    const std::string &consumer_uid = {},
    const std::string &consumer_name = {});
```

**Internally:**
1. `find_datablock_consumer_impl(shm_name, secret, nullptr, nullptr, nullptr, uid, name)`
   — wrapped in try-catch (DataBlock constructor throws on nonexistent SHM; returns nullptr)
2. Read `logical_unit_size` from DataBlock header
3. `compute_field_layout(expected_slot_schema, expected_packing)` → `expected_size`
4. **VALIDATE**: `header.logical_unit_size` (after cache-line rounding of expected_size)
   matches the DataBlock's `effective_logical_unit_size()`. Mismatch → error + return nullptr.
5. **VALIDATE**: schema hash if available in header
6. Own the `DataBlockConsumer`
7. Return `QueueReader`

---

## 4. Schema Validation (Consumer Side)

The consumer validates that the upstream producer's SHM matches its expectations:

1. **Size match**: consumer computes expected slot size from its schema + packing.
   Compares against `header.logical_unit_size` (producer's stored size).
   Mismatch means schema or packing disagree — error.

2. **Schema hash match**: if the DataBlock header has a schema hash (stored by
   producer), compare against hash computed from consumer's expected schema.
   Mismatch means the field layout differs — error.

3. **Flexzone size match**: consumer reads `flexible_zone_size` from header.
   If consumer has a fz schema, compute expected fz size and validate.

Validation happens inside `create_reader()` — the caller gets nullptr on failure.

---

## 5. Operation Isolation

### 5.1 Through QueueWriter / QueueReader (unified interface)

All data flow operations — identical for SHM and ZMQ:
- Slot: `write_acquire`, `write_commit`, `write_discard`, `read_acquire`, `read_release`
- Flexzone: `write_flexzone`, `read_flexzone`, `flexzone_size`, `sync_flexzone_checksum`
- Checksum: `set_checksum_policy`, `update_checksum`, `verify_checksum`
- Metrics: `metrics`, `reset_metrics`, `capacity`, `set_configured_period`
- Query: `item_size`, `name`, `policy_info`

### 5.2 On hub::Producer / hub::Consumer (role-level, not queue)

SHM-specific metadata that the role API exposes to scripts:
```cpp
SharedSpinLock &Producer::spinlock(size_t index);
size_t Producer::spinlock_count() const;
// Same for Consumer.
```

These delegate to ShmQueue's internal DataBlock. No public `shm()` accessor.

### 5.3 Template RAII path (C++ native API)

For `push<F,D>()`, `synced_write<F,D>()`, `pull<F,D>()`:
```cpp
DataBlockProducer *ShmQueue::raw_producer() noexcept;
DataBlockConsumer *ShmQueue::raw_consumer() noexcept;
```

Internal accessor only — used by the messaging facade. Not part of the public
queue interface.

### 5.4 Deprecated: `Producer::shm()` / `Consumer::shm()` — REMOVED

No public accessor to raw DataBlock. All callers migrate:
- Spinlock → `producer->spinlock(idx)`
- Identity → `producer->hub_uid()` etc.
- Flexzone → through queue (`producer->write_flexzone()` delegates to ShmQueue)
- Template RAII → facade sources from `ShmQueue::raw_producer()`

---

## 6. establish_channel() Changes

### Current (producer):
```cpp
// Step 1: create DataBlock externally
shm_producer = create_datablock_producer_impl(name, policy, opts.shm_config, ...);

// Step 2: pass to establish_channel
establish_channel(messenger, channel, std::move(shm_producer), opts);

// Inside establish_channel:
impl->shm = std::move(shm_producer);
impl->shm_queue_writer_ = ShmQueue::from_producer_ref(*impl->shm, opts.item_size, ...);
```

### New (producer):
```cpp
// establish_channel — no external DataBlock creation
establish_channel(messenger, channel, opts);

// Inside establish_channel:
if (opts.has_shm) {
    impl->shm_queue_writer_ = ShmQueue::create_writer(
        opts.channel_name, opts.zmq_schema, opts.zmq_packing,
        opts.fz_schema, opts.fz_packing,
        opts.shm_config.ring_buffer_capacity,
        opts.shm_config.physical_page_size,
        ...);
}
```

Same pattern for consumer — `create_reader` instead of external DataBlock attachment.

---

## 7. Role Host Changes

Role hosts pass schema through. No size computation:

```cpp
// OLD:
out_schema_slot_size_ = compute_schema_size(out_slot_spec_, packing);
opts.item_size = out_schema_slot_size_;
opts.shm_config.logical_unit_size = out_schema_slot_size_;
opts.flexzone_size = core_.out_schema_fz_size();
opts.shm_config.flex_zone_size = core_.out_schema_fz_size();

// NEW:
opts.zmq_schema = schema_spec_to_zmq_fields(out_slot_spec_);
opts.zmq_packing = packing;
opts.fz_schema = schema_spec_to_zmq_fields(out_fz_spec);
opts.fz_packing = packing;
// No size fields — queue computes them
```

Member variables `out_schema_slot_size_` / `in_schema_slot_size_` removed from
role hosts. `core_.set_out_fz_spec()` called after queue creation, reading size
from `queue->flexzone_size()`.

---

## 8. ProducerOptions / ConsumerOptions Changes

Remove size fields that ShmQueue now computes:

```cpp
// REMOVE from ProducerOptions:
size_t item_size;        // computed by ShmQueue from schema
size_t flexzone_size;    // computed by ShmQueue from fz schema

// ADD to ProducerOptions (if not already present):
std::vector<SchemaFieldDesc> fz_schema;   // flexzone schema fields
std::string fz_packing;                    // flexzone packing
```

`zmq_schema` and `zmq_packing` already exist on ProducerOptions — they serve
double duty for both ZmqQueue and ShmQueue now. Consider renaming to just
`schema` and `packing` since they're transport-agnostic.

---

## 9. Implementation Order

1. **ShmQueue::create_writer()** — new factory that creates DataBlock internally.
   Compute sizes from schema. Keep `from_producer_ref` temporarily for tests.

2. **ShmQueue::create_reader()** — new factory that attaches + validates schema.

3. **Update establish_channel()** in hub::Producer — use create_writer.
   Remove external DataBlock creation from Producer::create().

4. **Update establish_channel()** in hub::Consumer — use create_reader.

5. **Add spinlock/identity methods** to hub::Producer and hub::Consumer.
   Delegate to ShmQueue's internal DataBlock via `raw_producer()`/`raw_consumer()`.

6. **Route flexzone ops** in hub::Producer/Consumer through ShmQueue
   (eliminate direct `pImpl->shm->` flexzone calls).

7. **Remove `pImpl->shm`** from ProducerImpl/ConsumerImpl.
   Remove `Producer::shm()` / `Consumer::shm()` public accessors.

8. **Update facade `fn_get_shm`** to source from `ShmQueue::raw_producer()`.

9. **Migrate external callers** — API classes, Lua engine from
   `producer->shm()->spinlock()` to `producer->spinlock()`.

10. **Remove `opts.item_size` / `opts.flexzone_size`** from ProducerOptions/ConsumerOptions.
    Add `fz_schema` / `fz_packing` if not present.

11. **Role hosts** — remove size computation, pass schema through.
    Remove `out_schema_slot_size_` / `in_schema_slot_size_` members.

12. **Remove old factories** — `from_producer_ref`, `from_consumer_ref`,
    `from_producer`, `from_consumer`. Only `create_writer` / `create_reader` remain.

13. **Update tests**.
