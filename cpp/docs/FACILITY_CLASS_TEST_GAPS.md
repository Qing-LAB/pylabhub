# Facility Class Test Gaps - Addendum

**Document ID**: PLAN-TEST-002  
**Version**: 1.0.0  
**Date**: 2026-02-15  
**Parent**: TEST_MODERNIZATION_PLAN.md

## Executive Summary

Comprehensive review of facility class tests (Schema, Config, Header, Metrics) reveals critical gaps in dual-schema testing and config validation coverage.

**Critical Findings**:
- Schema tests use deprecated single-schema API
- No dedicated config validation error path tests
- Header dual-schema fields not verified
- Metrics coverage is good

---

## Detailed Findings by Facility

### 1. Schema System (BLDS)

#### test_schema_blds.cpp

**Status**: NEEDS_UPDATE  
**Priority**: P0 (Critical)  
**Effort**: 2-3 hours

**Current Coverage**:
- ✅ BLDSTypeID mapping
- ✅ BLDSBuilder API
- ✅ SchemaVersion pack/unpack
- ✅ generate_schema_info
- ✅ Schema hash computation (BLAKE2b)
- ❌ Dual-schema generation
- ❌ Edge cases (empty struct, max fields, nested types)

**Issues**:
- Tests only single-schema API (`generate_schema_info<T>()`)
- No tests for `flexzone_schema_hash` vs `datablock_schema_hash`
- No verification that both hashes are stored in header
- Missing edge case testing

**Required Test Cases**:
```cpp
TEST_CASE("DualSchema_GenerateBothHashes_Correctly") {
    auto flex_schema = generate_schema_info<FlexZoneMetadata>(...);
    auto data_schema = generate_schema_info<Message>(...);
    
    REQUIRE(flex_schema.hash.size() == 32);
    REQUIRE(data_schema.hash.size() == 32);
    REQUIRE(flex_schema.hash != data_schema.hash); // Different structures
}

TEST_CASE("DualSchema_BothHashesStoredInHeader") {
    auto producer = create_datablock_producer<FlexZone, Message>(...);
    auto *header = get_test_header(producer);
    
    // Verify both hashes are non-zero
    REQUIRE(has_non_zero_bytes(header->flexzone_schema_hash, 32));
    REQUIRE(has_non_zero_bytes(header->datablock_schema_hash, 32));
}

TEST_CASE("DualSchema_ValidateBothSchemas_BothMustMatch") {
    // Producer with FlexZoneA, MessageA
    auto producer = create_datablock_producer<FlexZoneA, MessageA>(...);
    
    // Consumer with FlexZoneA, MessageA - should succeed
    REQUIRE_NOTHROW(find_datablock_consumer<FlexZoneA, MessageA>(...));
    
    // Consumer with FlexZoneB, MessageA - should fail (flexzone mismatch)
    REQUIRE_THROWS_AS(
        find_datablock_consumer<FlexZoneB, MessageA>(...),
        SchemaValidationException
    );
    
    // Consumer with FlexZoneA, MessageB - should fail (datablock mismatch)
    REQUIRE_THROWS_AS(
        find_datablock_consumer<FlexZoneA, MessageB>(...),
        SchemaValidationException
    );
}

TEST_CASE("EdgeCase_EmptyFlexZoneSchema_IsValid") {
    struct EmptyFlexZone {};
    PYLABHUB_SCHEMA_BEGIN(EmptyFlexZone)
    PYLABHUB_SCHEMA_END(EmptyFlexZone)
    
    auto schema = generate_schema_info<EmptyFlexZone>(...);
    REQUIRE(schema.hash.size() == 32); // Still generates valid hash
}

TEST_CASE("EdgeCase_MaxFields_DoesNotOverflow") {
    // Test struct with many fields (e.g., 100+)
    // Verify hash generation succeeds
}

TEST_CASE("EdgeCase_NestedStructs_HashedCorrectly") {
    struct Inner { int x; };
    struct Outer { Inner inner; int y; };
    
    // Verify schema generation handles nesting
}
```

---

#### test_schema_validation.cpp

**Status**: NEEDS_UPDATE  
**Priority**: P0 (Critical)  
**Effort**: 2-3 hours

**Issues**:
- **Line 57-59**: Uses deprecated `create_datablock_producer(..., schema_v1)`
- **Line 62-63**: Uses deprecated `find_datablock_consumer(..., schema_v1)`
- Only validates single schema, not dual-schema architecture

**Actions Required**:
1. Replace all factory calls with dual-schema templates
2. Add separate flexzone/datablock mismatch tests
3. Verify error messages identify which schema mismatched

**Migration Example**:
```cpp
// OLD (Line 57-59):
auto producer = create_datablock_producer(hub_ref, channel,
                                          DataBlockPolicy::RingBuffer, config,
                                          schema_v1);

// NEW:
auto producer = create_datablock_producer<FlexZoneV1, SchemaV1>(
    hub_ref, channel, DataBlockPolicy::RingBuffer, config);
```

**Required Test Cases**:
```cpp
TEST_CASE("DualSchema_ConsumerFailsWithFlexZoneMismatch") {
    struct FlexZoneV1 { int counter; };
    struct FlexZoneV2 { int counter; float time; }; // Different!
    struct Message { uint64_t id; };
    
    auto producer = create_datablock_producer<FlexZoneV1, Message>(...);
    
    // Consumer with different flexzone type - should fail
    REQUIRE_THROWS_AS(
        find_datablock_consumer<FlexZoneV2, Message>(...),
        SchemaValidationException
    );
    
    // Verify error message mentions flexzone
    try {
        find_datablock_consumer<FlexZoneV2, Message>(...);
        FAIL("Should have thrown");
    } catch (const SchemaValidationException &e) {
        std::string msg = e.what();
        REQUIRE(msg.find("FlexZone") != std::string::npos);
        REQUIRE(msg.find("mismatch") != std::string::npos);
    }
}

TEST_CASE("DualSchema_ConsumerFailsWithDataBlockMismatch")
TEST_CASE("DualSchema_ConsumerFailsWithBothMismatch")
TEST_CASE("DualSchema_ConsumerSucceedsWhenBothMatch")
TEST_CASE("DualSchema_ErrorMessageIdentifiesWhichSchemaMismatched")
```

---

### 2. Config System (DataBlockConfig)

#### NEW FILE: test_datablock_config_validation.cpp

**Status**: MISSING  
**Priority**: P1 (High)  
**Effort**: 2-3 hours

**Justification**:
- Config validation code exists (`data_block.cpp:697-699, 908-928, 978-988`)
- Error paths are not exercised in any tests
- Critical for ensuring invalid configs are rejected early

**Required Test Cases**:
```cpp
TEST_CASE("ConfigValidation_FlexZoneSize_NotMultipleOf4K_Throws") {
    DataBlockConfig config;
    config.name = "test";
    config.physical_page_size = DataBlockPageSize::Size4K;
    config.ring_buffer_capacity = 10;
    config.policy = DataBlockPolicy::RingBuffer;
    config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
    
    // Invalid: not multiple of 4096
    config.flex_zone_size = 4097;
    
    REQUIRE_THROWS_AS(
        create_datablock_producer<void, Message>(hub, "test", policy, config),
        std::invalid_argument
    );
    
    // Verify error message mentions 4096 alignment
    try {
        create_datablock_producer<void, Message>(hub, "test", policy, config);
        FAIL("Should have thrown");
    } catch (const std::invalid_argument &e) {
        std::string msg = e.what();
        REQUIRE(msg.find("4096") != std::string::npos);
        REQUIRE(msg.find("flex_zone_size") != std::string::npos);
    }
}

TEST_CASE("ConfigValidation_FlexZoneSize_Zero_IsValid") {
    config.flex_zone_size = 0; // No flexzone
    
    // Should succeed
    REQUIRE_NOTHROW(
        create_datablock_producer<void, Message>(hub, "test", policy, config)
    );
}

TEST_CASE("ConfigValidation_FlexZoneSize_4096_IsValid") {
    config.flex_zone_size = 4096;
    REQUIRE_NOTHROW(...);
}

TEST_CASE("ConfigValidation_FlexZoneSize_8192_IsValid") {
    config.flex_zone_size = 8192;
    REQUIRE_NOTHROW(...);
}

TEST_CASE("ConfigValidation_LogicalUnitSize_NotMultipleOfPhysical_Throws") {
    config.physical_page_size = DataBlockPageSize::Size4K; // 4096
    config.logical_unit_size = 5000; // Not multiple of 4096
    
    REQUIRE_THROWS_AS(..., std::invalid_argument);
}

TEST_CASE("ConfigValidation_LogicalUnitSize_LessThanPhysical_Throws") {
    config.physical_page_size = DataBlockPageSize::Size4K; // 4096
    config.logical_unit_size = 2048; // Less than physical
    
    REQUIRE_THROWS_AS(..., std::invalid_argument);
}

TEST_CASE("ConfigValidation_LogicalUnitSize_Zero_DefaultsToPhysical") {
    config.physical_page_size = DataBlockPageSize::Size4K;
    config.logical_unit_size = 0; // Default to physical
    
    auto producer = create_datablock_producer<void, Message>(...);
    
    // Should use physical_page_size (4096)
    // Verify via slot size check
}

TEST_CASE("ConfigValidation_RingBufferCapacity_Zero_Throws") {
    config.ring_buffer_capacity = 0;
    REQUIRE_THROWS_AS(..., std::invalid_argument);
}

TEST_CASE("ConfigValidation_PhysicalPageSize_Unset_Throws") {
    config.physical_page_size = DataBlockPageSize::Unset;
    REQUIRE_THROWS_AS(..., std::invalid_argument);
}

TEST_CASE("ConfigValidation_Policy_Unset_Throws") {
    config.policy = DataBlockPolicy::Unset;
    REQUIRE_THROWS_AS(..., std::invalid_argument);
}

TEST_CASE("ConfigValidation_ConsumerSyncPolicy_Unset_Throws") {
    config.consumer_sync_policy = ConsumerSyncPolicy::Unset;
    REQUIRE_THROWS_AS(..., std::invalid_argument);
}

TEST_CASE("ConfigHelper_EffectiveLogicalUnitSize_DefaultsToPhysical") {
    config.physical_page_size = DataBlockPageSize::Size4K;
    config.logical_unit_size = 0;
    
    REQUIRE(config.effective_logical_unit_size() == 4096);
}

TEST_CASE("ConfigHelper_EffectiveLogicalUnitSize_UsesConfiguredValue") {
    config.physical_page_size = DataBlockPageSize::Size4K;
    config.logical_unit_size = 8192;
    
    REQUIRE(config.effective_logical_unit_size() == 8192);
}

TEST_CASE("ConfigHelper_StructuredBufferSize_CalculatesCorrectly") {
    config.ring_buffer_capacity = 10;
    config.logical_unit_size = 4096;
    
    REQUIRE(config.structured_buffer_size() == 10 * 4096);
}

TEST_CASE("ConfigHelper_StructuredBufferSize_WithZeroCapacity_UsesOne") {
    config.ring_buffer_capacity = 0;
    config.logical_unit_size = 4096;
    
    // Should treat as 1 slot minimum
    REQUIRE(config.structured_buffer_size() == 4096);
}
```

**Validation Code References** (to ensure coverage):
- `data_block.cpp:697-699` - flex_zone_size alignment check
- `data_block.cpp:908-913` - policy validation
- `data_block.cpp:917-922` - consumer_sync_policy validation
- `data_block.cpp:927-928` - physical_page_size validation
- `data_block.cpp:978-988` - logical_unit_size validation
- `data_block.cpp:991` - ring_buffer_capacity validation

---

### 3. Header Structure (SharedMemoryHeader)

#### Add to test_slot_protocol.cpp or NEW test_header_structure.cpp

**Status**: MISSING  
**Priority**: P1 (High)  
**Effort**: 1-2 hours

**Current Coverage**:
- ✅ Header access via diagnostic API
- ✅ Header initialization (implicit)
- ✅ Layout checksum validation
- ❌ Dual-schema field verification
- ❌ Runtime size/alignment checks

**Required Test Cases**:
```cpp
TEST_CASE("Header_DualSchemaFields_InitializedCorrectly") {
    struct FlexZoneMetadata { int counter; };
    struct Message { uint64_t id; };
    
    PYLABHUB_SCHEMA_BEGIN(FlexZoneMetadata)
        PYLABHUB_SCHEMA_MEMBER(counter)
    PYLABHUB_SCHEMA_END(FlexZoneMetadata)
    
    PYLABHUB_SCHEMA_BEGIN(Message)
        PYLABHUB_SCHEMA_MEMBER(id)
    PYLABHUB_SCHEMA_END(Message)
    
    auto producer = create_datablock_producer<FlexZoneMetadata, Message>(...);
    
    // Need test accessor to get header pointer
    auto *header = get_header_for_testing(producer.get());
    
    // Verify flexzone_schema_hash is non-zero
    bool has_flexzone = std::any_of(
        header->flexzone_schema_hash,
        header->flexzone_schema_hash + 32,
        [](uint8_t byte) { return byte != 0; }
    );
    REQUIRE(has_flexzone);
    
    // Verify datablock_schema_hash is non-zero
    bool has_datablock = std::any_of(
        header->datablock_schema_hash,
        header->datablock_schema_hash + 32,
        [](uint8_t byte) { return byte != 0; }
    );
    REQUIRE(has_datablock);
    
    // Verify hashes are different (not duplicated)
    REQUIRE(std::memcmp(header->flexzone_schema_hash,
                        header->datablock_schema_hash, 32) != 0);
}

TEST_CASE("Header_DualSchemaFields_MatchExpectedHashes") {
    auto flex_schema = generate_schema_info<FlexZoneMetadata>(...);
    auto data_schema = generate_schema_info<Message>(...);
    
    auto producer = create_datablock_producer<FlexZoneMetadata, Message>(...);
    auto *header = get_header_for_testing(producer.get());
    
    // Verify stored hashes match generated hashes
    REQUIRE(std::memcmp(header->flexzone_schema_hash,
                        flex_schema.hash.data(), 32) == 0);
    REQUIRE(std::memcmp(header->datablock_schema_hash,
                        data_schema.hash.data(), 32) == 0);
}

TEST_CASE("Header_SchemaVersion_StoredCorrectly") {
    auto producer = create_datablock_producer<FlexZone, Message>(...);
    auto *header = get_header_for_testing(producer.get());
    
    // Verify schema_version is set (not zero)
    REQUIRE(header->schema_version != 0);
}

TEST_CASE("Header_Size_Is4096Bytes_RuntimeCheck") {
    // Runtime verification of compile-time static_assert
    REQUIRE(sizeof(SharedMemoryHeader) == 4096);
    
    // Also verify via offsetof and reserved_header size
    size_t calculated_size = 
        offsetof(SharedMemoryHeader, reserved_header) + 
        sizeof(SharedMemoryHeader::reserved_header);
    REQUIRE(calculated_size == 4096);
}

TEST_CASE("Header_Alignment_Is4096Bytes_RuntimeCheck") {
    REQUIRE(alignof(SharedMemoryHeader) == 4096);
    
    // Verify actual instance alignment
    SharedMemoryHeader dummy;
    REQUIRE(reinterpret_cast<uintptr_t>(&dummy) % 4096 == 0);
}

TEST_CASE("Header_SchemaMacro_IncludesDualSchemaFields") {
    // Verify PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS includes both
    // This is a compile-time test - if the macro is wrong, data_block.cpp
    // won't compile. But we can verify at runtime by checking offsets.
    
    size_t flex_offset = offsetof(SharedMemoryHeader, flexzone_schema_hash);
    size_t data_offset = offsetof(SharedMemoryHeader, datablock_schema_hash);
    
    REQUIRE(flex_offset < data_offset); // flexzone comes before datablock
    REQUIRE(data_offset - flex_offset == 32); // 32 bytes apart
}
```

**Implementation Note**:
Need to add test accessor function (friend function or test-only API):
```cpp
// In data_block.hpp or test_helpers.hpp
namespace test {
    SharedMemoryHeader *get_header_for_testing(DataBlockProducer *producer);
}
```

---

### 4. Metrics System

**Status**: ✅ OK  
**Coverage**: Comprehensive

**Current Tests**:
- ✅ Metric counter increments
- ✅ Heartbeat registration/update/unregister
- ✅ Consumer/producer alive detection
- ✅ Error count tracking
- ✅ Diagnostic API (`is_writer_alive`, `is_process_alive`)

**Potential Additions**:
```cpp
TEST_CASE("Metrics_SchemaMismatchCount_IncrementedOnFailure") {
    // If schema_mismatch_count exists in SharedMemoryHeader
    auto producer = create_datablock_producer<FlexZoneA, MessageA>(...);
    auto *header = get_header_for_testing(producer.get());
    
    uint64_t initial_count = header->schema_mismatch_count.load();
    
    // Attempt to attach with mismatched schema
    try {
        find_datablock_consumer<FlexZoneB, MessageA>(...);
    } catch (...) {
        // Expected
    }
    
    // Verify count incremented
    REQUIRE(header->schema_mismatch_count.load() == initial_count + 1);
}

TEST_CASE("Metrics_ChecksumFailures_IncrementedOnFailure") {
    // If checksum_failures exists and is tracked separately
}
```

**Action Required**:
1. Verify if `schema_mismatch_count` field exists in `SharedMemoryHeader`
2. If yes, add test as shown above
3. Verify if `checksum_failures` is a separate metric
4. If yes, ensure it's tested in checksum validation tests

---

## Priority Summary

### P0 (Critical - Blocking)
1. ✅ `test_schema_blds.cpp` - Add dual-schema test cases (2-3h)
2. ✅ `test_schema_validation.cpp` - Update to dual-schema API (2-3h)

### P1 (High - Important)
3. ➕ Create `test_datablock_config_validation.cpp` - Config error paths (2-3h)
4. ➕ Add header dual-schema field tests to `test_slot_protocol.cpp` or new file (1-2h)

### P2 (Medium - Enhancement)
5. 🔄 Add edge case tests to `test_schema_blds.cpp` (empty, max fields, nested) (1h)
6. 🔄 Add metrics tests for schema_mismatch_count if it exists (30min)

**Total Effort**: 9-13 hours for facility class test modernization

---

## Integration with Main Test Plan

These facility class tests should be completed as part of **Phase 1 (Critical)** of the main test modernization plan, alongside the transaction API and schema validation updates.

**Updated Phase 1 Timeline**:
- Transaction API rewrite: 6 hours
- Schema validation update: 3 hours
- **Schema BLDS dual-schema tests: 3 hours** (NEW)
- **Config validation tests: 3 hours** (NEW)
- **Header field tests: 2 hours** (NEW)
- Testing and validation: 3 hours

**New Phase 1 Total**: 20 hours (2.5 days)

---

**Document Control**  
Created: 2026-02-15  
Last Modified: 2026-02-15  
Parent: TEST_MODERNIZATION_PLAN.md  
Maintained by: QA Team
