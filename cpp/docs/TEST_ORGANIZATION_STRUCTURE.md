# DataHub Test Organization Structure

**Document ID**: TEST-ORG-001  
**Version**: 1.0.0  
**Date**: 2026-02-15

## Overview

This document defines the organizational structure for DataHub tests, separating by API level and test category for clarity and maintainability.

---

## Mandatory: Do Not Delete C API Tests

**The C API is the foundation; other layers are built on top. C API tests must not be removed.**

See **`C_API_TEST_POLICY.md`** for the full rule and the list of C API test assets that must be preserved. In short:

- **test_layer2_service/test_slot_rw_coordinator.cpp** – Pure C SlotRWCoordinator API.
- **test_layer3_datahub/test_recovery_api.cpp** – Recovery API (extern "C").
- **test_layer3_datahub/workers/recovery_workers.cpp** – Recovery API workers.
- **test_layer3_datahub/workers/slot_protocol_workers.cpp** – Uses `slot_rw_*` C API; keep this coverage.

Any reorganization or refactor must retain (or replace with equivalent) this C API test coverage.

---

## Directory Structure

```
tests/
├── test_layer0_platform/          # Platform abstraction tests (existing)
├── test_layer1_base/               # Base utility tests (existing)
├── test_layer2_service/            # Service layer tests (existing)
├── test_layer3_datahub/            # DataHub tests (REORGANIZE)
│   ├── c_api/                      # NEW: C-level API tests
│   │   ├── test_c_datablock_basic.cpp
│   │   ├── test_c_datablock_error_paths.cpp
│   │   └── workers/
│   │       └── c_datablock_workers.cpp
│   │
│   ├── cpp_primitive/              # NEW: C++ primitive handle tests
│   │   ├── test_slot_protocol.cpp                    (MOVE from parent)
│   │   ├── test_datablock_mutex.cpp                  (MOVE from parent)
│   │   ├── test_recovery_api.cpp                     (MOVE from parent)
│   │   ├── test_error_handling.cpp                   (MOVE from parent)
│   │   ├── test_phase_a_protocol.cpp                 (MOVE from parent)
│   │   └── workers/
│   │       ├── slot_protocol_workers.cpp             (MOVE from parent)
│   │       ├── recovery_workers.cpp                  (MOVE from parent)
│   │       └── ...
│   │
│   ├── cpp_schema/                 # NEW: C++ schema-aware tests
│   │   ├── test_schema_blds.cpp                      (MOVE from parent)
│   │   ├── test_schema_validation.cpp                (MOVE from parent)
│   │   ├── test_dual_schema_integration.cpp          (NEW)
│   │   └── workers/
│   │       ├── schema_blds_workers.cpp               (MOVE from parent)
│   │       └── schema_validation_workers.cpp         (MOVE from parent)
│   │
│   ├── cpp_raii/                   # NEW: C++ RAII transaction layer tests
│   │   ├── test_raii_transaction.cpp                 (RENAME from test_transaction_api.cpp)
│   │   ├── test_raii_exception_safety.cpp            (NEW - split from above)
│   │   ├── test_raii_iterator.cpp                    (NEW - split from above)
│   │   └── workers/
│   │       └── raii_transaction_workers.cpp          (RENAME from transaction_api_workers.cpp)
│   │
│   ├── facility/                   # NEW: Facility class tests
│   │   ├── test_datablock_config.cpp                 (NEW)
│   │   ├── test_header_structure.cpp                 (NEW)
│   │   └── test_metrics.cpp                          (extract from various)
│   │
│   ├── integration/                # NEW: Integration & multiprocess tests
│   │   ├── test_message_hub.cpp                      (MOVE from parent)
│   │   ├── test_multiprocess_stress.cpp              (NEW)
│   │   └── workers/
│   │       └── messagehub_workers.cpp                (MOVE from parent)
│   │
│   └── test_patterns.h             # Shared test utilities (keep)
│       shared_test_helpers.h        # Shared test utilities (keep)
│
└── test_raii_layer/                # Existing RAII-specific tests
    └── test_result.cpp              # Keep as-is
```

---

## Naming Conventions

### Test File Names

**Format**: `test_<level>_<component>_<category>.cpp`

**Levels**:
- `c_` - C-API level
- `cpp_` - C++ (when level ambiguous, omit prefix)
- (no prefix for single-level components)

**Categories** (suffix):
- (none) - Normal usage tests
- `_error` - Error path tests
- `_stress` - Stress/load tests
- `_multiprocess` - Cross-process tests
- `_racing` - Race condition tests

**Examples**:
- `test_c_datablock_basic.cpp` - C-API basic operations
- `test_c_datablock_error_paths.cpp` - C-API error handling
- `test_slot_protocol.cpp` - C++ primitive slot operations
- `test_raii_transaction.cpp` - RAII transaction API
- `test_schema_validation.cpp` - Schema validation (level implied)

### Test Case Names

**Format**: `Level_Component_Scenario_ExpectedResult`

**Examples**:
```cpp
// C-API tests
TEST(CApi_DataBlock, CreateProducer_ValidConfig_Success)
TEST(CApi_DataBlock, CreateProducer_InvalidConfig_ReturnsError)

// C++ Primitive tests
TEST(Primitive_SlotProtocol, AcquireWriteSlot_WhenAvailable_Success)
TEST(Primitive_SlotProtocol, AcquireWriteSlot_WhenFull_Timeout)

// C++ Schema-Aware tests
TEST(Schema_Validation, ConsumerAttach_SchemaMismatch_Throws)
TEST(Schema_DualValidation, FlexZoneMismatch_DataBlockMatch_Throws)

// RAII Layer tests
TEST(RAII_Transaction, WriteTransaction_NormalFlow_Success)
TEST(RAII_Exception, WriteTransaction_ThrowBeforeCommit_AutoCleanup)
```

### Worker File Names

**Format**: `<component>_<category>_workers.cpp`

**Examples**:
- `c_datablock_workers.cpp`
- `slot_protocol_workers.cpp`
- `raii_transaction_workers.cpp`
- `schema_validation_workers.cpp`

---

## Test Categories by Directory

### c_api/
**Purpose**: Test C-level API (typeless, error codes)  
**Standards**: Must follow C-API standards (see API_SURFACE_DOCUMENTATION.md)  
**Coverage**:
- ✅ Normal usage
- ✅ Error paths (all error codes)
- ✅ NULL pointer handling
- ✅ Size validation
- ✅ Resource cleanup

**Example Tests**:
```cpp
TEST(CApi_DataBlock, CreateProducer_ValidConfig_ReturnsNonNull)
TEST(CApi_DataBlock, CreateProducer_NullHub_ReturnsNull)
TEST(CApi_Slot, AcquireWrite_ValidProducer_ReturnsHandle)
TEST(CApi_Slot, AcquireWrite_InvalidTimeout_ReturnsError)
```

---

### cpp_primitive/
**Purpose**: Test C++ primitive handles (DataBlockProducer/Consumer, Slot handles)  
**API Level**: Level 1 (Primitive Handles)  
**Coverage**:
- ✅ Direct slot acquisition/release
- ✅ Handle lifetime management
- ✅ Cross-process coordination
- ✅ Checksum validation
- ✅ Recovery scenarios
- ✅ Racing conditions

**Example Tests**:
```cpp
TEST(Primitive_Slot, AcquireWriteSlot_AfterTimeout_Success)
TEST(Primitive_Slot, WriteSlot_ChecksumEnforced_AutoVerified)
TEST(Primitive_Recovery, ProducerCrash_ConsumerDetects)
TEST(Primitive_Racing, ConcurrentWrites_SingleReader_Ordered)
```

**Test Strategy Documentation Required**:
- Multiprocess tests MUST document process roles and sequencing
- Racing tests MUST document expected race conditions
- Stress tests MUST be tunable via environment variable

---

### cpp_schema/
**Purpose**: Test C++ schema-aware factory functions  
**API Level**: Level 3 (Schema-Aware Factory)  
**Coverage**:
- ✅ Dual-schema generation (FlexZone + DataBlock)
- ✅ Schema hash computation (BLAKE2b)
- ✅ Schema validation at consumer attachment
- ✅ Schema version compatibility
- ✅ Error messages (clear identification of which schema failed)

**Example Tests**:
```cpp
TEST(Schema_DualGeneration, CreateProducer_BothSchemas_StoredCorrectly)
TEST(Schema_Validation, ConsumerAttach_FlexZoneMismatch_ThrowsWithClearError)
TEST(Schema_Validation, ConsumerAttach_DataBlockMismatch_ThrowsWithClearError)
TEST(Schema_Validation, ConsumerAttach_BothMatch_Success)
TEST(Schema_BLDS, GenerateSchemaInfo_EmptyStruct_ValidHash)
TEST(Schema_BLDS, GenerateSchemaInfo_MaxFields_NoOverflow)
```

---

### cpp_raii/
**Purpose**: Test C++ RAII transaction layer  
**API Level**: Level 4 (RAII Transaction Layer)  
**Coverage**:
- ✅ `with_transaction<FlexZoneT, DataBlockT>()` API
- ✅ Automatic resource management (RAII)
- ✅ Exception safety (cleanup on throw)
- ✅ Non-terminating iterator (`ctx.slots()`)
- ✅ Typed access (`ctx.flexzone()`, `slot.content()`)
- ✅ Result<T, E> error handling

**Example Tests**:
```cpp
TEST(RAII_Transaction, WriteTransaction_BasicFlow_Success)
TEST(RAII_Transaction, ReadTransaction_IterateSlots_Success)
TEST(RAII_Exception, WriteTransaction_ThrowBeforeCommit_SlotReleased)
TEST(RAII_Exception, ReadTransaction_ThrowDuringRead_SlotReleased)
TEST(RAII_Iterator, NonTerminating_Timeout_ReturnsError)
TEST(RAII_Iterator, NonTerminating_MultipleSlots_ReturnsResults)
```

**Special Requirements**:
- All tests must verify exception safety
- All tests must verify resource cleanup
- Tests must use dual-schema types (TestFlexZone + TestMessage)

---

### facility/
**Purpose**: Test facility classes (Config, Header, Metrics)  
**Coverage**:
- ✅ DataBlockConfig validation (all error paths)
- ✅ SharedMemoryHeader structure (size, alignment, fields)
- ✅ Metrics tracking (counters, heartbeats)

**Example Tests**:
```cpp
// Config tests
TEST(Facility_Config, FlexZoneSize_NotMultipleOf4K_Throws)
TEST(Facility_Config, LogicalUnitSize_LessThanPhysical_Throws)
TEST(Facility_Config, EffectiveLogicalUnitSize_DefaultsToPhysical)

// Header tests
TEST(Facility_Header, DualSchemaFields_InitializedCorrectly)
TEST(Facility_Header, Size_Is4096Bytes_RuntimeCheck)
TEST(Facility_Header, SchemaHashesStored_MatchGenerated)

// Metrics tests
TEST(Facility_Metrics, SchemaMismatchCount_IncrementedOnFailure)
TEST(Facility_Metrics, Heartbeat_RegisterUnregister_CountCorrect)
```

---

### integration/
**Purpose**: Test end-to-end scenarios, multiprocess, MessageHub integration  
**Coverage**:
- ✅ MessageHub broker registration/discovery
- ✅ Multiprocess producer/consumer coordination
- ✅ Stress tests (tunable load levels)
- ✅ Cross-process schema validation

**Example Tests**:
```cpp
TEST(Integration_Hub, RegisterProducer_DiscoverConsumer_Success)
TEST(Integration_Multiprocess, ThreeProducers_FiveConsumers_AllDataReceived)
TEST(Integration_Stress, HighLoad_NoDataLoss) // STRESS_LEVEL env var
TEST(Integration_Schema, CrossProcess_SchemaMismatch_ConsumerFails)
```

**Test Strategy Requirements**:
- MUST document process count and roles
- MUST document synchronization strategy
- MUST document expected results and verification method
- MUST support tunable stress levels

---

## Migration Plan

### Phase 1: Create Directory Structure
```bash
cd tests/test_layer3_datahub
mkdir -p c_api/workers
mkdir -p cpp_primitive/workers
mkdir -p cpp_schema/workers
mkdir -p cpp_raii/workers
mkdir -p facility
mkdir -p integration/workers
```

### Phase 2: Move Existing Files
```bash
# Primitive tests
mv test_slot_protocol.cpp cpp_primitive/
mv test_datablock_mutex.cpp cpp_primitive/
mv test_recovery_api.cpp cpp_primitive/
mv test_error_handling.cpp cpp_primitive/
mv test_phase_a_protocol.cpp cpp_primitive/
mv workers/slot_protocol_workers.cpp cpp_primitive/workers/
mv workers/recovery_workers.cpp cpp_primitive/workers/
# ... (move other workers)

# Schema tests
mv test_schema_blds.cpp cpp_schema/
mv test_schema_validation.cpp cpp_schema/
mv workers/schema_blds_workers.cpp cpp_schema/workers/
mv workers/schema_validation_workers.cpp cpp_schema/workers/

# RAII tests
mv test_transaction_api.cpp cpp_raii/test_raii_transaction.cpp
mv workers/transaction_api_workers.cpp cpp_raii/workers/raii_transaction_workers.cpp

# Integration tests
mv test_message_hub.cpp integration/
mv workers/messagehub_workers.cpp integration/workers/
```

### Phase 3: Update CMakeLists.txt
Update `tests/test_layer3_datahub/CMakeLists.txt` to reflect new structure:
```cmake
# C-API tests
add_subdirectory(c_api)

# C++ Primitive tests
add_subdirectory(cpp_primitive)

# C++ Schema-Aware tests
add_subdirectory(cpp_schema)

# C++ RAII tests
add_subdirectory(cpp_raii)

# Facility tests
add_subdirectory(facility)

# Integration tests
add_subdirectory(integration)
```

### Phase 4: Update Test Content
- Fix dual-schema issues
- Add missing tests (config validation, header verification)
- Improve documentation

---

## Test Documentation Standards

Every test file MUST start with:

```cpp
/**
 * @file test_<name>.cpp
 * @brief <one-line description>
 * 
 * @test_level [C-API | C++ Primitive | C++ Schema-Aware | C++ RAII | Facility | Integration]
 * @api_version v1.0.0
 * 
 * Test Coverage:
 * - Normal usage: <YES|NO|PARTIAL>
 * - Corner cases: <YES|NO|PARTIAL>
 * - Error paths: <YES|NO|PARTIAL>
 * - Racing conditions: <YES|NO|N/A>
 * - Stress testing: <YES|NO|N/A>
 * 
 * Dependencies:
 * - <list external dependencies: processes, files, timing>
 */
```

Every multiprocess test MUST document:

```cpp
/**
 * @test TestName
 * @brief One-line description
 * 
 * @test_strategy Multiprocess Test
 * 
 * Process A (Role):
 *   1. Step 1
 *   2. Step 2
 *   3. ...
 * 
 * Process B (Role):
 *   1. Step 1
 *   2. Step 2
 *   3. ...
 * 
 * Race Conditions:
 *   - Describe expected races
 * 
 * Expected Result:
 *   - Process A: <exit code, output>
 *   - Process B: <exit code, output>
 * 
 * Verification:
 *   - How result is verified
 * 
 * Stress Level: <None | Tunable via STRESS_LEVEL env var>
 */
```

---

## Benefits of This Structure

1. **Clarity**: Clear separation by API level and purpose
2. **Discoverability**: Easy to find relevant tests
3. **Maintainability**: Related tests grouped together
4. **Consistency**: Naming conventions enforced
5. **Documentation**: Required documentation ensures understanding
6. **Scalability**: Easy to add new test categories

---

## API Issue Found During Organization

**Issue**: `find_datablock_consumer<FlexZoneT, DataBlockT>(..., NO config)` overload exists  
**Location**: `data_block.hpp:1486-1503`  
**Problem**: Validates schemas but NOT config (size, alignment, capacity) - dangerous!  
**Solution**: Remove this overload - users should use non-template version if they want no validation  
**Action Required**: Fix in API cleanup pass

---

**Document Control**  
Created: 2026-02-15  
Status: Proposed  
Approval Required: Yes  
Implementation: Pending user approval
