# Data Exchange Hub - Design Status Summary
**Date:** 2026-02-07  
**Status:** Phase 1 Complete, Phase 2 In Progress  
**Document:** Based on HEP-core-0002 Critical Review

---

## DESIGN COMPLETION STATUS

### ✅ Phase 1: Critical Blockers (COMPLETE)

| Task | Status | Key Decisions |
|------|--------|---------------|
| **P1: Ring Buffer Policy** | ✅ Complete | Dual-Chain Architecture (Flexible + Fixed) |
| **P2: MessageHub Thread Safety** | ✅ Complete | Internal mutex, thread-safe by default |
| **P3: Checksum Policy** | ✅ Complete | Manual/Enforced policies, Fixed buffers only |
| **P4: TOCTTOU Race** | ✅ Complete | SlotRWCoordinator with memory fences |
| **P5: Memory Barriers** | ✅ Complete | memory_order_acquire on commit_index reads |

### ✅ Phase 2: High Priority (COMPLETE)

| Task | Status | Key Decisions |
|------|--------|---------------|
| **P6: Broker Integration** | ✅ Complete | Minimal discovery service (3 messages) |
| **P2.5: Heartbeat** | ✅ Complete | Peer-to-peer in shared memory |
| **P10: Observability** | ✅ Complete | 256-byte metrics in header, Python/CLI access |

### 🟡 Phase 2: High Priority (REMAINING)

| Task | Status | Estimated Effort |
|------|--------|------------------|
| **P7: Layer 2 Transaction API** | Not Started | ~300 lines, 2-3 days |
| **P8: Error Recovery API** | Not Started | ~200 lines, 2 days |
| **P9: Schema Validation** | Not Started | ~150 lines, 1-2 days |

### 🟢 Phase 3: Performance & Scalability (OPTIONAL)

| Task | Status | Priority |
|------|--------|----------|
| **P11: Cache Line Alignment** | Not Started | Medium |
| **P12: Spinlock Capacity** | Not Started | Low-Medium |
| **P13: Huge Page Support** | Not Started | Medium |
| **P14: Bulk Transfer API** | Not Started | Medium |

### 🔵 Phase 4: Security & Documentation (OPTIONAL)

| Task | Status | Priority |
|------|--------|----------|
| **P15: Shared Secret Strength** | Not Started | Medium |
| **P16: Integrity Protection** | Not Started | Medium |
| **P17: Missing Design Elements** | Not Started | Medium |
| **P18: API Completeness** | Not Started | Medium |

---

## KEY ARCHITECTURAL DECISIONS

### 1. Dual-Chain Memory Architecture (P1)

```
┌─────────────────────────────────────────────┐
│ SharedMemoryHeader                          │
├─────────────────────────────────────────────┤
│ Per-Slot RW State Array                     │  (N × 64 bytes)
├─────────────────────────────────────────────┤
│ Slot Checksum Array (optional)              │  (N × 33 bytes)
├─────────────────────────────────────────────┤
│ TABLE 1: Flexible Zone Chain                │
│  - FineGrained (user-managed atomics)       │
│  - Multiple zones, variable sizes           │
│  - No system-managed locks                  │
├─────────────────────────────────────────────┤
│ TABLE 2: Fixed Buffer Chain                 │
│  - CoarseGrained (system-managed locks)     │
│  - Ring buffer with SlotRWCoordinator       │
│  - Automatic coordination                   │
└─────────────────────────────────────────────┘
```

**Benefits:**
- Clear separation of user vs. system control
- Flexible zones for metadata/schema
- Fixed buffers for bulk data streaming

---

### 2. SlotRWCoordinator Abstraction (P4)

**Three-Layer Architecture:**

```
Layer 3: Header-Only Templates (User-Friendly)
├─ SlotRWAccess::with_write_access(lambda)
├─ SlotRWAccess::with_read_access(lambda)
└─ SlotRWAccess::with_typed_write<T>(lambda)

Layer 2: C++ Wrappers (Optional)
├─ SlotWriteGuard (RAII)
└─ SlotReadGuard (RAII)

Layer 1: C Interface (ABI-Stable)
├─ slot_rw_acquire_write(timeout_ms)
├─ slot_rw_acquire_read(out_generation)
├─ slot_rw_commit()
└─ slot_rw_release_*()
```

**Key Features:**
- ✅ TOCTTOU-safe (atomic + memory fences)
- ✅ ABI-stable (C interface, opaque types)
- ✅ Zero-overhead (inline templates)
- ✅ Exception-safe (RAII guards)
- ✅ Type-safe (compile-time checks)

---

### 3. Minimal Broker Design (P6)

**Scope:** Pure discovery service (no data routing)

**Protocol (3 messages only):**
1. `REG_REQ` - Producer registers channel
2. `DISC_REQ` - Consumer discovers channel
3. `DEREG_REQ` - Producer deregisters (optional)

**Response:**
```json
{
  "status": "OK",
  "shm_name": "datablock_sensor_12345",
  "schema_hash": "a1b2c3d4...",
  "zmq_endpoint": "ipc:///tmp/sensor.ipc"  // Optional
}
```

**After discovery:** All communication is peer-to-peer (broker not involved).

---

### 4. Peer-to-Peer Heartbeat (P2.5)

**Design:** Heartbeat slots in shared memory (zero network overhead)

```cpp
struct SharedMemoryHeader {
    struct ConsumerHeartbeat {
        std::atomic<uint64_t> consumer_id;
        std::atomic<uint64_t> last_heartbeat_ns;
        uint8_t padding[48];  // Cache line
    } consumer_heartbeats[8];  // Max 8 consumers
};
```

**Consumer:**
```cpp
consumer->update_heartbeat();  // Atomic write, ~10 ns
```

**Producer:**
```cpp
producer->for_each_consumer([](uint64_t id, uint64_t last_hb) {
    if (elapsed(last_hb) > 5s) {
        LOG_WARN("Consumer {} timed out", id);
    }
});
```

**Benefits:**
- Zero network overhead
- Lock-free (atomic operations)
- Works even if broker down
- Producer-controlled

---

### 5. Integrated Observability (P10)

**Metrics in SharedMemoryHeader (256 bytes):**

```cpp
// Slot coordination metrics
std::atomic<uint64_t> writer_timeout_count;
std::atomic<uint64_t> writer_blocked_total_ns;
std::atomic<uint64_t> reader_race_detected;
std::atomic<uint64_t> reader_peak_count;

// Error tracking
std::atomic<uint64_t> last_error_timestamp_ns;
std::atomic<uint32_t> last_error_code;
std::atomic<uint32_t> error_sequence;
std::atomic<uint64_t> slot_acquire_errors;
std::atomic<uint64_t> checksum_failures;
std::atomic<uint64_t> zmq_send_failures;

// Heartbeat tracking
std::atomic<uint64_t> heartbeat_sent_count;
std::atomic<uint64_t> heartbeat_failed_count;

// Performance
std::atomic<uint64_t> total_slots_written;
std::atomic<uint64_t> total_slots_read;
std::atomic<uint64_t> uptime_seconds;
```

**Access:**
- **C API:** `datablock_get_metrics(shm_name, &metrics)`
- **Python:** `metrics = get_metrics("datablock_sensor_12345")`
- **CLI:** `datablock-inspect datablock_sensor_12345`

**Automatic recording:** Errors recorded at source (no manual instrumentation).

---

### 6. Checksum Policy (P3)

**Two policies:**
- **Manual:** User calls `update_checksum()` / `verify_checksum()` explicitly
- **Enforced:** Automatic on `release_write_slot()` / `release_consume_slot()`

**Scope:**
- **Flexible zones:** Always Manual (user coordinates with atomics)
- **Fixed buffers:** Manual or Enforced (SlotRWState.write_lock ensures atomicity)

**Default:** Manual (performance first, opt-in safety)

---

### 7. Lock Implementation Separation (P3)

**Two types of locks:**

| Type | Location | Purpose | Size |
|------|----------|---------|------|
| **SharedSpinLock** | Header.spinlock_states[8] | User coordination (flexible zones) | 32 bytes × 8 |
| **SlotRWState** | Separate array | System coordination (slot lifecycle) | 64 bytes × N |

**Rationale:**
- Different semantics (exclusive vs. multi-reader)
- Performance critical (SlotRWState: hot path)
- API clarity (SharedSpinLock: user-visible, SlotRWState: internal)

**Implementation:** Separate structs, shared atomic helpers (inline utilities)

---

## REMAINING WORK

### Critical for Production

| Task | Why Critical | Effort |
|------|--------------|--------|
| **P7: Layer 2 Transaction API** | Exception safety, RAII convenience | 2-3 days |
| **P8: Error Recovery API** | Production debugging, stuck slot recovery | 2 days |
| **P9: Schema Validation** | Prevent ABI mismatches | 1-2 days |

**Total:** 5-7 days to production-ready.

### Optional Enhancements

- **P11-P14:** Performance optimizations (cache alignment, huge pages, bulk API)
- **P15-P16:** Security hardening (stronger secrets, HMAC integrity)
- **P17-P18:** Documentation completeness (disaster recovery, API gaps)

**Can defer** until after initial production deployment.

---

## IMPLEMENTATION SCOPE ESTIMATE

### Core Infrastructure (Must Implement)

| Component | Lines of Code | Effort |
|-----------|---------------|--------|
| SlotRWCoordinator (C API) | ~500 | 3 days |
| SlotRWAccess (Templates) | ~200 | 1 day |
| Broker Service | ~300 | 2 days |
| Messaging Toolkit | ~200 | 1 day |
| Metrics API | ~100 | 1 day |
| Python Bindings | ~200 | 1 day |
| CLI Tools | ~100 | 0.5 days |
| **Total** | **~1,600** | **9.5 days** |

### Remaining Tasks (P7-P9)

| Component | Lines of Code | Effort |
|-----------|---------------|--------|
| Transaction API (Layer 2) | ~300 | 2 days |
| Error Recovery API | ~200 | 2 days |
| Schema Validation | ~150 | 1 day |
| **Total** | **~650** | **5 days** |

### Grand Total

**~2,250 lines of production code, ~14-15 days of implementation.**

---

## DESIGN QUALITY ASSESSMENT

### ✅ Strengths

1. **Clear Architecture**
   - Dual-chain separation (flexible vs. fixed)
   - Clean abstraction layers (C API → Templates)
   - Minimal broker scope

2. **Performance**
   - Zero-copy data access
   - Lock-free reads (atomic reader_count)
   - Fast writes (10-50 ns atomic operations)

3. **Robustness**
   - TOCTTOU-safe coordination
   - Memory ordering correct (acquire/release)
   - Integrated error tracking
   - Crash recovery (EOWNERDEAD, PID checks)

4. **Observability**
   - Comprehensive metrics (256 bytes)
   - Python/CLI access
   - Automatic error recording

5. **Simplicity**
   - No framework (just building blocks)
   - Users compose patterns
   - Clear error handling (codes, not exceptions)

### ⚠️ Remaining Concerns

1. **P7: No exception safety yet**
   - Primitive API requires manual cleanup
   - Need RAII wrappers (Layer 2)

2. **P8: Limited error recovery**
   - No force_reset_slot() for stuck states
   - No CLI tools for emergency procedures

3. **P9: Schema validation stub**
   - Schema hash in protocol but no enforcement
   - Need validation algorithm

4. **Documentation gaps**
   - Many design decisions documented here, not in HEP-core-0002
   - Need to sync design doc with review decisions

---

## NEXT STEPS

### Immediate (This Session)

1. **Update HEP-core-0002** with completed designs:
   - Section 12: Add SlotRWCoordinator specification
   - Section 13: Update with Dual-Chain architecture
   - Section 16: Add minimal broker protocol
   - Section 15: Add complete metrics specification

2. **Consolidate TODO list** with clear priorities

### Short-Term (Next 1-2 Weeks)

1. Implement P7 (Layer 2 Transaction API)
2. Implement P8 (Error Recovery API)
3. Implement P9 (Schema Validation)

### Medium-Term (Next Month)

1. Implement core infrastructure (~1,600 lines)
2. Write comprehensive tests
3. Performance benchmarking
4. Production deployment

---

## DESIGN CONFIDENCE LEVEL

| Aspect | Confidence | Notes |
|--------|------------|-------|
| **Memory Model** | 95% | Well-specified, correct ordering |
| **Synchronization** | 95% | TOCTTOU resolved, fences correct |
| **Broker Protocol** | 90% | Minimal scope reduces risk |
| **Error Handling** | 90% | Comprehensive, needs testing |
| **Performance** | 85% | Needs benchmarking validation |
| **API Design** | 90% | Clean layers, needs Layer 2 |

**Overall: Ready for implementation** with minor refinements during coding.

---

## CHANGE LOG

### 2026-02-07 Design Session

**Major Decisions:**
1. Dual-Chain Architecture (vs. uniform units)
2. SlotRWCoordinator abstraction (vs. raw atomics)
3. Minimal broker (vs. full message bus)
4. Peer-to-peer heartbeat (vs. broker-mediated)
5. Integrated metrics (vs. separate monitoring)

**Documents Created:**
- `CRITICAL_REVIEW_DataHub_2026-02-07.md` (3190 lines)
- `DESIGN_STATUS_2026-02-07.md` (this document)

**Next:** Update HEP-core-0002 with all design decisions.

---

## REFERENCES

- **Critical Review:** `cpp/docs/code_review/CRITICAL_REVIEW_DataHub_2026-02-07.md`
- **Design Document:** `cpp/docs/hep/hep-core-0002-data-hub-structured.md`
- **Implementation:** `cpp/src/include/utils/data_block.hpp`
- **Sync Primitives:** `cpp/src/include/utils/data_header_sync_primitives.hpp`
