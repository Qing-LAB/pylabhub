# Data Exchange Hub - Consolidated TODO
**Date:** 2026-02-07  
**Status:** 8/18 tasks complete, 3 critical remaining

---

## 🎯 IMMEDIATE PRIORITIES

### 1. Update HEP-core-0002 Document (~10 hours)

**Purpose:** Sync design document with all decisions from critical review.

**Sections to update:**
- [ ] **Section 13:** Replace with Dual-Chain Architecture (1.5h)
- [ ] **Section 12:** Add SlotRWCoordinator specification (2h)
- [ ] **Section 15:** Update memory layout with metrics (1h)
- [ ] **Section 16:** Simplify broker protocol to 3 messages (1.5h)
- [ ] **Section 17:** Add new Observability section (1.5h)
- [ ] **Sections 9-11, 18:** Update checksums, API, memory ordering (2.5h)

**Tool:** Use `HEP_UPDATE_ACTION_PLAN.md` as guide.

---

### 2. Complete Critical Design Tasks (P7-P9) - ~5-7 days

#### P7: Layer 2 Transaction API (~2-3 days)

**What:** RAII wrappers for exception safety

```cpp
// Design needed:
template<typename Func>
void with_write_transaction(
    DataBlockProducer* producer,
    Func&& lambda,
    int timeout_ms = 1000
);
// Auto-acquire, invoke lambda, auto-commit, auto-release (even on exception)

template<typename Func>
void with_read_transaction(
    DataBlockConsumer* consumer,
    uint64_t slot_id,
    Func&& lambda
);
// Auto-acquire, invoke lambda, auto-release (even on exception)
```

**Tasks:**
- [ ] Define transaction API (RAII classes + templates)
- [ ] Specify exception safety (strong guarantee)
- [ ] Document performance (zero overhead)
- [ ] Add usage examples
- [ ] Document Layer 1 vs Layer 2 trade-offs

---

#### P8: Error Recovery API (~2 days)

**What:** Tools to diagnose and fix stuck states

```cpp
// Design needed:
namespace pylabhub::recovery {
    // Diagnostic
    SlotState get_slot_state(shm_name, slot_index);
    bool is_slot_stuck(shm_name, slot_index);
    
    // Recovery
    void force_reset_slot(shm_name, slot_index);
    void force_reset_all_slots(shm_name);
    
    // Validation
    std::vector<SlotDiagnostic> validate_all_slots(shm_name);
}

// CLI tool
$ datablock-admin diagnose --shm-name foo
Slot 0: COMMITTED (healthy)
Slot 1: WRITING (stuck for 30s!) ⚠️
Slot 2: DRAINING (2 readers)

$ datablock-admin force-reset --shm-name foo --slot 1
[INFO] Slot 1 reset to FREE
```

**Tasks:**
- [ ] Design recovery operations (force_reset_slot, skip_slot)
- [ ] Add ErrorRecovery API to Section 10
- [ ] Design CLI tools (datablock-admin)
- [ ] Add emergency procedures appendix

---

#### P9: Schema Validation (~1-2 days)

**What:** Validate producer/consumer schema compatibility

```cpp
// Design needed:
struct SchemaDescriptor {
    std::string name;
    std::string version;
    std::string hash;  // BLAKE2b of schema
};

// Validation on attach
auto consumer = DataBlockConsumer::attach(
    shm_name, 
    secret,
    expected_schema  // NEW: schema validation
);
// Throws if schema mismatch
```

**Tasks:**
- [ ] Define schema hash computation (BLAKE2b of what?)
- [ ] Add schema validation to attach() flow
- [ ] Specify broker schema registry (optional)
- [ ] Document version compatibility rules

---

## 🟢 OPTIONAL ENHANCEMENTS (Can Defer)

### Phase 3: Performance (~3-5 days)

- [ ] P11: Cache Line Alignment (0.5 days)
- [ ] P12: Spinlock Capacity (0.5 days)
- [ ] P13: Huge Page Support (1 day)
- [ ] P14: Bulk Transfer API (1 day)

**Recommendation:** Benchmark first, optimize if needed.

---

### Phase 4: Security (~1-2 days)

- [ ] P15: Shared Secret Strength (0.5 days)
- [ ] P16: Integrity Protection (1 day)

**Recommendation:** Address if deploying over untrusted networks.

---

### Phase 5: Documentation (~2-3 days)

- [ ] P17: Missing Design Elements (1-2 days)
  - Versioning strategy
  - Disaster recovery procedures
  - Performance tuning guide
  - Handover protocol

- [ ] P18: API Completeness (1 day)
  - Timeout configuration API
  - API audit and gap analysis

**Recommendation:** Continuous improvement.

---

## IMPLEMENTATION ROADMAP

### Milestone 1: Design Complete (Target: End of Week 1)

**Deliverables:**
- [x] Critical review complete (3190 lines)
- [ ] HEP-core-0002 updated (~10 hours)
- [ ] P7-P9 designed (~2-3 days)

**Status:** 8/11 tasks complete

---

### Milestone 2: Core Implementation (Target: Week 2-3)

**Deliverables:**
- [ ] SlotRWCoordinator (C API + templates): 3 days
- [ ] Broker service: 2 days
- [ ] Messaging toolkit: 1 day
- [ ] Metrics API + Python bindings: 1 day
- [ ] Transaction API (Layer 2): 2 days
- [ ] Error recovery + CLI tools: 2 days
- [ ] Schema validation: 1 day

**Total:** ~12 days of implementation

---

### Milestone 3: Testing & Validation (Target: Week 4)

**Deliverables:**
- [ ] Unit tests (all APIs)
- [ ] Integration tests (producer-consumer)
- [ ] Stress tests (TOCTTOU, race conditions)
- [ ] Performance benchmarks
- [ ] ARM/RISC-V validation (memory ordering)

**Total:** ~3-5 days

---

### Milestone 4: Production Deployment (Target: Week 5)

**Deliverables:**
- [ ] Documentation complete
- [ ] Examples and tutorials
- [ ] Monitoring setup
- [ ] Production deployment

---

## RISK ASSESSMENT

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Memory ordering bugs** | Medium | Critical | Test on ARM, use ThreadSanitizer |
| **TOCTTOU races** | Low | High | Comprehensive stress tests |
| **ABI breaks during impl** | Low | Medium | Freeze API signatures now |
| **Performance regression** | Low | Medium | Benchmark continuously |
| **Schema mismatches** | Medium | Medium | Implement P9 validation |

**Overall Risk:** **LOW** - Core architecture is solid.

---

## DECISION LOG

### Major Architectural Decisions (2026-02-07)

1. **Dual-Chain Architecture** - Separate flexible zones from fixed buffers
2. **SlotRWCoordinator** - Three-layer abstraction (C API + Templates)
3. **Minimal Broker** - Discovery only (3 messages)
4. **Peer-to-Peer Heartbeat** - Shared memory slots (zero network)
5. **Integrated Metrics** - 256 bytes in header, automatic recording
6. **Lock Separation** - SharedSpinLock vs SlotRWState (different purposes)
7. **Checksum Simplification** - Manual/Enforced only (was 4 policies)
8. **Memory Ordering** - acquire/release (not seq_cst)

All decisions documented in `CRITICAL_REVIEW_DataHub_2026-02-07.md`.

---

## TEAM ASSIGNMENTS (Suggested)

**If multiple developers:**

- **Developer 1:** SlotRWCoordinator implementation (P4)
- **Developer 2:** Broker service (P6) + Transaction API (P7)
- **Developer 3:** Metrics/monitoring (P10) + Error recovery (P8)
- **Documentation:** HEP-core-0002 updates + Schema validation (P9)

**If single developer:** Follow roadmap sequentially.

---

## CONTACT / ESCALATION

**Blockers:**
- API design questions → Review design doc first
- Implementation questions → Refer to code examples in review
- Architectural concerns → Re-review design decisions

**Approval Needed:**
- HEP-core-0002 updates (after first draft)
- API signatures (before implementation)
- Breaking changes (if discovered during impl)

---

**Last Updated:** 2026-02-07  
**Next Review:** After HEP document updates
