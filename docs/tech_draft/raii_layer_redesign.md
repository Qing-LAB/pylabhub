# RAII Layer Redesign — Draft Design

**Status**: Draft (2026-03-28)
**Scope**: SlotIterator, TransactionContext, typed queue wrappers, timing unification
**Depends on**: Timing unification (remove from DataBlock), config parser hardening

---

## 1. Motivation

The current RAII layer (SlotIterator, TransactionContext, SlotRef) is tied to DataBlock
directly. It cannot work with ZmqQueue. It also has timing inconsistencies with the main
loop used by role hosts.

Three problems to solve together:

1. **Timing unification**: Loop policy and configured_period_us live in DataBlock pImpl,
   but DataBlock doesn't use them for execution. The main loop reads config directly.
   The SlotIterator reads DataBlock metrics. Two sources of truth.

2. **RAII timing consistency**: SlotIterator implements only simple FixedRate (sleep to
   last_acquire + period). The main loop supports MaxRate, FixedRate, and
   FixedRateWithCompensation with retry-acquire, deadline tracking, and overrun detection.

3. **RAII queue abstraction**: SlotIterator uses `DataBlockProducer*`/`DataBlockConsumer*`
   directly. It should work through the queue abstraction (`QueueWriter`/`QueueReader`)
   so it supports both SHM and ZMQ transport.

---

## 2. Design: Typed Queue Wrappers

A standalone addon header `typed_queue.hpp` provides compile-time typed wrappers
over the existing `void*` queue interface:

```cpp
// typed_queue.hpp — C++ RAII addon, not part of shared library ABI
#include "utils/hub_queue.hpp"

namespace pylabhub::hub
{

template <typename SlotT>
class TypedQueueWriter
{
    QueueWriter *q_;
public:
    explicit TypedQueueWriter(QueueWriter &q) : q_(&q) {}

    SlotT* write_acquire(std::chrono::milliseconds timeout) {
        return static_cast<SlotT*>(q_->write_acquire(timeout));
    }
    void write_commit()  { q_->write_commit(); }
    void write_discard() { q_->write_discard(); }

    QueueMetrics metrics() const { return q_->metrics(); }
    size_t item_size() const { return q_->item_size(); }
    // ... other forwarding as needed
};

template <typename SlotT>
class TypedQueueReader
{
    QueueReader *q_;
public:
    explicit TypedQueueReader(QueueReader &q) : q_(&q) {}

    const SlotT* read_acquire(std::chrono::milliseconds timeout) {
        return static_cast<const SlotT*>(q_->read_acquire(timeout));
    }
    void read_release() { q_->read_release(); }

    QueueMetrics metrics() const { return q_->metrics(); }
    size_t item_size() const { return q_->item_size(); }
};

} // namespace pylabhub::hub
```

Properties:
- Non-owning (raw pointer to existing queue)
- No virtual dispatch for the typed path (just a cast)
- No ABI impact (header-only, not exported from shared lib)
- `static_assert(sizeof(SlotT) <= q_->item_size())` at construction for safety

---

## 3. Design: Timing at Queue Level

### 3.1 Remove from DataBlock

- Delete `DataBlockProducerImpl::loop_policy` (dead — never read)
- Delete `DataBlockProducerImpl::configured_period_us` (only copied to metrics)
- Delete `DataBlockConsumerImpl::loop_policy` and `configured_period_us`
- Delete `DataBlockProducer::set_loop_policy()` and `DataBlockConsumer::set_loop_policy()`
- `ContextMetrics::configured_period_us` remains — but set from the queue level, not DataBlock

### 3.2 Timing config struct

A small POD struct that carries the validated timing config through the system:

```cpp
// In loop_timing_policy.hpp or a new timing_params.hpp:
struct LoopTimingParams
{
    LoopTimingPolicy policy{LoopTimingPolicy::MaxRate};
    uint64_t         period_us{0};      // 0 = MaxRate
    double           io_wait_ratio{0.1}; // for compute_short_timeout
};
```

This is set once from `parse_timing_config()` and passed to:
- The queue (for `QueueMetrics::configured_period_us` reporting)
- The SlotIterator (for timing execution)
- The main loop (for `compute_next_deadline`)

### 3.3 Queue stores timing for metrics

`QueueReader`/`QueueWriter` base class gains:

```cpp
void set_timing_params(const LoopTimingParams &params);
```

ShmQueue: stores params, copies `period_us` to `ContextMetrics::configured_period_us`
ZmqQueue: stores params, copies `period_us` to `configured_period_us_` atomic

This replaces `set_configured_period(uint64_t)` and `ShmQueue::set_loop_policy()`.

---

## 4. Design: SlotIterator Timing

### 4.1 Construction

SlotIterator receives timing params at construction (from TransactionContext):

```cpp
SlotIterator(TypedQueueWriter<SlotT> &queue,
             const LoopTimingParams &timing);
```

No more `DataBlockProducer*` handle. No more reading metrics for timing.

### 4.2 operator++() timing discipline

Matches the main loop step-by-step:

```
operator++():
  1. Auto-commit/release previous slot
  2. Record work_end = now()
  3. If previous deadline exists: check overrun (work_end > deadline)
  4. Compute next_deadline via compute_next_deadline(policy, prev_deadline, cycle_start, period)
  5. Compute short_timeout from period * io_wait_ratio
  6. Retry-acquire with short_timeout until deadline budget exhausted or success
  7. If FixedRate/Compensation and now < deadline: sleep_until(deadline)
  8. Record cycle_start = now()
  9. Return Result
```

### 4.3 Overrun reporting

SlotIterator tracks overruns internally:

```cpp
uint64_t overrun_count() const noexcept;
```

The TransactionContext exposes this to callers. For role hosts using RAII instead of
the manual loop, `overrun_count()` replaces `core_.loop_overrun_count()`.

---

## 5. Migration Path

### Phase 1: Timing unification (prerequisite) ✅ DONE 2026-03-29
- [x] Remove timing from DataBlock (`set_loop_policy`, `loop_policy`, `configured_period_us` pImpl fields)
- [x] Add `LoopTimingParams` struct to `loop_timing_policy.hpp`
- [x] Add `set_configured_period()` to QueueReader/QueueWriter base class
- [x] `establish_channel()` uses unified `queue->set_configured_period()` for both SHM and ZMQ
- [x] Config parser: `loop_timing` required, strict cross-field validation, null-as-absent
- [x] Role host Options: `LoopTimingParams timing` replaces 3 separate fields
- [x] ContextMetrics: private fields, accessor API (`set_*`/`*_val()`/`clear()`), renamed to `context_metrics.hpp`
- [x] `DataBlockProducer::mutable_metrics()` for queue-layer write access
- [x] 1177/1177 tests pass

### Phase 2: Typed queue wrappers
- Create `typed_queue.hpp`
- Unit tests for TypedQueueWriter/TypedQueueReader

### Phase 3: SlotIterator redesign
- SlotIterator uses TypedQueueWriter/TypedQueueReader
- Full timing discipline in operator++()
- Overrun detection
- Acquire retry with budget
- Tests: timing parity with main loop

### Phase 4: TransactionContext update
- TransactionContext creates TypedQueue from QueueWriter*/QueueReader*
- ctx.slots() passes LoopTimingParams to SlotIterator
- Existing RAII user code unchanged (same for-range pattern)

---

## 6. What Does NOT Change

- QueueWriter/QueueReader virtual interface (void* stays)
- ShmQueue/ZmqQueue implementations
- Script engine path (uses void* directly)
- Role host main loop (reads config directly — option to migrate to RAII later)
- Shared library ABI (typed_queue.hpp is header-only addon)

---

## 7. Open Questions

1. Should `LoopTimingParams` include `heartbeat_interval_ms`? The main loop does
   heartbeat update; SlotIterator currently does `m_handle->update_heartbeat()`.
   With queue abstraction, heartbeat is a DataBlock-specific concept (not on QueueWriter).

2. Should the SlotIterator support message drain (inbox + ctrl messages)? The main
   loop drains before callback. The RAII loop body is the callback equivalent —
   messages would need to be available inside the for-range body.

3. Auto-publish semantics: currently tied to `SlotWriteHandle::commit()`. With
   QueueWriter, it would be `write_commit()`. The RAII safety guarantee (commit on
   normal exit, discard on exception) needs to map cleanly.

---

## 8. Cross-References

- HEP-CORE-0008: LoopPolicy and IterationMetrics (timing measurement)
- HEP-CORE-0009 §2.6: Policy reference (LoopTimingPolicy, LoopPolicy)
- `docs/todo/RAII_LAYER_TODO.md`: Existing RAII layer task tracking
- `src/include/utils/slot_iterator.hpp`: Current implementation
- `src/include/utils/transaction_context.hpp`: Current TransactionContext
