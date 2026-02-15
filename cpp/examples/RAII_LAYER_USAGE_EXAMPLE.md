# RAII Layer Usage Examples

This document provides quick-reference examples for the Phase 3 C++ RAII Layer.

## Quick Start

```cpp
#include "utils/data_block.hpp"
using namespace pylabhub::hub;
```

## Producer: Type-Safe Write Transaction

```cpp
// Define your data structures
struct FlexZone {
    std::atomic<uint32_t> counter{0};
    bool shutdown_flag{false};
};

struct Message {
    uint64_t seq_num;
    char data[128];
};

// Create producer with schema
auto producer = create_datablock_producer<Message>(
    hub, "my_datablock", CreationPolicy::CreateOrAttach, config);

// Write transaction with typed access
producer->with_transaction<FlexZone, Message>(
    100ms,  // timeout
    [](WriteTransactionContext<FlexZone, Message> &ctx) {
        // Access flexible zone
        auto zone = ctx.flexzone();
        zone.get().counter++;
        
        // Non-terminating iterator
        for (auto slot_result : ctx.slots(50ms)) {
            if (!slot_result.is_ok()) {
                if (slot_result.error() == SlotAcquireError::Timeout) {
                    // Check shutdown condition
                    if (zone.get().shutdown_flag) break;
                    continue;  // Retry
                }
                break;  // Fatal error
            }
            
            // Got a slot!
            auto &slot = slot_result.content();
            slot.get().seq_num = zone.get().counter;
            strcpy(slot.get().data, "Hello RAII!");
            
            ctx.commit();
            break;  // Or continue for more slots
        }
    });
```

## Consumer: Type-Safe Read Transaction

```cpp
// Attach consumer with schema validation
auto consumer = find_datablock_consumer<Message>(
    hub, "my_datablock", std::nullopt, expected_config);

// Read transaction
consumer->with_transaction<FlexZone, Message>(
    100ms,
    [](ReadTransactionContext<FlexZone, Message> &ctx) {
        auto zone = ctx.flexzone();
        
        for (auto slot_result : ctx.slots(50ms)) {
            if (!slot_result.is_ok()) {
                if (slot_result.error() == SlotAcquireError::Timeout) {
                    ctx.update_heartbeat();  // Keep alive during idle
                    if (zone.get().shutdown_flag) break;
                    continue;
                }
                break;
            }
            
            auto &slot = slot_result.content();
            
            // Validate read (checksum, staleness)
            if (!ctx.validate_read()) {
                continue;  // Skip invalid slot
            }
            
            // Type-safe read
            const auto &msg = slot.get();
            std::cout << "Received: " << msg.data << std::endl;
            
            if (/* done condition */) break;
        }
    });
```

## Key Features

### 1. Result<T, E> Error Handling

```cpp
for (auto slot_result : ctx.slots(50ms)) {
    if (!slot_result.is_ok()) {
        switch (slot_result.error()) {
            case SlotAcquireError::Timeout:
                // Expected, retry possible
                break;
            case SlotAcquireError::NoSlot:
                // Ring buffer full, wait
                break;
            case SlotAcquireError::Error:
                // Fatal, terminate
                return;
        }
        continue;
    }
    
    // Use slot_result.content()
}
```

### 2. Non-Terminating Iterator

The iterator **never ends** on timeout/no-slot - only on fatal errors. You must explicitly `break`:

```cpp
for (auto slot_result : ctx.slots(50ms)) {
    if (!slot_result.is_ok()) {
        // Check your shutdown condition
        if (should_stop()) break;
        continue;  // Keep trying
    }
    
    // Process slot...
    
    if (done_condition()) break;  // User-controlled exit
}
```

### 3. Type Safety

```cpp
// Compile-time type checking
WriteSlotRef<Message> slot = ...;
slot.get().seq_num = 42;  // Type-safe
slot.get().invalid_field;  // Compile error!

// Raw access for advanced use
auto raw_span = slot.raw_access();  // std::span<std::byte>
```

### 4. Schema Validation

```cpp
// At transaction entry, validates:
// - sizeof(FlexZone) <= config.flex_zone_size
// - sizeof(Message) <= config.ring_buffer.slot_bytes
// Throws std::runtime_error on mismatch
```

### 5. Automatic Cleanup

```cpp
producer->with_transaction<FlexZone, Message>(100ms, [](auto &ctx) {
    for (auto slot_result : ctx.slots(50ms)) {
        if (!slot_result.is_ok()) continue;
        
        auto &slot = slot_result.value();
        slot.get().data = 42;
        
        // Exception thrown - slot automatically released
        if (error_condition) {
            throw std::runtime_error("Error!");
        }
        
        ctx.commit();
        break;
    }
});  // Context destructor ensures cleanup
```

### 6. Flexible Zone Access

```cpp
// Typed access
auto zone = ctx.flexzone();
zone.get().counter++;

// Raw access
auto raw_zone = zone.raw_access();  // std::span<std::byte>

// For void FlexZone (no typed structure)
producer->with_transaction<void, Message>(100ms, [](auto &ctx) {
    auto zone = ctx.flexzone();  // ZoneRef<void>
    auto raw = zone.raw_access();  // Only raw access available
});
```

### 7. Heartbeat Management

```cpp
// Automatic: Updated on slot acquire/release

// Explicit (when idle in loop):
ctx.update_heartbeat();

// Standalone (outside transaction):
producer->update_heartbeat();
consumer->update_heartbeat();  // Uses registered heartbeat slot
```

## Error Handling Patterns

### Pattern 1: Retry with Timeout

```cpp
for (auto slot_result : ctx.slots(50ms)) {
    if (!slot_result.is_ok()) {
        if (slot_result.error() == SlotAcquireError::Timeout) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        throw std::runtime_error("Fatal error");
    }
    // Process slot...
}
```

### Pattern 2: Event-Driven Break

```cpp
for (auto slot_result : ctx.slots(50ms)) {
    if (!slot_result.is_ok()) {
        // Check flexzone event flags
        if (zone.get().shutdown_flag) break;
        continue;
    }
    // Process slot...
}
```

### Pattern 3: Count-Based Break

```cpp
int processed = 0;
for (auto slot_result : ctx.slots(50ms)) {
    if (!slot_result.is_ok()) continue;
    
    // Process slot...
    
    if (++processed >= MAX_SLOTS) break;
}
```

## Complete Example

See `examples/raii_layer_example.cpp` for a comprehensive working example demonstrating all features.

## Migration from Old API

### Old API (Deprecated)
```cpp
// Old: with_write_transaction
with_write_transaction(producer, 100, [](WriteTransactionContext &ctx) {
    ctx.slot().write(...);
    ctx.slot().commit(...);
});
```

### New RAII API
```cpp
// New: with_transaction
producer->with_transaction<FlexZone, Message>(100ms, [](auto &ctx) {
    for (auto slot_result : ctx.slots(50ms)) {
        if (!slot_result.is_ok()) continue;
        auto &slot = slot_result.value();
        slot.get().data = ...;
        ctx.commit();
        break;
    }
});
```

## Best Practices

1. **Always check `is_ok()`** before accessing `value()`
2. **Use typed access** (`slot.get()`) for safety; fall back to `raw_access()` only when needed
3. **Update heartbeat** during long idle periods in iterator loops
4. **Validate reads** with `ctx.validate_read()` to catch stale/corrupt data
5. **Break explicitly** - the iterator won't end on timeout
6. **Let RAII handle cleanup** - don't manually release slots

## Thread Safety

- Each thread should create its own `TransactionContext`
- `DataBlockProducer` and `DataBlockConsumer` are thread-safe
- `SlotRef`, `ZoneRef`, and iterators are **not** thread-safe (thread-local use only)
