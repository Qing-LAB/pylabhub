# Critical Design Review: Data Exchange Hub (HEP-core-0002)

**Date:** 2026-02-07  
**Reviewer:** AI Assistant + Human Reviewer  
**Document Version:** hep-core-0002-data-hub-structured.md (3866 lines)  
**Scope:** Architecture, API Design, Synchronization, Protocols, Safety, Performance  
**Status:** ✅ 8/18 tasks complete, 3 critical remaining, ready for implementation

---

## EXECUTIVE SUMMARY

### Review Outcome: ✅ STRONG FOUNDATION, NEEDS COMPLETION

**Overall Assessment:**
- **Architecture:** ✅ Excellent (Dual-chain, clear separation, clean abstractions)
- **Synchronization:** ✅ Solid (TOCTTOU resolved, memory ordering correct, race-free)
- **API Design:** ✅ Good (ABI-stable C interface, zero-cost templates, type-safe)
- **Observability:** ✅ Comprehensive (256-byte metrics, automatic error tracking)
- **Broker Design:** ✅ Simple (minimal scope, out of critical path)

**Critical Issues:** 18 identified (6 critical, 6 high, 6 medium)  
**Issues Resolved:** 8 (all Phase 1 blockers + 3 Phase 2 high priority)  
**Issues Remaining:** 10 (3 high priority required for production, 7 optional)

**Production Readiness:** 70% complete (design phase)  
**Confidence Level:** High (90%+ on core architecture and synchronization)

### Key Achievements

**Major Architectural Decisions:**
1. **Dual-Chain Architecture** - Separate FineGrained (flexible zones) from CoarseGrained (fixed buffers)
2. **SlotRWCoordinator** - Three-layer abstraction (C API + C++ wrappers + templates) for TOCTTOU-safe coordination
3. **Minimal Broker** - Pure discovery service (3 messages), no data routing
4. **Peer-to-Peer Heartbeat** - Shared memory slots, zero network overhead
5. **Integrated Metrics** - 256 bytes automatic error tracking, Python/CLI access
6. **Lock Separation** - SharedSpinLock (user) vs SlotRWState (system)
7. **Memory Ordering** - Correct acquire/release throughout
8. **Checksum Simplification** - Manual/Enforced policies only

**Code Estimates:**
- Core infrastructure: ~1,300 lines
- Remaining tasks (P7-P9): ~650 lines
- Total production code: ~1,950 lines

**Timeline Estimate:**
- HEP document updates: ~10 hours
- P7-P9 design completion: ~5 days
- Implementation: ~14 days
- Testing: ~3-5 days
- **Total: 5 weeks to production**

### Recommendation

**✅ PROCEED WITH IMPLEMENTATION** after:
1. Update HEP-core-0002 with completed designs (~10 hours)
2. Complete P7-P9 design (~5 days)
3. Freeze API signatures

**Risk Level:** LOW - Core architecture is solid, remaining work is additive.

---

## QUICK REFERENCE

**Completed Design Documents:**
- `CRITICAL_REVIEW_DataHub_2026-02-07.md` - This document (3,190 lines)
- `DESIGN_STATUS_2026-02-07.md` - Status summary
- `HEP_UPDATE_ACTION_PLAN.md` - Section-by-section update plan
- `OUTSTANDING_TASKS_2026-02-07.md` - Remaining work breakdown
- `CONSOLIDATED_TODO_2026-02-07.md` - Implementation roadmap

**Next Steps:**
1. Review and approve designs
2. Update HEP-core-0002
3. Complete P7-P9 design
4. Begin implementation

---

## REVIEW STATUS DASHBOARD

### Overall Assessment
- **Architecture:** ✅ Sound (Two-tier sync, zero-copy design)
- **Implementation:** ⚠️ **NOT PRODUCTION READY** (Critical gaps identified, now designed)
- **Documentation:** ✅ Excellent (Comprehensive, well-structured)
- **Safety:** ✅ **BLOCKERS RESOLVED** (Race conditions fixed, thread safety ensured)

### Critical Metrics
- **Production Blockers Found:** 6 issues (P1-P5, MessageHub)
- **Production Blockers Resolved:** 6 issues ✅
- **High Priority Issues:** 6 issues (3 resolved, 3 remaining)
- **Estimated Effort to Production:** 3-4 weeks (was 4-6 weeks)

---

## TODO CHECKLIST

### 🔴 Phase 1: Critical Blockers (MUST FIX - 1-2 weeks)

#### **P1: Ring Buffer Policy Enforcement** (CRITICAL) ✅ DESIGN COMPLETE
- [x] **Task 1.1:** Review and document complete ring buffer queue full/empty logic
  - [x] **DECIDED:** Dual-Chain Architecture (Flexible zones + Fixed buffer ring)
  - [x] Specify exact algorithm for queue occupancy check: `(write_index - read_index) >= capacity`
  - [x] Define backpressure mechanism: **Block with exponential backoff (default)**
  - [x] Document timeout behavior: Return error/empty handle on timeout
  - [x] Add state machine diagram for queue states
- [x] **Task 1.2:** Define producer blocking strategy
  - [x] **CHOSEN: Hybrid approach** - Spin briefly (100μs), then exponential backoff
  - [x] Documented in revised Section 13.3
- [x] **Task 1.3:** Define consumer empty-queue behavior
  - [x] **DECIDED:** Block with timeout (consistent with producer)
  - [x] Integration with iterator API: iterator.try_next() handles internally
  - [x] Timeout semantics: Return AcquireError::Timeout or QueueEmpty
- [x] **Task 1.4:** Update API documentation
  - [x] Add preconditions/postconditions to acquire_write_slot()
  - [x] Document error codes: AcquireError enum with 9 error types
  - [x] Add usage examples for each policy

**ARCHITECTURAL DECISION:**
```
DUAL-CHAIN ARCHITECTURE (2026-02-07)
====================================
TABLE 1: Flexible Zone Chain (FineGrained)
  - Multiple flexible zones (0 to N)
  - User provides struct with std::atomic members
  - Access: flexible_zone<T>(index)
  - Coordination: User-managed (atomics/ZMQ/external)
  - Purpose: Metadata, state, telemetry, coordination

TABLE 2: Fixed Buffer Chain (CoarseGrained)
  - Ring buffer of fixed-size slots (1 to M)
  - Access: Iterator or acquire_slot()
  - Coordination: System-managed mutex (single producer)
  - Purpose: Frames, samples, payloads, bulk data
```

**IMPACT ON OTHER TASKS:**
- P2, P3: Ring buffer mutex applies to Table 2 only
- P7: Broker needs to track both chains separately
- P10: Transaction API simplified (two distinct patterns)
- P11: Alignment only matters for Table 2 slots

#### **P2: MessageHub Thread Safety** (CRITICAL) ✅ DESIGN COMPLETE
- [x] **Task 2.1:** Design thread-safety strategy for MessageHub
  - [x] **CHOSEN: Option A - Internal mutex**
  - [x] Mutex scope: Cover entire request/response cycle (simplicity priority)
  - [x] Performance overhead: ~50-100ns (<0.2% of network latency)
  - [x] Documented in Section 16
- [x] **Task 2.2:** Update MessageHub class specification
  - [x] **DECIDED:** Change API contract to "Thread-safe (internal mutex)"
  - [x] Remove "not thread-safe" warning, replace with guarantee
  - [x] Document performance: mutex overhead negligible vs network
  - [x] Add multi-threaded usage examples
- [x] **Task 2.3:** Define concurrent usage patterns
  - [x] Producer + consumer in same process ✓
  - [x] Multiple threads sending notifications ✓
  - [x] Flexible zone updates + notifications ✓

**DESIGN DECISION:**
```
MessageHub Thread Safety (2026-02-07)
=====================================
Solution: Internal std::mutex protecting all socket operations

Implementation:
  class MessageHubImpl {
      zmq::socket_t m_socket;
      mutable std::mutex m_socket_mutex;  // Protects all socket ops
  };

API Contract:
  All MessageHub methods are thread-safe. Concurrent calls from
  multiple threads are supported via internal locking.

Performance:
  - Mutex overhead: 50-100ns
  - Network latency: 10-50μs
  - Impact: <0.2% (negligible)

Rationale:
  - Simple implementation (no per-thread sockets)
  - Transparent to users (no external locking needed)
  - Sufficient for typical usage patterns
  - Can upgrade to per-thread sockets later if needed
```

**CONTEXT FOR DUAL-CHAIN ARCHITECTURE:**
- Producer threads update flexible zones (atomic) + send notifications (mutex-protected)
- Consumer threads iterate ring buffer (mutex-protected) + send heartbeats (mutex-protected)
- Both chains can safely use MessageHub from different threads

#### **P3: Checksum Policy Enforcement** (HIGH) ✅ DESIGN COMPLETE
- [x] **Task 3.1:** Design automatic checksum enforcement
  - [x] **DECIDED:** Compute on release (after commit, before lock release)
  - [x] Specify behavior on failure:
    - Producer: Log error + continue (availability priority)
    - Consumer: Log error + return false (integrity priority)
  - [x] Document performance impact: ~1-2 μs per slot (BLAKE2b 4KB)
- [x] **Task 3.2:** Update ChecksumPolicy specification
  - [x] **SIMPLIFIED:** Two policies - Manual, Enforced (was: Disabled/Explicit/EnforceOnRelease)
  - [x] Add enforcement flow diagrams
  - [x] Document error recovery on checksum mismatch
- [x] **Task 3.3:** Define checksum failure modes
  - [x] Producer: Logs error, continues (non-blocking)
  - [x] Consumer: Returns false, user can retry or skip slot

**DESIGN DECISION:**
```
Checksum Policy Model (2026-02-07)
===================================
Two Policies: Manual, Enforced

Checksum Storage:
- Always present if enable_checksum=true
- Array of 32-byte BLAKE2b hashes (one per slot)
- Toolkit always available regardless of policy

Table 1 (Flexible Zone Chain):
- Checksum: Always Manual (user-coordinated)
- Rationale: Atomic updates can occur during checksum computation
- User must coordinate with write_generation counter or flags

Table 2 (Fixed Buffer Chain):
- Checksum: Manual or Enforced (configurable)
- Rationale: Writer holds SlotRWState.write_lock (per-slot atomic PID lock) → atomically correct checksum
  Note: This is NOT DataBlockMutex (Tier 1 OS mutex). This is the Tier 2 per-slot atomic write lock (std::atomic<uint64_t> write_lock).
  Note: SlotRWState is allocated as a separate array (one per slot), NOT from the control zone's fixed spinlock pool (spinlock_states[8]).

Policy Semantics:
- Manual: User calls update_checksum() / verify_checksum() explicitly
- Enforced: Auto-called in release_write_slot() / release_consume_slot()

Default: Manual (performance by default, opt-in safety)

Failure Handling:
- Producer: Log + continue (avoid blocking ring buffer)
- Consumer: Log + return false (user detects corrupted data)
```

**INTEGRATION WITH DUAL-CHAIN:**
- Flexible zones: No enforced checksums (user atomics provide integrity)
- Fixed buffers: Optional enforced checksums (SlotRWState.write_lock ensures consistency)

**ARCHITECTURAL DECISION: Lock Implementation Separation (2026-02-07)**
```
DECISION: Keep SharedSpinLock and SlotRWState as separate implementations
=========================================================================

Rationale:
1. Different semantics:
   - SharedSpinLock: Exclusive, recursive, timeout, crash recovery
   - SlotRWState: Multi-reader/single-writer, non-recursive, state machine

2. Performance critical:
   - SlotRWState accessed on EVERY read/write (hot path)
   - Wrapper overhead unacceptable (~5-10ns per operation)
   - SharedSpinLock accessed less frequently (flexible zones)

3. API clarity:
   - SharedSpinLock: User-visible (acquire_spinlock API)
   - SlotRWState: Internal only (hidden in slot handles)

4. Memory layout:
   - SharedSpinLock: 32 bytes (fixed pool of 8 in header)
   - SlotRWState: 48 bytes (array scales with ring_buffer_capacity)

Implementation Strategy:
- Keep separate structs (optimal for each use case)
- Share low-level atomic helpers (inline utility functions)
- No base class, no virtual calls, no wrapper overhead
- Independent evolution for each lock type

Memory Allocation:
- SharedSpinLockState: Fixed pool in SharedMemoryHeader.spinlock_states[8]
- SlotRWState: Separate array after header (ring_buffer_capacity × 48 bytes)
- NO allocation from shared spinlock pool for SlotRWState
```

#### **P4: TOCTTOU Race Condition** (MEDIUM-HIGH) ✅ DESIGN COMPLETE
- [x] **Task 4.1:** Analyze race window in reader coordination
  - [x] **IDENTIFIED:** Race between slot_state check and reader_count increment
  - [x] Timeline documented: Writer can change state after reader checks but before reader registers
  - [x] Impact: Reader could read partially overwritten data (silent corruption)
- [x] **Task 4.2:** Design race mitigation strategy
  - [x] **CHOSEN: Atomic coordination with memory fences + double-check**
  - [x] Minimize race window (immediate increment after check)
  - [x] Memory fence (ensure writer sees reader_count update)
  - [x] Double-check (detect if race occurred)
  - [x] Safe abort (undo increment if race detected)
- [x] **Task 4.3:** Update synchronization documentation
  - [x] Complete SlotRWCoordinator abstraction designed
  - [x] C interface for ABI stability
  - [x] Header-only template wrappers for convenience
  - [x] Integrated observability metrics

**ARCHITECTURAL DECISION: SlotRWCoordinator Abstraction (2026-02-07)**
```
TOCTTOU MITIGATION STRATEGY
============================

Problem: Time-Of-Check-To-Time-Of-Use race in reader acquisition
- Reader checks: slot_state == COMMITTED
- [RACE WINDOW]
- Reader increments: reader_count++
- Writer can slip in: sees reader_count==0, starts writing

Solution: Three-Layer Architecture
===================================

Layer 1: C Interface (ABI-Stable, Dynamic Library)
--------------------------------------------------
Functions:
- slot_rw_acquire_write(rw_state, timeout_ms) → SlotAcquireResult
- slot_rw_acquire_read(rw_state, out_generation) → SlotAcquireResult
- slot_rw_commit(rw_state)
- slot_rw_release_write(rw_state)
- slot_rw_release_read(rw_state)
- slot_rw_validate_read(rw_state, generation) → bool
- slot_rw_get_metrics(header, out_metrics)
- slot_rw_reset_metrics(header)

Opaque Structure (64 bytes, cache-aligned):
- SlotRWState { uint8_t _opaque[64]; }

Internal Fields (SlotRWStateInternal):
- std::atomic<uint64_t> write_lock (PID-based)
- std::atomic<uint32_t> reader_count
- std::atomic<uint8_t> slot_state (FREE/WRITING/COMMITTED/DRAINING)
- std::atomic<uint8_t> writer_waiting
- std::atomic<uint64_t> write_generation
- uint8_t padding[36] (64-byte total)

Layer 2: Header-Only Template Wrappers (Zero-Cost Abstraction)
---------------------------------------------------------------
Classes:
- SlotRWAccess::with_write_access(rw_state, buffer, size, lambda)
- SlotRWAccess::with_read_access(rw_state, buffer, size, lambda)
- SlotRWAccess::with_typed_write<T>(rw_state, buffer, size, lambda)
- SlotRWAccess::with_typed_read<T>(rw_state, buffer, size, lambda)

Features:
- RAII guarantee (lock always released)
- Lambda-based (clean syntax)
- Type-safe (compile-time checking)
- Exception-safe (guard cleanup)
- Auto-commit (on lambda success)
- Zero overhead (inline templates)

Layer 3: Integrated Observability (SharedMemoryHeader Metrics)
---------------------------------------------------------------
Counters (8 × uint64_t = 64 bytes):
- writer_timeout_count (overflow detection)
- writer_blocked_total_ns (performance tracking)
- write_lock_contention (bug detection, should be 0)
- write_generation_wraps (activity tracking)
- reader_not_ready_count (underflow detection)
- reader_race_detected (TOCTTOU tracking)
- reader_validation_failed (wrap-around detection)
- reader_peak_count (capacity planning)

API:
- SlotRWMetricsView metrics(header)
- metrics.is_healthy() → bool
- metrics.has_overflow() → bool
- metrics.writer_timeout_rate() → double

TOCTTOU Prevention Mechanism
=============================

Reader Acquisition (slot_rw_acquire_read):
1. Load slot_state (memory_order_acquire)
2. Check: if state != COMMITTED, return NOT_READY
3. Increment reader_count (memory_order_acq_rel) ← Minimize race window
4. Memory fence (memory_order_seq_cst) ← Force writer visibility
5. Re-check slot_state (memory_order_acquire)
6. If state != COMMITTED:
   - Decrement reader_count (undo)
   - Increment reader_race_detected metric
   - Return NOT_READY
7. Capture write_generation (optimistic validation)
8. Return OK

Writer Acquisition (slot_rw_acquire_write):
1. Acquire write_lock (PID-based CAS)
2. Wait for reader_count == 0:
   - Memory fence (memory_order_seq_cst) ← Force reader visibility
   - Load reader_count (memory_order_acquire)
   - If count == 0, break
   - Exponential backoff, check timeout
3. If timeout: increment writer_timeout_count, return TIMEOUT
4. Set slot_state = WRITING (memory_order_release)
5. Memory fence (memory_order_seq_cst)
6. Return OK

Guarantees:
- If reader passes double-check, writer WILL see reader_count > 0
- If writer changes state, reader WILL detect it in double-check
- Race detected → safe abort, metrics updated
- No silent corruption possible

Memory Ordering Proof:
- Reader: acq_rel + seq_cst fence → happens-before writer's acquire
- Writer: seq_cst fence + release → happens-before reader's acquire
- Bidirectional synchronization guaranteed by seq_cst fences

Usage Example:
==============

Producer (Type-Safe):
```cpp
SlotRWAccess::with_typed_write<SensorData>(
    &rw_state[slot_idx], buffer, size,
    [&](SensorData& data) {
        data.timestamp = now();
        data.value = sensor.read();
    },
    timeout_ms
);
// Lock auto-released, data auto-committed
```

Consumer (Type-Safe):
```cpp
SlotRWAccess::with_typed_read<SensorData>(
    &rw_state[slot_idx], buffer, size,
    [&](const SensorData& data) {
        process(data);
    },
    validate_generation=true  // Detect wrap-around
);
// Lock auto-released
```

Monitoring:
```cpp
SlotRWMetricsView metrics(header);
if (metrics.has_overflow()) {
    LOG_WARN("Ring buffer overflow! Timeout rate: {:.2f}%",
             metrics.writer_timeout_rate() * 100);
}
if (metrics.reader_races() > 0) {
    LOG_INFO("TOCTTOU races detected: {} (safely aborted)",
             metrics.reader_races());
}
```

Benefits:
- ✓ TOCTTOU-safe (atomic + fences)
- ✓ ABI-stable (C interface)
- ✓ Convenient (with_* templates)
- ✓ Type-safe (compile-time checks)
- ✓ Observable (integrated metrics)
- ✓ Exception-safe (RAII)
- ✓ Zero-overhead (inline templates)
```

#### **P5: Memory Barriers in Iterator** (MEDIUM) ✅ DESIGN COMPLETE
- [x] **Task 5.1:** Review memory ordering in iterator operations
  - [x] **IDENTIFIED:** `get_commit_index()` must use `memory_order_acquire`
  - [x] `seek_latest()` inherits correct ordering from `get_commit_index()`
  - [x] `seek_to()` has no ordering requirements (local variable only)
  - [x] `try_next()` correct (delegates to `get_commit_index()` and SlotRWCoordinator)
- [x] **Task 5.2:** Update Section 11.3 with memory ordering annotations
  - [x] Document acquire/release synchronization chain
  - [x] Producer: `commit_index.fetch_add(1, release)` ✓ correct
  - [x] Consumer: `commit_index.load(acquire)` ← FIX REQUIRED
  - [x] Cross-reference with Section 12.4 (SlotRWCoordinator)

**DESIGN DECISION: Memory Ordering in Iterator (2026-02-07)**
```
PROBLEM: Missing Memory Ordering in commit_index Access
========================================================

Issue: Consumer iterator reads commit_index without proper memory ordering
- Producer commits with: commit_index.fetch_add(1, memory_order_release)
- Consumer reads with: commit_index.load() ← defaults to seq_cst or relaxed
- Missing acquire on consumer side breaks synchronization

Impact:
- On x86: Rare (strong memory model masks the bug)
- On ARM/RISC-V: CRITICAL (weak memory model allows reordering)
- Result: Consumer may read stale slot data (silent corruption)

Solution: Enforce memory_order_acquire on All Consumer Reads
=============================================================

Fix 1: Update DataBlockConsumer::get_commit_index()
----------------------------------------------------
```cpp
uint64_t DataBlockConsumer::get_commit_index() const {
    // MUST use acquire to synchronize with producer's release
    // Establishes happens-before relationship:
    //   Producer: commit_index.fetch_add(1, release)
    //   Consumer: commit_index.load(acquire)
    // Guarantees all data written before commit is visible after load
    return header->commit_index.load(std::memory_order_acquire);
}
```

Fix 2: Document Iterator Memory Ordering
-----------------------------------------
```cpp
void DataBlockSlotIterator::seek_latest() {
    // Synchronizes with producer via acquire on commit_index
    uint64_t latest = consumer->get_commit_index();  // ← acquire inside
    last_seen_slot_id = latest;
    
    // No additional fence needed - acquire is sufficient
}

void DataBlockSlotIterator::seek_to(uint64_t slot_id) {
    // No synchronization needed - pure local state update
    last_seen_slot_id = slot_id - 1;
}

NextResult DataBlockSlotIterator::try_next(int timeout_ms) {
    // Step 1: Check for new data (acquire on commit_index)
    uint64_t current_commit = consumer->get_commit_index();  // ← acquire
    
    if (last_seen_slot_id >= current_commit) {
        return {Status::NoData, {}};
    }
    
    // Step 2: Acquire slot (SlotRWCoordinator handles memory ordering)
    uint64_t slot_id = last_seen_slot_id + 1;
    auto handle = consumer->acquire_consume_slot(slot_id, timeout_ms);
    
    if (handle) {
        last_seen_slot_id = slot_id;
        return {Status::Success, std::move(handle)};
    }
    
    return {Status::Error, {}};
}
```

Fix 3: Update write_index and read_index Access
------------------------------------------------
All atomic index accesses must use correct ordering:

```cpp
// Producer side (already correct):
header->write_index.fetch_add(1, std::memory_order_acq_rel);  // ✓
header->commit_index.fetch_add(1, std::memory_order_release); // ✓

// Consumer side (needs fixing):
uint64_t DataBlockConsumer::get_write_index() const {
    return header->write_index.load(std::memory_order_acquire);  // FIX
}

uint64_t DataBlockConsumer::get_read_index() const {
    return header->read_index.load(std::memory_order_acquire);  // FIX
}
```

Memory Ordering Cheat Sheet
============================

Producer Commit Path:
1. Write data to slot buffer
2. slot_state = COMMITTED (release) ───┐
3. commit_index++ (release) ───────────┤
                                       │
Consumer Iterator Path:                │ Synchronizes-with
1. commit_index.load(acquire) ←────────┤
2. slot_state.load(acquire) ←──────────┘
3. Read slot buffer (guaranteed visible)

Key Guarantees:
- Producer's release synchronizes-with consumer's acquire
- Happens-before relationship established
- All writes before release visible after acquire
- No additional fences needed (acquire/release sufficient)

Platform Impact:
- x86: seq_cst is overkill (but works), relaxed is broken
- ARM: acquire/release required (relaxed breaks, seq_cst overkill)
- Recommendation: Use acquire/release (portable, efficient)

Performance:
- acquire/release: ~0 ns overhead on x86, ~2-5 ns on ARM
- seq_cst: ~5-10 ns overhead (full memory barrier)
- relaxed: ~0 ns but BREAKS CORRECTNESS on weak memory models

Testing:
- x86 tests may pass even with bugs (strong memory model)
- MUST test on ARM or use ThreadSanitizer (detects races)
- Stress test with high contention (exposes race windows)
```

---

### 🟡 Phase 2: High Priority (Production Hardening - 3-4 weeks)

#### **P6: Broker Integration + Heartbeat Infrastructure** (HIGH) ✅ DESIGN COMPLETE
- [x] **Task 6.1:** Define broker core responsibilities
  - [x] **DECIDED:** Broker = Pure discovery service (minimal scope)
  - [x] Broker handles: Registration, discovery, lifecycle events
  - [x] Broker does NOT handle: Data routing, heartbeat tracking
  - [x] Rationale: Keep broker out of critical path
- [x] **Task 6.2:** Complete minimal control plane protocol
  - [x] Essential messages: REG_REQ, DISC_REQ, DEREG_REQ (3 only)
  - [x] Response format with error codes
  - [x] Discovery with exponential backoff retry
  - [x] Total protocol: ~300 lines of code
- [x] **Task 6.3:** Design peer-to-peer communication patterns
  - [x] Option A: Shared memory only (poll-based)
  - [x] Option B: Shared memory + direct ZeroMQ (push notifications)
  - [x] Option C: Broker-mediated PUB/SUB (not recommended)
  - [x] Provide minimal toolkit (~200 lines) + documentation patterns
- [x] **Task 6.4:** Design heartbeat as peer-to-peer (not broker)
  - [x] **DECIDED:** Heartbeat in shared memory (zero network overhead)
  - [x] Per-consumer heartbeat slots in SharedMemoryHeader
  - [x] Producer checks liveness via atomic reads
  - [x] Optional: User can implement broker-mediated if needed
- [x] **Task 6.5:** Complete error handling and metrics
  - [x] Error codes with timeouts for all operations
  - [x] Integrated metrics (256 bytes in SharedMemoryHeader)
  - [x] Automatic error recording at source
  - [x] Python/script access for monitoring
  - [x] CLI tools for inspection and reset

**ARCHITECTURAL DECISION: Minimal Broker + Peer-to-Peer (2026-02-07)**
```
DESIGN PHILOSOPHY: Broker Out of Critical Path
===============================================

Core Principle:
- Broker = Control plane only (discovery, registry)
- Producer/Consumer = Data plane (shared memory + optional ZeroMQ)
- No broker involvement after discovery

Broker Responsibilities (Minimal)
==================================

MUST Do:
1. Discovery Service
   - Consumer asks: "Where is channel X?"
   - Broker responds: shm_name, schema_hash, optional zmq_endpoint
   
2. Registry Service
   - Track: Which producer owns which channel
   - Track: Which consumers attached where (optional)
   
3. Lifecycle Events (Optional)
   - Producer startup/shutdown
   - Consumer attach/detach

MUST NOT Do:
❌ Route data notifications (that's peer-to-peer)
❌ Be in critical path for data transfer
❌ Manage shared memory access (that's SlotRWCoordinator)
❌ Track heartbeat (that's peer-to-peer in shared memory)

Minimal Protocol (3 Messages)
==============================

REG_REQ (Producer Registration):
```json
{
  "type": "REG_REQ",
  "channel_name": "sensor_temperature",
  "shm_name": "datablock_sensor_12345",
  "schema_hash": "a1b2c3d4...",
  "shared_secret_hash": "e5f6g7h8...",
  "zmq_endpoint": "ipc:///tmp/sensor.ipc"  // Optional for notifications
}
Response: {"status": "OK|ERROR|CONFLICT"}
```

DISC_REQ (Consumer Discovery):
```json
{
  "type": "DISC_REQ",
  "channel_name": "sensor_temperature",
  "shared_secret_hash": "e5f6g7h8..."
}
Response: {
  "status": "OK|NOT_FOUND|AUTH_FAILED",
  "shm_name": "datablock_sensor_12345",
  "schema_hash": "a1b2c3d4...",
  "zmq_endpoint": "ipc:///tmp/sensor.ipc"  // Optional
}
```

DEREG_REQ (Producer Deregistration - Optional):
```json
{
  "type": "DEREG_REQ",
  "channel_name": "sensor_temperature"
}
Response: {"status": "OK"}
```

Peer-to-Peer Communication
===========================

Pattern 1: Shared Memory Only (Simplest)
-----------------------------------------
Consumer polls shared memory (no notifications).
- Zero broker involvement after discovery
- True zero-copy
- Acceptable latency for low-frequency data

Pattern 2: Shared Memory + Direct ZeroMQ (Recommended)
-------------------------------------------------------
Producer sends notifications directly to consumer.
- Broker provides zmq_endpoint in discovery response
- Producer: PUB socket (ipc:///tmp/sensor.ipc)
- Consumer: SUB socket (connects to endpoint)
- Push notifications (low latency)
- Still zero-copy for data (shared memory)

Pattern 3: Broker PUB/SUB (Not Recommended)
--------------------------------------------
Producer sends notifications via broker.
- Adds latency (broker in critical path)
- Only use if direct connection not feasible

Heartbeat: Peer-to-Peer in Shared Memory
=========================================

Design: Per-Consumer Slots in SharedMemoryHeader
-------------------------------------------------
```cpp
struct SharedMemoryHeader {
    // Heartbeat slots (512 bytes for 8 consumers)
    struct ConsumerHeartbeat {
        std::atomic<uint64_t> consumer_id;        // UUID or PID
        std::atomic<uint64_t> last_heartbeat_ns;  // Timestamp
        uint8_t padding[48];  // Cache line alignment
    } consumer_heartbeats[8];  // Max 8 consumers
};
```

Consumer: Update Heartbeat (No Network!)
-----------------------------------------
```cpp
// Periodic thread or manual call
consumer->update_heartbeat();  // Atomic write to shared memory
```

Producer: Check Liveness
-------------------------
```cpp
producer->for_each_consumer([](uint64_t consumer_id, uint64_t last_hb) {
    auto elapsed_ns = get_timestamp() - last_hb;
    if (elapsed_ns > 5'000'000'000) {  // 5 seconds
        LOG_WARN("Consumer {} timed out", consumer_id);
        // Optional: cleanup, alert
    }
});
```

Benefits:
- Zero network overhead (atomic writes in shared memory)
- Lock-free (atomic operations)
- Producer-controlled (no broker dependency)
- Works even if broker down

Messaging Toolkit (Minimal)
============================

Provide building blocks, not framework:

```cpp
namespace pylabhub::messaging {
    // Socket creation helpers (~100 lines)
    zmq::socket_t create_pub_socket(ctx, endpoint);
    zmq::socket_t create_sub_socket(ctx, endpoint, topic);
    
    // Timeout helpers (~50 lines)
    SendResult send_with_timeout(socket, data, size, timeout_ms);
    RecvResult recv_with_timeout(socket, buf, size, out_size, timeout_ms);
    
    // Serialization (~50 lines)
    std::vector<uint8_t> serialize(const T& msg);
    T deserialize(const std::vector<uint8_t>& data);
}

Total: ~200 lines + documentation of common patterns
```

Error Handling Strategy
========================

All blocking operations have timeouts:

```cpp
// Shared memory layer
SlotAcquireResult slot_rw_acquire_write(rw_state, timeout_ms);
// Returns: OK, NOT_READY, TIMEOUT, LOCKED, ERROR

// ZeroMQ layer
SendResult send_with_timeout(socket, data, size, timeout_ms);
// Returns: OK, TIMEOUT, DISCONNECTED, ERROR

RecvResult recv_with_timeout(socket, buf, size, out_size, timeout_ms);
// Returns: OK, TIMEOUT, DISCONNECTED, ERROR

// Broker discovery
DiscoveryResult discover_channel(broker, channel, secret, timeout_ms, retries);
// Returns: OK, NOT_FOUND, AUTH_FAILED, TIMEOUT, ERROR
```

User decides retry policy (no hidden retries in library).

Integrated Metrics Infrastructure
==================================

SharedMemoryHeader Metrics Section (256 bytes):
------------------------------------------------
```cpp
struct SharedMemoryHeader {
    // Slot coordination (64 bytes)
    std::atomic<uint64_t> writer_timeout_count;
    std::atomic<uint64_t> writer_blocked_total_ns;
    std::atomic<uint64_t> write_lock_contention;
    std::atomic<uint64_t> write_generation_wraps;
    std::atomic<uint64_t> reader_not_ready_count;
    std::atomic<uint64_t> reader_race_detected;
    std::atomic<uint64_t> reader_validation_failed;
    std::atomic<uint64_t> reader_peak_count;
    
    // Error tracking (96 bytes)
    std::atomic<uint64_t> last_error_timestamp_ns;
    std::atomic<uint32_t> last_error_code;
    std::atomic<uint32_t> error_sequence;
    std::atomic<uint64_t> slot_acquire_errors;
    std::atomic<uint64_t> slot_commit_errors;
    std::atomic<uint64_t> checksum_failures;
    std::atomic<uint64_t> zmq_send_failures;
    std::atomic<uint64_t> zmq_recv_failures;
    std::atomic<uint64_t> zmq_timeout_count;
    // ... more error counters
    
    // Heartbeat (32 bytes)
    std::atomic<uint64_t> heartbeat_sent_count;
    std::atomic<uint64_t> heartbeat_failed_count;
    std::atomic<uint64_t> last_heartbeat_ns;
    
    // Performance (64 bytes)
    std::atomic<uint64_t> total_slots_written;
    std::atomic<uint64_t> total_slots_read;
    std::atomic<uint64_t> total_bytes_written;
    std::atomic<uint64_t> uptime_seconds;
};
```

C API for Cross-Language Access:
---------------------------------
```c
// Get metrics snapshot
int datablock_get_metrics(const char* shm_name, DataBlockMetrics* out);

// Reset metrics (for testing/maintenance)
int datablock_reset_metrics(const char* shm_name);

// Get error string
const char* datablock_error_string(uint32_t error_code);
```

Python Monitoring:
------------------
```python
from pylabhub_monitor import get_metrics, reset_metrics

# Read metrics
metrics = get_metrics("datablock_sensor_12345")
if metrics.writer_timeout_count > threshold:
    alert("High timeout rate!")

# Reset after maintenance
reset_metrics("datablock_sensor_12345")
```

CLI Tool:
---------
```bash
$ datablock-inspect datablock_sensor_12345

Performance:
  Slots written:  1,234,567
  Slots read:     1,234,560
  
Errors:
  Last error:     SLOT_ACQUIRE_TIMEOUT
  Error sequence: 42
  
Writer:
  Timeouts:       42
  
✓  Healthy
```

Automatic Error Recording:
---------------------------
Errors recorded at source (no manual instrumentation):

```cpp
// Automatically records error on failure
if (timed_out) {
    header->writer_timeout_count.fetch_add(1);
    RECORD_ERROR(header, SLOT_ACQUIRE_TIMEOUT);
    return SLOT_ACQUIRE_TIMEOUT;
}
```

Benefits:
- Comprehensive observability (all failure modes tracked)
- Zero overhead (atomic increments)
- Scriptable (Python/Lua access)
- Runtime reset (no restart needed)
- Production-ready monitoring

Implementation Scope:
- Broker service: ~300 lines
- Messaging toolkit: ~200 lines
- Metrics API: ~100 lines
- Python bindings: ~200 lines
- CLI tool: ~100 lines
- Total: ~900 lines of production code
```

#### **P7: Layer 2 Transaction API** (HIGH) ✅ DESIGN COMPLETE
- [x] **Task 7.1:** Complete Layer 2 API specification
  - [x] **COMPLETED:** Template functions (`with_write_transaction`, `with_read_transaction`, `with_next_slot`)
  - [x] RAII guard classes (WriteTransactionGuard, ReadTransactionGuard)
  - [x] Strong exception safety guarantee (all-or-nothing)
  - [x] Performance: ~10 ns overhead (~9%), negligible
- [x] **Task 7.2:** Add implementation guidelines to Section 10.3
  - [x] Complete implementation with inline templates
  - [x] Move semantics for guards
  - [x] Exception handling with rollback
  - [x] Usage examples (producer, consumer, iterator)
- [x] **Task 7.3:** Document Layer 1 vs Layer 2 trade-offs
  - [x] Performance comparison table
  - [x] When to use each layer
  - [x] Migration guide from Layer 1 to Layer 2

**DESIGN DECISION: Lambda-Based RAII Transactions (2026-02-07)**
```
APPROACH: Template-Based Transaction API
========================================

Core Concept:
- Lambda-based transactions with automatic cleanup
- RAII guards for advanced control flow
- Zero-overhead inline templates
- Strong exception safety (all-or-nothing)

API Layers:
-----------

1. Template Functions (Primary Interface):
```cpp
// Producer write
with_write_transaction(producer, timeout_ms, [&](SlotWriteHandle& slot) {
    write_data(slot);
    // Auto-commit on success, auto-release always
});

// Consumer read
with_read_transaction(consumer, slot_id, timeout_ms, 
    [&](const SlotConsumeHandle& slot) {
        process_data(slot);
        // Auto-release always
    });

// Iterator convenience
with_next_slot(iterator, timeout_ms, [&](const SlotConsumeHandle& slot) {
    process_data(slot);
    // Auto-release always
});
```

2. RAII Guards (Advanced Control Flow):
```cpp
// Write transaction guard
WriteTransactionGuard tx(producer, timeout_ms);
if (!tx) {
    // Acquisition failed
    return;
}

write_data(tx.slot());

if (validation_failed()) {
    tx.abort();  // Do not commit
    return;
}

tx.commit();  // Explicit commit
// Auto-release on scope exit
```

Exception Safety Guarantees:
----------------------------

Producer Write:
1. Lambda throws → slot NOT committed, slot released, exception propagated
2. Lambda succeeds → slot committed, slot released
3. Early return → slot committed (normal path)
4. Destructor is noexcept → release always happens

Consumer Read:
1. Lambda throws → slot released, exception propagated
2. Lambda succeeds → slot released
3. Destructor is noexcept → release always happens

State Transitions:
-----------------

Write Transaction:
BEFORE: Slot in FREE
SUCCESS: Slot in COMMITTED, released
FAILURE: Slot in FREE (rollback), released

Read Transaction:
BEFORE: Slot in COMMITTED
SUCCESS: Slot in COMMITTED (unchanged), released
FAILURE: Slot in COMMITTED (unchanged), released

Performance:
-----------

| Operation | Layer 1 | Layer 2 | Overhead |
|-----------|---------|---------|----------|
| Acquire   | ~50 ns  | ~50 ns  | 0 ns     |
| Lambda    | N/A     | ~10 ns  | ~10 ns   |
| Commit    | ~30 ns  | ~30 ns  | 0 ns     |
| Release   | ~30 ns  | ~30 ns  | 0 ns     |
| **Total** | **110 ns** | **120 ns** | **~10 ns** |

Overhead: ~9% (negligible for most use cases)
Optimization: Full inlining, zero heap allocation

When to Use Layer 1 vs Layer 2:
-------------------------------

Layer 1 (Primitive API):
- Performance-critical (every ns counts)
- Cross-iteration state (hold handle across loops)
- Custom coordination (building abstractions)
- No exceptions (embedded, -fno-exceptions)

Layer 2 (Transaction API):
- Standard application code (recommended)
- Safety first (no resource leaks)
- Data processing (video, audio, sensors)
- Exception handling (C++ with exceptions)

Benefits:
--------

✓ Strong exception safety (all-or-nothing)
✓ Zero overhead (~10 ns, inline templates)
✓ Convenient (lambda-based, minimal boilerplate)
✓ RAII guarantees (impossible to leak resources)
✓ Backward compatible (Layer 1 unchanged)
✓ Type-safe (compile-time checking)
✓ Composable (can nest with care)

Implementation:
--------------

Template-based with std::invoke:
- Inline everything → zero function call overhead
- Move semantics for guards → no copies
- constexpr if → compile-time branching
- No heap allocation → no allocation failures

Testing:
-------

- Unit tests: Exception safety, move semantics
- Integration tests: Producer-consumer combinations
- Stress tests: High-frequency with exceptions
- Benchmark: Verify <10 ns overhead

Estimated Implementation:
------------------------

- Template functions: ~200 lines
- RAII guards: ~100 lines
- Tests: ~400 lines
- Total: ~700 lines, 3-4 days

See: P7_LAYER2_TRANSACTION_API_DESIGN.md for complete specification
```

#### **P8: Error Recovery API** (MEDIUM-HIGH) ✅ DESIGN COMPLETE
- [x] **Task 8.1:** Design error recovery mechanisms
  - [x] **COMPLETED:** Diagnostic functions (diagnose_slot, diagnose_all_slots, find_stuck_slots)
  - [x] Recovery operations (force_reset_slot, release_zombie_readers/writers)
  - [x] Safety constraints (PID liveness check, force flag)
  - [x] Auto-recovery with dry-run mode
- [x] **Task 8.2:** Add ErrorRecovery API to Section 10
  - [x] C API (ABI-stable, cross-language)
  - [x] C++ wrapper classes (convenience)
  - [x] Usage examples for common failures
  - [x] Safety mechanisms and limitations
- [x] **Task 8.3:** Add emergency procedures appendix
  - [x] CLI tools specification (datablock-admin)
  - [x] Manual intervention procedures (3 scenarios)
  - [x] Python bindings for scripting
  - [x] Monitoring integration (Prometheus, auto-recovery daemon)

**DESIGN DECISION: Safe Error Recovery with Diagnostics (2026-02-07)**
```
APPROACH: Diagnostic-First Recovery with Safety Checks
======================================================

Core Concept:
- Always diagnose before recovery
- Check PID liveness (never reset active processes)
- Dry-run mode by default
- Comprehensive logging and auditing

Production Scenarios:
-------------------

1. Stuck Writer:
   - Slot in WRITING, write_lock held by dead PID
   - Recovery: force_reset_slot (after PID check)

2. Zombie Readers:
   - reader_count > 0, but PIDs dead
   - Recovery: release_zombie_readers (per-reader PID check)

3. Corrupted State:
   - Invalid slot_state, inconsistent counters
   - Recovery: validate_integrity + force_reset (destructive)

4. Stale Heartbeat:
   - Consumer heartbeat > 5s old
   - Recovery: cleanup_dead_consumers

CLI Tool: datablock-admin
-------------------------

Commands:
1. diagnose            - Inspect all slots (read-only, safe)
2. force-reset-slot    - Reset single slot (checks PID first)
3. force-reset-all     - Reset all slots (DANGEROUS)
4. release-zombie-*    - Release dead process locks
5. auto-recover        - Automated recovery (dry-run default)
6. validate            - Integrity check
7. cleanup-heartbeats  - Remove dead consumer slots

Examples:
```bash
# Diagnose stuck slots
$ datablock-admin diagnose --shm-name foo
Slot 1: WRITING (stuck 45s) ⚠️ STUCK
  - Write lock: PID 12345 (DEAD)
  - Recommendation: force-reset-slot 1

# Safe recovery (checks PID)
$ datablock-admin force-reset-slot --shm-name foo --slot 1
[INFO] PID 12345 is DEAD, safe to reset
[OK] Slot 1 reset to FREE

# Auto-recover (dry-run first)
$ datablock-admin auto-recover --shm-name foo --dry-run
Would release zombie writer on slot 1 (PID 12345)
Would clear dead consumer (PID 67890)

# Apply fixes
$ datablock-admin auto-recover --shm-name foo
Released zombie writer on slot 1
Cleared dead consumer (PID 67890)
```

Safety Mechanisms:
-----------------

1. PID Liveness Check:
   - Check /proc/<pid> on Linux
   - Check OpenProcess() on Windows
   - Never reset if PID alive

2. Force Flag:
   - Default: false (safe mode)
   - true = override safety (DANGEROUS)
   - Always log when force=true

3. Dry-Run Mode:
   - auto_recover(dry_run=true) reports only
   - User reviews before applying
   - Prevents accidental resets

4. Comprehensive Logging:
   ```cpp
   LOG_WARN("RECOVERY: Resetting slot {}", slot_index);
   LOG_WARN("  Before: state={}, lock={}, readers={}");
   reset_slot_state(...);
   LOG_WARN("  After: state=FREE, lock=0, readers=0");
   LOG_WARN("RECOVERY: Slot {} reset complete", slot_index);
   ```

5. Metrics Tracking:
   - header->recovery_actions (total count)
   - header->last_recovery_timestamp_ns
   - Audit trail for forensics

Python Bindings:
---------------

```python
from pylabhub_admin import (
    diagnose_all_slots,
    auto_recover,
    cleanup_dead_consumers
)

# Diagnose
diagnostics = diagnose_all_slots("datablock_foo")
stuck_slots = [d for d in diagnostics if d.is_stuck]

# Auto-recover (safe)
actions = auto_recover("datablock_foo", dry_run=False)
for action in actions:
    print(f"Performed: {action}")
```

Monitoring Integration:
----------------------

Auto-Recovery Daemon:
```python
def recovery_daemon(shm_names, check_interval=60):
    while True:
        for shm_name in shm_names:
            actions = auto_recover(shm_name, dry_run=False)
            if actions:
                alert(f"Auto-recovery: {actions}")
        time.sleep(check_interval)
```

Prometheus Metrics:
```python
stuck_slots_gauge = Gauge('datablock_stuck_slots', ...)
stuck_slots_gauge.labels(shm_name).set(stuck_count)
```

Emergency Procedures:
--------------------

Procedure 1: Stuck Writer
- Symptom: acquire_write_slot() timeouts
- Diagnosis: datablock-admin diagnose
- Recovery: force-reset-slot (if PID dead)

Procedure 2: Zombie Readers
- Symptom: Writer waits forever
- Diagnosis: Check reader_count > 0
- Recovery: release-zombie-readers (if PIDs dead)

Procedure 3: Corrupted Header
- Symptom: Integrity check fails
- Diagnosis: datablock-admin validate
- Recovery: Delete shm, restart (DESTRUCTIVE)

Benefits:
--------

✓ Safe recovery (PID checks, force flag)
✓ Observable (diagnostics before recovery)
✓ Automated (auto-recover, monitoring)
✓ Scriptable (Python bindings)
✓ Auditable (comprehensive logging)
✓ Production-ready (emergency procedures)

Implementation:
--------------

- C API: ~300 lines (ABI-stable)
- C++ wrappers: ~150 lines
- CLI tool: ~200 lines
- Python bindings: ~100 lines
- Tests: ~200 lines
- Total: ~950 lines, 3-4 days

See: P8_ERROR_RECOVERY_API_DESIGN.md for complete specification
```

#### **P9: Schema Validation** (MEDIUM-HIGH)
- [ ] **Task 9.1:** Complete schema negotiation protocol
  - [ ] Define schema hash computation
  - [ ] Specify broker registry behavior
  - [ ] Document version compatibility rules
- [ ] **Task 9.2:** Update Section 10.1.2
  - [ ] Add schema validation algorithm
  - [ ] Document failure modes
  - [ ] Add migration examples

#### **P10: Observability API** (MEDIUM) ✅ DESIGN COMPLETE
- [x] **Task 10.1:** Design metrics and monitoring API
  - [x] **COMPLETED:** DataBlockMetrics structure (256 bytes in SharedMemoryHeader)
  - [x] Metrics collected: Slot coordination, errors, heartbeat, performance
  - [x] Performance impact: Zero (atomic increments, no locks)
  - [x] Integrated with P4 (SlotRWCoordinator) and P6 (Broker/Heartbeat)
- [x] **Task 10.2:** Add observability section to documentation
  - [x] C API: datablock_get_metrics(), datablock_reset_metrics()
  - [x] Python bindings for scripting
  - [x] CLI tool: datablock-inspect
  - [x] Integration examples with monitoring systems

**NOTE:** Observability was designed as part of P4 (SlotRWCoordinator metrics) and P6 (error tracking).
See P4 and P6 design sections for complete specification.

---

### 🟢 Phase 3: Performance & Scalability (2-3 weeks)

#### **P11: Cache Line Alignment** (MEDIUM)
- [ ] **Task 11.1:** Review SlotRWState alignment
  - [ ] Document current alignment and cache line effects
  - [ ] Specify alignas(64) requirement
  - [ ] Estimate performance impact
- [ ] **Task 11.2:** Update Section 15.3 with alignment requirements
  - [ ] Add memory layout diagrams
  - [ ] Document false sharing mitigation
  - [ ] Add performance benchmarks

#### **P12: Spinlock Capacity** (LOW-MEDIUM)
- [ ] **Task 12.1:** Analyze spinlock usage patterns
  - [ ] Survey common use cases
  - [ ] Determine if 8 is sufficient
  - [ ] Propose new limit (16? 32? dynamic?)
- [ ] **Task 12.2:** Update spinlock specification
  - [ ] Change MAX_SHARED_SPINLOCKS constant
  - [ ] Update Section 12.3
  - [ ] Document allocation strategy

#### **P13: Huge Page Support** (MEDIUM)
- [ ] **Task 13.1:** Design huge page allocation strategy
  - [ ] Define threshold for enabling huge pages
  - [ ] Specify fallback behavior
  - [ ] Document platform-specific APIs
- [ ] **Task 13.2:** Add huge page support to Section 15
  - [ ] Update memory allocation section
  - [ ] Add configuration options
  - [ ] Document performance benefits

#### **P14: Bulk Transfer API** (MEDIUM)
- [ ] **Task 14.1:** Design batch acquisition API
  - [ ] Define acquire_write_slots_batch()
  - [ ] Specify atomicity guarantees
  - [ ] Document failure modes (partial success?)
- [ ] **Task 14.2:** Add bulk API to Section 10
  - [ ] Add API specification
  - [ ] Add usage examples
  - [ ] Document performance characteristics

---

### 🔵 Phase 4: Security & Robustness (1-2 weeks)

#### **P15: Shared Secret Strength** (MEDIUM)
- [ ] **Task 15.1:** Review shared secret security model
  - [ ] Analyze threat model
  - [ ] Determine if 64-bit is acceptable
  - [ ] Propose alternatives (128-bit, HMAC, etc.)
- [ ] **Task 15.2:** Update Section 8 with security analysis
  - [ ] Document security guarantees and limitations
  - [ ] Add threat model
  - [ ] Recommend best practices

#### **P16: Integrity Protection** (MEDIUM)
- [ ] **Task 16.1:** Design mandatory integrity protection
  - [ ] Define when checksums are required
  - [ ] Specify HMAC-based authentication option
  - [ ] Document key management
- [ ] **Task 16.2:** Update Section 9 with integrity model
  - [ ] Add integrity protection options
  - [ ] Document security vs performance trade-offs
  - [ ] Add configuration guidance

---

### 🟣 Phase 5: Documentation Completeness (1 week)

#### **P17: Missing Design Elements**
- [ ] **Task 17.1:** Add versioning strategy (Section 8.x)
  - [ ] Define version compatibility matrix
  - [ ] Specify migration procedures
  - [ ] Document backward/forward compatibility
- [ ] **Task 17.2:** Add disaster recovery procedures (Appendix H)
  - [ ] Emergency shutdown procedures
  - [ ] CLI tools specification
  - [ ] Corruption detection and repair
- [ ] **Task 17.3:** Add performance tuning guide (Section 14.6)
  - [ ] Policy selection decision tree
  - [ ] Capacity sizing calculator
  - [ ] Latency vs throughput trade-off table
- [ ] **Task 17.4:** Complete handover protocol specification (Section 16.3)
  - [ ] Define handover message format
  - [ ] Specify consumer migration steps
  - [ ] Document cleanup procedures

#### **P18: API Completeness**
- [ ] **Task 18.1:** Add timeout configuration API (Section 10.x)
  - [ ] Define DataBlockTimeouts structure
  - [ ] Add set_timeouts() methods
  - [ ] Document default values
- [ ] **Task 18.2:** Review API for missing operations
  - [ ] Audit all use case scenarios
  - [ ] Identify API gaps
  - [ ] Add missing methods

---

## DETAILED FINDINGS

---

## 1. CRITICAL DESIGN DEFECTS (Production Blockers)

### 1.1 P1: Ring Buffer Policy Enforcement Incomplete ⚠️ CRITICAL

**Issue:** Queue full/empty detection logic is **not implemented** in current codebase.

**Evidence from Design Document:**
- Section 18.4.1 explicitly documents this as P1 critical issue
- Design shows queue check: `if (slot_id - read_idx >= ring_buffer_capacity)` but implementation missing

**Location in Design:** Section 18.4.1 (Lines ~2969-3047)

**Implementation Gap:**
```cpp
// Current implementation (data_block.cpp) - NO QUEUE FULL CHECK
SlotWriteHandle acquire_write_slot(int timeout_ms) {
    uint64_t slot_id = write_index.load();
    // ❌ MISSING: Queue full check for RingBuffer policy
    // ❌ MISSING: Backpressure mechanism
    uint32_t slot_index = slot_id % ring_buffer_capacity;
    // ... proceeds without validation
}
```

**Consequences:**
1. **Data Corruption:** Producer overwrites uncommitted slots when queue is full
2. **Lost Data:** Consumer reads uninitialized memory when queue is empty
3. **No Backpressure:** Producer cannot detect full queue, continues writing blindly
4. **Undefined Behavior:** Ring buffer semantics violated

**Required Design Changes:**

Need to specify in Section 13.3 (RingBuffer Policy):

1. **Queue Occupancy Check Algorithm:**
   ```
   occupancy = write_index - read_index
   if (occupancy >= ring_buffer_capacity) {
       // Queue is full
   }
   ```

2. **Backpressure Strategy Options:**
   - **Option A:** Spin-wait with exponential backoff (simple, CPU-intensive)
   - **Option B:** Futex-based blocking (efficient, Linux-only)
   - **Option C:** Hybrid (spin briefly, then block)

3. **Timeout Behavior:**
   - Define what happens when producer blocks beyond timeout
   - Document return value and error codes

4. **Consumer Empty Queue Behavior:**
   - Should consumer block when `read_index == commit_index`?
   - Integration with iterator API

**Risk Level:** **CRITICAL** - RingBuffer policy unusable in production without this fix.

---

### 1.2 P2: MessageHub Not Thread-Safe ⚠️ CRITICAL

**Issue:** ZeroMQ sockets are **NOT thread-safe**, but `MessageHub` lacks internal synchronization.

**Evidence:**
1. **Design Document (Section 18.4.3):** Explicitly identifies this as P3 critical
2. **Implementation (message_hub.cpp:138-195):** No mutex protection on socket operations
3. **Header Documentation (message_hub.hpp:28-29):** Warning mentions "not thread-safe" but expects external sync

**Location in Design:** Section 18.4.3 (Lines ~3072-3131)

**Code Analysis:**
```cpp
// message_hub.cpp - UNSAFE concurrent access
bool MessageHub::send_request(const char *header, ...) {
    // ❌ NO LOCKING HERE
    zmq::send_multipart(pImpl->m_socket, request_parts);  // DATA RACE
    zmq::recv_multipart(pImpl->m_socket, ...);             // DATA RACE
}
```

**Race Condition Scenarios:**
- **Thread A:** Producer calls `send_request()` for registration
- **Thread B:** Consumer calls `send_notification()` for heartbeat
- **Result:** Socket state corruption → crash or hang

**Required Design Changes:**

Need to add to Section 16 (Message Protocols and Coordination):

**Subsection 16.4: MessageHub Thread Safety**

1. **Thread Safety Model:**
   - **Option A:** Internal mutex protection (recommended)
     - Pros: Transparent to users, simple
     - Cons: Contention if high message rate
     - Implementation: `std::mutex m_socket_mutex`
   
   - **Option B:** Per-thread socket instances
     - Pros: No contention
     - Cons: Complex socket lifecycle management
     - Implementation: `thread_local zmq::socket_t`
   
   - **Option C:** Document external synchronization requirement
     - Pros: Zero overhead
     - Cons: Error-prone, users must manage

2. **Performance Impact Analysis:**
   - Mutex overhead: ~50-100ns
   - Network latency: ~10-50μs
   - Conclusion: Mutex overhead negligible (<1%)

3. **API Contract:**
   ```
   All MessageHub methods are thread-safe and may be called
   concurrently from multiple threads. Internal locking ensures
   atomic request/response cycles.
   ```

**Recommendation:** Implement **Option A** (internal mutex)

**Why This Matters:**
- Multi-threaded applications are **common** (e.g., GUI thread + data thread)
- Documenting "not thread-safe" is **insufficient** - users expect basic safety
- Adding mutex has **minimal overhead** compared to network operations

**Risk Level:** **CRITICAL** - Silent data corruption in multi-threaded use.

---

### 1.3 P3: Checksum Policy Not Enforced ⚠️ HIGH

**Issue:** `ChecksumPolicy::EnforceOnRelease` is defined but **not implemented**.

**Location in Design:** Section 18.4.4 (Lines ~3134-3203)

**Expected Behavior:**
```cpp
config.checksum_policy = ChecksumPolicy::EnforceOnRelease;
// ✅ Should AUTO-update checksum on release_write_slot()
// ✅ Should AUTO-verify checksum on release_consume_slot()
```

**Actual Behavior:**
```cpp
// ❌ User must MANUALLY call update_checksum_slot()
producer->update_checksum_slot(slot_index);  // Easy to forget!
```

**Required Design Changes:**

Need to update Section 9.1 (Integrity and Validation):

**Subsection 9.1.3: Checksum Policy Enforcement**

1. **Policy Semantics:**
   
   - **Explicit Policy:**
     - User manually calls `update_checksum_slot()` and `verify_checksum_slot()`
     - No automatic enforcement
     - Use case: Fine-grained control, performance-critical paths
   
   - **EnforceOnRelease Policy:**
     - **Producer:** Auto-updates checksum in `release_write_slot()`
     - **Consumer:** Auto-verifies checksum in `release_consume_slot()`
     - Checksum failure → returns false, logs error
     - Use case: Data integrity critical, automation preferred

2. **Failure Handling:**
   
   **Producer side:**
   ```
   On checksum computation failure:
   - Option A: Log error, continue (warn only)
   - Option B: Log error, return false (fail hard)
   - Recommended: Option A (avoid deadlock)
   ```
   
   **Consumer side:**
   ```
   On checksum verification failure:
   - Option A: Log error, continue (accept bad data)
   - Option B: Log error, return false (reject slot)
   - Recommended: Option B (data integrity critical)
   ```

3. **Performance Impact:**
   - BLAKE2b for 4KB slot: ~1-2 μs
   - Amortized over data transfer: <1% overhead
   - Acceptable for most use cases

**Consequences:**
- **Silent Data Corruption:** User forgets manual checksum → corrupted data undetected
- **API Confusion:** Policy exists but has no effect → misleading documentation

**Risk Level:** **HIGH** - Data integrity compromise.

---

### 1.4 P4: TOCTTOU Race Condition in Reader Coordination ⚠️ MEDIUM-HIGH

**Issue:** Time-of-Check to Time-of-Use race in consumer slot acquisition.

**Location in Design:** Section 12.4.3 (Lines ~2258-2291)

**Design Document Claims (Section 12.4.3, Step 5):**
> "Double-check slot state (TOCTTOU mitigation)"

**Code Flow:**
```cpp
// Step 3: Check state
if (rw_state->slot_state.load() != COMMITTED) return nullptr;  // ✅ Check

// Step 4: Increment reader_count
rw_state->reader_count.fetch_add(1, acquire);  // ⏱️ WINDOW HERE

// Step 5: Double-check state
if (rw_state->slot_state.load() != COMMITTED) {  // ✅ Recheck
    rw_state->reader_count.fetch_sub(1, release);
    return nullptr;
}
```

**Problem Identified:**
Between Step 3 and Step 4, the **producer** could:
1. Wrap around to reuse this slot
2. Change `slot_state` from `COMMITTED` to `WRITING`
3. Start overwriting buffer

**Is the Double-Check Sufficient?**
- ✅ **YES** for detecting state change after reader_count increment
- ❌ **NO** for preventing buffer overwrite during read

**Race Window Analysis:**

```
Timeline:
T0: Consumer checks state = COMMITTED ✓
T1: Producer wraps around, sees reader_count == 0
T2: Producer acquires write_lock
T3: Consumer increments reader_count (too late!)
T4: Producer starts writing (doesn't see reader!)
T5: Consumer starts reading (reads partially overwritten data!)
```

**Root Cause:** No **atomic transaction** combining state check + reader_count increment.

**Required Design Changes:**

Need to add to Section 12.4 (Per-Slot Reader/Writer Coordination):

**Subsection 12.4.5: TOCTTOU Race Mitigation**

1. **Race Condition Analysis:**
   - Document the exact race window
   - Explain why double-check is insufficient
   - Show timeline diagram

2. **Mitigation Options:**

   **Option A: write_generation Checking (Lightweight)**
   ```cpp
   // Before reading
   uint64_t gen_before = rw_state->write_generation.load(acquire);
   
   // Read data
   std::memcpy(buffer, slot_data, size);
   
   // After reading
   uint64_t gen_after = rw_state->write_generation.load(acquire);
   if (gen_before != gen_after) {
       // Data was overwritten during read - ABORT
       return false;
   }
   ```
   - Pros: Low overhead (~10ns per check)
   - Cons: Detection only (not prevention)
   - Use case: Acceptable if rare, can retry

   **Option B: Read-Lock Protocol (Stronger)**
   ```cpp
   // Acquire read-lock (CAS-based)
   if (!try_acquire_read_lock(rw_state, timeout_ms)) {
       return nullptr;
   }
   // Read data (exclusive protection)
   std::memcpy(buffer, slot_data, size);
   // Release read-lock
   release_read_lock(rw_state);
   ```
   - Pros: Prevents race completely
   - Cons: Higher overhead (~50-100ns), adds complexity
   - Use case: Mission-critical data integrity

   **Option C: Copy-on-Read (Safest, Expensive)**
   - Always copy slot data to private buffer under lock
   - Pros: Perfect isolation
   - Cons: Defeats zero-copy design
   - Use case: Not recommended

3. **Recommended Approach:** **Option A** (write_generation checking)
   - Add generation check to `release_consume_slot()`
   - Document that consumers may see `GENERATION_MISMATCH` errors
   - User can retry with iterator

4. **Update Section 12.4.3 Consumer Read Path:**
   - Add generation checking steps
   - Update sequence diagram
   - Document error handling

**Risk Level:** **MEDIUM** - Narrow race window, but leads to corrupted reads. Acceptable with mitigation.

---

### 1.5 P5: Missing Memory Barriers in Iterator Seek Operations ⚠️ MEDIUM

**Issue:** `DataBlockSlotIterator::seek_latest()` and `seek_to()` lack memory ordering.

**Location in Design:** Section 11.3 (Lines ~1828-1877)

**Code from Design (Section 11.3):**
```cpp
void seek_latest() {
    last_seen_slot_id = consumer.get_commit_index();  // ❌ No ordering specified
}

void seek_to(uint64_t slot_id) {
    last_seen_slot_id = slot_id - 1;  // ❌ Just a store
}
```

**Problem:**
- No `memory_order_acquire` on reading `commit_index`
- Subsequent `try_next()` may see stale slot data due to CPU reordering
- Violates happens-before relationship

**Required Design Changes:**

Need to update Section 11.3 (Iterator Path):

**Add Subsection 11.3.2: Memory Ordering in Iterator Operations**

1. **Memory Ordering Requirements:**
   
   ```cpp
   void seek_latest() {
       // MUST use acquire to synchronize with producer's release
       uint64_t latest = header->commit_index.load(std::memory_order_acquire);
       last_seen_slot_id = latest;
       
       // Ensure subsequent slot reads see committed data
       std::atomic_thread_fence(std::memory_order_acquire);
   }
   
   void seek_to(uint64_t slot_id) {
       last_seen_slot_id = slot_id - 1;
       // No fence needed here (local variable only)
   }
   ```

2. **Synchronization Guarantees:**
   - `seek_latest()` synchronizes-with producer's `commit_index.fetch_add(..., release)`
   - Ensures all data written before commit is visible after seek
   - Subsequent `try_next()` sees consistent slot data

3. **Cross-References:**
   - Link to Section 12.2 (Memory Ordering)
   - Reference producer commit path in Section 11.1

**Risk Level:** **MEDIUM** - Rare on x86 (strong memory model), but critical on ARM.

---

## 2. SYNCHRONIZATION AND RACE CONDITION ISSUES

### 2.1 P6: active_consumer_count Stale on Consumer Crash ⚠️ HIGH

**Issue:** When consumer crashes, `active_consumer_count` not decremented.

**Location in Design:** Section 18.4.6 (Lines ~3239-3278)

**Consequence:**
- Shared memory **never unlinked** (producer checks `if (count == 0) unlink()`)
- Minor resource leak until system reboot

**Design Acknowledges (Section 18.4.6):** "Not a critical bug, but poor resource management"

**Required Design Changes:**

Need to update Section 16 (Message Protocols and Coordination):

**Add Subsection 16.5: Consumer Liveness Tracking**

1. **Heartbeat Protocol:**
   ```
   Consumer sends PYLABHUB_HB_REQ every 2 seconds
   Broker tracks last_heartbeat timestamp
   If no heartbeat for 5 seconds → consumer considered dead
   ```

2. **Cleanup Protocol:**
   ```
   On consumer timeout:
   1. Broker broadcasts PYLABHUB_CONS_DROP message
   2. Producer receives notification
   3. Producer decrements active_consumer_count
   4. If count reaches 0, producer unlinks shared memory
   ```

3. **Edge Cases:**
   - Network partition: False positive timeout (acceptable)
   - Slow consumer: Increase heartbeat interval
   - Broker restart: All consumers considered dead (re-registration required)

**Dependencies:** Requires broker integration (P6)

**Risk Level:** **LOW** - Only affects resource cleanup, not correctness. Can defer until broker complete.

---

### 2.2 Missing Slot Transition Validation

**Issue:** No enforcement of valid state machine transitions.

**Design Shows State Machine (Section 12.4.1):**
```
FREE → WRITING → COMMITTED → DRAINING → FREE
```

**Required Design Changes:**

Need to add to Section 12.4.1:

**Subsection 12.4.1.1: State Transition Validation**

1. **Valid Transitions:**
   ```
   FREE → WRITING        (producer acquires slot)
   WRITING → COMMITTED   (producer commits)
   COMMITTED → DRAINING  (producer wants to reuse, readers active)
   COMMITTED → FREE      (producer wraps, no readers)
   DRAINING → FREE       (last reader exits)
   ```

2. **Invalid Transitions (errors to detect):**
   ```
   WRITING → FREE        (producer crashed mid-write)
   DRAINING → WRITING    (attempted reuse while draining)
   COMMITTED → WRITING   (skipped FREE state)
   ```

3. **Validation Strategy:**
   ```cpp
   void transition_slot_state(SlotRWState* rw, SlotState expected, SlotState next) {
       SlotState current = rw->slot_state.load(acquire);
       if (current != expected) {
           LOG_ERROR("Invalid state transition: {} -> {} (expected {})",
                     state_to_string(current),
                     state_to_string(next),
                     state_to_string(expected));
           // Recovery: Force reset to FREE or throw exception
       }
       rw->slot_state.store(next, release);
   }
   ```

4. **Recovery Procedures:**
   - Stuck in WRITING: Force to FREE (data loss acceptable)
   - Stuck in DRAINING: Wait longer or force to FREE
   - Add `force_reset_slot()` to error recovery API

**Risk Level:** **MEDIUM** - Helps detect producer crashes, aids debugging.

---

## 3. PROTOCOL AND COORDINATION DEFECTS

### 3.1 P7: Broker Integration Incomplete ⚠️ HIGH

**Issue:** Control plane is a **stub** - discovery, notifications, heartbeat non-functional.

**Location in Design:** Section 18.4.5 (Lines ~3206-3236)

**Evidence:**
- Design Section 18.4.5: "Broker Integration is Stub (HIGH)"
- MessageHub exists but no broker service
- Manual coordination required (out-of-band config)

**Required Design Changes:**

Need to complete Section 16 (Message Protocols and Coordination):

**Add Section 16.6: Broker Service Architecture**

1. **Broker Components:**
   ```
   Components:
   - Registry: map<channel_name, ProducerMetadata>
   - Consumer Tracker: map<consumer_id, ConsumerMetadata>
   - Heartbeat Monitor: Thread checking timeouts
   - Notification Router: PUB/SUB topics per channel
   ```

2. **Broker State Machine:**
   - Add diagram showing broker lifecycle
   - Document startup, shutdown, crash recovery

3. **Threading Model:**
   - Single-threaded with event loop (simple)
   - Multi-threaded with thread pool (scalable)
   - Choose and document

4. **Persistence:**
   - In-memory only (ephemeral registry)
   - Persistent storage (survive broker restart)
   - Recommendation: Start with in-memory, add persistence later

5. **Protocol Messages (Complete Specification):**
   
   **PYLABHUB_REG_REQ (Producer Registration):**
   ```json
   Request: {
     "channel_name": "sensor_data",
     "shm_name": "datablock_sensor_12345",
     "policy": "Single",
     "unit_block_size": 4096,
     "ring_buffer_capacity": 1,
     "shared_secret_hash": "<base64>",  // Hash, not raw secret
     "schema_hash": "<base64>",
     "schema_version": "2.0"
   }
   
   Response: {
     "status": "OK" | "ERROR",
     "channel_id": 12345,
     "error_message": "..." // if ERROR
   }
   ```
   
   **PYLABHUB_DISC_REQ (Consumer Discovery):**
   ```json
   Request: {
     "channel_name": "sensor_data",
     "client_id": "consumer_ui_001",
     "schema_version": "2.0"  // Optional: for validation
   }
   
   Response: {
     "status": "OK" | "NOT_FOUND" | "SCHEMA_MISMATCH",
     "shm_name": "datablock_sensor_12345",
     "policy": "Single",
     "unit_block_size": 4096,
     "ring_buffer_capacity": 1,
     "shared_secret": "<base64>",  // Encrypted if CurveZMQ
     "schema_hash": "<base64>",
     "schema_version": "2.0"
   }
   ```
   
   **PYLABHUB_HB_REQ (Heartbeat):**
   ```json
   Request: {
     "client_id": "consumer_ui_001",
     "channel_id": 12345
   }
   
   Response: {
     "status": "OK" | "UNKNOWN_CLIENT"
   }
   ```
   
   **PYLABHUB_CONS_DROP (Broker Notification):**
   ```json
   Notification (broadcast): {
     "channel_id": 12345,
     "consumer_id": "consumer_ui_001",
     "reason": "HEARTBEAT_TIMEOUT" | "EXPLICIT_DETACH"
   }
   ```

**Missing Components:**
1. Broker service implementation
2. Protocol messages (`CONS_DROP`, `READY_NOTIFY`)
3. Schema negotiation implementation
4. Integration tests

**Impact on System:**
- **No automatic discovery** → manual config error-prone
- **No heartbeat tracking** → stale consumer count (P6)
- **No crash notifications** → producer unaware of dead consumers

**Risk Level:** **HIGH** - System usable but fragile without control plane.

---

### 3.2 Producer Handover Protocol Missing

**Issue:** "Handover expansion" mentioned but **not specified**.

**Location in Design:** Section 5.1, mentioned briefly in Section 1.3

**Design Claims (Section 5.1):**
> "Expansion uses handover (create new block; switch consumers)"

**Required Design Changes:**

Need to add complete section:

**Section 16.7: Producer Handover Protocol**

1. **Use Case:**
   - Producer needs more capacity (8 slots → 16 slots)
   - Producer wants to change configuration (checksums on → off)
   - Producer recovery after crash

2. **Handover Sequence:**
   
   **Step 1: Producer Creates New Block**
   ```cpp
   auto new_producer = create_datablock_producer(
       hub, "sensor_data_v2",  // New channel name or version
       policy, new_config
   );
   ```
   
   **Step 2: Producer Registers Handover**
   ```json
   PYLABHUB_HANDOVER_NOTIFY {
     "old_channel": "sensor_data",
     "new_channel": "sensor_data_v2",
     "cutover_slot_id": 12345,  // Last slot in old block
     "grace_period_ms": 5000
   }
   ```
   
   **Step 3: Broker Notifies Consumers**
   ```json
   PYLABHUB_HANDOVER_NOTIFY (broadcast to all consumers) {
     "old_shm_name": "datablock_sensor_12345",
     "new_shm_name": "datablock_sensor_67890",
     "cutover_slot_id": 12345,
     "new_shared_secret": "<base64>",
     "grace_period_ms": 5000
   }
   ```
   
   **Step 4: Consumers Migrate**
   ```cpp
   // Consumer receives handover notification
   auto new_consumer = find_datablock_consumer(
       hub, "sensor_data_v2", new_shared_secret
   );
   
   // Finish reading old block up to cutover_slot_id
   while (old_iterator.last_slot_id() < cutover_slot_id) {
       old_iterator.try_next(1000);
   }
   
   // Switch to new block
   auto new_iterator = new_consumer->slot_iterator();
   ```
   
   **Step 5: Producer Cleanup**
   ```cpp
   // Wait for grace period
   std::this_thread::sleep_for(std::chrono::milliseconds(5000));
   
   // Check if all consumers detached
   if (old_producer->get_active_consumer_count() == 0) {
       // Destroy old block (unlink shared memory)
       old_producer.reset();
   }
   ```

3. **Data Continuity Guarantees:**
   - No data loss: All committed slots in old block must be readable
   - Slot ID continuity: New block starts at `cutover_slot_id + 1`
   - Consumer sees monotonic slot IDs across handover

4. **Failure Scenarios:**
   - **Producer crashes during handover:** Old block remains valid, consumers continue
   - **Consumer misses notification:** Continues on old block until it's destroyed (graceful degradation)
   - **Handover timeout:** Producer forces cleanup after extended grace period

5. **Rollback Procedure:**
   - If handover fails (e.g., consumers can't attach new block)
   - Producer sends `HANDOVER_CANCEL` notification
   - Consumers remain on old block

**Critical Scenario Example:**
- Producer has 8 slots but needs 16
- Creates new DataBlock with 16 slots
- ❓ **What happens to in-flight consumers reading old block?**
- Answer: Consumers finish reading old block up to cutover slot, then switch

**Risk Level:** **HIGH** - Expansion impossible without this specification.

---

## 4. API DESIGN AND ABSTRACTION GAPS

### 4.1 P8: Missing Error Recovery API ⚠️ MEDIUM-HIGH

**Issue:** No public API for error recovery scenarios.

**Common Failure Modes:**
1. Producer crash during write → slot stuck in `WRITING` state
2. Consumer crash holding spinlock → deadlock
3. Checksum mismatch → how to skip bad slot?

**Required Design Changes:**

Need to add new section:

**Section 10.5: Error Recovery API (Expert Mode)**

1. **Recovery Operations:**
   
   ```cpp
   class DataBlockProducer {
   public:
       // Force reset slot to FREE state (DANGEROUS - use with extreme caution)
       // Preconditions:
       // - All consumers must be detached or aware of reset
       // - Caller must hold management mutex
       bool force_reset_slot(uint32_t slot_index);
       
       // Detect and recover from producer crash
       // Scans all slots, resets any stuck in WRITING state
       // Returns count of recovered slots
       uint32_t recover_stuck_slots();
       
       // Check slot state health
       SlotState query_slot_state(uint32_t slot_index) const;
   };
   
   class DataBlockConsumer {
   public:
       // Skip to next valid slot after corruption
       // Returns false if no valid slot found within timeout
       bool skip_to_next_valid_slot(DataBlockSlotIterator& iter, int timeout_ms);
       
       // Attempt to recover from corrupted slot
       // Options:
       // - Skip slot (data loss)
       // - Wait for producer to rewrite (may block indefinitely)
       enum class CorruptionRecovery {
           Skip,       // Skip this slot, move to next
           WaitRetry,  // Wait and retry verification
           Abort       // Throw exception
       };
       void set_corruption_recovery_policy(CorruptionRecovery policy);
   };
   ```

2. **Emergency CLI Tools Specification:**
   
   ```bash
   # Force cleanup (dangerous)
   datablock-admin cleanup --shm-name datablock_sensor_12345 --force
   
   # Validate integrity
   datablock-admin validate --shm-name datablock_sensor_12345
   
   # Reset stuck slot
   datablock-admin reset-slot --shm-name datablock_sensor_12345 --slot 3
   
   # List all DataBlocks
   datablock-admin list
   
   # Show detailed status
   datablock-admin status --shm-name datablock_sensor_12345
   ```

3. **Safety Constraints:**
   - All recovery operations are **dangerous**
   - Must be performed with all consumers detached or paused
   - Document preconditions clearly
   - Add `--force` flag to confirm dangerous operations

4. **Recovery Procedures:**
   
   **Scenario A: Producer Crashed Mid-Write**
   ```
   Detection: Slot stuck in WRITING state for > 5 seconds
   Recovery:
   1. Stop producer process
   2. Run: datablock-admin reset-slot --slot N
   3. Restart producer
   4. Producer skips that slot ID, continues from next
   ```
   
   **Scenario B: Checksum Mismatch**
   ```
   Detection: verify_checksum_slot() returns false
   Recovery:
   1. Consumer logs error with slot ID
   2. Consumer skips slot (calls skip_to_next_valid_slot())
   3. Continue iteration
   Data loss: 1 slot
   ```
   
   **Scenario C: Deadlock (All Spinlocks Held)**
   ```
   Detection: All acquire operations timeout
   Recovery:
   1. Identify dead process PIDs (liveness check)
   2. Run: datablock-admin force-unlock-all
   3. Restart affected processes
   ```

**User Impact Before Fix:**
- Crash → restart entire application (no graceful recovery)
- Corrupted slot → iterates forever (no way to skip)

**Risk Level:** **MEDIUM-HIGH** - Critical for production resilience.

---

### 4.2 P9: Incomplete Schema Validation ⚠️ MEDIUM-HIGH

**Issue:** Schema negotiation described but **not enforced** in factory functions.

**Location in Design:** Section 10.1.2 (Lines ~1039-1100)

**Design Shows (Section 10.1.2):**
```cpp
auto consumer = find_datablock_consumer(hub, "sensor", secret, expected_config);
//                                                              ^^^^^^^^^^^^^^^ Optional param
```

**Implementation Status:**
- Overload exists in header (line 428)
- Schema hash checking **not implemented**
- Broker registry **not functional**

**Required Design Changes:**

Need to complete Section 10.1.2:

**Add Subsection 10.1.2.1: Schema Validation Algorithm**

1. **Schema Hash Computation:**
   
   ```cpp
   // Producer computes schema hash
   struct SensorData {
       uint64_t timestamp_ns;
       float temperature;
       float pressure;
   };
   
   // Hash input: struct definition + field names + sizes
   std::string schema_definition = R"(
       struct SensorData {
           uint64_t timestamp_ns;  // 8 bytes, offset 0
           float temperature;      // 4 bytes, offset 8
           float pressure;         // 4 bytes, offset 12
       }
   )";
   
   uint8_t schema_hash[32];
   crypto_generichash(schema_hash, 32, 
                      reinterpret_cast<const uint8_t*>(schema_definition.data()),
                      schema_definition.size(),
                      nullptr, 0);
   ```

2. **Validation at Attach Time:**
   
   ```cpp
   std::unique_ptr<DataBlockConsumer> find_datablock_consumer(
       MessageHub& hub,
       const std::string& channel_name,
       uint64_t shared_secret,
       const DataBlockConfig& expected_config  // Optional
   ) {
       // 1. Discovery via broker
       auto metadata = hub.discover_channel(channel_name);
       
       // 2. Validate shared secret
       if (metadata.shared_secret_hash != hash(shared_secret)) {
           throw std::runtime_error("Shared secret mismatch");
       }
       
       // 3. Validate schema (if expected_config provided)
       if (expected_config.schema_hash != nullptr) {
           if (std::memcmp(metadata.schema_hash,
                          expected_config.schema_hash, 32) != 0) {
               throw std::runtime_error("Schema mismatch: "
                   "producer and consumer have incompatible data structures");
           }
       }
       
       // 4. Validate configuration
       if (metadata.unit_block_size != expected_config.unit_block_size) {
           throw std::runtime_error("Unit block size mismatch");
       }
       
       // 5. Attach to shared memory
       auto consumer = std::make_unique<DataBlockConsumer>(metadata.shm_name);
       
       // 6. Send ATTACH_ACK to broker
       hub.send_attach_ack(channel_name, consumer->get_consumer_id());
       
       return consumer;
   }
   ```

3. **Version Compatibility Rules:**
   
   ```
   Compatible changes (allow attach):
   - Adding optional fields at end of struct (with size check)
   - Increasing flexible zone size
   - Changing checksum policy (producer's choice)
   
   Incompatible changes (reject attach):
   - Changing field types or order
   - Removing fields
   - Changing unit block size
   - Changing ring buffer capacity (unless RingBuffer policy)
   ```

4. **Broker Enforcement:**
   - Broker stores schema hash on registration
   - Broker rejects duplicate registrations with different schemas
   - Broker returns schema hash on discovery
   - Consumer validates before attach

**Consequence:**
- Producer and consumer use **incompatible schemas** → silent data corruption
- No version checking → old consumer reads new format → crash

**Risk Level:** **MEDIUM-HIGH** - Data integrity compromise.

---

### 4.3 P10: Transaction API (Layer 2) Not Implemented ⚠️ HIGH

**Issue:** Design documents Layer 2 API but **not implemented**.

**Location in Design:** Section 10.3 (Lines ~1404-1511)

**Design Shows (Section 10.3):**
```cpp
with_write_transaction(*producer, 100, [&](SlotWriteHandle& slot) {
    // Exception-safe writes
});
```

**Status:** Not in codebase (Section 18.2: "Not Started")

**Required Design Changes:**

Need to complete Section 10.3:

**Add Subsection 10.3.6: Complete API Specification**

1. **All Transaction Functions:**
   
   ```cpp
   namespace pylabhub::hub::transaction {
   
   // Producer write transaction
   template <typename Func>
   auto with_write_transaction(
       DataBlockProducer& producer,
       int timeout_ms,
       Func&& func
   ) -> std::invoke_result_t<Func, SlotWriteHandle&>;
   
   // Consumer read transaction
   template <typename Func>
   auto with_consume_transaction(
       DataBlockConsumer& consumer,
       uint64_t slot_id,
       int timeout_ms,
       Func&& func
   ) -> std::invoke_result_t<Func, const SlotConsumeHandle&>;
   
   // Iterator-based consumer transaction
   template <typename Func>
   auto with_next_transaction(
       DataBlockSlotIterator& iterator,
       int timeout_ms,
       Func&& func
   ) -> std::invoke_result_t<Func, const SlotConsumeHandle&>;
   
   // Flexible zone transaction (with spinlock)
   template <typename Func>
   auto with_flexible_zone_transaction(
       DataBlockProducer& producer,
       size_t spinlock_index,
       Func&& func
   ) -> std::invoke_result_t<Func, std::span<std::byte>>;
   
   } // namespace transaction
   ```

2. **Exception Safety Guarantees:**
   
   ```
   Strong exception guarantee:
   - If func() throws, slot is released
   - No resource leaks
   - DataBlock state remains consistent
   
   Basic exception guarantee:
   - If acquire fails, returns false/nullptr
   - Slot not acquired, no cleanup needed
   ```

3. **Implementation Guidelines:**
   
   ```cpp
   // Header-only implementation (in transaction.hpp)
   template <typename Func>
   auto with_write_transaction(
       DataBlockProducer& producer,
       int timeout_ms,
       Func&& func
   ) -> std::invoke_result_t<Func, SlotWriteHandle&>
   {
       auto slot = producer.acquire_write_slot(timeout_ms);
       if (!slot) {
           throw std::runtime_error("Failed to acquire write slot");
       }
       
       // RAII guard ensures release on scope exit
       struct SlotGuard {
           DataBlockProducer& prod;
           SlotWriteHandle& handle;
           ~SlotGuard() {
               prod.release_write_slot(handle);
           }
       } guard{producer, *slot};
       
       // Invoke user function
       return std::invoke(std::forward<Func>(func), *slot);
   }
   ```

4. **Usage Examples:**
   
   ```cpp
   // Example 1: Simple write
   transaction::with_write_transaction(*producer, 100,
       [&](SlotWriteHandle& slot) {
           auto buffer = slot.buffer_span();
           std::memcpy(buffer.data(), &data, sizeof(data));
           slot.commit(sizeof(data));
       });
   
   // Example 2: Conditional write
   bool written = false;
   try {
       transaction::with_write_transaction(*producer, 100,
           [&](SlotWriteHandle& slot) {
               if (validate_data(data)) {
                   slot.write(&data, sizeof(data));
                   slot.commit(sizeof(data));
                   written = true;
               }
           });
   } catch (const std::runtime_error& e) {
       LOG_ERROR("Write failed: {}", e.what());
   }
   
   // Example 3: Iterator-based read
   transaction::with_next_transaction(iterator, 1000,
       [&](const SlotConsumeHandle& slot) {
           auto buffer = slot.buffer_span();
           process_data(buffer);
       });
   ```

5. **Performance Impact:**
   - Lambda invocation overhead: ~10-20 ns
   - RAII guard overhead: ~5-10 ns
   - Total overhead: ~15-30 ns (negligible compared to slot acquisition)

**Impact:**
- Users stuck with Layer 1 (manual cleanup) → resource leak risk
- No exception safety → crashes leave spinlocks held

**Recommendation:** **Prioritize Layer 2 implementation** (1 week effort) before production release.

**Risk Level:** **HIGH** - Usability and safety issue.

---

### 4.4 P11: Missing Observability/Metrics API ⚠️ MEDIUM

**Issue:** No way to query system health or performance metrics.

**User Questions:**
- How full is the ring buffer?
- What is current write/read throughput?
- Are there any stuck slots?
- How many consumers are active? (can query but no liveness info)

**Required Design Changes:**

Need to add new section:

**Section 10.6: Observability and Metrics API**

1. **Metrics Structure:**
   
   ```cpp
   struct DataBlockMetrics {
       // Operational counters
       uint64_t total_writes_attempted;
       uint64_t total_writes_committed;
       uint64_t total_writes_failed;
       
       uint64_t total_reads_attempted;
       uint64_t total_reads_completed;
       uint64_t total_reads_failed;
       
       // Queue state (RingBuffer only)
       uint64_t queue_occupancy;        // write_index - read_index
       uint64_t queue_capacity;         // ring_buffer_capacity
       float queue_utilization_percent; // (occupancy / capacity) * 100
       
       // Consumer tracking
       uint32_t active_consumer_count;
       uint32_t peak_consumer_count;
       
       // Error tracking
       uint64_t checksum_errors;
       uint64_t timeout_errors;
       uint64_t state_transition_errors;
       
       // Performance (rolling average over last N operations)
       std::chrono::microseconds avg_write_latency_us;
       std::chrono::microseconds p99_write_latency_us;
       std::chrono::microseconds avg_read_latency_us;
       std::chrono::microseconds p99_read_latency_us;
       
       // Throughput (ops per second)
       double write_ops_per_sec;
       double read_ops_per_sec;
       uint64_t bytes_written_per_sec;
       uint64_t bytes_read_per_sec;
       
       // Timestamps
       std::chrono::system_clock::time_point last_write_time;
       std::chrono::system_clock::time_point last_read_time;
       std::chrono::system_clock::time_point metrics_collection_time;
   };
   ```

2. **API Methods:**
   
   ```cpp
   class DataBlockProducer {
   public:
       // Get current metrics snapshot
       DataBlockMetrics get_metrics() const;
       
       // Reset performance counters (preserves operational state)
       void reset_metrics();
       
       // Enable/disable metrics collection (default: enabled)
       void set_metrics_enabled(bool enabled);
   };
   
   class DataBlockConsumer {
   public:
       // Get metrics from consumer perspective
       DataBlockMetrics get_metrics() const;
       
       // Consumer-specific metrics
       uint64_t get_slots_consumed() const;
       uint64_t get_bytes_consumed() const;
   };
   ```

3. **Metrics Collection Strategy:**
   
   - **Lightweight counters:** Atomic increment on critical path (~1-2 ns overhead)
   - **Latency tracking:** Sample every Nth operation (configurable, default N=1000)
   - **Throughput calculation:** Rolling window (last 60 seconds)
   - **Memory overhead:** ~512 bytes per DataBlock

4. **Usage Examples:**
   
   ```cpp
   // Monitoring loop
   while (running) {
       auto metrics = producer->get_metrics();
       
       // Check queue health
       if (metrics.queue_utilization_percent > 90.0) {
           LOG_WARN("Queue almost full: {}%", metrics.queue_utilization_percent);
       }
       
       // Check error rates
       if (metrics.checksum_errors > 100) {
           LOG_ERROR("High checksum error rate: {} errors", metrics.checksum_errors);
       }
       
       // Report throughput
       std::cout << "Write throughput: " << metrics.write_ops_per_sec << " ops/sec\n";
       std::cout << "Bandwidth: " << metrics.bytes_written_per_sec / 1e6 << " MB/sec\n";
       
       std::this_thread::sleep_for(std::chrono::seconds(1));
   }
   ```

5. **Integration with Monitoring Systems:**
   
   ```cpp
   // Prometheus exporter example
   void export_to_prometheus(const DataBlockMetrics& metrics) {
       prometheus::Gauge queue_occupancy;
       queue_occupancy.Set(metrics.queue_occupancy);
       
       prometheus::Counter total_writes;
       total_writes.Increment(metrics.total_writes_committed);
   }
   ```

**Risk Level:** **MEDIUM** - Important for production operations and debugging.

---

## 5. PERFORMANCE AND SCALABILITY CONCERNS

### 5.1 P12: Spinlock Allocation Limited to 8 ⚠️ LOW-MEDIUM

**Issue:** Only 8 shared spinlocks available (`MAX_SHARED_SPINLOCKS = 8`).

**Location in Design:** Section 12.3, SharedMemoryHeader definition (line 137 in data_block.hpp)

**Design Rationale (Section 12.1):**
- Spinlocks protect flexible zone and user coordination

**Problem Scenarios:**
1. **Multi-sensor fusion** (Section 3.7): 4 sensors = 4 spinlocks → only 4 remaining for app
2. **Complex coordination:** User needs per-stream locks → exhausts quota quickly

**Required Design Changes:**

Need to update Section 12.3:

**Add Subsection 12.3.5: Spinlock Capacity Planning**

1. **Current Limitation Analysis:**
   - 8 spinlocks = 8 × 16 bytes = 128 bytes overhead
   - Reasonable for most use cases
   - Insufficient for complex multi-channel coordination

2. **Proposed Solutions:**
   
   **Option A: Increase Static Limit (Simple)**
   ```cpp
   static constexpr size_t MAX_SHARED_SPINLOCKS = 32;  // Was 8
   ```
   - Pros: Simple, backward compatible (if version bumped)
   - Cons: Fixed limit, wastes memory if unused
   - Memory overhead: 32 × 16 = 512 bytes (acceptable)
   
   **Option B: Configurable Limit (Flexible)**
   ```cpp
   struct DataBlockConfig {
       size_t max_spinlocks = 8;  // Default 8, can increase
   };
   ```
   - Pros: User controls memory vs functionality trade-off
   - Cons: Version compatibility issues, variable header size
   
   **Option C: Dynamic Allocation (Complex)**
   - Allocate spinlocks on-demand from separate region
   - Pros: No fixed limit
   - Cons: Complex implementation, slower allocation

3. **Recommendation:** **Option A** (increase to 32)
   - Most use cases need < 32 spinlocks
   - Memory overhead negligible (512 bytes vs MB of data)
   - Simple to implement (change constant + bump version)

4. **Update SharedMemoryHeader:**
   ```cpp
   static constexpr size_t MAX_SHARED_SPINLOCKS = 32;  // Increased from 8
   SharedSpinLockState shared_spinlocks[MAX_SHARED_SPINLOCKS];
   std::atomic_flag spinlock_allocated[MAX_SHARED_SPINLOCKS];
   ```

5. **Usage Guidance:**
   - Reserve spinlocks 0-7 for system use (flexible zone, etc.)
   - Spinlocks 8-31 available for user coordination
   - Document spinlock allocation strategy in Section 10

**Risk Level:** **LOW** - Annoyance, not blocker. Easy fix.

---

### 5.2 P13: Cache Line Contention in Multi-Reader ⚠️ MEDIUM

**Issue:** `SlotRWState` is 48 bytes (Section 15.3), **not aligned** to 64-byte cache line.

**Location in Design:** Section 15.3 (Lines ~2789-2814), Section 14.2.2 (Lines ~2442-2455)

**Consequence:**
- Multiple `SlotRWState` structs share same cache line
- Reader on slot N causes **false sharing** for slot N+1

**Design Shows (Section 14.2.2):**
> "~8-10 readers before cache line bouncing dominates"

**Measurement:**
- 8 readers: 7.0x scaling
- 16 readers: 10.0x scaling (should be 16x if no contention)

**Required Design Changes:**

Need to update Section 15.3:

**Add Subsection 15.3.1: Cache Line Alignment**

1. **Problem Analysis:**
   
   ```
   Cache line: 64 bytes
   SlotRWState: 48 bytes
   
   Layout without alignment:
   [Slot 0: 48B][Slot 1: 16B|48B split][Slot 2: 32B|48B split]...
                        ^
                        Cache line boundary - FALSE SHARING
   
   Result: Slot 0 reader evicts Slot 1 cache line
   ```

2. **Solution:**
   
   ```cpp
   struct alignas(64) SlotRWState {  // ✅ Force 64-byte alignment
       std::atomic<uint64_t> write_lock;        // 8 bytes
       std::atomic<uint32_t> reader_count;      // 4 bytes
       std::atomic<uint64_t> write_generation;  // 8 bytes
       std::atomic<uint8_t> slot_state;         // 1 byte
       std::atomic<uint8_t> writer_waiting;     // 1 byte
       uint8_t padding[42];                     // 42 bytes padding → 64 total
   };
   ```

3. **Memory Overhead:**
   - Before: 48 bytes/slot × N slots
   - After: 64 bytes/slot × N slots
   - Overhead: 16 bytes/slot (33% increase)
   - Example: 16-slot ring = 256 bytes extra (negligible)

4. **Performance Impact:**
   - Expected improvement: 8 readers: 7.0x → 7.8x
   - Expected improvement: 16 readers: 10.0x → 14.0x
   - Cost: 33% more memory in per-slot region

5. **Implementation:**
   - Update `SlotRWState` definition in Section 15.3
   - Add `alignas(64)` specifier
   - Adjust padding to 42 bytes
   - Bump DataBlock version to 5

**Risk Level:** **MEDIUM** - Significant performance impact for multi-reader use cases.

---

### 5.3 P14: No Huge Page Support ⚠️ MEDIUM

**Issue:** Large DataBlocks (e.g., 4GB) use default 4KB pages → TLB thrashing.

**Performance Impact:**
- **Without huge pages:** 1M TLB entries needed for 4GB (thrashes TLB)
- **With huge pages:** Only 2K entries (2MB pages)

**Required Design Changes:**

Need to add new subsection:

**Section 15.5: Huge Page Support**

1. **When to Use Huge Pages:**
   
   ```
   Threshold: Enable if structured_buffer_size > 64 MB
   
   Benefits:
   - Reduced TLB misses (3-5x improvement for large buffers)
   - Lower page table overhead
   - Better memory locality
   
   Drawbacks:
   - Requires system configuration (may fail)
   - Increased memory fragmentation risk
   - Not portable (platform-specific)
   ```

2. **Implementation Strategy:**
   
   **Linux (mmap with MAP_HUGETLB):**
   ```cpp
   #ifdef __linux__
   if (m_size > 64 * 1024 * 1024) {  // 64 MB threshold
       m_mapped_address = mmap(
           nullptr, m_size,
           PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_HUGETLB,  // Request huge pages
           m_shm_fd, 0
       );
       
       if (m_mapped_address == MAP_FAILED) {
           // Fallback to regular pages
           LOG_WARN("Huge page allocation failed, using regular pages");
           m_mapped_address = mmap(
               nullptr, m_size,
               PROT_READ | PROT_WRITE,
               MAP_SHARED,  // No MAP_HUGETLB
               m_shm_fd, 0
           );
       }
   }
   #endif
   ```
   
   **Windows (Large Pages):**
   ```cpp
   #ifdef _WIN32
   LARGE_INTEGER li;
   li.QuadPart = m_size;
   
   m_shm_handle = CreateFileMappingW(
       INVALID_HANDLE_VALUE,
       nullptr,
       PAGE_READWRITE | SEC_LARGE_PAGES,  // Request large pages
       li.HighPart,
       li.LowPart,
       name
   );
   
   if (!m_shm_handle) {
       // Fallback to regular pages
       m_shm_handle = CreateFileMappingW(..., PAGE_READWRITE, ...);
   }
   #endif
   ```

3. **Configuration:**
   
   ```cpp
   struct DataBlockConfig {
       bool enable_huge_pages = true;   // Auto-enable if available
       size_t huge_page_threshold = 64 * 1024 * 1024;  // 64 MB default
   };
   ```

4. **System Requirements:**
   
   **Linux:**
   ```bash
   # Check available huge pages
   cat /proc/meminfo | grep Huge
   
   # Reserve huge pages (requires root)
   echo 1024 > /proc/sys/vm/nr_hugepages  # 1024 × 2MB = 2GB
   ```
   
   **Windows:**
   - Requires `SeLockMemoryPrivilege`
   - Must be granted via Local Security Policy

5. **Fallback Behavior:**
   - If huge page allocation fails → fallback to regular pages
   - Log warning but continue operation
   - User can check metrics to see if huge pages are in use

**Recommendation:** Implement with auto-detection and fallback.

**Risk Level:** **MEDIUM** - Important for large buffer performance, but has fallback.

---

### 5.4 P15: No Bulk Transfer API ⚠️ MEDIUM

**Issue:** Users must loop over `acquire_*_slot()` for batch operations.

**Inefficiency:**
```cpp
// Current: Acquire one slot at a time
for (int i = 0; i < 1000; ++i) {
    auto slot = producer->acquire_write_slot(100);  // 1000 atomic ops
    slot->write(&data[i], sizeof(data[i]));
    slot->commit(sizeof(data[i]));
}
```

**Required Design Changes:**

Need to add to Section 10:

**Subsection 10.7: Bulk Transfer API**

1. **API Design:**
   
   ```cpp
   class DataBlockProducer {
   public:
       // Acquire multiple slots at once
       std::vector<SlotWriteHandle> acquire_write_slots_batch(
           size_t count,
           int timeout_ms = 0
       );
       
       // Release multiple slots at once
       bool release_write_slots_batch(
           std::vector<SlotWriteHandle>& handles
       );
   };
   
   class DataBlockConsumer {
   public:
       // Consume multiple slots at once
       std::vector<SlotConsumeHandle> acquire_consume_slots_batch(
           size_t count,
           int timeout_ms = 0
       );
       
       // Release multiple slots at once
       bool release_consume_slots_batch(
           std::vector<SlotConsumeHandle>& handles
       );
   };
   ```

2. **Atomicity Guarantees:**
   
   ```
   acquire_write_slots_batch(N) guarantees:
   - All N slots are consecutive (slot_id, slot_id+1, ..., slot_id+N-1)
   - All-or-nothing: Either all N slots acquired, or none
   - Atomic advance of write_index by N
   
   Failure modes:
   - Queue full (only M < N slots available) → returns empty vector
   - Timeout waiting for N slots → returns empty vector
   ```

3. **Performance Benefit:**
   
   ```cpp
   // Before (1000 atomic ops):
   for (int i = 0; i < 1000; ++i) {
       acquire_write_slot(100);  // Atomic fetch_add each time
   }
   
   // After (1 atomic op):
   auto slots = acquire_write_slots_batch(1000, 100);  // Single fetch_add(1000)
   for (auto& slot : slots) {
       slot.write(...);
   }
   ```
   
   **Speedup:** ~10-50x for batch acquisition (depending on contention)

4. **Usage Example:**
   
   ```cpp
   // Batch write
   std::vector<SensorReading> readings = acquire_sensor_data_batch(1000);
   
   auto slots = producer->acquire_write_slots_batch(readings.size(), 5000);
   if (slots.empty()) {
       LOG_ERROR("Failed to acquire {} slots", readings.size());
       return;
   }
   
   for (size_t i = 0; i < slots.size(); ++i) {
       slots[i].write(&readings[i], sizeof(readings[i]));
       slots[i].commit(sizeof(readings[i]));
   }
   
   producer->release_write_slots_batch(slots);
   ```

5. **Implementation Notes:**
   - Single atomic operation: `write_index.fetch_add(count, release)`
   - Check queue space before acquisition
   - Return empty vector on failure (don't throw)

**Risk Level:** **MEDIUM** - Performance improvement for batch workloads.

---

## 6. SECURITY AND VALIDATION GAPS

### 6.1 P16: Shared Secret is 64-bit (Weak) ⚠️ MEDIUM

**Issue:** Design uses 64-bit shared secret (Section 8.2).

**Location in Design:** Section 8.2 (Lines ~934-943), Section 8.3 (Lines ~945-950)

**Security Analysis:**
- 64 bits = 2^64 ≈ 1.8×10^19 combinations
- **Brute force:** ~10^9 attempts/sec → 210 days to exhaust keyspace
- **PID guessing:** Attacker observes system PIDs, guesses secret

**Design Admits (Section 8.2):**
> "Not strong cryptographic protection"

**Problem:**
- Marketed as "access control" but provides **weak** protection
- Users may assume it's secure → false sense of security

**Required Design Changes:**

Need to update Section 8:

**Add Subsection 8.4: Security Analysis and Threat Model**

1. **Threat Model:**
   
   ```
   Threats addressed:
   - Accidental attachment (wrong process)
   - Casual snooping (another user's process)
   
   Threats NOT addressed:
   - Determined attacker with system access
   - Privilege escalation attacks
   - Memory dumps (secret visible in RAM)
   - Side-channel attacks
   ```

2. **Security Levels:**
   
   **Level 1: 64-bit Shared Secret (Current)**
   - Protection: Accidental attachment only
   - Strength: Weak against brute force (~210 days)
   - Use case: Non-sensitive data, trusted environment
   
   **Level 2: 128-bit Shared Secret (Recommended)**
   - Protection: Strong against brute force
   - Strength: Computationally infeasible (2^128 = 10^38 combinations)
   - Use case: Sensitive data, untrusted environment
   - Implementation: Change `uint64_t` → `uint8_t[16]`
   
   **Level 3: HMAC-Based Authentication (Maximum)**
   - Protection: Strong integrity + authentication
   - Strength: Cryptographically secure
   - Use case: Critical infrastructure, hostile environment
   - Implementation: HMAC each slot + header

3. **Recommended Changes:**
   
   **Short-term (Quick Fix):**
   ```cpp
   struct SharedMemoryHeader {
       uint8_t shared_secret[16];  // 128-bit secret (was uint64_t)
       // ... rest of header
   };
   ```
   
   **Long-term (Comprehensive Security):**
   - Add HMAC-based slot authentication
   - Integrate with CurveZMQ encryption (control plane)
   - Add key rotation support via handover protocol

4. **Documentation Updates:**
   - Clarify security limitations in Section 8
   - Add warning about weak 64-bit secret
   - Recommend 128-bit for production
   - Document threat model and use cases

**Risk Level:** **MEDIUM** - Security issue, but requires system access to exploit.

---

### 6.2 P17: No Integrity Protection Between Producer and Consumer ⚠️ MEDIUM

**Issue:** BLAKE2b checksums are **optional** and **can be disabled**.

**Attack Scenario:**
1. Attacker guesses shared secret (see P16)
2. Attaches as malicious consumer
3. **Modifies data in shared memory** while producer writes
4. No integrity protection → corruption undetected (if checksums disabled)

**Design Assumption (Section 2.3):**
> "Consumers are trusted to follow API"

**Problem:** Assumption violated if secret is compromised.

**Required Design Changes:**

Need to update Section 9:

**Add Subsection 9.3: Integrity Protection Models**

1. **Integrity Options:**
   
   **Option A: No Checksums (Fastest)**
   - Trust: Assumes trusted consumers
   - Detection: None
   - Use case: Internal apps, high-performance requirements
   
   **Option B: BLAKE2b Checksums (Current)**
   - Trust: Detects accidental corruption
   - Detection: Post-facto (after read)
   - Use case: General production use
   - Overhead: ~1-2 μs per slot
   
   **Option C: HMAC Authentication (Strongest)**
   - Trust: Cryptographic integrity + authentication
   - Detection: Prevents tampering
   - Use case: Security-critical applications
   - Overhead: ~2-5 μs per slot
   - Implementation: HMAC-SHA256 with shared key

2. **HMAC Implementation Sketch:**
   
   ```cpp
   struct DataBlockConfig {
       bool enable_hmac = false;  // Opt-in for security-critical apps
       uint8_t hmac_key[32];      // Derived from shared_secret
   };
   
   // Producer: Compute HMAC on commit
   void commit_slot(SlotWriteHandle& slot) {
       uint8_t hmac[32];
       crypto_auth(hmac, slot.buffer_span().data(), slot.size(), config.hmac_key);
       
       // Store HMAC in slot checksum region
       std::memcpy(slot_checksum_ptr, hmac, 32);
       
       slot_state.store(COMMITTED, release);
   }
   
   // Consumer: Verify HMAC on acquire
   bool acquire_consume_slot(uint64_t slot_id) {
       // ... existing acquire logic ...
       
       if (config.enable_hmac) {
           uint8_t expected_hmac[32];
           crypto_auth(expected_hmac, slot.buffer_span().data(), slot.size(), config.hmac_key);
           
           uint8_t stored_hmac[32];
           std::memcpy(stored_hmac, slot_checksum_ptr, 32);
           
           if (crypto_verify_32(expected_hmac, stored_hmac) != 0) {
               LOG_ERROR("HMAC verification failed - slot tampered!");
               return false;
           }
       }
       
       return true;
   }
   ```

3. **Security vs Performance Trade-off:**
   
   | Mode | Overhead | Security | Use Case |
   |------|----------|----------|----------|
   | No Checksums | 0 ns | None | Trusted environment, max perf |
   | BLAKE2b | 1-2 μs | Detects corruption | General production |
   | HMAC | 2-5 μs | Prevents tampering | Security-critical |

4. **Recommendation:**
   - Make checksums **mandatory by default** (change default to `enable_checksum = true`)
   - Add HMAC option for security-critical deployments
   - Document security implications of disabling checksums

**Risk Level:** **MEDIUM** - Security issue, requires compromised secret + write access.

---

## 7. DOCUMENTATION COMPLETENESS

### 7.1 P18: Missing Versioning Strategy ⚠️ MEDIUM

**Issue:** Header has `version` field but **no migration path** documented.

**Location in Design:** Brief mention in Appendix D (FAQ Q4, lines ~3539-3551)

**Questions:**
- How to upgrade from version 4 to version 5?
- Can old consumers read new producer data?
- Is version negotiation supported?

**Required Design Changes:**

Need to add new section:

**Section 8.5: Versioning and Migration Strategy**

1. **Version Compatibility Matrix:**
   
   | Producer Version | Consumer Version | Compatible? | Notes |
   |------------------|------------------|-------------|-------|
   | 4 | 4 | ✅ Yes | Exact match |
   | 5 | 4 | ⚠️ Conditional | If no breaking changes |
   | 4 | 5 | ✅ Yes | Consumer can read older format |
   | 5 | 5 | ✅ Yes | Exact match |

2. **Version Evolution Rules:**
   
   **Backward Compatible Changes (Minor Version Bump):**
   - Adding new fields at end of header
   - Adding new optional features (new spinlocks, counters)
   - Increasing MAX_SHARED_SPINLOCKS
   - Adding new DataBlockPolicy enum values
   
   **Breaking Changes (Major Version Bump):**
   - Changing field sizes or offsets
   - Removing fields
   - Changing memory layout
   - Incompatible synchronization changes

3. **Migration Procedures:**
   
   **Scenario A: Non-Breaking Upgrade (v4 → v4.1)**
   ```
   1. Producer upgrades to v4.1 (backward compatible)
   2. Consumers can continue using v4 client library
   3. Consumers gradually upgrade to v4.1 when convenient
   ```
   
   **Scenario B: Breaking Upgrade (v4 → v5)**
   ```
   1. Producer creates new DataBlock with v5 format
   2. Producer initiates handover protocol (Section 16.7)
   3. Consumers receive HANDOVER_NOTIFY
   4. Consumers upgrade client library to v5
   5. Consumers attach to new v5 DataBlock
   6. Old v4 DataBlock destroyed after grace period
   ```

4. **Version Negotiation:**
   
   ```cpp
   // Consumer attach validation
   if (header->version > DATABLOCK_VERSION_SUPPORTED) {
       throw std::runtime_error(
           "DataBlock version too new: got " + std::to_string(header->version) +
           ", max supported " + std::to_string(DATABLOCK_VERSION_SUPPORTED) +
           ". Upgrade client library."
       );
   }
   
   if (header->version < DATABLOCK_VERSION_MIN_SUPPORTED) {
       throw std::runtime_error(
           "DataBlock version too old: got " + std::to_string(header->version) +
           ", min supported " + std::to_string(DATABLOCK_VERSION_MIN_SUPPORTED) +
           ". Upgrade producer."
       );
   }
   ```

5. **Compatibility Constants:**
   
   ```cpp
   static constexpr uint32_t DATABLOCK_VERSION = 4;              // Current version
   static constexpr uint32_t DATABLOCK_VERSION_MIN_SUPPORTED = 4; // Oldest we can read
   static constexpr uint32_t DATABLOCK_VERSION_SUPPORTED = 4;     // Newest we can read
   ```

**Risk Level:** **MEDIUM** - Important for long-term maintainability.

---

### 7.2 P19: No Disaster Recovery Plan ⚠️ LOW-MEDIUM

**Issue:** What happens when everything goes wrong?

**Required Design Changes:**

Need to add new appendix:

**Appendix H: Emergency Procedures and Disaster Recovery**

1. **Emergency Scenarios:**
   
   **Scenario A: Corrupted SharedMemoryHeader**
   - **Detection:** Magic number wrong, init_state invalid
   - **Recovery:**
     ```bash
     # Force cleanup (destroys all data)
     datablock-admin force-unlink --shm-name datablock_sensor_12345
     
     # Producer recreates DataBlock
     # Consumers reattach
     ```
   - **Data loss:** All uncommitted data lost
   
   **Scenario B: Deadlock (All Spinlocks Held by Dead Processes)**
   - **Detection:** All acquire operations timeout
   - **Recovery:**
     ```bash
     # Identify dead PIDs
     datablock-admin list-locks --shm-name datablock_sensor_12345
     
     # Force unlock all
     datablock-admin force-unlock-all --shm-name datablock_sensor_12345
     
     # Restart affected processes
     ```
   - **Data loss:** In-flight writes lost
   
   **Scenario C: /dev/shm Full (Linux)**
   - **Detection:** `shm_open()` fails with `ENOSPC`
   - **Recovery:**
     ```bash
     # List all shared memory segments
     ls -lh /dev/shm
     
     # Cleanup stale segments
     find /dev/shm -name "datablock_*" -mtime +1 -delete
     
     # Or increase /dev/shm size
     mount -o remount,size=2G /dev/shm
     ```
   
   **Scenario D: Consumer Stuck Reading (Never Releases Slot)**
   - **Detection:** `reader_count` non-zero for >5 minutes
   - **Recovery:**
     ```bash
     # Identify stuck consumer
     datablock-admin show-readers --shm-name datablock_sensor_12345
     
     # Kill stuck consumer process
     kill -9 <PID>
     
     # PID liveness check will detect dead consumer
     # reader_count automatically cleaned up
     ```

2. **CLI Tools Specification:**
   
   ```bash
   # General status
   datablock-admin status --shm-name <name>
     # Shows: version, consumer count, queue state, slot states
   
   # List all DataBlocks
   datablock-admin list
     # Shows: name, size, producer PID, consumer count
   
   # Force cleanup (DANGEROUS)
   datablock-admin force-unlink --shm-name <name> --force
     # Requires --force flag for safety
   
   # Validate integrity
   datablock-admin validate --shm-name <name>
     # Checks: magic number, checksums, state transitions
   
   # Reset stuck slot
   datablock-admin reset-slot --shm-name <name> --slot <N>
     # Forces slot to FREE state
   
   # Show locks
   datablock-admin list-locks --shm-name <name>
     # Shows: spinlock owners, reader counts, write locks
   
   # Force unlock
   datablock-admin force-unlock-all --shm-name <name> --force
     # Clears all locks (DANGEROUS)
   
   # Show readers
   datablock-admin show-readers --shm-name <name> --slot <N>
     # Shows: PIDs holding reader_count
   ```

3. **Manual Intervention Procedures:**
   
   **Procedure: Complete System Reset**
   ```bash
   # 1. Stop all producers and consumers
   killall producer_app consumer_app
   
   # 2. Force cleanup all DataBlocks
   datablock-admin list | awk '{print $1}' | xargs -I {} \
       datablock-admin force-unlink --shm-name {} --force
   
   # 3. Restart broker (if used)
   systemctl restart pylabhub-broker
   
   # 4. Restart applications
   systemctl restart producer_app
   systemctl restart consumer_app
   ```

4. **Disaster Recovery Checklist:**
   
   ```
   [ ] Stop all producer/consumer processes
   [ ] Backup /dev/shm contents (if forensics needed)
   [ ] Run datablock-admin validate on all segments
   [ ] Force cleanup corrupted segments
   [ ] Restart broker service
   [ ] Restart producer applications
   [ ] Restart consumer applications
   [ ] Verify system health with metrics
   [ ] Monitor logs for recurring errors
   ```

**Risk Level:** **LOW-MEDIUM** - Not urgent, but critical for production operations.

---

### 7.3 P20: No Performance Tuning Guide ⚠️ LOW

**Issue:** Section 14.4 has optimization guidelines but **no decision tree**.

**User Question:** "Should I use Single, DoubleBuffer, or RingBuffer?"

**Required Design Changes:**

Need to add new subsection:

**Section 14.7: Performance Tuning Decision Tree**

1. **Policy Selection Flowchart:**
   
   ```
   START: What are your requirements?
   
   Q1: Do you need every data sample?
       YES → Go to Q2
       NO → Use Single Policy (latest-value semantics)
   
   Q2: Is producer faster than consumer?
       YES → Go to Q3
       NO → Use DoubleBuffer (stable read)
   
   Q3: How much can you buffer?
       < 100 samples → Use DoubleBuffer
       > 100 samples → Use RingBuffer
   
   Q4: What happens on queue full?
       Producer blocks → RingBuffer with backpressure
       Drop oldest → RingBuffer with overwrite (future feature)
       Producer fails → RingBuffer with error return
   ```

2. **Capacity Sizing Calculator:**
   
   ```
   Formula:
   capacity = max(
       (producer_rate / consumer_rate) × safety_factor,
       burst_size
   )
   
   Examples:
   
   - Producer: 1000 FPS, Consumer: 30 FPS
     capacity = (1000 / 30) × 1.5 = 50 slots
   
   - Producer: burst 10K samples in 1s, Consumer: steady 100/s
     capacity = 10000 / 100 = 100 slots
   
   - Producer: variable rate, Consumer: real-time
     capacity = P99_latency_ms × producer_rate + 50% margin
   ```

3. **Latency vs Throughput Trade-off Table:**
   
   | Configuration | Latency (p50) | Throughput | Use Case |
   |---------------|---------------|------------|----------|
   | Single, 64B, no checksum | 200 ns | 10M msgs/sec | Control loops |
   | Single, 4KB, no checksum | 500 ns | 2M msgs/sec | Sensor streams |
   | DoubleBuffer, 4MB, checksum | 12 ms | 30 FPS | Video frames |
   | RingBuffer, 4KB, checksum | 5 μs | 500K msgs/sec | Data logging |
   | RingBuffer, 1KB, no checksum | 800 ns | 2M msgs/sec | High-freq logging |

4. **Optimization Checklist:**
   
   ```
   For minimum latency:
   [ ] Use Single policy
   [ ] Small slots (64-4096 bytes)
   [ ] Disable checksums
   [ ] Disable MessageHub notifications (use polling)
   [ ] Pin threads to dedicated cores
   [ ] Set real-time priority (SCHED_FIFO)
   
   For maximum throughput:
   [ ] Use large slots (4MB+)
   [ ] RingBuffer with capacity 64-256
   [ ] Batch processing on consumer
   [ ] Disable checksums (if acceptable)
   [ ] Enable huge pages (for > 64MB buffers)
   
   For maximum reliability:
   [ ] Enable checksums
   [ ] Use EnforceOnRelease policy
   [ ] Enable broker heartbeats
   [ ] RingBuffer with backpressure
   [ ] Monitor metrics for errors
   ```

**Risk Level:** **LOW** - Quality of life improvement, not critical.

---

## NEXT STEPS

### Immediate Actions (Before Implementation)

1. **Review this document with stakeholders**
   - Confirm priorities (P1-P3 must be fixed)
   - Agree on solutions for critical issues
   - Allocate resources (4-6 weeks estimated)

2. **Create detailed task breakdown**
   - Break each P1-P3 task into sub-tasks
   - Assign owners
   - Set milestones

3. **Design revisions (this document)**
   - Work through TODO checklist
   - Update design document with agreed solutions
   - Add missing sections (broker, handover, recovery, etc.)

4. **Implementation planning**
   - After design is complete and reviewed
   - Create implementation tasks
   - Set up testing infrastructure

### Workflow for Each Task

For each task in the checklist:

1. **Review** - Understand the problem
2. **Propose** - Present multiple solution options
3. **Agree** - Choose best approach with stakeholder
4. **Design** - Update documentation with chosen solution
5. **Validate** - Review design change
6. **Defer implementation** - Wait until all design complete

---

## CONCLUSION

The Data Exchange Hub design demonstrates **strong architectural foundations** with its two-tier synchronization model and zero-copy approach. However, the implementation has **critical gaps** that prevent production deployment:

### Critical Blockers (P1-P3)
- ❌ Ring buffer logic incomplete
- ❌ MessageHub not thread-safe
- ❌ Checksum policy not enforced
- ❌ TOCTTOU race condition
- ❌ Memory barriers missing

### High Priority (P4-P10)
- ⚠️ Broker integration incomplete
- ⚠️ Layer 2 API missing
- ⚠️ Error recovery mechanisms missing
- ⚠️ Schema validation incomplete
- ⚠️ Observability API missing

### Estimated Effort
- **Phase 1 (Critical):** 1-2 weeks
- **Phase 2 (Hardening):** 3-4 weeks
- **Phase 3 (Performance):** 2-3 weeks
- **Total:** 4-6 weeks to production readiness

### Recommendation

**DO NOT deploy to production** until:
1. All P1-P3 issues resolved
2. Broker integration complete (P7)
3. Layer 2 Transaction API implemented (P10)
4. Comprehensive integration tests pass
5. Performance benchmarks meet targets

The design is salvageable with focused effort on completing the specification before implementation.

---

**End of Review Document**
