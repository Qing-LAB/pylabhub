# DataHub Test Audit and Cleanup Plan

**Document ID**: TEST-AUDIT-001  
**Version**: 1.0.0  
**Date**: 2026-02-15  
**Status**: Active

---

## Objectives

1. **Verify C API tests** - Ensure correctness against current implementation
2. **Remove/rewrite obsolete tests** - Tests based on old C++ model must go
3. **Clarify test logic** - Add strategy comments, especially for concurrency
4. **Complete coverage** - Normal, corner cases, error paths, racing with tunable stress
5. **Organize logically** - Group by: facility, C API, C++ abstraction layers
6. **Eliminate redundancy** - Find and remove duplicate tests
7. **Fill coverage gaps** - Add tests for revised design components

---

## Test Inventory and Classification

### Layer 2: C API Foundation (DO NOT DELETE)
**Protected by C_API_TEST_POLICY.md**

| File | Purpose | Status | Action |
|------|---------|--------|--------|
| `test_layer2_service/test_slot_rw_coordinator.cpp` | Pure C SlotRW API (acquire/commit/release, metrics) | **PROTECTED** | Audit for correctness |
| `test_layer3_datahub/test_recovery_api.cpp` | Recovery API (extern "C") | **PROTECTED** | Audit for correctness |
| `test_layer3_datahub/workers/recovery_workers.cpp` | Recovery API worker impls | **PROTECTED** | Audit for correctness |
| `test_layer3_datahub/workers/slot_protocol_workers.cpp` | Uses slot_rw_* C API | **PROTECTED** | Audit for correctness |

### Layer 3: DataHub Tests (AUDIT NEEDED)

| File | Focus | Likely Issues | Action Needed |
|------|-------|---------------|---------------|
| `test_schema_blds.cpp` | BLDS schema generation | May be OK | Verify against current schema API |
| `test_schema_validation.cpp` | Schema validation | **OLD MODEL** - likely uses deprecated API | Rewrite or remove |
| `test_slot_protocol.cpp` | Slot operations | Mixed C/C++ API usage? | Audit: separate C API tests from C++ |
| `test_phase_a_protocol.cpp` | Phase A protocol (flex zone, checksum, config) | Uses old non-template API | **REWRITE** to template API |
| `test_error_handling.cpp` | Error paths | May use old API | Audit and update |
| `test_message_hub.cpp` | MessageHub integration | May be OK | Verify |
| `test_transaction_api.cpp` | RAII transaction layer | Uses old template overload? | Verify template usage |
| `test_datablock_mutex.cpp` | Mutex/locking | Generic name, unclear purpose | Audit: rename or remove |

### Worker Files (Implementation of test cases)

| File | Purpose | Issues | Action |
|------|---------|--------|--------|
| `workers/schema_blds_workers.cpp` | BLDS tests | Check for old API | Audit |
| `workers/schema_validation_workers.cpp` | Validation tests | Likely old model | Audit/rewrite |
| `workers/slot_protocol_workers.cpp` | Slot tests | **C API usage** - protect | Audit correctness |
| `workers/phase_a_workers.cpp` | Phase A tests | **Uses non-template API** | **REWRITE** |
| `workers/error_handling_workers.cpp` | Error tests | Check API usage | Audit |
| `workers/messagehub_workers.cpp` | MessageHub | Check | Audit |
| `workers/transaction_api_workers.cpp` | Transaction tests | Check template usage | Audit |

---

## Audit Criteria

### 1. C API Correctness (Mandatory)
- [ ] Calls correct C API functions (slot_rw_*, recovery_api)
- [ ] Uses current header structure (dual schema fields, metrics)
- [ ] Tests all error codes
- [ ] Tests NULL pointer handling
- [ ] Stress tests with tunable parameters

### 2. C++ API Usage (Audit All)
- [ ] Uses template API, not removed non-template wrappers
- [ ] Schema generation correct (BLDS macros for both FlexZone and DataBlock)
- [ ] Config passed correctly (pointer vs reference)
- [ ] Tests both schema validation paths (match and mismatch)

### 3. Test Logic Clarity
- [ ] **Strategy comment** at top: what is being tested, how
- [ ] **Sequence comment** for racing/multithread: expected interleaving
- [ ] Clear assertion messages
- [ ] No magic numbers (use named constants)

### 4. Coverage Requirements
- [ ] **Normal usage** - happy path
- [ ] **Corner cases** - empty, full, boundary conditions
- [ ] **Error paths** - all error codes, validation failures
- [ ] **Racing** - concurrent access, stress levels (tunable via env or arg)

### 5. Organization
- [ ] File named by purpose: `test_<layer>_<component>_<category>.cpp`
- [ ] Grouped in correct directory: `facility/`, `c_api/`, `cpp_primitive/`, `cpp_schema/`, `cpp_raii/`

---

## Execution Plan

### Phase 1: Inventory and Classify (Current)
1. List all test files
2. Categorize by layer and API usage
3. Identify protected C API tests
4. Identify tests using old/deprecated API
5. Create detailed audit checklist

### Phase 2: C API Test Audit
1. **test_slot_rw_coordinator.cpp** - verify against current SlotRWState, metrics API
2. **test_recovery_api.cpp** - verify recovery_api.hpp usage
3. **slot_protocol_workers.cpp** - ensure C API usage documented and correct
4. Add missing C API test cases (error codes, stress)

### Phase 3: Obsolete Test Removal/Rewrite
1. **phase_a_workers.cpp** - REWRITE to use template API
2. **schema_validation tests** - check for deprecated single-schema API usage
3. Remove any tests calling deleted non-template wrappers

### Phase 4: Test Logic Enhancement
1. Add strategy comments to all racing/multithread tests
2. Add sequence diagrams or comments for complex interactions
3. Parameterize stress levels (env var or test parameter)

### Phase 5: Reorganization
1. Create directory structure per TEST_ORGANIZATION_STRUCTURE.md
2. Move/rename tests to logical groups
3. Update CMakeLists.txt
4. Update test discovery

### Phase 6: Coverage Gaps
1. Facility tests: checksum, validation, metrics, schema (separate from integration)
2. C++ primitive tests: direct handle usage, lifecycle
3. Dual-schema validation tests: both FlexZone and DataBlock mismatch scenarios
4. RAII tests: exception safety, iterator behavior

---

## Audit Checklist Template

For each test file:

```markdown
## File: <filename>

### Classification
- [ ] Layer: 0 (Platform) / 1 (Base) / 2 (Service/C API) / 3 (DataHub)
- [ ] API Level: C API / C++ Primitive / C++ Schema / C++ RAII
- [ ] Protected: Yes (C API) / No

### Current State
- [ ] Uses correct API (template vs non-template vs C)
- [ ] Correct header structure assumptions
- [ ] Clear test strategy documented
- [ ] Covers normal usage
- [ ] Covers corner cases
- [ ] Covers error paths
- [ ] Covers racing conditions (if applicable)
- [ ] Stress levels tunable

### Issues Found
- List issues here

### Action Required
- [ ] Keep as-is
- [ ] Audit and fix
- [ ] Rewrite
- [ ] Remove (obsolete)
- [ ] Move/rename

### Notes
```

---

## References

- **C_API_TEST_POLICY.md** - Protected C API test assets
- **TEST_ORGANIZATION_STRUCTURE.md** - Directory structure and naming
- **IMPLEMENTATION_GUIDANCE.md** - Best practices, pitfalls
- **API_SURFACE_DOCUMENTATION.md** - Current API levels
- **DESIGN_VERIFICATION_CHECKLIST.md** - What is actually implemented
