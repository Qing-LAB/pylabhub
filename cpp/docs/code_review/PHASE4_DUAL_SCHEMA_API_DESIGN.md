# Phase 4 Design: Dual-Schema API (FlexZone + DataBlock)

**Date**: 2026-02-15  
**Decision**: FlexZone is REQUIRED by design, must have equal treatment with DataBlock  
**Status**: Phase 4 API Design

---

## Core Principle

**FlexZone is not optional - it's a fundamental part of the architecture.**

Even if minimal (e.g., empty struct, single char), it must exist and be validated the same way as DataBlock.

---

## Phase 4 API Design

### New Producer Creation (Required Both Types)

```cpp
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(
    MessageHub &hub,
    const std::string &name,
    DataBlockPolicy policy,
    const DataBlockConfig &config
    // No schema instances needed - generated from template types
);
```

**Usage**:
```cpp
// Always specify BOTH types
auto producer = create_datablock_producer<FlexZoneMetadata, Message>(
    *hub, "channel", DataBlockPolicy::RingBuffer, config
);

// For minimal flexzone, use empty struct
struct EmptyFlexZone {};

PYLABHUB_SCHEMA_BEGIN(EmptyFlexZone)
PYLABHUB_SCHEMA_END(EmptyFlexZone)

auto producer = create_datablock_producer<EmptyFlexZone, Message>(
    *hub, "channel", policy, config
);
```

---

### New Consumer Discovery (Required Both Types)

```cpp
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(
    MessageHub &hub,
    const std::string &name,
    uint64_t shared_secret,
    const DataBlockConfig &expected_config
);
```

**Usage**:
```cpp
// Must match producer's types
auto consumer = find_datablock_consumer<FlexZoneMetadata, Message>(
    *hub, "channel", 0, expected_config
);
```

---

### Updated SharedMemoryHeader

```cpp
struct SharedMemoryHeader
{
    // OLD (Phase 3):
    // uint8_t schema_hash[32];  // Ambiguous - which schema?
    
    // NEW (Phase 4):
    uint8_t flexzone_schema_hash[32];   // BLAKE2b-256 of FlexZone BLDS
    uint8_t datablock_schema_hash[32];  // BLAKE2b-256 of DataBlock BLDS
    
    // For empty flexzone: hash of empty BLDS string ""
};
```

---

### Transaction API (Infer Types from Handle)

**Option A: Explicit types (Phase 3 style)**
```cpp
producer->with_transaction<FlexZoneMetadata, Message>(timeout, lambda);
```

**Option B: Type inference (Phase 4 preferred)**
```cpp
// Types inferred from producer handle
producer->with_transaction(timeout, [](auto &ctx) {
    // ctx type: WriteTransactionContext<FlexZoneMetadata, Message>
    // Types come from producer's template parameters
});
```

---

## Implementation Details

### Producer Creation Implementation

```cpp
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(
    MessageHub &hub,
    const std::string &name,
    DataBlockPolicy policy,
    const DataBlockConfig &config)
{
    // Compile-time validation
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable");
    
    // Generate BOTH schemas at compile-time
    pylabhub::schema::SchemaInfo flexzone_schema = 
        pylabhub::schema::generate_schema_info<FlexZoneT>(
            "FlexZone", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    pylabhub::schema::SchemaInfo datablock_schema = 
        pylabhub::schema::generate_schema_info<DataBlockT>(
            "DataBlock", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    // Validate sizes match config
    if constexpr (!std::is_void_v<FlexZoneT>)
    {
        if (config.flex_zone_size < sizeof(FlexZoneT))
        {
            throw std::invalid_argument(
                "config.flex_zone_size (" + std::to_string(config.flex_zone_size) +
                ") too small for FlexZoneT (" + std::to_string(sizeof(FlexZoneT)) + ")");
        }
    }
    
    size_t slot_size = config.effective_logical_unit_size();
    if (slot_size < sizeof(DataBlockT))
    {
        throw std::invalid_argument(
            "slot size (" + std::to_string(slot_size) +
            ") too small for DataBlockT (" + std::to_string(sizeof(DataBlockT)) + ")");
    }
    
    // Create producer and store BOTH schemas
    auto producer = std::make_unique<DataBlockProducer>();
    
    // Initialize shared memory, store schemas in header
    producer->init(hub, name, policy, config);
    
    auto *header = producer->header();
    std::memcpy(header->flexzone_schema_hash, 
                flexzone_schema.hash.data(), 32);
    std::memcpy(header->datablock_schema_hash, 
                datablock_schema.hash.data(), 32);
    
    return producer;
}
```

---

### Consumer Attachment Implementation

```cpp
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(
    MessageHub &hub,
    const std::string &name,
    uint64_t shared_secret,
    const DataBlockConfig &expected_config)
{
    // Generate expected schemas
    auto expected_flexzone = pylabhub::schema::generate_schema_info<FlexZoneT>(
        "FlexZone", {1, 0, 0});
    auto expected_datablock = pylabhub::schema::generate_schema_info<DataBlockT>(
        "DataBlock", {1, 0, 0});
    
    // Discover and attach
    auto consumer = std::make_unique<DataBlockConsumer>();
    consumer->attach(hub, name, shared_secret, expected_config);
    
    auto *header = consumer->header();
    
    // Validate BOTH schemas
    if (std::memcmp(header->flexzone_schema_hash, 
                    expected_flexzone.hash.data(), 32) != 0)
    {
        throw pylabhub::schema::SchemaValidationException(
            "FlexZone schema mismatch: producer and consumer have different FlexZone types",
            expected_flexzone.hash,
            header->flexzone_schema_hash);
    }
    
    if (std::memcmp(header->datablock_schema_hash, 
                    expected_datablock.hash.data(), 32) != 0)
    {
        throw pylabhub::schema::SchemaValidationException(
            "DataBlock schema mismatch: producer and consumer have different DataBlock types",
            expected_datablock.hash,
            header->datablock_schema_hash);
    }
    
    return consumer;
}
```

---

### with_transaction Implementation (Type-Safe)

**Option A: Keep explicit types for backward compatibility**
```cpp
template <typename FlexZoneT, typename DataBlockT, typename Func>
auto DataBlockProducer::with_transaction(
    std::chrono::milliseconds timeout, 
    Func &&func)
{
    // Runtime validation: template types must match stored schemas
    validate_template_types<FlexZoneT, DataBlockT>();
    
    WriteTransactionContext<FlexZoneT, DataBlockT> ctx(this, timeout);
    return std::forward<Func>(func)(ctx);
}
```

**Option B: Store types in handle for inference**
```cpp
// DataBlockProducer becomes templated
template <typename FlexZoneT, typename DataBlockT>
class TypedDataBlockProducer : public DataBlockProducer
{
public:
    template <typename Func>
    auto with_transaction(std::chrono::milliseconds timeout, Func &&func)
    {
        // Types are part of class template - no need to specify
        WriteTransactionContext<FlexZoneT, DataBlockT> ctx(this, timeout);
        return std::forward<Func>(func)(ctx);
    }
};

// Factory returns typed handle
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<TypedDataBlockProducer<FlexZoneT, DataBlockT>>
create_datablock_producer(...)
{
    // ...
}
```

---

## Empty FlexZone Pattern

For datablocks that don't need shared metadata:

```cpp
// Define minimal empty struct
struct EmptyFlexZone
{
    // No fields - just a placeholder
};

PYLABHUB_SCHEMA_BEGIN(EmptyFlexZone)
    // No members
PYLABHUB_SCHEMA_END(EmptyFlexZone)

// Use in producer/consumer
auto producer = create_datablock_producer<EmptyFlexZone, Message>(
    *hub, "channel", policy, config
);

// Config must still allocate space (minimum 1 page)
config.flex_zone_size = 4096;  // Even for empty struct
```

**BLDS for EmptyFlexZone**: `""` (empty string)  
**Hash**: BLAKE2b-256 of empty string (deterministic)

---

## Migration Path

### Phase 3 (Current - Deprecated)

```cpp
// OLD API: Only one type
auto producer = create_datablock_producer<Message>(
    *hub, "channel", policy, config, Message{}
);

producer->with_transaction<FlexZoneMetadata, Message>(timeout, lambda);
// ⚠️ FlexZoneMetadata not validated!
```

### Phase 4 (New API)

```cpp
// NEW API: Both types required
auto producer = create_datablock_producer<FlexZoneMetadata, Message>(
    *hub, "channel", policy, config
);

// Option A: Explicit (backward compatible)
producer->with_transaction<FlexZoneMetadata, Message>(timeout, lambda);

// Option B: Inferred (if using TypedDataBlockProducer)
producer->with_transaction(timeout, lambda);
```

---

## Benefits

1. **Consistency**: Both types treated equally
2. **Type safety**: Cannot mismatch FlexZone types between producer/consumer
3. **Early detection**: Schema mismatch caught at attach time
4. **Clear intent**: API makes it obvious that FlexZone is required
5. **Compile-time validation**: Template ensures correct types

---

## Example: Full Workflow

```cpp
// ============================================================================
// Data Structures (Both Required)
// ============================================================================

struct FlexZoneMetadata
{
    std::atomic<bool> shutdown_flag{false};
    std::atomic<uint64_t> message_count{0};
};

PYLABHUB_SCHEMA_BEGIN(FlexZoneMetadata)
    PYLABHUB_SCHEMA_MEMBER(shutdown_flag)
    PYLABHUB_SCHEMA_MEMBER(message_count)
PYLABHUB_SCHEMA_END(FlexZoneMetadata)

struct Message
{
    uint64_t timestamp_ns;
    float temperature;
};

PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
PYLABHUB_SCHEMA_END(Message)

// ============================================================================
// Producer
// ============================================================================

DataBlockConfig config;
config.physical_page_size = DataBlockPageSize::Size_4K;
config.ring_buffer_capacity = 16;
config.flex_zone_size = 4096;  // Must fit FlexZoneMetadata
config.policy = DataBlockPolicy::RingBuffer;
config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
config.checksum_type = ChecksumType::BLAKE2b;

// Create with BOTH types
auto producer = create_datablock_producer<FlexZoneMetadata, Message>(
    *hub, "sensor_channel", DataBlockPolicy::RingBuffer, config
);

// What happens internally:
// 1. Generate FlexZoneMetadata BLDS: "shutdown_flag:u8;message_count:u64"
// 2. Generate Message BLDS: "timestamp_ns:u64;temperature:f32"
// 3. Compute both BLAKE2b hashes
// 4. Store in header->flexzone_schema_hash[32] and header->datablock_schema_hash[32]
// 5. Validate sizeof(FlexZoneMetadata) <= 4096
// 6. Validate sizeof(Message) <= logical_unit_size

// ============================================================================
// Consumer
// ============================================================================

// Attach with BOTH types (must match producer)
auto consumer = find_datablock_consumer<FlexZoneMetadata, Message>(
    *hub, "sensor_channel", 0, config
);

// What happens internally:
// 1. Generate expected FlexZoneMetadata BLDS
// 2. Generate expected Message BLDS
// 3. Compute expected hashes
// 4. Read stored hashes from header
// 5. Compare: if mismatch → throw SchemaValidationException
// 6. Attachment succeeds only if BOTH schemas match

// ============================================================================
// Transaction (Types Already Validated)
// ============================================================================

// Option A: Explicit types (redundant but clear)
producer->with_transaction<FlexZoneMetadata, Message>(100ms, 
    [](auto &ctx) {
        auto zone = ctx.flexzone();  // ZoneRef<FlexZoneMetadata>
        zone.get().message_count++;
        
        for (auto &slot : ctx.slots(50ms)) {
            if (!slot.is_ok()) continue;
            auto &content = slot.content();  // SlotRef<Message>
            content.get().temperature = 25.5f;
            ctx.commit();
            break;
        }
    }
);

// Option B: Inferred types (cleaner, Phase 4B)
producer->with_transaction(100ms, [](auto &ctx) {
    // ctx type is WriteTransactionContext<FlexZoneMetadata, Message>
    // Inferred from producer's template parameters
    auto zone = ctx.flexzone();
    // ...
});
```

---

## Error Scenarios (Caught at Right Time)

### Scenario 1: FlexZone Type Mismatch

```cpp
// Producer
auto producer = create_datablock_producer<FlexZoneA, Message>(...);

// Consumer with wrong FlexZone type
auto consumer = find_datablock_consumer<FlexZoneB, Message>(...);
// ❌ Throws SchemaValidationException: "FlexZone schema mismatch"
// Caught at: consumer attach time
```

### Scenario 2: DataBlock Type Mismatch

```cpp
// Producer
auto producer = create_datablock_producer<FlexZone, MessageA>(...);

// Consumer with wrong DataBlock type
auto consumer = find_datablock_consumer<FlexZone, MessageB>(...);
// ❌ Throws SchemaValidationException: "DataBlock schema mismatch"
// Caught at: consumer attach time
```

### Scenario 3: Size Mismatch

```cpp
struct LargeFlexZone
{
    char buffer[8192];  // 8KB
};

config.flex_zone_size = 4096;  // Only 4KB

auto producer = create_datablock_producer<LargeFlexZone, Message>(...);
// ❌ Throws std::invalid_argument: "flex_zone_size too small"
// Caught at: producer creation time
```

---

## Recommendation

**Implement Phase 4A immediately**:
1. Update `create_datablock_producer<FlexZoneT, DataBlockT>()`
2. Update `find_datablock_consumer<FlexZoneT, DataBlockT>()`
3. Extend `SharedMemoryHeader` with dual schema hashes
4. Keep explicit types in `with_transaction<FlexZoneT, DataBlockT>()`
5. Update all examples and tests

**Phase 4B (optional, later)**:
- Type-erased handle with stored template parameters
- Type inference in `with_transaction()`

This makes FlexZone a first-class citizen as it should be!
