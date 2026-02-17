# Modernized BLDS Schema Integration for RAII Layer

**Date**: 2026-02-15  
**Purpose**: Design clean schema validation API for `with_transaction()` integration  
**Related**: `TYPE_SAFETY_AND_VALIDATION_FLOW.md`, Phase 3 RAII Layer

---

## Table of Contents

1. [Problem Statement](#problem-statement)
2. [Current State Analysis](#current-state-analysis)
3. [Design Goals](#design-goals)
4. [Proposed API Design](#proposed-api-design)
5. [Implementation Strategy](#implementation-strategy)
6. [Migration Path](#migration-path)
7. [Examples](#examples)

---

## Problem Statement

### Current Gap

The RAII layer (`with_transaction<FlexZoneT, DataBlockT>()`) does NOT validate BLDS schema hashes:

```cpp
// Current behavior: Only sizeof() validation, NO schema hash check
producer->with_transaction<FlexZone, Message>(timeout, [](auto &ctx) {
    // This compiles and runs even if FlexZone/Message don't match
    // the producer's registered schema!
});
```

### Issues

1. **Type aliasing vulnerability**: `with_transaction<WrongType, Message>()` succeeds if `sizeof(WrongType) == sizeof(Message)`
2. **No runtime schema validation**: Template types not compared against producer's stored BLDS hash
3. **Manual schema registration required**: Must call old API `create_datablock_producer<Schema>()` separately
4. **Inconsistent API**: Old schema API doesn't integrate with new RAII layer

---

## Current State Analysis

### Existing BLDS Infrastructure (✅ Working)

1. **Schema Macros** (`schema_blds.hpp`):
   ```cpp
   PYLABHUB_SCHEMA_BEGIN(SensorData)
       PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
       PYLABHUB_SCHEMA_MEMBER(temperature)
   PYLABHUB_SCHEMA_END(SensorData)
   ```

2. **Schema Generation**:
   ```cpp
   auto schema = pylabhub::schema::generate_schema_info<SensorData>(
       "SensorHub.SensorData",
       pylabhub::schema::SchemaVersion{1, 0, 0}
   );
   // schema.blds = "timestamp_ns:u64;temperature:f32"
   // schema.hash = BLAKE2b-256 hash (32 bytes)
   ```

3. **Schema Storage**:
   - Producer stores hash in `SharedMemoryHeader::schema_hash[32]`
   - Consumer validates on attach via `find_datablock_consumer<Schema>()`

### Old API (Schema-Aware, ✅ Working but Not RAII-Integrated)

```cpp
// Producer: Register schema
auto producer = create_datablock_producer<SensorData>(
    hub, "sensor", policy, config, SensorData{}
);

// Consumer: Validate schema on attach
auto consumer = find_datablock_consumer<SensorData>(
    hub, "sensor", secret, config, SensorData{}
);
```

**Problem**: This doesn't integrate with `with_transaction<FlexZone, Message>()`

---

## Design Goals

### Primary Goals

1. **Automatic schema validation** in `with_transaction()` without explicit macro registration
2. **Clean API**: One-line producer/consumer creation with schema inference
3. **Compile-time + runtime safety**: Template types validated against stored hash
4. **Backward compatible**: Don't break existing code

### Non-Goals

- Full C++ reflection (not available in C++20)
- Runtime schema evolution (deferred to future work)
- Schema registry/discovery (handled by MessageHub)

---

## Proposed API Design

### Option 1: Schema-Annotated Handles (Recommended)

**Concept**: Producer/Consumer handles carry schema type information.

#### API

```cpp
// ============================================================================
// Producer: Schema-aware handle creation
// ============================================================================

// Method 1: Explicit schema types (compile-time checked)
auto producer = create_datablock_producer_typed<FlexZone, Message>(
    hub, "sensor", policy, config
);
// Returns: ProducerHandle<FlexZone, Message>

// Method 2: Macro-based schema registration (existing)
auto producer = create_datablock_producer<SensorData>(
    hub, "sensor", policy, config, SensorData{}
);
// Returns: ProducerHandle<void, SensorData>


// ============================================================================
// Transaction: Automatic schema validation
// ============================================================================

producer->with_transaction(timeout, [](auto &ctx) {
    // Template types inferred from producer handle!
    // ctx type: WriteTransactionContext<FlexZone, Message>
    
    auto zone = ctx.flexzone();  // ZoneRef<FlexZone>
    for (auto &slot : ctx.slots(50ms)) {
        if (!slot.is_ok()) continue;
        auto &content = slot.content();  // SlotRef<Message>
        content.get().data = ...;
        ctx.commit();
    }
});

// Compile error: Wrong types
producer->with_transaction<WrongZone, Message>(timeout, lambda);
// ERROR: template argument mismatch with ProducerHandle<FlexZone, Message>
```

#### Type Safety

```cpp
template <typename FlexZoneT, typename DataBlockT>
class ProducerHandle
{
public:
    // Only allow matching template types
    template <typename Func>
    auto with_transaction(std::chrono::milliseconds timeout, Func &&func)
        -> std::invoke_result_t<Func, WriteTransactionContext<FlexZoneT, DataBlockT> &>
    {
        // Runtime validation: Compare template hash with stored hash
        validate_schema_on_first_use();
        
        WriteTransactionContext<FlexZoneT, DataBlockT> ctx(this, timeout);
        return std::forward<Func>(func)(ctx);
    }

private:
    std::once_flag m_schema_validated;
    
    void validate_schema_on_first_use()
    {
        std::call_once(m_schema_validated, [this]() {
            // Generate BLDS for FlexZoneT and DataBlockT
            auto flexzone_schema = generate_schema_info<FlexZoneT>(
                "FlexZone", SchemaVersion{1, 0, 0}
            );
            auto datablock_schema = generate_schema_info<DataBlockT>(
                "DataBlock", SchemaVersion{1, 0, 0}
            );
            
            // Compare with stored hash in SharedMemoryHeader
            auto *header = this->header();
            if (std::memcmp(header->schema_hash, datablock_schema.hash.data(), 32) != 0)
            {
                throw SchemaValidationException(
                    "with_transaction: DataBlock schema mismatch",
                    datablock_schema.hash,
                    header->schema_hash
                );
            }
        });
    }
};
```

### Option 2: Schema Validator Object (Alternative)

**Concept**: Separate schema validation from handle creation.

```cpp
// Create untyped producer (existing API)
auto producer = create_datablock_producer(hub, "sensor", policy, config);

// Register schema expectations
auto validator = SchemaValidator<FlexZone, Message>::for_producer(producer);

// Validated transaction (validator enforces types)
validator.with_transaction(timeout, [](auto &ctx) {
    // ctx type: WriteTransactionContext<FlexZone, Message>
    // ...
});
```

**Pros**: Doesn't require changing producer/consumer handle types  
**Cons**: Extra object, more verbose API

---

## Implementation Strategy

### Phase 1: Extend SharedMemoryHeader for Dual Schema Storage

**Current**: `schema_hash[32]` (single hash, typically for DataBlock slot type)

**Proposed**: Split into flexzone + datablock hashes

```cpp
struct SharedMemoryHeader
{
    // OLD (Phase 3):
    // uint8_t schema_hash[32];  // Single hash for DataBlock slot type
    
    // NEW (Phase 4):
    uint8_t flexzone_schema_hash[32];   // BLAKE2b-256 of FlexZone BLDS
    uint8_t datablock_schema_hash[32];  // BLAKE2b-256 of DataBlock slot BLDS
    
    // For void flexzone: all zeros
};
```

**Migration**: Use reserved bytes in header (already allocated for future use).

### Phase 2: Add Templated Producer/Consumer Creation

```cpp
// New API: Type-aware creation
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<ProducerHandle<FlexZoneT, DataBlockT>>
create_datablock_producer_typed(
    MessageHub &hub,
    const std::string &name,
    DataBlockPolicy policy,
    const DataBlockConfig &config
)
{
    // Generate schemas
    auto flexzone_schema = generate_schema_info<FlexZoneT>("FlexZone", {1, 0, 0});
    auto datablock_schema = generate_schema_info<DataBlockT>("DataBlock", {1, 0, 0});
    
    // Create producer and store hashes in header
    auto producer = std::make_unique<ProducerHandle<FlexZoneT, DataBlockT>>();
    producer->init(hub, name, policy, config, flexzone_schema, datablock_schema);
    
    return producer;
}
```

### Phase 3: Implement Lazy Schema Validation in `with_transaction()`

See Option 1 implementation above (using `std::call_once`).

### Phase 4: Update Examples and Documentation

---

## Migration Path

### Immediate (Phase 3 Complete)

1. ✅ Fix `raii_layer_example.cpp` checksum type error (CRC32 → BLAKE2b)
2. ✅ Document current limitation in examples
3. ✅ Add warning comment in `with_transaction()` Doxygen

### Short-Term (Phase 4)

1. Extend `SharedMemoryHeader` with dual schema hashes
2. Implement `create_datablock_producer_typed<FlexZone, Message>()`
3. Add lazy validation to `with_transaction()` (Solution 3 from previous doc)
4. Update `raii_layer_example.cpp` to demonstrate schema validation

### Long-Term (Phase 5+)

1. Automatic C++ reflection (when C++26 available)
2. Schema version negotiation (backward compatibility)
3. Schema registry service (distributed validation)

---

## Examples

### Example 1: Basic Schema-Validated Transaction

```cpp
// Define types with BLDS macros
struct FlexZoneMetadata
{
    std::atomic<bool> shutdown_flag;
    std::atomic<uint64_t> message_counter;
};

PYLABHUB_SCHEMA_BEGIN(FlexZoneMetadata)
    PYLABHUB_SCHEMA_MEMBER(shutdown_flag)
    PYLABHUB_SCHEMA_MEMBER(message_counter)
PYLABHUB_SCHEMA_END(FlexZoneMetadata)

struct Message
{
    uint64_t sequence_num;
    uint64_t timestamp_ns;
    uint32_t producer_id;
    uint32_t checksum;
    char payload[128];
};

PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(sequence_num)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(producer_id)
    PYLABHUB_SCHEMA_MEMBER(checksum)
    PYLABHUB_SCHEMA_MEMBER(payload)
PYLABHUB_SCHEMA_END(Message)

// Create schema-aware producer
auto producer = create_datablock_producer_typed<FlexZoneMetadata, Message>(
    hub, "message_hub", DataBlockPolicy::RingBuffer, config
);

// Transaction automatically validates schema
producer->with_transaction(100ms, [](auto &ctx) {
    // Compile-time: ctx type is WriteTransactionContext<FlexZoneMetadata, Message>
    // Runtime: Schema hash validated on first call (cached thereafter)
    
    auto zone = ctx.flexzone();
    for (auto &slot : ctx.slots(50ms)) {
        if (!slot.is_ok()) continue;
        auto &content = slot.content();
        content.get().sequence_num = zone.get().message_counter++;
        ctx.commit();
        break;
    }
});
```

### Example 2: Consumer with Schema Validation

```cpp
// Consumer attaches with same schema types
auto consumer = find_datablock_consumer_typed<FlexZoneMetadata, Message>(
    hub, "message_hub", secret, config
);

// Runtime validation: Compare consumer's expected schema hash with producer's
// Throws SchemaValidationException if mismatch

consumer->with_transaction(100ms, [](auto &ctx) {
    auto zone = ctx.flexzone();
    
    for (auto &slot : ctx.slots(50ms)) {
        if (!slot.is_ok()) {
            if (slot.error() == SlotAcquireError::Timeout) {
                if (zone.get().shutdown_flag) break;
                continue;
            }
            break;
        }
        
        auto &content = slot.content();
        
        // Validate read (checksums, staleness)
        if (!ctx.validate_read()) continue;
        
        const auto &msg = content.get();
        std::cout << "Received: seq=" << msg.sequence_num << std::endl;
    }
});
```

### Example 3: Manual Schema Building (Advanced)

For types without BLDS macro registration:

```cpp
// Manual BLDS construction
pylabhub::schema::BLDSBuilder builder;
builder.add_member("timestamp_ns", "u64", offsetof(Message, timestamp_ns), sizeof(uint64_t));
builder.add_member("temperature", "f32", offsetof(Message, temperature), sizeof(float));
std::string blds = builder.build();

// Create SchemaInfo
pylabhub::schema::SchemaInfo schema;
schema.name = "CustomMessage";
schema.blds = blds;
schema.struct_size = sizeof(Message);
schema.version = {1, 0, 0};
schema.compute_hash();

// Use with old API
auto producer = create_datablock_producer_impl(
    hub, "sensor", policy, config, &schema
);
```

---

## Checksum Type Issue (Bug Fix)

### Problem in `raii_layer_example.cpp`

```cpp
config.enable_checksum = ChecksumType::CRC32;  // ❌ WRONG: CRC32 doesn't exist
```

### Root Cause

- `ChecksumType` enum only has `BLAKE2b` and `Unset`
- CRC32 was never implemented (likely leftover from design phase)
- Example file used incorrect value

### Why No CRC32?

1. **Security**: CRC32 is NOT cryptographically secure (trivial to forge)
2. **Performance**: BLAKE2b is competitive with CRC32 for small messages (<1KB)
3. **Consistency**: Using one algorithm (BLAKE2b) simplifies implementation
4. **Future-proof**: BLAKE2b is modern, widely adopted, and quantum-resistant

### Fix

```cpp
// CORRECT:
config.checksum_type = ChecksumType::BLAKE2b;  // ✅ Only supported algorithm
config.checksum_policy = ChecksumPolicy::Enforced;  // Or Manual, or None
```

### Design Decision Record

**Decision**: DataHub uses **BLAKE2b exclusively** for all checksums (slot, flexzone, schema).

**Rationale**:
- **Security**: Cryptographic-grade integrity protection
- **Performance**: ~1 GB/sec on modern CPUs (libsodium optimized)
- **Simplicity**: One algorithm, one implementation, consistent everywhere
- **Standards compliance**: BLAKE2b is IETF-standardized (RFC 7693)

**Alternative considered**: CRC32
- **Rejected because**:
  - Not cryptographically secure (collision attacks trivial)
  - Marginal performance gain (~10-20% faster) not worth security risk
  - DataHub is designed for mission-critical systems requiring integrity guarantees

---

## Summary

### API Changes (Phase 4)

```diff
// OLD (Phase 3 - current)
auto producer = create_datablock_producer(hub, "name", policy, config);
producer->with_transaction<FlexZone, Message>(timeout, lambda);
// ⚠️ No schema hash validation

// NEW (Phase 4 - proposed)
auto producer = create_datablock_producer_typed<FlexZone, Message>(
    hub, "name", policy, config
);
producer->with_transaction(timeout, lambda);  // Types inferred from handle
// ✅ Automatic schema hash validation (lazy, cached)
```

### Benefits

1. **Type safety**: Compile-time template matching with handle types
2. **Schema validation**: Runtime BLDS hash comparison (one-time cost)
3. **Clean API**: No manual schema registration needed
4. **Performance**: Lazy validation with `std::call_once` (amortized zero-cost)
5. **Backward compatible**: Old API still works

### Recommendation

**Implement Phase 4 in next session**:
1. Fix checksum type bug in example
2. Extend SharedMemoryHeader for dual schema hashes
3. Implement `create_datablock_producer_typed<FlexZone, DataBlock>()`
4. Add lazy validation to `with_transaction()`
5. Update examples with full schema validation

---

## Appendix: BLDS Example Output

```cpp
struct Message
{
    uint64_t timestamp_ns;
    float temperature;
    float pressure;
    char device_id[16];
};

// BLDS (type-only):
"timestamp_ns:u64;temperature:f32;pressure:f32;device_id:u8[16]"

// BLDS (with layout):
"timestamp_ns:u64@0:8;temperature:f32@8:4;pressure:f32@12:4;device_id:u8[16]@16:16"

// BLAKE2b-256 hash (32 bytes):
0x7f3a2b1c... (hex)
```
