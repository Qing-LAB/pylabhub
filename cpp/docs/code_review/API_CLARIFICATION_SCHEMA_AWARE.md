# DataBlock API Clarification: Schema-Aware vs Non-Schema Functions

**Date**: 2026-02-15  
**Context**: Understanding the current API signatures for producer/consumer creation

---

## Overview

DataHub provides **two parallel APIs** for creating producers and consumers:

1. **Schema-Aware API** (Template version with BLDS validation)
2. **Non-Schema API** (Basic version without schema validation)

The differences are subtle but important for understanding how schema validation works.

---

## Producer Creation APIs

### Option 1: Schema-Aware (Recommended for Type Safety)

```cpp
template <typename Schema>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(
    MessageHub &hub,              // Reference, not pointer
    const std::string &name,
    DataBlockPolicy policy,       // RingBuffer, DoubleBuffer, Single
    const DataBlockConfig &config,
    const Schema &schema_instance // ← KEY: Enables schema validation
);
```

**Usage Example**:
```cpp
auto producer = create_datablock_producer<Message>(
    *hub,                          // Dereference if hub is shared_ptr<MessageHub>
    "message_channel",
    DataBlockPolicy::RingBuffer,
    config,
    Message{}                      // Dummy instance for template deduction
);
```

**What it does**:
1. Generates BLDS schema at compile-time from `Message` type
2. Computes BLAKE2b-256 hash of schema
3. Stores schema hash in `SharedMemoryHeader::schema_hash[32]`
4. Consumers can validate against this stored hash

---

### Option 2: Non-Schema (No Validation)

```cpp
std::unique_ptr<DataBlockProducer>
create_datablock_producer(
    MessageHub &hub,
    const std::string &name,
    DataBlockPolicy policy,
    const DataBlockConfig &config
    // ← No schema_instance parameter
);
```

**Usage Example**:
```cpp
auto producer = create_datablock_producer(
    *hub,
    "message_channel",
    DataBlockPolicy::RingBuffer,
    config
    // No Message{} - no schema validation
);
```

**What it does**:
1. Creates producer without storing schema hash
2. `SharedMemoryHeader::schema_hash` is zeroed out
3. No schema validation on consumer attach

---

## Consumer Discovery APIs

### Option 1: Schema-Aware (Validates Against Producer)

```cpp
template <typename Schema>
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(
    MessageHub &hub,
    const std::string &name,
    uint64_t shared_secret,
    const DataBlockConfig &expected_config,
    const Schema &schema_instance    // ← KEY: Enables validation
);
```

**Usage Example**:
```cpp
auto consumer = find_datablock_consumer<Message>(
    *hub,
    "message_channel",
    0,                 // shared_secret (0 = default/discover)
    expected_config,
    Message{}          // Validates against producer's schema
);
```

**What it does**:
1. Generates expected BLDS schema from `Message` type
2. Computes expected BLAKE2b-256 hash
3. Reads stored hash from `SharedMemoryHeader::schema_hash`
4. Compares hashes - throws `SchemaValidationException` if mismatch

---

### Option 2: Non-Schema (No Validation)

```cpp
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(
    MessageHub &hub,
    const std::string &name,
    uint64_t shared_secret,
    const DataBlockConfig &expected_config
    // ← No schema_instance parameter
);
```

**Usage Example**:
```cpp
auto consumer = find_datablock_consumer(
    *hub,
    "message_channel",
    0,
    expected_config
    // No Message{} - no schema validation
);
```

**What it does**:
1. Attaches to producer without schema validation
2. Only validates config (ring buffer size, slot size, etc.)
3. No guarantee that consumer's `Message` type matches producer's

---

## Key Parameters Explained

### `MessageHub &hub` (Reference, Not Pointer!)

```cpp
// CORRECT:
std::shared_ptr<MessageHub> hub = MessageHub::get_instance();
auto producer = create_datablock_producer<Message>(*hub, ...);
                                                  // ↑ Dereference!

// WRONG:
auto producer = create_datablock_producer<Message>(hub, ...);
// Error: cannot convert 'shared_ptr<MessageHub>' to 'MessageHub&'
```

**Why reference?** 
- ABI stability: References don't change size across compilers
- Lifetime management: Caller owns the hub, function doesn't take ownership
- Standard practice: Factory functions take resources by reference

---

### `DataBlockPolicy policy` (Not `CreationPolicy`)

**Correct enum**:
```cpp
enum class DataBlockPolicy
{
    Single,          // Single slot (overwrite)
    DoubleBuffer,    // Two slots (ping-pong)
    RingBuffer       // N slots (FIFO queue)
};
```

**Example**:
```cpp
config.policy = DataBlockPolicy::RingBuffer;  // Set in config as well

auto producer = create_datablock_producer<Message>(
    *hub,
    "channel",
    DataBlockPolicy::RingBuffer,  // ← Must match config.policy
    config,
    Message{}
);
```

**Note**: `CreationPolicy` doesn't exist in the current API. If you see it in old code, it's likely from an archived version.

---

### `const Schema &schema_instance` (Dummy for Template Deduction)

```cpp
Message{}  // Default-constructed instance
```

**Purpose**: C++ template deduction. The function needs to know what `Schema` type you want.

**Why not just `create_datablock_producer<Message>(...)`?**
- We do use `<Message>`! But the function also needs the parameter for:
  - Overload resolution (schema vs non-schema version)
  - Future extensibility (could pass custom schema builder)

**The parameter is unused internally**:
```cpp
template <typename Schema>
std::unique_ptr<DataBlockProducer>
create_datablock_producer(..., const Schema &schema_instance)
{
    (void)schema_instance;  // ← Explicitly marked unused
    
    // Schema is generated from template type Schema, not from instance
    auto schema_info = generate_schema_info<Schema>(...);
    // ...
}
```

---

## Complete Example Comparison

### Schema-Aware (Current Best Practice)

```cpp
#include "utils/schema_blds.hpp"

struct Message
{
    uint64_t timestamp_ns;
    float temperature;
};

// Register schema
PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
PYLABHUB_SCHEMA_END(Message)

// Producer
auto hub = MessageHub::get_instance();
auto producer = create_datablock_producer<Message>(
    *hub, "sensor", DataBlockPolicy::RingBuffer, config, Message{}
);
// ✓ Schema hash stored in shared memory

// Consumer
auto consumer = find_datablock_consumer<Message>(
    *hub, "sensor", 0, config, Message{}
);
// ✓ Schema hash validated - throws if mismatch
```

---

### Non-Schema (Minimal - No Type Safety)

```cpp
struct Message
{
    uint64_t timestamp_ns;
    float temperature;
};

// No PYLABHUB_SCHEMA_* macros needed

// Producer
auto hub = MessageHub::get_instance();
auto producer = create_datablock_producer(
    *hub, "sensor", DataBlockPolicy::RingBuffer, config
);
// ⚠️ No schema hash stored

// Consumer
auto consumer = find_datablock_consumer(
    *hub, "sensor", 0, config
);
// ⚠️ No schema validation - consumer could use wrong struct type!
```

---

## Integration with RAII Layer

### Current State (Phase 3)

The RAII layer's `with_transaction<FlexZoneT, DataBlockT>()` does **NOT** automatically validate schema hashes:

```cpp
// Producer with schema
auto producer = create_datablock_producer<Message>(
    *hub, "sensor", policy, config, Message{}
);

// Transaction uses template types
producer->with_transaction<FlexZone, Message>(timeout, [](auto &ctx) {
    // ⚠️ FlexZone and Message types are NOT validated against stored schema!
    // Only sizeof() checks are performed
});
```

**Why the gap?**
- Producer is type-erased (`DataBlockProducer` doesn't store template type info)
- `with_transaction` accepts arbitrary template types
- No way to enforce that template types match producer's registered schema

---

### Proposed Solution (Phase 4)

**Option A: Schema-aware handles** (see `MODERNIZED_SCHEMA_INTEGRATION.md`):

```cpp
// Create typed producer handle
auto producer = create_datablock_producer_typed<FlexZone, Message>(
    *hub, "sensor", policy, config
);
// Type: ProducerHandle<FlexZone, Message>

// Transaction infers types from handle
producer->with_transaction(timeout, [](auto &ctx) {
    // ✓ ctx type: WriteTransactionContext<FlexZone, Message>
    // ✓ Schema validated on first call (cached)
});

// Compile error if wrong types used
producer->with_transaction<OtherZone, Message>(...);
// ERROR: template argument mismatch
```

---

## Common Mistakes and Fixes

### Mistake 1: Passing `shared_ptr` instead of dereferencing

```cpp
std::shared_ptr<MessageHub> hub = ...;

// ❌ WRONG:
auto producer = create_datablock_producer<Message>(hub, ...);
// Error: no matching function (cannot convert shared_ptr to reference)

// ✅ CORRECT:
auto producer = create_datablock_producer<Message>(*hub, ...);
```

---

### Mistake 2: Using wrong enum type

```cpp
// ❌ WRONG:
create_datablock_producer<Message>(*hub, "name", CreationPolicy::CreateOrAttach, config, Message{});
// Error: 'CreationPolicy' does not name a type

// ✅ CORRECT:
create_datablock_producer<Message>(*hub, "name", DataBlockPolicy::RingBuffer, config, Message{});
```

---

### Mistake 3: Forgetting schema instance for validation

```cpp
// ❌ WRONG (no schema validation):
auto producer = create_datablock_producer<Message>(*hub, "name", policy, config);
// Compiles, but no schema validation!

// ✅ CORRECT (with schema validation):
auto producer = create_datablock_producer<Message>(*hub, "name", policy, config, Message{});
```

---

### Mistake 4: Wrong shared_secret type

```cpp
// ❌ WRONG:
find_datablock_consumer<Message>(*hub, "name", std::nullopt, config, Message{});
// Error: cannot convert 'std::nullopt_t' to 'uint64_t'

// ✅ CORRECT:
find_datablock_consumer<Message>(*hub, "name", 0, config, Message{});
// Use 0 for default/discover
```

---

## Summary Table

| Feature | Non-Schema API | Schema-Aware API |
|---------|---------------|------------------|
| **Producer signature** | `create_datablock_producer(hub, name, policy, config)` | `create_datablock_producer<Schema>(hub, name, policy, config, Schema{})` |
| **Consumer signature** | `find_datablock_consumer(hub, name, secret, config)` | `find_datablock_consumer<Schema>(hub, name, secret, config, Schema{})` |
| **BLDS macros required** | No | Yes (`PYLABHUB_SCHEMA_BEGIN/END`) |
| **Schema hash stored** | No (zeroed) | Yes (32-byte BLAKE2b) |
| **Runtime validation** | Config only | Config + Schema hash |
| **Type safety** | Compile-time only | Compile-time + Runtime |
| **Use case** | Quick prototyping | Production systems |

---

## Recommendation

**Always use schema-aware API** for production code:

```cpp
// 1. Define struct with BLDS macros
PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(field1)
    PYLABHUB_SCHEMA_MEMBER(field2)
PYLABHUB_SCHEMA_END(Message)

// 2. Create producer with schema instance
auto producer = create_datablock_producer<Message>(
    *hub, "channel", DataBlockPolicy::RingBuffer, config, Message{}
);

// 3. Consumer validates schema on attach
auto consumer = find_datablock_consumer<Message>(
    *hub, "channel", 0, config, Message{}
);

// 4. Use RAII layer for transactions
producer->with_transaction<FlexZone, Message>(timeout, lambda);
```

This provides:
- ✓ Compile-time type checking
- ✓ Runtime schema validation
- ✓ Protection against ABI mismatches
- ✓ Clear error messages on schema mismatch
