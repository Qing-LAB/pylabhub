# Data Exchange Hub - Documentation Guide

**Last Updated:** 2026-02-08
**Status:** Implementation Ready

---

## 📋 Quick Start

**Read this first:** [`HEP-CORE-0002-DataHub-FINAL.md`](./HEP-CORE-0002-DataHub-FINAL.md)

This is the **single authoritative source** for the Data Exchange Hub design. All previous drafts and working documents have been archived.

---

## 📚 Document Organization

### Active Documents

| Document | Purpose | Lines | Status |
|----------|---------|-------|--------|
| **HEP-CORE-0002-DataHub-FINAL.md** | Complete technical specification | 2,948 | ✅ Complete |

### Archived Documents

All design evolution documents moved to: [`docs/archive/data-hub/`](../archive/data-hub/)

This includes:
- `CRITICAL_REVIEW_DataHub_2026-02-07.md` - Design review process
- `hep-core-0002-data-hub.md` - Original draft
- `hep-core-0002-data-hub-structured.md` - Structured revision
- `P7_LAYER2_TRANSACTION_API_DESIGN.md` - Transaction API design
- `P8_ERROR_RECOVERY_API_DESIGN.md` - Error recovery design
- `P9_SCHEMA_VALIDATION_DESIGN.md` - Schema validation notes
- `INDEX.md` - Document navigation (historical)
- Various TODO and status documents

**Note:** These are historical references only. For implementation, use **HEP-CORE-0002-DataHub-FINAL.md**.

---

## 🎯 What's in the Final Specification

### Section 1: Executive Summary
- Design philosophy and principles
- Key architectural decisions (Dual-Chain, SlotRWCoordinator, Minimal Broker)
- Production readiness status (70% complete, 1 task remaining)

### Section 2: System Architecture
- High-level architecture diagram
- Dual-Chain model (Flexible zones vs Fixed buffers)
- Two-tier synchronization (OS mutex vs Atomic coordination)
- Component interaction flows

### Section 3: Memory Layout and Data Structures
- Complete shared memory organization (4KB header + arrays)
- `SharedMemoryHeader` structure (256-byte metrics, heartbeat slots)
- `SlotRWState` structure (48 bytes, cache-aligned)
- Recommended slot metadata pattern

### Section 4: Synchronization Model
- SlotRWState coordination (writer/reader flows)
- TOCTTOU race mitigation (double-check + memory fences)
- Memory ordering reference (acquire/release semantics)
- SharedSpinLock for flexible zones

### Section 5: API Specification (All Layers)
- **Layer 0:** C Interface (ABI-stable, cross-language)
- **Layer 1.75:** Template Wrappers (`with_typed_write<T>`)
- **Layer 2:** Transaction API (lambda-based RAII)
- Complete code examples for each layer

### Section 6: Control Plane Protocol
- Minimal broker protocol (3 messages)
- JSON message formats
- Discovery flow sequence diagram
- Peer-to-peer heartbeat (zero network overhead)

### Section 7: Common Usage Patterns
Four detailed scenarios:
1. **Sensor Streaming** (Single Policy) - Latest value
2. **Video Frames** (DoubleBuffer) - Stable processing
3. **Data Queue** (RingBuffer) - Lossless logging
4. **Multi-Camera Sync** - Flexible zone coordination

### Section 8: Error Handling and Recovery
- CLI tool: `datablock-admin` (diagnose, force-reset, auto-recover)
- PID liveness checks (Linux/Windows/macOS)
- Emergency procedures (3 scenarios)
- Python monitoring bindings

### Section 9: Performance Characteristics
- Operation latencies (median 120ns, 95th 180ns)
- Throughput benchmarks (4KB: 4.7 GB/s, 64KB: 9.5 GB/s)
- Memory ordering overhead comparison

### Section 10: Security and Integrity
- Shared secret (64-byte capability token)
- BLAKE2b checksums (Manual vs Enforced)
- OS-level access control

### Section 11: Schema Validation ⚠️ REMAINING TASK
- BLDS (Basic Layout Description String) format
- BLAKE2b-256 hash computation
- Producer/consumer validation flows
- **Status:** Design task (2-4 days effort)

### Section 12: Implementation Guidelines
- 5 coding patterns (DO/DON'T examples)
- Memory ordering cheat sheet
- Error handling strategy

### Section 13: Testing Strategy
- Test organization (unit/integration/stress)
- 5 key test scenarios
- Platform coverage (x86, ARM, ThreadSanitizer)

### Section 14: Deployment and Operations
- `datablock-inspect` CLI tool
- Python monitoring API
- Prometheus metrics exporter
- Auto-recovery daemon

### Section 15: Appendices
- Glossary (23 terms)
- Quick reference card (6 patterns)
- Design decision log (4 major choices)
- FAQ (5 questions)

---

## 🚀 Implementation Roadmap

### Current Status
- ✅ Architecture: Complete and validated
- ✅ Synchronization: TOCTTOU resolved, memory ordering correct
- ✅ API Layers: All 5 layers specified with examples
- ✅ Error Recovery: CLI tools and procedures defined
- ⚠️ Schema Validation: Detailed specification in Section 11 (to implement)

### Timeline (5 weeks to production)

**Week 1: Design Finalization**
- Complete P9 Schema Validation design (~2-4 days)
- Freeze API signatures
- Review and approval

**Week 2-3: Core Implementation**
- SlotRWCoordinator: 500 lines, 3 days
- Broker service: 300 lines, 2 days
- Transaction API: 300 lines, 2.5 days
- Error recovery: 200 lines, 2 days
- Schema validation: 150 lines, 1.5 days
- Subtotal: ~1,950 lines, 14 days

**Week 4: Testing**
- Unit tests: 500 lines, 2 days
- Integration tests: 200 lines, 2 days
- Stress tests: 100 lines, 1 day
- Subtotal: ~800 lines, 5 days

**Week 5: Deployment**
- Documentation: 2 days
- Deployment + monitoring: 3 days

---

## 🎓 For Different Audiences

### For Implementers
1. Read **Section 1** (Executive Summary)
2. Study **Section 4** (Synchronization Model) - Critical for correctness
3. Review **Section 5** (API Specification) - All layers with examples
4. Follow **Section 12** (Implementation Guidelines)
5. Reference **Section 3** (Memory Layout) as needed

### For Users
1. Read **Section 1.2** (Design Philosophy)
2. Study **Section 7** (Common Usage Patterns) - Pick your scenario
3. Use **Section 5.4** (Layer 2 Transaction API) - Recommended for applications
4. Consult **Appendix B** (Quick Reference) for patterns

### For Operators
1. Read **Section 8** (Error Handling) - CLI tools and procedures
2. Study **Section 14** (Deployment) - Monitoring and recovery
3. Use **Appendix D** (FAQ) for troubleshooting
4. Set up auto-recovery daemon from Section 14

### For Reviewers
1. Read **Section 1** (Executive Summary) - Design maturity
2. Review **Section 2** (Architecture) - High-level decisions
3. Examine **Section 4** (Synchronization) - Correctness proof
4. Check **Section 11** (Schema Validation) - Remaining task

---

## 📊 Key Metrics

### Design Completion
- **Architecture:** 100% ✅
- **Synchronization:** 100% ✅
- **API Specification:** 100% ✅
- **Error Recovery:** 100% ✅
- **Schema Validation:** 80% (detailed spec ready, implementation pending)
- **Overall:** 90% complete

### Confidence Level
- **Core Design:** 95% (validated, proven)
- **Synchronization:** 95% (TOCTTOU resolved, memory ordering correct)
- **Performance:** 85% (estimated, needs validation)
- **Production Readiness:** 70% (needs P9 completion + implementation)

### Code Estimates
- **Production Code:** ~1,950 lines
- **Test Code:** ~800 lines
- **Total:** ~2,750 lines
- **Implementation Time:** ~14 days (coding) + ~5 days (testing)

---

## ⚠️ Critical Notes

### Memory Ordering
**Critical on ARM/RISC-V platforms!** All `commit_index`, `write_index`, `read_index` accesses MUST use `memory_order_acquire` (consumer) and `memory_order_release` (producer). See Section 4.3 for complete reference.

### TOCTTOU Prevention
Double-check + memory fences are **required** in reader acquisition. Do not simplify the logic without understanding the race condition. See Section 4.2.3.

### PID-Based Locking
All recovery operations MUST check process liveness before resetting locks. See Section 8 for PID check implementations.

### Testing Requirements
- **ThreadSanitizer:** Mandatory for ARM builds
- **Stress Testing:** High contention scenarios required
- **Platform Coverage:** Test on both x86 and ARM

---

## 🔗 Related Documents

- **HEP-core-0003:** Cross-Platform FileLock (dependency)
- **HEP-core-0004:** Async Logger (used for error logging)
- **CLAUDE.md:** Build system and architecture overview

---

## 📝 Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2026-02-08 | 1.0 | Complete unified specification (all 15 sections) |
| 2026-02-07 | 0.5 | Initial sections 1-5 |
| 2026-01-07 | 0.1 | HEP created |

---

## 📞 Questions?

For design questions, consult:
1. **Section 15: Appendices** - Glossary and FAQ
2. **Archived CRITICAL_REVIEW** - Design rationale and alternatives
3. **Code examples** - Throughout Sections 5-7

For implementation questions:
1. **Section 12** - Implementation Guidelines
2. **CLAUDE.md** - Build system and conventions

---

**Last Updated:** 2026-02-08
**Document Maintainer:** Quan Qing
**Status:** Ready for Implementation 🚀
