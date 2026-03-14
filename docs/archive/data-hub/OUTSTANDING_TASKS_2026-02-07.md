# Outstanding Design Tasks - Summary
**Date:** 2026-02-07  
**Source:** CRITICAL_REVIEW_DataHub_2026-02-07.md

---

## COMPLETION STATUS

### ✅ COMPLETED (8 tasks)

**Phase 1: Critical Blockers (5/5 complete)**
1. ✅ P1: Ring Buffer Policy Enforcement
2. ✅ P2: MessageHub Thread Safety
3. ✅ P3: Checksum Policy Enforcement
4. ✅ P4: TOCTTOU Race Condition
5. ✅ P5: Memory Barriers in Iterator

**Phase 2: High Priority (3/3 core tasks complete)**
6. ✅ P6: Broker Integration (minimal protocol)
7. ✅ P2.5: Heartbeat Infrastructure (peer-to-peer)
8. ✅ P10: Observability API (integrated metrics)

---

## 🔴 CRITICAL REMAINING TASKS (Phase 2)

### P7: Layer 2 Transaction API (HIGH) - ~2-3 days

**Why Critical:**
- Current primitive API requires manual cleanup → resource leaks
- No exception safety → crashes leave slots locked
- Difficult to use correctly

**What's Needed:**
```cpp
// Transaction wrappers with RAII
class WriteTransaction {
    // Auto-acquire, auto-commit, auto-release
    // Exception-safe
};

template<typename Func>
void with_transaction_write(Func&& lambda);

template<typename Func>
void with_transaction_read(Func&& lambda);
```

**Tasks:**
- [ ] Design transaction API (RAII wrappers)
- [ ] Specify exception safety guarantees
- [ ] Add to Section 10.3 in HEP document
- [ ] Document Layer 1 vs Layer 2 trade-offs

**Estimated Implementation:** ~300 lines of code

---

### P8: Error Recovery API (MEDIUM-HIGH) - ~2 days

**Why Important:**
- Production systems get stuck (slots in WRITING, DRAINING states)
- Need tools to diagnose and fix without restart

**What's Needed:**
```cpp
// Recovery operations
void force_reset_slot(uint32_t slot_index);
void skip_stuck_slot();
void validate_slot_states();

// CLI tool
$ datablock-admin force-reset --shm-name foo --slot 3
```

**Tasks:**
- [ ] Design recovery operations
- [ ] Add ErrorRecovery API to Section 10
- [ ] Add emergency procedures appendix
- [ ] CLI tools specification

**Estimated Implementation:** ~200 lines of code

---

### P9: Schema Validation (MEDIUM-HIGH) - ~1-2 days

**Why Important:**
- Prevent ABI mismatches (producer writes struct A, consumer expects struct B)
- Schema hash in protocol but no validation

**What's Needed:**
```cpp
// Schema validation on attach
auto consumer = DataBlockConsumer::attach(shm_name, secret);
// Should verify: consumer's schema_hash == producer's schema_hash

// Explicit schema check
bool validate_schema(const std::string& expected_hash);
```

**Tasks:**
- [ ] Complete schema negotiation protocol
- [ ] Add validation to attach() implementation
- [ ] Define broker schema registry
- [ ] Document version compatibility rules

**Estimated Implementation:** ~150 lines of code

---

## 🟡 OPTIONAL ENHANCEMENTS (Phase 3-5)

### Performance & Scalability (Phase 3)

| Task | Priority | Effort | Reason |
|------|----------|--------|--------|
| **P11: Cache Line Alignment** | Medium | 0.5 days | False sharing mitigation, 5-10% speedup |
| **P12: Spinlock Capacity** | Low | 0.5 days | 8 locks sufficient for most use cases |
| **P13: Huge Page Support** | Medium | 1 day | Large buffers (>2MB), 10-20% speedup |
| **P14: Bulk Transfer API** | Medium | 1 day | Batch operations, amortize overhead |

**Recommendation:** Defer until after production deployment, benchmark first.

---

### Security & Robustness (Phase 4)

| Task | Priority | Effort | Reason |
|------|----------|--------|--------|
| **P15: Shared Secret Strength** | Medium | 0.5 days | 64-bit may be weak for untrusted networks |
| **P16: Integrity Protection** | Medium | 1 day | HMAC for authentication (vs checksums) |

**Recommendation:** Address if deploying over untrusted networks.

---

### Documentation Completeness (Phase 5)

| Task | Priority | Effort | Reason |
|------|----------|--------|--------|
| **P17: Missing Design Elements** | Medium | 1-2 days | Handover protocol, disaster recovery, tuning guide |
| **P18: API Completeness** | Medium | 1 day | Timeout configuration, API audit |

**Recommendation:** Continuous improvement during implementation.

---

## IMMEDIATE NEXT STEPS

### Step 1: Update HEP-core-0002 Document (~10 hours)

**Priority sections:**
1. Section 13 (Dual-Chain) - Most fundamental
2. Section 12 (SlotRWCoordinator) - Core coordination
3. Section 15 (Memory layout) - Updated structures
4. Section 16 (Broker protocol) - Simplified design
5. Section 17 (Observability) - New section

**Tool:** Use action plan in `HEP_UPDATE_ACTION_PLAN.md`

---

### Step 2: Complete Remaining Critical Tasks (P7-P9) (~5-7 days)

1. **P7: Layer 2 Transaction API** (2-3 days)
   - RAII wrappers
   - Exception safety
   - with_transaction() helpers

2. **P8: Error Recovery API** (2 days)
   - Recovery operations
   - CLI tools
   - Emergency procedures

3. **P9: Schema Validation** (1-2 days)
   - Validation algorithm
   - Broker schema registry
   - Version compatibility

---

### Step 3: Implementation (~14-15 days)

**Core infrastructure:**
- SlotRWCoordinator: 3 days
- Broker service: 2 days
- Messaging toolkit: 1 day
- Metrics API: 1 day
- Python bindings: 1 day
- Transaction API: 2-3 days
- Error recovery: 2 days
- Schema validation: 1 day
- Tests: 2-3 days

---

## SUCCESS CRITERIA

### Design Complete When:
- [ ] All P1-P9 tasks marked complete
- [ ] HEP-core-0002 updated with all decisions
- [ ] No contradictions between design doc and review
- [ ] All API specifications are ABI-stable
- [ ] All memory layouts documented with sizes
- [ ] All protocols have sequence diagrams
- [ ] All error codes defined

### Implementation Ready When:
- [ ] Design document approved
- [ ] API signatures frozen
- [ ] Memory layout finalized
- [ ] Protocol messages defined
- [ ] Test plan written

---

## EFFORT SUMMARY

| Phase | Tasks | Design Effort | Implementation Effort |
|-------|-------|---------------|----------------------|
| **Phase 1** | P1-P5 | ✅ Complete | ~7 days |
| **Phase 2 (Critical)** | P6, P2.5, P10 | ✅ Complete | ~5 days |
| **Phase 2 (Remaining)** | P7-P9 | Not Started | ~5-7 days |
| **Phase 3-5** | P11-P18 | Optional | ~5-10 days |
| **Documentation** | HEP updates | ~10 hours | N/A |
| **Total** | 18 tasks | ~10 hours remaining | ~17-22 days |

---

## RECOMMENDATION

**Current State:** Design is 70% complete, implementation-ready for core features.

**Next Actions:**
1. Update HEP-core-0002 with completed designs (~10 hours)
2. Complete P7-P9 design (~2-3 days)
3. Begin implementation of core infrastructure

**Target:** Production-ready in 3-4 weeks (design + implementation + testing).

**Risk:** Low - Core architecture is solid, remaining tasks are additive.

---

## FILES REFERENCE

- **Critical Review:** `cpp/docs/code_review/CRITICAL_REVIEW_DataHub_2026-02-07.md`
- **Design Status:** `cpp/docs/code_review/DESIGN_STATUS_2026-02-07.md`
- **Action Plan:** `cpp/docs/code_review/HEP_UPDATE_ACTION_PLAN.md`
- **This Summary:** `cpp/docs/code_review/OUTSTANDING_TASKS_2026-02-07.md`
- **Design Document:** `cpp/docs/hep/hep-core-0002-data-hub-structured.md` (TO BE UPDATED)
