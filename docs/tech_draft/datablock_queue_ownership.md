# DataBlock / Queue Ownership Redesign

**Status**: Design (2026-04-02)
**Scope**: ShmQueue owns DataBlock, strict schema validation, operation isolation

---

## 1. Goal

Make ShmQueue symmetric with ZmqQueue: the queue creates and owns its transport,
receives schema as input, and computes sizes internally. Strict isolation — all
data operations go through the queue abstraction. No DataBlock bypass from the
role API layer.

---

## 2. Current Problem

```
hub::Producer owns DataBlock (unique_ptr)
hub::Producer owns ShmQueue (unique_ptr, non-owning ref to DataBlock)
                 ↓
DataBlock created externally → size passed in → ShmQueue wraps it passively
```

Issues:
- ShmQueue doesn't compute sizes — trusts external input
- hub::Producer bypasses ShmQueue for flexzone ops (direct DataBlock access)
- External callers use `producer->shm()` to reach DataBlock for spinlocks
- No schema validation at consumer side beyond hash comparison
- Size computation scattered across role hosts

---

## 3. Target Architecture

```
hub::Producer owns ShmQueue (unique_ptr)
                 ↓
ShmQueue owns DataBlock (created internally from schema)
ShmQueue is the authority on sizes and schema validation
```

### 3.1 Producer side (SHM creator)

```cpp
// ShmQueue factory — creates DataBlock internally
std::unique_ptr<QueueWriter> ShmQueue::create_writer(
    const std::vector<SchemaFieldDesc> &slot_schema,
    const std::string &packing,
    const DataBlockConfig &shm_config,    // ring capacity, page size, secret, etc.
    const std::string &channel_name,
    const SchemaInfo *fz_schema_info,     // optional flexzone schema hash
    const SchemaInfo *slot_schema_info);  // optional slot schema hash

// Internal:
//   1. compute_field_layout(slot_schema, packing) → item_size
//   2. config.logical_unit_size = item_size
//   3. create_datablock_producer_impl(channel_name, config, fz_info, slot_info)
//   4. Store DataBlock, return QueueWriter
```

### 3.2 Consumer side (SHM attacher)

```cpp
// ShmQueue factory — attaches to existing DataBlock, validates schema
std::unique_ptr<QueueReader> ShmQueue::create_reader(
    const std::string &shm_name,
    uint64_t shared_secret,
    const std::vector<SchemaFieldDesc> &expected_slot_schema,
    const std::string &expected_packing,
    const std::string &channel_name,
    const DataBlockConfig *expected_config);  // optional validation

// Internal:
//   1. find_datablock_consumer_impl(shm_name, secret, expected_config, ...)
//   2. Read logical_unit_size from DataBlock header
//   3. compute_field_layout(expected_slot_schema, expected_packing) → expected_size
//   4. VALIDATE: header logical_unit_size >= expected_size (after cache-line rounding)
//   5. VALIDATE: header schema_hash matches expected schema hash
//   6. Store DataBlock, return QueueReader
```

### 3.3 Processor

- Input (consumer side): `create_reader` attaches to upstream producer's SHM,
  validates schema against processor's `in_slot_schema`
- Output (producer side): `create_writer` creates new SHM with processor's
  `out_slot_schema`

---

## 4. Schema Validation (Consumer Side)

Current: consumer only checks `expected_schema_hash` — a BLAKE2b hash stored in
the SHM header by the producer. This validates schema identity but not size.

New: consumer validates THREE things:
1. **Schema hash match** — `header.schema_hash == expected_schema_hash` (identity)
2. **Size match** — `header.logical_unit_size >= compute_field_layout(schema, packing)`
   (structural compatibility after cache-line rounding)
3. **Field count/type match** (optional, if both sides store the schema field list)

Validation 1 catches schema version mismatches.
Validation 2 catches alignment/packing mismatches (e.g., producer used "aligned",
consumer expects "packed" — sizes would differ).

---

## 5. Operation Isolation

### 5.1 Operations that go through ShmQueue (queue abstraction)

All data and control operations:
- Slot: `write_acquire`, `write_commit`, `write_discard`, `read_acquire`, `read_release`
- Flexzone: `write_flexzone`, `read_flexzone`, `flexzone_size`, `sync_flexzone_checksum`
- Checksum: `set_checksum_options`, `update_checksum`, `verify_checksum`
- Metrics: `metrics`, `reset_metrics`, `capacity`, `set_configured_period`
- Policy: `policy_info`, `set_checksum_policy`

### 5.2 Operations on hub::Producer/Consumer (NOT queue — role-level)

Spinlock and identity are SHM metadata, not data flow. These should be on
Producer/Consumer directly — NOT requiring `shm()` escape hatch:

```cpp
// New methods on hub::Producer / hub::Consumer:
SharedSpinLock &spinlock(size_t index);
size_t spinlock_count() const;

// Identity (forwarded from DataBlock header):
std::string hub_uid() const;
std::string hub_name() const;
std::string producer_uid() const;   // producer only
std::string consumer_uid() const;   // consumer only
```

These delegate internally to the DataBlock owned by ShmQueue. The caller never
gets a raw `DataBlockProducer*` / `DataBlockConsumer*`.

### 5.3 Template RAII path (C++ native API)

`push<F,D>()`, `synced_write<F,D>()`, `pull<F,D>()`, `set_read_handler<F,D>()`
call `DataBlockProducer::with_transaction<F,D>()` directly.

These need a raw `DataBlockProducer*` / `DataBlockConsumer*`. ShmQueue provides:

```cpp
// Internal accessor — for template RAII path only, not for role hosts.
DataBlockProducer *ShmQueue::raw_producer() noexcept;
DataBlockConsumer *ShmQueue::raw_consumer() noexcept;
```

The messaging facade `fn_get_shm` lambda sources from ShmQueue:
```cpp
impl->facade.fn_get_shm = [](void *ctx) -> DataBlockProducer * {
    auto *sq = static_cast<ProducerImpl *>(ctx)->shm_queue_writer_.get();
    return sq ? static_cast<ShmQueue *>(sq)->raw_producer() : nullptr;
};
```

### 5.4 `Producer::shm()` / `Consumer::shm()` — DEPRECATED

Remove the public `shm()` accessor. All callers migrate:
- Spinlock → `producer->spinlock(idx)` / `consumer->spinlock(idx)`
- Identity → `producer->hub_uid()` etc.
- Template RAII → facade (unchanged, but sources from ShmQueue internally)
- Flexzone → already on Producer/Consumer (but now delegates to ShmQueue)

---

## 6. hub::Producer/Consumer Changes

### Current ProducerImpl:
```cpp
struct ProducerImpl {
    unique_ptr<DataBlockProducer> shm;           // REMOVED
    unique_ptr<QueueWriter>       shm_queue_writer_;  // ShmQueue (now owning DataBlock)
    unique_ptr<QueueWriter>       zmq_queue_;
    QueueWriter                  *queue_writer_;  // alias to active queue
    ...
};
```

### New ProducerImpl:
```cpp
struct ProducerImpl {
    unique_ptr<QueueWriter>       shm_queue_writer_;  // ShmQueue (owns DataBlock)
    unique_ptr<QueueWriter>       zmq_queue_;
    QueueWriter                  *queue_writer_;  // alias to active queue
    ...
};
```

`establish_channel()` changes:
```cpp
// OLD:
shm_producer = create_datablock_producer_impl(...);  // external creation
establish_channel(messenger, channel, std::move(shm_producer), opts);
// ... inside establish_channel:
ShmQueue::from_producer_ref(*impl->shm, item_size, ...);

// NEW:
// DataBlock creation moves inside ShmQueue factory
establish_channel(messenger, channel, opts);
// ... inside establish_channel:
impl->shm_queue_writer_ = ShmQueue::create_writer(
    opts.zmq_schema, opts.zmq_packing, opts.shm_config, ...);
```

---

## 7. Role Host Changes

Role hosts no longer compute sizes:

```cpp
// OLD:
out_schema_slot_size_ = scripting::compute_schema_size(out_slot_spec_, packing);
opts.item_size = out_schema_slot_size_;
opts.shm_config.logical_unit_size = out_schema_slot_size_;

// NEW:
// Just pass schema through — ShmQueue computes sizes
opts.zmq_schema = scripting::schema_spec_to_zmq_fields(out_slot_spec_);
opts.zmq_packing = packing;
// opts.shm_config.logical_unit_size NOT set — ShmQueue computes it
// opts.item_size NOT needed — ShmQueue computes it
```

The `out_schema_slot_size_` / `in_schema_slot_size_` member variables on role
hosts can be removed entirely. `schema_fz_size` on core_ computed by ShmQueue
during creation and stored via a callback or return value.

---

## 8. Flexzone Size Propagation

ShmQueue computes flexzone size during creation. But `core_.set_out_fz_spec()`
needs the size for metrics and engine validation.

Two options:
(a) ShmQueue exposes `flexzone_size()` — already exists. Role host calls it
    after queue creation and stores on core_.
(b) `establish_channel()` returns the computed sizes alongside the Producer.

Option (a) is simpler — the role host calls `producer->flexzone_size()` after
`establish_channel()` and sets core_.

---

## 9. Implementation Order

1. **ShmQueue factory refactor**: `create_writer()` and `create_reader()` that
   create/attach DataBlock internally. Keep `from_producer_ref` / `from_consumer_ref`
   temporarily for backward compat.

2. **Consumer schema validation**: `create_reader()` validates size + hash.

3. **Add spinlock/identity methods** to hub::Producer and hub::Consumer.
   Delegate to ShmQueue's internal DataBlock.

4. **Route flexzone ops through ShmQueue** in hub::Producer/Consumer
   (eliminate `pImpl->shm->flexible_zone_span()` calls).

5. **Update establish_channel()**: use `ShmQueue::create_writer()` /
   `create_reader()` instead of external DataBlock creation.

6. **Remove `pImpl->shm` from ProducerImpl/ConsumerImpl**. Remove
   `Producer::shm()` / `Consumer::shm()` public accessors.

7. **Update facade `fn_get_shm`** to source from ShmQueue.

8. **Migrate external callers** (API classes, Lua engine, tests) from
   `producer->shm()->spinlock()` to `producer->spinlock()`.

9. **Remove `opts.item_size` / `opts.flexzone_size`** from ProducerOptions /
   ConsumerOptions (now computed by ShmQueue).

10. **Role hosts**: remove size computation, pass schema through.

11. **Update tests**.
