# DataBlock / Queue Ownership — Design Investigation

**Status**: Investigation (2026-04-02)
**Scope**: Relationship between DataBlock, ShmQueue, and hub::Producer/Consumer

---

## 1. Problem Statement

ShmQueue and ZmqQueue are asymmetric in their relationship to the underlying transport:

- **ZmqQueue**: Creates and owns its ZMQ sockets. Receives schema + packing,
  computes layout internally via `compute_field_layout`. Self-contained.
- **ShmQueue**: Does NOT create or own DataBlock. Receives a pre-created
  `DataBlockProducer*`/`DataBlockConsumer*` as a non-owning reference.
  Receives pre-computed `item_size` and `flexzone_size` as raw numbers.

This asymmetry means:
- Slot/flexzone sizes must be computed externally and passed to ShmQueue
- `hub::Producer`/`hub::Consumer` access DataBlock directly for flexzone ops,
  bypassing the queue abstraction
- Schema-to-size computation is duplicated across role hosts

---

## 2. Current Architecture

```
                         hub::Producer (ProducerImpl)
                        /            |              \
               owns DataBlock   owns ShmQueue    owns ZmqQueue
               (unique_ptr)    (non-owning ref)   (owning)
                    |               |
                    +--- ShmQueue borrows DataBlock pointer
```

### DataBlock creation chain (producer side):
1. Role host computes sizes from schema (currently via engine->type_sizeof)
2. Role host sets `opts.shm_config.logical_unit_size = schema_slot_size`
3. `Producer::create(opts)` → `create_datablock_producer_impl(name, config)`
4. DataBlock allocates SHM with `config.logical_unit_size` as slot stride
5. `establish_channel()` creates ShmQueue with `from_producer_ref(*shm, item_size, ...)`
6. ShmQueue stores the pre-computed `item_size` for `commit()` calls

### ZmqQueue creation chain (for comparison):
1. Role host passes `opts.zmq_schema` (field list) + `opts.zmq_packing`
2. `establish_channel()` → `ZmqQueue::pull_from(endpoint, schema, packing, ...)`
3. ZmqQueue internally: `compute_field_layout(schema, packing)` → gets `item_sz`
4. ZmqQueue allocates buffers based on computed size

---

## 3. DataBlock Access Patterns

### 3.1 Operations that go through ShmQueue (clean path)

All slot data operations already go through ShmQueue:
- `write_acquire()` / `write_commit()` / `write_discard()`
- `read_acquire()` / `read_release()`
- Checksum operations (slot + flexzone)
- Metrics (capacity, period, counters)

ShmQueue also already provides:
- `write_flexzone()` / `read_flexzone()` / `flexzone_size()`
- `sync_flexzone_checksum()`

### 3.2 Operations that bypass ShmQueue (direct DataBlock access)

In `hub::Producer`:
- `write_flexzone()` → `pImpl->shm->flexible_zone_span()` (redundant — ShmQueue has this)
- `read_flexzone()` → same
- `flexzone_size()` → same
- `sync_flexzone_checksum()` → `pImpl->shm->update_checksum_flexible_zone()` (redundant)

In `hub::Consumer`:
- `read_flexzone()` → `pImpl->shm->flexible_zone_span()` (redundant)
- `flexzone_size()` → same

### 3.3 External access via `producer->shm()` / `consumer->shm()` public accessor

**Spinlock access only** (all callers):
- Python ProducerAPI/ConsumerAPI/ProcessorAPI: `shm()->get_spinlock(idx)`, `shm()->spinlock_count()`
- Lua engine: same spinlock pattern
- Tests: spinlock + identity queries (`hub_uid`, `producer_name`, etc.)

### 3.4 Template RAII path (C++ API)

`push<F,D>()`, `synced_write<F,D>()`, `pull<F,D>()` etc. call
`DataBlockProducer::with_transaction<F,D>()` directly via the messaging facade's
`fn_get_shm` function pointer. This is the C++ RAII API for native plugins —
it bypasses both ShmQueue and the queue abstraction entirely.

---

## 4. Design Options

### Option A: ShmQueue owns DataBlock (full encapsulation)

ShmQueue factory receives schema + packing + SHM config, creates DataBlock
internally, computes sizes. Symmetric with ZmqQueue.

**Pros:**
- Clean layering — queue owns its transport
- Schema-to-size computation in one place (ShmQueue)
- Role host passes schemas, not numbers
- Symmetric with ZmqQueue

**Complications:**
- Template RAII path needs `DataBlockProducer*` — ShmQueue must expose it
- Spinlock access needs new ShmQueue methods or a raw accessor
- 14+ `pImpl->shm != nullptr` checks need updating
- Close/destruction ordering changes
- Facade `fn_get_shm` needs to source from ShmQueue

### Option B: Keep ownership in ProducerImpl, eliminate bypasses (incremental)

DataBlock stays in ProducerImpl. But:
1. Route all flexzone ops through ShmQueue (eliminate redundant `pImpl->shm->` calls)
2. Add spinlock/identity accessors to Producer/Consumer (eliminate `shm()` public accessor)
3. Move size computation to `establish_channel()` (compute from schema, not external input)
4. Deprecate `Producer::shm()` / `Consumer::shm()` public accessors

**Pros:**
- Minimal disruption — no ownership change
- Incrementally improves encapsulation
- Template RAII path unaffected

**Complications:**
- Asymmetry remains (ShmQueue doesn't own DataBlock)
- `establish_channel` computes sizes — still external to queue

### Option C: ShmQueue computes sizes but doesn't own DataBlock (hybrid)

ShmQueue receives schema + packing in its factory (like ZmqQueue), computes
sizes internally via `compute_field_layout`. But DataBlock is still created
externally and passed in. ShmQueue validates that the DataBlock's
`logical_unit_size` matches its computed size.

**Pros:**
- ShmQueue becomes the size authority (like ZmqQueue)
- Size computation in one place
- Less disruptive than full ownership transfer
- Template RAII path unaffected

**Complications:**
- DataBlock still created externally with pre-computed size — who computes it?
- `establish_channel()` would compute size for DataBlockConfig, then ShmQueue
  re-computes for validation. Slight duplication but serves as cross-check.

---

## 5. Recommendation

**Option C (hybrid) as immediate step, Option A as long-term goal.**

Immediate:
1. ShmQueue factory receives schema + packing (not raw sizes)
2. ShmQueue computes `item_size` / `flexzone_size` from schema via `compute_field_layout`
3. ShmQueue validates against the DataBlock's actual `logical_unit_size`
4. `establish_channel()` also computes sizes for DataBlockConfig creation
   (using the same `compute_field_layout`) — one-time computation, no role host involvement
5. Role hosts pass schema + packing, never pre-compute sizes
6. Eliminate `Producer::shm()` / `Consumer::shm()` public accessors:
   - Add spinlock/identity methods to Producer/Consumer directly
   - Route flexzone ops through ShmQueue (already implemented there)

Long-term (Option A):
- ShmQueue creates DataBlock internally
- Template RAII path accesses DataBlock through ShmQueue accessor
- Full symmetry with ZmqQueue

---

## 6. Affected Files

### Immediate changes (Option C):
- `schema_field_layout.hpp` — shared size computation (already created)
- `hub_shm_queue.cpp` — factory receives schema, computes sizes
- `hub_producer.cpp` — `establish_channel()` computes sizes for DataBlockConfig
- `hub_consumer.cpp` — same
- `hub_producer.hpp` — deprecate/remove `shm()` accessor, add spinlock methods
- `hub_consumer.hpp` — same
- `producer_api.cpp`, `consumer_api.cpp`, `processor_api.cpp` — use new spinlock methods
- `lua_engine.cpp` — use new spinlock methods
- Role host `.cpp` files — pass schema instead of pre-computed sizes
- `engine_module_params.cpp` — remove size computation
- Tests — update `shm()` calls

### Long-term changes (Option A):
- `hub_shm_queue.cpp` — create DataBlock in factory
- `hub_producer.cpp` — remove DataBlock from ProducerImpl
- Template RAII methods — source DataBlock pointer from ShmQueue
