# DataHub API Surface Documentation

**Document ID**: DOC-API-001  
**Version**: 1.0.0  
**Date**: 2026-02-15

## Overview

This document describes the complete API surface for DataHub (DataBlock) operations, organized by abstraction level and use case.

**Important**: This is the first stable release (v1.0.0) with a single, consistent API. There are no deprecated functions or legacy compatibility layers.

---

## API Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│  Level 4: RAII Transaction Layer (Highest Level)            │
│  • with_transaction<FlexZoneT, DataBlockT>()                │
│  • TransactionContext (WriteTransactionContext/Read...)     │
│  • Result<SlotRef<T>, SlotAcquireError>                     │
│  • SlotIterator                                             │
│  └─> Recommended for all new code                          │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Level 3: C++ Schema-Aware Factory (Type-Safe)              │
│  • create_datablock_producer<FlexZoneT, DataBlockT>()       │
│  • find_datablock_consumer<FlexZoneT, DataBlockT>()         │
│  └─> Generates SchemaInfo, validates at runtime            │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Level 2: C++ Non-Schema Factory (Basic Type-Safe)          │
│  • create_datablock_producer(hub, name, policy, config)     │
│  • find_datablock_consumer(hub, name, secret, [config])     │
│  └─> No schema validation, config-only                     │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Level 1: C++ Primitive Handles (Low-Level)                 │
│  • DataBlockProducer::acquire_write_slot()                   │
│  • DataBlockConsumer::acquire_consume_slot()                 │
│  • SlotWriteHandle / SlotConsumeHandle                       │
│  • DataBlockSlotIterator                                     │
│  └─> Manual slot management, explicit release              │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  Level 0: C API (Lowest Level, FFI Boundary)                │
│  • pylabhub_datablock_create_producer()                      │
│  • pylabhub_datablock_find_consumer()                        │
│  • pylabhub_slot_acquire_write() / _read()                   │
│  └─> Typeless (void*, size_t), error codes                 │
└─────────────────────────────────────────────────────────────┘
```

---

## Level 4: RAII Transaction Layer

**Status**: Current, Recommended  
**File**: `transaction_context.hpp`, `with_transaction.hpp`

### Factory Function

```cpp
template <typename FlexZoneT, typename DataBlockT, typename Func>
auto with_transaction(DataBlockProducer &producer, Func &&operation);

template <typename FlexZoneT, typename DataBlockT, typename Func>
auto with_transaction(DataBlockConsumer &consumer, Func &&operation);
```

**Features**:
- Automatic slot acquisition/release (RAII)
- Compile-time type checking (`static_assert` for `trivially_copyable`)
- Runtime schema validation (BLDS hash matching)
- Automatic heartbeat updates
- Exception-safe resource management
- Non-terminating slot iterator

**Usage**:
```cpp
auto result = with_transaction<FlexZoneMetadata, Message>(
    producer,
    [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx) {
        auto &zone = ctx.zone().get();
        zone.counter++;
        
        for (auto &slot : ctx.slots()) {
            auto &msg = slot.content().get();
            msg.sequence_num = zone.counter;
        }
    }
);
```

### Transaction Context Types

```cpp
// Write transaction
WriteTransactionContext<FlexZoneT, DataBlockT>
  ├─ zone() -> ZoneRef<FlexZoneT>           // Flexible zone access
  ├─ slots() -> SlotIterator                // Non-terminating iterator
  └─ update_heartbeat()                     // Manual heartbeat

// Read transaction
ReadTransactionContext<FlexZoneT, DataBlockT>
  ├─ zone() -> const ZoneRef<FlexZoneT>    // Read-only zone
  ├─ slots() -> SlotIterator                // Non-terminating iterator
  └─ update_heartbeat()                     // Manual heartbeat
```

### Result Type

```cpp
Result<SlotRef<T>, SlotAcquireError>
  ├─ content() -> SlotRef<T>&              // Access slot content
  ├─ error() -> SlotAcquireError           // Get error if failed
  ├─ has_content() -> bool                  // Check if successful
  └─ has_error() -> bool                    // Check if failed

SlotRef<T>
  ├─ get() -> T&                            // Typed access
  ├─ raw_access() -> std::span<std::byte>  // Raw buffer access
  ├─ slot_index() -> size_t                 // Ring buffer index
  └─ slot_id() -> uint64_t                  // Monotonic slot ID
```

---

## Level 3: C++ Schema-Aware Factory

**Status**: Stable, production-ready  
**File**: `data_block.hpp`

### Dual-Schema API

```cpp
template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, 
                          DataBlockPolicy policy, const DataBlockConfig &config);

template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, 
                        uint64_t shared_secret, const DataBlockConfig *expected_config = nullptr);
```

**Features**:
- Generates BLDS schema for **both** FlexZone and DataBlock types
- Stores both `flexzone_schema_hash[32]` and `datablock_schema_hash[32]` in `SharedMemoryHeader`
- Validates both schemas at consumer attachment
- Compile-time checks: `static_assert(is_trivially_copyable_v<T>)`
- Runtime checks: Size validation, schema hash matching

**Usage**:
```cpp
struct FlexZoneMetadata { uint32_t counter; };
struct Message { uint64_t timestamp; uint32_t data; };

PYLABHUB_SCHEMA_BEGIN(FlexZoneMetadata)
    PYLABHUB_SCHEMA_MEMBER(counter)
PYLABHUB_SCHEMA_END(FlexZoneMetadata)

PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(timestamp)
    PYLABHUB_SCHEMA_MEMBER(data)
PYLABHUB_SCHEMA_END(Message)

auto producer = create_datablock_producer<FlexZoneMetadata, Message>(
    hub, "my_channel", DataBlockPolicy::RingBuffer, config);

auto consumer = find_datablock_consumer<FlexZoneMetadata, Message>(
    hub, "my_channel", shared_secret, &config);
```

---

## Level 2: C++ Non-Schema Factory (type-erased)

**Status**: Stable; use only when types are unknown at compile time  
**File**: `data_block.hpp`

```cpp
[[nodiscard]] std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, 
                          DataBlockPolicy policy, const DataBlockConfig &config);

[[nodiscard]] std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret);

[[nodiscard]] std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config);
```

**When to use Level 2 vs Level 3**  
- **Normal C++ with known FlexZone/DataBlock types:** use Level 3 (template API). There is **no benefit** to calling the non-template overloads from typed C++; they do not store or validate schema.  
- **Level 2 is for:** code that genuinely has no compile-time types—e.g. the **C API implementation** (Level 0 calls into this), **recovery** (reattaches to existing SHM without knowing the original types), or **language bindings** that manage schema elsewhere.  
- So: prefer the template API for the C++ abstraction layer; use the non-template API only when implementing the C layer, recovery, or bindings.

**Limitations**:
- No compile-time type checks
- No runtime schema validation (header schema hashes are zeroed)
- User must ensure ABI compatibility manually

---

## Level 1: C++ Primitive Handles

**Status**: Stable, for advanced use cases  
**File**: `data_block.hpp`

### DataBlockProducer

```cpp
class DataBlockProducer {
  // Slot acquisition
  [[nodiscard]] std::unique_ptr<SlotWriteHandle> 
  acquire_write_slot(int timeout_ms = -1);
  
  bool release_write_slot(std::unique_ptr<SlotWriteHandle> handle);
  
  // Flexible zone access
  template <typename T> T& flexible_zone();
  std::span<std::byte> flexible_zone_span() noexcept;
  
  // Checksum
  bool update_checksum_flexible_zone() noexcept;
  bool verify_checksum_flexible_zone() const noexcept;
  
  // Heartbeat
  void update_heartbeat() noexcept;
  
  // Spinlock
  [[nodiscard]] std::unique_ptr<SharedSpinLockGuardOwning> 
  acquire_spinlock(size_t index, const std::string &debug_name = "");
};
```

### DataBlockConsumer

```cpp
class DataBlockConsumer {
  // Slot acquisition
  [[nodiscard]] std::unique_ptr<SlotConsumeHandle> 
  acquire_consume_slot(int timeout_ms = -1);
  
  bool release_consume_slot(std::unique_ptr<SlotConsumeHandle> handle);
  
  // Iteration
  [[nodiscard]] DataBlockSlotIterator slot_iterator();
  
  // Flexible zone access (read-only)
  template <typename T> const T& flexible_zone() const;
  std::span<const std::byte> flexible_zone_span() const noexcept;
  
  // Checksum
  bool verify_checksum_flexible_zone() const noexcept;
  
  // Heartbeat
  [[nodiscard]] int register_heartbeat();
  void update_heartbeat(int slot);
  void update_heartbeat() noexcept;  // Uses registered slot
  void unregister_heartbeat(int slot);
};
```

### Slot Handles

```cpp
class SlotWriteHandle {
  size_t slot_index() const noexcept;
  uint64_t slot_id() const noexcept;
  std::span<std::byte> buffer_span() noexcept;
  std::span<const std::byte> flexible_zone_span() const noexcept;
  bool write(const void *src, size_t len, size_t offset = 0) noexcept;
  bool update_checksum_slot() noexcept;
};

class SlotConsumeHandle {
  size_t slot_index() const noexcept;
  uint64_t slot_id() const noexcept;
  std::span<const std::byte> buffer_span() const noexcept;
  std::span<const std::byte> flexible_zone_span() const noexcept;
  bool read(void *dst, size_t len, size_t offset = 0) const noexcept;
  bool verify_checksum_slot() const noexcept;
  bool is_valid() const noexcept;  // Check if not overwritten
};
```

---

## Level 0: C API (FFI Boundary)

**Status**: Stable, for C/Python/other language bindings  
**File**: `data_block.h` (C header), `data_block_c_api.cpp`

**Principles**:
- Typeless: `void*` for data, `size_t` for sizes
- Error codes: `int` return values, negative = error
- No exceptions: `noexcept` / `__attribute__((nothrow))`
- No name mangling: `extern "C"`
- No templates: Monomorphized functions

### Factory Functions

```c
int pylabhub_datablock_create_producer(
    void *hub, const char *name, int policy,
    const void *config, size_t config_size,
    void **out_producer);

int pylabhub_datablock_find_consumer(
    void *hub, const char *name, uint64_t shared_secret,
    void **out_consumer);
```

### Slot Operations

```c
int pylabhub_slot_acquire_write(void *producer, int timeout_ms, void **out_handle);
int pylabhub_slot_release_write(void *producer, void *handle);
int pylabhub_slot_write_data(void *handle, const void *data, size_t len, size_t offset);

int pylabhub_slot_acquire_read(void *consumer, int timeout_ms, void **out_handle);
int pylabhub_slot_release_read(void *consumer, void *handle);
int pylabhub_slot_read_data(void *handle, void *data, size_t len, size_t offset);
```

---

## API Selection Guide

| Use Case | Recommended API | Reason |
|----------|----------------|--------|
| New production C++ code | Level 4 (RAII) | Safest, most ergonomic |
| Schema validation without RAII | Level 3 (Schema-Aware) | Type-safe with full validation |
| Prototyping without types | Level 2 (Non-Schema) | Quick setup, no schema |
| Performance-critical loop | Level 1 (Primitive) | Manual control, zero overhead |
| FFI / language bindings | Level 0 (C API) | C ABI compatibility |

---

## Schema Validation Behavior by API Level

| API Level | FlexZone Schema | DataBlock Schema | Validation Timing |
|-----------|----------------|------------------|-------------------|
| Level 4 (RAII) | ✓ Required | ✓ Required | Compile-time + Runtime (on first access) |
| Level 3 (Schema-Aware) | ✓ Required | ✓ Required | Compile-time + Runtime (on create/find) |
| Level 2 (Non-Schema) | ✗ None | ✗ None | Config-only (size, alignment) |
| Level 1 (Primitive) | ✗ None | ✗ None | Manual user responsibility |
| Level 0 (C API) | ✗ None | ✗ None | Caller responsibility |

---

**Document Control**  
Last Modified: 2026-02-15  
Maintained by: DataHub API Team  
Review Cycle: On API changes or quarterly
