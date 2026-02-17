# Correct Architecture: C-API Storage vs C++ Type Safety

**Date**: 2026-02-15  
**Key Insight**: C-API being typeless is fine - it's just storage. The fix is simply storing BOTH schemas.

---

## Proper Separation of Concerns

### C-API Layer (Typeless - Correct Design)

**Purpose**: Storage and memory management (no type knowledge needed)

```c
// C-API: Just stores raw bytes - no types!
struct SharedMemoryHeader
{
    // OLD (Phase 3):
    uint8_t schema_hash[32];  // ← Single hash, ambiguous
    
    // NEW (Phase 4):
    uint8_t flexzone_schema_hash[32];   // ← Just bytes, no type info
    uint8_t datablock_schema_hash[32];  // ← Just bytes, no type info
    
    size_t flexible_zone_size;          // Raw size
    uint32_t logical_unit_size;         // Raw size
    // ... all typeless!
};
```

**What C-API does**: 
- ✅ Allocates memory regions
- ✅ Stores hash bytes (32 bytes each)
- ✅ Provides access to raw memory
- ❌ **Does NOT** know about types, schemas, or validation

**This is CORRECT** - C-API should be typeless!

---

### C++ API Layer (Typed - Where Type Safety Lives)

**Purpose**: Type safety, schema generation, validation

```cpp
// C++ API: Knows about types and generates schemas
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(...)
{
    // C++ generates schemas from types
    auto flexzone_schema = generate_schema_info<FlexZoneT>(...);
    auto datablock_schema = generate_schema_info<DataBlockT>(...);
    
    // C++ stores schema hashes in C-level header (just bytes)
    auto *header = get_header();  // C-level struct
    memcpy(header->flexzone_schema_hash, flexzone_schema.hash.data(), 32);
    memcpy(header->datablock_schema_hash, datablock_schema.hash.data(), 32);
    
    // C-API doesn't care what these bytes mean!
}
```

**What C++ API does**:
- ✅ Template type parameters (`FlexZoneT`, `DataBlockT`)
- ✅ Generate BLDS schemas from types
- ✅ Compute BLAKE2b hashes
- ✅ Store hashes in C-level header (as raw bytes)
- ✅ Validate hashes on consumer attach

---

## The Actual Problem (Simplified)

### Current State (Phase 3)

**C-API storage**:
```c
struct SharedMemoryHeader
{
    uint8_t schema_hash[32];  // ← Only ONE hash slot!
};
```

**C++ API**:
```cpp
// Only generates ONE schema (for DataBlockT)
auto producer = create_datablock_producer<Message>(..., Message{});
//                                         ^^^^^^^ Only one type

// But transaction uses TWO types!
producer->with_transaction<FlexZoneMetadata, Message>(...);
//                         ^^^^^^^^^^^^^^^^ Where's its hash?
```

**Problem**: C-API only has storage for ONE hash, but we need TWO.

---

### Phase 4 Fix (Correct Approach)

**C-API storage** (add one field):
```c
struct SharedMemoryHeader
{
    // Add storage for second hash
    uint8_t flexzone_schema_hash[32];   // NEW
    uint8_t datablock_schema_hash[32];  // NEW (renamed from schema_hash)
};
```

**C++ API** (generate and store both):
```cpp
// Generate TWO schemas (from two template types)
template <typename FlexZoneT, typename DataBlockT>
auto producer = create_datablock_producer<FlexZoneT, DataBlockT>(...);
//                                         ^^^^^^^^^^  ^^^^^^^^^^
//                                         Both types provided

// C++ generates both schemas and stores both hashes
void store_schemas(SharedMemoryHeader *header)
{
    auto flexzone_schema = generate_schema_info<FlexZoneT>(...);
    auto datablock_schema = generate_schema_info<DataBlockT>(...);
    
    // Store in C-level header (it doesn't care about types)
    memcpy(header->flexzone_schema_hash, flexzone_schema.hash.data(), 32);
    memcpy(header->datablock_schema_hash, datablock_schema.hash.data(), 32);
}
```

---

## Implementation: Pure Storage Extension

### Step 1: Extend C-Level Header (Just Add Storage)

```cpp
// data_block.hpp
struct alignas(4096) SharedMemoryHeader
{
    // === Identification and Versioning ===
    std::atomic<uint32_t> magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t total_block_size;

    // === Security and Schema ===
    uint8_t shared_secret[64];
    
    // OLD (Phase 3):
    // uint8_t schema_hash[32];  // Ambiguous - which schema?
    
    // NEW (Phase 4): Separate storage for each schema
    uint8_t flexzone_schema_hash[32];   // BLAKE2b hash bytes (typeless)
    uint8_t datablock_schema_hash[32];  // BLAKE2b hash bytes (typeless)
    
    uint32_t schema_version;  // Combined version (or separate if needed)
    uint8_t padding_sec[28];  // Adjust padding to maintain alignment
    
    // ... rest of header ...
};
```

**Key point**: C-API just added **storage** (two 32-byte arrays). No type knowledge!

---

### Step 2: C++ Layer Fills the Storage

```cpp
// data_block.cpp: create_datablock_producer_impl
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockProducer>
create_datablock_producer_impl(
    MessageHub &hub,
    const std::string &name,
    DataBlockPolicy policy,
    const DataBlockConfig &config)
{
    // C++ generates schemas from template types
    auto flexzone_schema = pylabhub::schema::generate_schema_info<FlexZoneT>(
        "FlexZone", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    auto datablock_schema = pylabhub::schema::generate_schema_info<DataBlockT>(
        "DataBlock", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    // Create producer (C-API allocates memory)
    auto producer = std::make_unique<DataBlockProducer>();
    producer->init(hub, name, policy, config);
    
    // Get C-level header (typeless storage)
    auto *header = producer->header();
    
    // Store schema hashes (just copying bytes - no types!)
    std::memcpy(header->flexzone_schema_hash, 
                flexzone_schema.hash.data(), 32);
    std::memcpy(header->datablock_schema_hash, 
                datablock_schema.hash.data(), 32);
    
    // C-API doesn't know what these bytes mean!
    // It's just storage for the C++ layer to use later
    
    return producer;
}
```

---

### Step 3: C++ Layer Validates Against Storage

```cpp
// data_block.cpp: find_datablock_consumer_impl
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer_impl(
    MessageHub &hub,
    const std::string &name,
    uint64_t shared_secret,
    const DataBlockConfig &expected_config)
{
    // Attach to shared memory (C-API)
    auto consumer = std::make_unique<DataBlockConsumer>();
    consumer->attach(hub, name, shared_secret, expected_config);
    
    // Get C-level header (typeless storage)
    auto *header = consumer->header();
    
    // C++ generates expected schemas from template types
    auto expected_flexzone = pylabhub::schema::generate_schema_info<FlexZoneT>(
        "FlexZone", {1, 0, 0});
    auto expected_datablock = pylabhub::schema::generate_schema_info<DataBlockT>(
        "DataBlock", {1, 0, 0});
    
    // Validate: Compare hash bytes (typeless comparison!)
    if (std::memcmp(header->flexzone_schema_hash, 
                    expected_flexzone.hash.data(), 32) != 0)
    {
        throw SchemaValidationException("FlexZone schema mismatch");
    }
    
    if (std::memcmp(header->datablock_schema_hash, 
                    expected_datablock.hash.data(), 32) != 0)
    {
        throw SchemaValidationException("DataBlock schema mismatch");
    }
    
    return consumer;
}
```

---

## Clean Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│ User Code (C++)                                             │
│                                                             │
│ auto producer = create_datablock_producer<FlexZone, Msg>() │
│                                            ^^^^^^^^  ^^^    │
│                                            Types (C++)      │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│ C++ API Layer (Type-Aware)                                  │
│                                                             │
│ 1. Generate BLDS: "shutdown_flag:u8;count:u64"             │
│ 2. Compute hash: 0x7f3a2b1c... (32 bytes)                  │
│ 3. Call C-API to store hash bytes                          │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│ C-API Layer (Typeless Storage)                              │
│                                                             │
│ struct SharedMemoryHeader {                                 │
│     uint8_t flexzone_schema_hash[32];  // Just bytes!       │
│     uint8_t datablock_schema_hash[32]; // Just bytes!       │
│     size_t flexible_zone_size;         // Just number!      │
│ };                                                          │
│                                                             │
│ memcpy(header->flexzone_schema_hash, bytes, 32);           │
│       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^ ^^              │
│       Storage location                Data  Size            │
└─────────────────────────────────────────────────────────────┘
```

**Key insight**: C-API is a "dumb storage layer" - it just holds bytes. The C++ layer does all the type work!

---

## Why This is the Right Approach

### ✅ Advantages

1. **C-API stays simple**: Just storage, no type complexity
2. **C++ owns type safety**: Templates, validation, all at C++ layer
3. **Clean separation**: Storage vs logic
4. **Easy to extend**: Add more schema types without changing C-API design
5. **Language agnostic**: Other languages (Python, Rust) can implement same C++ logic

### ❌ What We DON'T Do

1. ❌ Make C-API "type-aware" (not its job)
2. ❌ Add C-level validation logic (belongs in C++)
3. ❌ Expose C++ types in C headers (wrong layer)

---

## Summary: The Fix is Simple

**All we need**:

1. **C-API**: Add `flexzone_schema_hash[32]` field (just storage)
2. **C++ API**: Update `create_datablock_producer<FlexZone, DataBlock>()` to fill both fields
3. **C++ API**: Update `find_datablock_consumer<FlexZone, DataBlock>()` to validate both fields

**The C-API doesn't need to change its philosophy** - it stays typeless. We just give it **more storage slots** for the C++ layer to use!

Your insight is spot on: The C-API being typeless is **correct**. The problem is just that it only had storage for ONE schema hash when we need TWO. The fix is pure storage extension at the C level, with all the type work staying in C++.
