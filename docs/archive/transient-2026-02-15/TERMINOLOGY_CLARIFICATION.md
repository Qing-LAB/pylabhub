# Critical Clarification: Schema vs Data Structure Terminology

**Date**: 2026-02-15  
**Issue**: Confusing terminology in documentation regarding "Schema" template parameter

---

## The Confusion

### What I Wrote (Confusing):
```cpp
template <typename Schema>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(..., const Schema &schema_instance);
```

### What It Really Means:
```cpp
template <typename YourDataStructure>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(..., const YourDataStructure &data_structure_instance);
```

**"Schema" is just a generic template parameter name**. It doesn't mean you need a separate "schema" type - it means "whatever struct type you're storing in the datablock slots".

---

## Correct Understanding

### Example Data Structure

```cpp
struct Message
{
    uint64_t timestamp_ns;
    float temperature;
};

// Register BLDS schema for this structure
PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
PYLABHUB_SCHEMA_END(Message)
```

### Create Producer

```cpp
auto producer = create_datablock_producer<Message>(
    //                                     ^^^^^^^ Your actual data structure type
    *hub,
    "sensor",
    DataBlockPolicy::RingBuffer,
    config,
    Message{}  // Dummy instance of YOUR data structure
    //^^^^^^^ Not a "Schema" object - just an instance of Message
);
```

**What happens**:
1. Template instantiation: `create_datablock_producer<Message>`
2. Compiler generates BLDS string for `Message` type
3. Computes BLAKE2b-256 hash of BLDS
4. Stores hash in `SharedMemoryHeader::schema_hash[32]`

---

## The CRITICAL Architectural Issue You Discovered

### Current API Limitation

```cpp
// OLD API: Only stores ONE schema (for datablock slots)
auto producer = create_datablock_producer<Message>(
    //                                     ^^^^^^^ Only Message schema is stored!
    *hub, "channel", policy, config, Message{}
);

// But RAII layer uses TWO types!
producer->with_transaction<FlexZoneMetadata, Message>(timeout, lambda);
//                         ^^^^^^^^^^^^^^^^  ^^^^^^^ 
//                         FlexZone type     Datablock/slot type
```

**The problem**:
- `create_datablock_producer<Message>()` only stores the **Message** schema hash
- `FlexZoneMetadata` schema is **NOT** stored or validated anywhere!
- `with_transaction<FlexZoneMetadata, Message>()` uses both types, but only `Message` is validated

---

## What Each Type Means

### In `with_transaction<FlexZoneMetadata, Message>`

```cpp
producer->with_transaction<FlexZoneMetadata, Message>(timeout,
//                         ^^^^^^^^^^^^^^^^  ^^^^^^^ 
//                         Template Param 1  Template Param 2
    [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx)
    {
        // FlexZoneMetadata - for ctx.flexzone()
        auto zone = ctx.flexzone();  // Returns ZoneRef<FlexZoneMetadata>
        zone.get().some_field = ...;
        
        // Message - for ctx.slots()
        for (auto &slot : ctx.slots(50ms)) {
            auto &content = slot.content();  // Returns SlotRef<Message>
            content.get().temperature = ...;
        }
    }
);
```

**Architecture**:
```
┌─────────────────────────────────────────────────────┐
│            Shared Memory Layout                     │
├─────────────────────────────────────────────────────┤
│  Control Zone:                                      │
│    - SharedMemoryHeader                             │
│    - SlotRWStates                                   │
│    - Checksums                                      │
├─────────────────────────────────────────────────────┤
│  Flexible Zone (N × 4K):                            │
│    ┌───────────────────────────────────────┐       │
│    │  FlexZoneMetadata (user-defined)      │       │
│    │  - Shared state across all consumers  │       │
│    │  - Event flags, counters, config      │       │
│    │                                        │       │
│    │  sizeof(FlexZoneMetadata) <= flex_zone_size │
│    └───────────────────────────────────────┘       │
├─────────────────────────────────────────────────────┤
│  Ring Buffer (Structured Data):                     │
│    ┌───────────────────────────────────────┐       │
│    │  Slot 0: Message                      │       │
│    ├───────────────────────────────────────┤       │
│    │  Slot 1: Message                      │       │
│    ├───────────────────────────────────────┤       │
│    │  ...                                  │       │
│    ├───────────────────────────────────────┤       │
│    │  Slot N-1: Message                    │       │
│    └───────────────────────────────────────┘       │
│    sizeof(Message) <= slot_bytes                    │
└─────────────────────────────────────────────────────┘
```

---

## Current Schema Storage Limitation

### In SharedMemoryHeader

```cpp
struct SharedMemoryHeader
{
    // ... other fields ...
    
    uint8_t schema_hash[32];  // ← Only ONE hash stored!
    //                           Which schema does this represent?
    //                           Answer: The DATABLOCK/SLOT schema (Message)
    
    // ❌ NO storage for FlexZone schema!
};
```

### What Gets Validated (Phase 3 - Current)

```cpp
// Producer creation
auto producer = create_datablock_producer<Message>(*hub, ...);
// ✓ Stores Message BLDS hash in header->schema_hash

// Consumer attachment
auto consumer = find_datablock_consumer<Message>(*hub, ...);
// ✓ Validates Message BLDS hash against stored hash

// Transaction
producer->with_transaction<FlexZoneMetadata, Message>(...);
//                         ^^^^^^^^^^^^^^^^  ^^^^^^^ 
//                         ❌ NOT validated  ✓ Only sizeof() checked
//                                              (NOT BLDS hash!)
```

---

## Phase 4 Solution Preview

### Extend SharedMemoryHeader

```cpp
struct SharedMemoryHeader
{
    // ... other fields ...
    
    // OLD (Phase 3):
    // uint8_t schema_hash[32];  // Ambiguous - which schema?
    
    // NEW (Phase 4):
    uint8_t flexzone_schema_hash[32];   // BLDS hash for FlexZone type
    uint8_t datablock_schema_hash[32];  // BLDS hash for DataBlock/slot type
};
```

### New API

```cpp
// Producer creation with BOTH schemas
auto producer = create_datablock_producer_typed<FlexZoneMetadata, Message>(
    *hub, "channel", policy, config
);
// ✓ Stores FlexZoneMetadata BLDS hash in header->flexzone_schema_hash
// ✓ Stores Message BLDS hash in header->datablock_schema_hash

// Transaction validates BOTH at runtime
producer->with_transaction(timeout, [](auto &ctx) {
    // ✓ FlexZoneMetadata validated (lazy, cached)
    // ✓ Message validated (lazy, cached)
    // ctx type inferred: WriteTransactionContext<FlexZoneMetadata, Message>
});
```

---

## Correct Example (Phase 3 - Current Limitations)

```cpp
// ============================================================================
// Data Structures
// ============================================================================

// Flexible Zone: Shared metadata (NOT validated by old API!)
struct FlexZoneMetadata
{
    std::atomic<uint32_t> active_producers{0};
    std::atomic<bool> shutdown_flag{false};
};

PYLABHUB_SCHEMA_BEGIN(FlexZoneMetadata)
    PYLABHUB_SCHEMA_MEMBER(active_producers)
    PYLABHUB_SCHEMA_MEMBER(shutdown_flag)
PYLABHUB_SCHEMA_END(FlexZoneMetadata)

// Datablock/Slot: Per-message data (validated by old API)
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
// Producer Creation
// ============================================================================

DataBlockConfig config;
config.flex_zone_size = 4096;  // For FlexZoneMetadata
config.ring_buffer.num_slots = 16;
config.ring_buffer.slot_bytes = 256;  // For Message

// OLD API: Only validates Message schema, NOT FlexZoneMetadata!
auto producer = create_datablock_producer<Message>(
    //                                     ^^^^^^^ Only Message schema stored
    *hub,
    "sensor",
    DataBlockPolicy::RingBuffer,
    config,
    Message{}  // ← This is the data structure instance, not a "Schema" object
);

// ⚠️ WARNING: FlexZoneMetadata schema is NOT stored or validated!

// ============================================================================
// Transaction
// ============================================================================

producer->with_transaction<FlexZoneMetadata, Message>(100ms,
    [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx)
    {
        // Runtime validation (Phase 3):
        // ✓ sizeof(FlexZoneMetadata) <= config.flex_zone_size
        // ✓ sizeof(Message) <= config.ring_buffer.slot_bytes
        // ❌ FlexZoneMetadata BLDS hash NOT validated (not stored!)
        // ❌ Message BLDS hash NOT validated (only checked at attach time)
        
        auto zone = ctx.flexzone();  // ZoneRef<FlexZoneMetadata>
        zone.get().active_producers++;
        
        for (auto &slot : ctx.slots(50ms)) {
            if (!slot.is_ok()) continue;
            
            auto &content = slot.content();  // SlotRef<Message>
            content.get().temperature = 25.5f;
            
            ctx.commit();
            break;
        }
    }
);
```

---

## Key Takeaways

1. **"Schema" is just a template parameter name** - replace it mentally with "YourDataStructure"

2. **Two types in `with_transaction<FlexZone, DataBlock>`**:
   - `FlexZone` = structure for shared metadata (flexible zone)
   - `DataBlock` = structure for per-slot data (ring buffer slots)

3. **Current limitation** (Phase 3):
   - Old API only stores ONE schema hash (for DataBlock/slot type)
   - FlexZone schema is NOT stored or validated
   - `with_transaction` only validates `sizeof()`, not BLDS hash

4. **Phase 4 solution**:
   - Store TWO schema hashes (flexzone + datablock)
   - Validate both at transaction entry
   - New API: `create_datablock_producer_typed<FlexZone, DataBlock>(...)`

---

## Terminology Cheat Sheet

| Term in Code | What It Really Means |
|--------------|---------------------|
| `template <typename Schema>` | Generic template parameter name for "your data structure type" |
| `create_datablock_producer<Message>` | Create producer for slots containing `Message` structs |
| `Message{}` | Dummy instance for template deduction (unused internally) |
| `with_transaction<FlexZone, Message>` | Transaction using `FlexZone` metadata and `Message` slots |
| "Schema validation" | Validating that struct layout matches via BLDS hash |
| "BLDS" | Basic Layout Description String (e.g., "timestamp_ns:u64;temperature:f32") |

Sorry for the confusion! The term "Schema" in the template parameter is just poor naming - it should have been `typename DataStructure` or `typename T` to be clearer.
