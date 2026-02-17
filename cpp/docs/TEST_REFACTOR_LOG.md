# Test Refactoring: Execution Log

**Start Date**: 2026-02-15  
**Status**: In Progress

---

## Phase 1: Foundation

### ✅ Step 1.1: Create Shared Test Types
**File**: `tests/test_framework/test_datahub_types.h`  
**Status**: CREATED

**Types Created:**
- `NoFlexZone` (std::monostate) - for tests without flex zone
- `TestFlexZone` - simple flex zone (counter, timestamp)
- `TestDataBlock` - simple data block (sequence, value, label)
- `MinimalData` - single field (for stress tests)
- `LargeTestData` - 1KB payload (for large data tests)
- `FrameMeta` - frame metadata (video/camera tests)
- `SensorData` - sensor data (IoT tests)

**Schemas**: All types have BLDS schema definitions

**Usage**: Include `test_datahub_types.h` in tests, use types with template API:
```cpp
auto producer = create_datablock_producer<TestFlexZone, TestDataBlock>(...);
auto consumer = find_datablock_consumer<TestFlexZone, TestDataBlock>(...);
```

---

## Phase 2: Delete Obsolete Tests

### Step 2.1: Delete phase_a Tests
**Files to delete:**
- [ ] `tests/test_layer3_datahub/test_phase_a_protocol.cpp`
- [ ] `tests/test_layer3_datahub/workers/phase_a_workers.cpp`
- [ ] `tests/test_layer3_datahub/workers/phase_a_workers.h`

**Reason:** Wrong layer, uses removed API, replaced by:
- C API checksum tests (new)
- C++ contract tests (new)
- C++ integration tests (rewritten slot_protocol)

**Action**: Proceeding with deletion...

---

## Next Steps
1. Delete phase_a files
2. Update CMakeLists.txt to remove phase_a
3. Create C API test directory structure
4. Create first C API test (checksum)
