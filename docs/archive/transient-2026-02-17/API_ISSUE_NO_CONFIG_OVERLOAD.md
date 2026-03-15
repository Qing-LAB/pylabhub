# API Issue: Dangerous No-Config Consumer Overload

**Issue ID**: API-ISSUE-001  
**Severity**: HIGH  
**Date Found**: 2026-02-15  
**Status**: NEEDS FIX

## Problem Description

The template `find_datablock_consumer<FlexZoneT, DataBlockT>()` has a dangerous overload that validates schemas but NOT configuration.

### Current API (Problematic)

```cpp
// Location: data_block.hpp:1486-1503

// This overload validates SCHEMAS but NOT CONFIG - DANGEROUS!
template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret)
{
    // Generates and validates schema hashes
    auto expected_flexzone = generate_schema_info<FlexZoneT>(...);
    auto expected_datablock = generate_schema_info<DataBlockT>(...);
    
    // But passes nullptr for config - NO SIZE/ALIGNMENT VALIDATION!
    return find_datablock_consumer_impl(hub, name, shared_secret, nullptr,
                                        &expected_flexzone, &expected_datablock);
}
```

### Why This Is Dangerous

1. **Schema validated, but not sizes**: Consumer could attach with correct schemas but wrong size expectations
2. **Buffer overflow risk**: If slot_size differs, consumer could read/write beyond bounds
3. **Alignment mismatch**: No verification of physical_page_size or logical_unit_size
4. **Capacity mismatch**: No verification of ring_buffer_capacity
5. **Defeats type safety**: The whole point of template API is type+size safety!

**Example Failure Scenario**:
```cpp
// Producer
DataBlockConfig config;
config.logical_unit_size = 8192;  // 8K slots
auto prod = create_datablock_producer<FlexZone, Message>(hub, name, policy, config);

// Consumer (using dangerous no-config overload)
auto cons = find_datablock_consumer<FlexZone, Message>(hub, name, secret);
// ^^^ Schema validated (OK), but consumer thinks slots might be 4K!
// If consumer assumes 4K slot and accesses beyond, BUFFER OVERFLOW!
```

## Comparison with Producer API

**Producer API** (correct):
```cpp
// Producer ALWAYS requires config - NO dangerous overload
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, 
                          DataBlockPolicy policy,
                          const DataBlockConfig &config);  // Config REQUIRED
```

Why is this correct? Because producer CREATES the shared memory, so config is mandatory.

**Consumer API** (inconsistent):
```cpp
// Overload 1: With config - SAFE
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(..., const DataBlockConfig &expected_config);  // GOOD

// Overload 2: Without config - DANGEROUS
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(...);  // BAD - schemas validated but not sizes!
```

## Proposed Solution

### Option 1: Remove Dangerous Overload (Recommended)

**Remove** the no-config template overload entirely:

```cpp
// REMOVE THIS:
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret);
```

**Rationale**:
- If user wants schema validation, they MUST provide config (safe)
- If user wants no validation, use non-template version (explicitly unsafe, consistent)
- No "half-validated" state (schemas OK but sizes wrong)

**Impact**:
- Breaking change: Code using this overload will fail to compile
- Easy migration: Add config parameter (should have been there anyway)

**Migration**:
```cpp
// OLD (compiles but dangerous):
auto consumer = find_datablock_consumer<FlexZone, Message>(hub, name, secret);

// NEW (required):
DataBlockConfig expected_config = /* get from producer or known values */;
auto consumer = find_datablock_consumer<FlexZone, Message>(hub, name, secret, expected_config);
```

### Option 2: Make Config Mandatory for Templates (Alternative)

Keep one template overload with config required:

```cpp
// ONLY overload for schema-aware API
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config);  // Config REQUIRED
```

No no-config version exists for templates.

### Option 3: Validate Config in No-Config Overload (Least Safe)

Keep the overload but read config from shared memory and validate:

```cpp
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret)
{
    // Read actual config from shared memory header
    // Validate against sizeof(FlexZoneT) and sizeof(DataBlockT)
    // Throw if sizes don't fit
    ...
}
```

**Problems**:
- Consumer doesn't know what config to EXPECT (can't verify match)
- Can only check "does my type fit?" not "does config match producer?"
- Less useful validation

## Recommendation

**Remove the dangerous overload (Option 1).**

API should be:

**Schema-Aware API** (config REQUIRED):
```cpp
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, 
                        uint64_t shared_secret,
                        const DataBlockConfig &expected_config);  // Config required for safety
```

**Non-Schema API** (config optional, explicitly unsafe):
```cpp
// With config validation, no schema validation
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, 
                        uint64_t shared_secret,
                        const DataBlockConfig &expected_config);

// NO validation at all (explicitly unsafe, for prototyping)
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, 
                        uint64_t shared_secret);
```

This gives clear separation:
- **Template API = Full validation** (schemas + config)
- **Non-template API = Choose your validation level** (config only, or none)

No dangerous "half-validated" middle ground.

## Files to Fix

1. `/home/qqing/Work/pylabhub/cpp/src/include/utils/data_block.hpp`
   - Lines 1486-1503: **DELETE** this template overload

2. `/home/qqing/Work/pylabhub/cpp/docs/API_SURFACE_DOCUMENTATION.md`
   - Update to reflect single template signature

3. Any tests using the dangerous overload:
   - Update to pass config parameter

## Testing Required After Fix

1. Verify template API requires config (compile test)
2. Verify non-template API allows no config (for backward compat)
3. Update all tests to use safe API

---

**Status**: Identified, solution proposed  
**Priority**: HIGH (API safety issue)  
**Breaking**: YES (removes unsafe overload)  
**Justification**: Safety > convenience  
**Approval**: Pending
