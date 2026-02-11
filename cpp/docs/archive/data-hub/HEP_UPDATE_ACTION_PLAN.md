# Action Plan: Update HEP-core-0002 with Design Decisions
**Date:** 2026-02-07  
**Source:** CRITICAL_REVIEW_DataHub_2026-02-07.md  
**Target:** hep-core-0002-data-hub-structured.md

---

## SECTIONS TO UPDATE

### 1. Section 12: Synchronization Model

**Add New Subsection 12.6: SlotRWCoordinator Abstraction**

Content to add:
- SlotRWState structure (64 bytes, opaque)
- C API specification (slot_rw_acquire_write, slot_rw_acquire_read, etc.)
- TOCTTOU prevention mechanism (atomic + memory fences)
- Header-only template wrappers (with_write_access, with_typed_read)
- Usage examples

**Update Subsection 12.4.3: Reader Flow**
- Add memory ordering annotations (memory_order_acquire)
- Update sequence diagrams with fence operations
- Document TOCTTOU prevention guarantees

**Update Subsection 12.4.2: Writer Flow**
- Add memory ordering annotations (memory_order_release)
- Document reader drain protocol with fences
- Add timeout handling

---

### 2. Section 13: Buffer Policies

**Major Update: Replace Uniform Units with Dual-Chain**

Remove:
- Old unified unit model
- ZoneAssignment configuration
- access_finegrained_unit<T> API (deprecated)

Add:
- **Section 13.1: Dual-Chain Architecture Overview**
  - Table 1: Flexible Zone Chain (FineGrained)
  - Table 2: Fixed Buffer Chain (CoarseGrained)
  - Rationale and use cases

- **Section 13.2: Flexible Zone Chain**
  - User-managed atomics
  - with_access<T> template API
  - Coordination patterns

- **Section 13.3: Fixed Buffer Chain**
  - Ring buffer semantics
  - SlotRWCoordinator coordination
  - Queue full/empty handling
  - Backpressure mechanism (block with exponential backoff)

---

### 3. Section 15: Memory Model and Layout

**Update Section 15.1: Memory Layout Diagram**

Add to memory map:
```
├─────────────────────────────────────────────┤
│ Per-Slot RW State Array                     │  N × 64 bytes (was 48)
│  - Updated size: 64 bytes (cache aligned)   │
│  - Padding for false sharing mitigation     │
├─────────────────────────────────────────────┤
│ Metrics Section (256 bytes)                 │  NEW!
│  - Slot coordination metrics (64B)          │
│  - Error tracking (96B)                     │
│  - Heartbeat tracking (32B)                 │
│  - Performance metrics (64B)                │
├─────────────────────────────────────────────┤
│ Consumer Heartbeat Slots (512 bytes)        │  NEW!
│  - 8 × 64-byte slots                        │
│  - consumer_id, last_heartbeat_ns           │
└─────────────────────────────────────────────┘
```

**Update Section 15.3: Per-Slot RW State**
- Increase size from 48 to 64 bytes
- Document cache line alignment (alignas(64))
- Add false sharing analysis

**Add Section 15.6: Observability Metrics Layout**
- Complete metrics structure specification
- Document atomic counter semantics
- Add memory overhead analysis (256 + 512 = 768 bytes)

---

### 4. Section 16: Message Protocols

**Major Simplification: Minimal Broker Protocol**

Replace current complex protocol with:

**Section 16.1: Broker Discovery Protocol**
- REG_REQ, DISC_REQ, DEREG_REQ (3 messages only)
- JSON format specification
- Error codes (OK, NOT_FOUND, AUTH_FAILED, TIMEOUT, ERROR)
- Retry strategy with exponential backoff

**Section 16.2: Peer-to-Peer Communication Patterns**
- Pattern 1: Shared memory only (poll)
- Pattern 2: Shared memory + direct ZeroMQ (push)
- Pattern 3: Broker PUB/SUB (not recommended)
- Usage examples for each pattern

**Section 16.3: Heartbeat Protocol (Peer-to-Peer)**
- ConsumerHeartbeat slots specification
- update_heartbeat() API
- Liveness checking algorithm
- Timeout detection and cleanup

**Remove:**
- Complex broker state machines
- Message routing specifications
- Broker-mediated heartbeat (replaced with peer-to-peer)

---

### 5. Section 9: Checksum and Integrity

**Update Section 9.2: Checksum Policy**

Simplify from 4 policies to 2:
- **Manual:** User calls checksum methods explicitly
- **Enforced:** Automatic on slot release

**Add Section 9.3: Checksum Scope**
- Flexible zones: Always Manual (user coordinates)
- Fixed buffers: Manual or Enforced (SlotRWState.write_lock ensures atomicity)
- Rationale: Writer holds write_lock during checksum computation

**Update Section 9.4: Checksum API**
- update_checksum_slot() - Fixed buffers
- verify_checksum_slot() - Fixed buffers
- compute_flexible_zone_checksum() - Flexible zones (user-coordinated)
- verify_flexible_zone_checksum() - Flexible zones

---

### 6. Section 10: API Design

**Update Section 10.2.5: DataBlockConsumer API**

Add memory ordering documentation:
```cpp
uint64_t get_commit_index() const;
// MUST use memory_order_acquire (synchronizes with producer)

uint64_t get_write_index() const;
// MUST use memory_order_acquire

uint64_t get_read_index() const;
// MUST use memory_order_acquire
```

**Update Section 10.2.8: DataBlockSlotIterator API**

Document memory ordering:
```cpp
void seek_latest();
// Synchronizes with producer via acquire on commit_index

NextResult try_next(int timeout_ms);
// Uses acquire ordering to ensure data visibility
```

**Add Section 10.4: Layer 1.5 - SlotRWCoordinator API**
- Complete C API specification
- Error codes and return values
- Timeout semantics
- Usage examples

**Add Section 10.5: Layer 1.75 - Header-Only Template Wrappers**
- SlotRWAccess class
- with_write_access() template
- with_typed_read<T>() template
- RAII guarantees

---

### 7. Section 11: Primitive Operations

**Update Section 11.3: Iterator Path**

**Add Subsection 11.3.2: Memory Ordering in Iterator**
- Document acquire ordering requirements
- Synchronization chain diagram
- Cross-reference to Section 12

---

### 8. New Section 17: Observability and Monitoring

**Add Complete Observability Specification:**

**Section 17.1: Metrics Infrastructure**
- SharedMemoryHeader metrics (256 bytes)
- Automatic error recording
- Zero-overhead design

**Section 17.2: Metrics API**
- C API: datablock_get_metrics()
- Python bindings
- CLI tools

**Section 17.3: Monitoring Patterns**
- Python control hub example
- Alert thresholds
- Health checks

**Section 17.4: Common Failure Modes**
- High timeout rate → increase capacity
- Lock contention → bug (multiple producers?)
- Reader races → timing issue
- Heartbeat failures → network/broker issue

---

### 9. Section 18: Implementation Status

**Update Section 18.1: Completed Features**

Mark as designed:
- ✅ SlotRWCoordinator (C API + templates)
- ✅ Dual-Chain architecture
- ✅ Checksum policy (Manual/Enforced)
- ✅ Memory ordering (acquire/release)
- ✅ Minimal broker protocol
- ✅ Peer-to-peer heartbeat
- ✅ Integrated metrics (256 bytes)

**Update Section 18.4: Critical Issues**

Mark as resolved:
- ✅ P1: Ring Buffer Policy (Dual-Chain)
- ✅ P2: MessageHub Thread Safety (Internal mutex)
- ✅ P3: Checksum Policy (Manual/Enforced)
- ✅ P4: TOCTTOU Race (SlotRWCoordinator)
- ✅ P5: Memory Barriers (acquire/release)
- ✅ P6: Broker Integration (Minimal protocol)
- ✅ P2.5: Heartbeat (Peer-to-peer)
- ✅ P10: Observability (Integrated metrics)

---

## ESTIMATED UPDATE EFFORT

| Section | Changes | Effort |
|---------|---------|--------|
| Section 12 | Add SlotRWCoordinator, update flows | 2 hours |
| Section 13 | Replace with Dual-Chain | 1.5 hours |
| Section 15 | Update memory layout, add metrics | 1 hour |
| Section 16 | Simplify broker protocol | 1.5 hours |
| Section 9 | Update checksum policies | 0.5 hours |
| Section 10 | Add Layer 1.5 API, update ordering | 1.5 hours |
| Section 11 | Add memory ordering subsection | 0.5 hours |
| Section 17 | New observability section | 1.5 hours |
| Section 18 | Update status | 0.5 hours |

**Total:** ~10 hours of documentation work.

---

## PRIORITY ORDER

1. **Section 13** (Dual-Chain) - Most fundamental change
2. **Section 12** (SlotRWCoordinator) - Core coordination
3. **Section 15** (Memory layout) - Updated structures
4. **Section 16** (Broker protocol) - Simplified design
5. **Section 17** (Observability) - New section
6. **Sections 9, 10, 11, 18** - Updates and refinements

---

## VALIDATION CHECKLIST

After updates, verify:
- [ ] All design decisions from review are incorporated
- [ ] Memory layout diagrams match new structures (64-byte SlotRWState, 256-byte metrics)
- [ ] API specifications match C interface (ABI-stable)
- [ ] Code examples use correct memory ordering
- [ ] Cross-references between sections are consistent
- [ ] No contradictions between old and new content
- [ ] All TODOs from critical review addressed in design

---

## TOOLS NEEDED

- **Mermaid diagrams:** Update sequence diagrams with memory fences
- **Code blocks:** Add SlotRWCoordinator C API
- **Tables:** Update memory layout tables
- **JSON examples:** Broker protocol messages

---

**Ready to begin updates?** Start with Section 13 (most fundamental).
