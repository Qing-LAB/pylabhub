# CRITICAL: Compile-Time Validation and Alignment Rules Explained

**Date**: 2026-02-15  
**Status**: Correcting major errors in examples and documentation

---

## Question 1: How is FlexZoneMetadata Verified at Compile-Time?

### Answer: YES, FlexZoneMetadata IS Validated!

FlexZoneMetadata validation happens in **two stages**: compile-time and runtime.

### Stage 1: Compile-Time (static_assert)

**Location**: `transaction_context.hpp:97-100`

```cpp
template <typename FlexZoneT, typename DataBlockT, bool IsWrite>
class TransactionContext
{
    // ✅ COMPILE-TIME validation
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");
};
```

**When**: During template instantiation of `with_transaction<FlexZoneMetadata, Message>()`

**What it checks**:
- ✅ `FlexZoneMetadata` is trivially copyable (bitwise copyable)
- ✅ No virtual functions (no vtable pointer)
- ✅ No `std::string`, `std::vector`, smart pointers
- ✅ No custom constructors/destructors

**Example that FAILS at compile-time**:
```cpp
struct BadFlexZone
{
    std::string name;  // ❌ NOT trivially copyable!
};

producer->with_transaction<BadFlexZone, Message>(timeout, lambda);
// Compile error: static_assert failed
```

---

### Stage 2: Runtime (validate_schema)

**Location**: `transaction_context.hpp:377-400`

```cpp
void TransactionContext::validate_schema()
{
    const auto &cfg = config();

    // ✅ RUNTIME size validation
    if constexpr (!std::is_void_v<FlexZoneT>)
    {
        if (cfg.flex_zone_size < sizeof(FlexZoneT))
        {
            throw std::runtime_error("flexible zone size too small");
        }
    }

    // Validate DataBlockT size
    size_t slot_size = cfg.ring_buffer.slot_bytes;  // ← SEE BELOW: THIS IS WRONG!
    if (slot_size < sizeof(DataBlockT))
    {
        throw std::runtime_error("slot size too small");
        }
}
```

**When**: At transaction entry (first line of `with_transaction()`)

**What it checks**:
- ✅ `sizeof(FlexZoneMetadata) <= config.flex_zone_size`
- ✅ `sizeof(Message) <= effective_slot_size`

---

### Summary: FlexZoneMetadata IS Validated

```
User Code:
    producer->with_transaction<FlexZoneMetadata, Message>(timeout, lambda);
                               ^^^^^^^^^^^^^^^^  ^^^^^^^ 
                                      ↓             ↓
                               ┌──────────────────────────────┐
                               │ Compile-Time (static_assert) │
                               │ - Is trivially copyable?     │
                               │ - No virtual functions?      │
                               └──────────────────────────────┘
                                      ↓
                               ┌──────────────────────────────┐
                               │ Runtime (validate_schema)    │
                               │ - sizeof() fits in region?   │
                               └──────────────────────────────┘
                                      ↓
                               ┌──────────────────────────────┐
                               │ ❌ Missing (Phase 4)          │
                               │ - BLDS hash validation       │
                               └──────────────────────────────┘
```

---

## Question 2: Slot Size Alignment - CRITICAL CORRECTION

### Answer: The Example Code is WRONG!

I need to correct a critical error in the examples I created.

### The Problem: Non-Existent API

**WRONG CODE (in my examples)**:
```cpp
config.ring_buffer.num_slots = 16;           // ❌ DOES NOT EXIST!
config.ring_buffer.slot_bytes = 256;         // ❌ DOES NOT EXIST!
```

**There is NO `ring_buffer` nested struct in `DataBlockConfig`!**

---

### Correct API (Current Implementation)

**Location**: `data_block.hpp:258-311`

```cpp
struct DataBlockConfig
{
    // Physical page size (allocation unit)
    DataBlockPageSize physical_page_size = DataBlockPageSize::Unset;
    
    // Logical slot size (must be >= physical_page_size)
    size_t logical_unit_size = 0;  // 0 = use physical_page_size
    
    // Number of slots
    uint32_t ring_buffer_capacity = 0;
    
    // Flexible zone size (MUST be 0 or multiple of 4096)
    size_t flex_zone_size = 0;
    
    // Effective slot size calculation
    size_t effective_logical_unit_size() const
    {
        if (logical_unit_size != 0)
            return logical_unit_size;
        return to_bytes(physical_page_size);  // Default to physical
    }
};
```

---

### Correct Configuration

```cpp
DataBlockConfig config;

// Step 1: Set physical page size
config.physical_page_size = DataBlockPageSize::Size_4K;  // 4096 bytes

// Step 2: Set logical slot size (or 0 to use physical)
config.logical_unit_size = 8192;  // 8K per slot (2 pages)
// OR: config.logical_unit_size = 0;  // Use physical (4K per slot)

// Step 3: Set number of slots
config.ring_buffer_capacity = 16;  // 16 slots

// Step 4: Set flexible zone size (MUST be multiple of 4096)
config.flex_zone_size = 4096;  // 1 page (4K)

// Other required fields
config.policy = DataBlockPolicy::RingBuffer;
config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
config.checksum_type = ChecksumType::BLAKE2b;
config.checksum_policy = ChecksumPolicy::Enforced;
```

---

### Alignment Rules

#### Rule 1: FlexZone Size (MUST be 4K-aligned or 0)

**Validation**: `data_block.cpp:699`

```cpp
if (config.flex_zone_size != 0 && (config.flex_zone_size % 4096 != 0))
{
    throw std::invalid_argument(
        "flex_zone_size must be 0 or a multiple of 4096");
}
```

**Valid values**:
- ✅ `0` (no flexzone)
- ✅ `4096` (1 page)
- ✅ `8192` (2 pages)
- ❌ `256` (ERROR!)
- ❌ `5000` (ERROR!)

---

#### Rule 2: Slot Size (MUST be multiple of physical_page_size)

**Validation**: `data_block.cpp:982-990`

```cpp
const size_t physical = to_bytes(config.physical_page_size);

if (config.logical_unit_size != 0 && 
    (config.logical_unit_size % physical != 0))
{
    throw std::invalid_argument(
        "logical_unit_size must be a multiple of physical_page_size");
}
```

**If `physical_page_size = 4096` (4K)**:

| `logical_unit_size` | Effective Slot Size | Valid? |
|---------------------|---------------------|--------|
| `0` | `4096` (default) | ✅ |
| `256` | `256` | ❌ Not multiple of 4096 |
| `4096` | `4096` | ✅ 1 page per slot |
| `8192` | `8192` | ✅ 2 pages per slot |
| `16384` | `16384` | ✅ 4 pages per slot |
| `5000` | `5000` | ❌ Not multiple of 4096 |

---

### Key Insight: Slot Size Does NOT Need to be 4K!

**You asked**: "i thought we've simplified to require [slot_bytes] to be always multiple of 4k"

**Answer**: **Only `logical_unit_size` needs to be a multiple of `physical_page_size`**.

**If you set `physical_page_size = DataBlockPageSize::Size_256` (256 bytes)**:
- Then `logical_unit_size` must be multiple of **256**, not 4096!
- Examples: 256, 512, 1024, 2048, 4096 are all valid

**Common confusion**:
- **FlexZone**: ALWAYS 4K-aligned (hardcoded)
- **Slot size**: Aligned to `physical_page_size` (configurable)

---

### Memory Layout Example

```cpp
config.physical_page_size = DataBlockPageSize::Size_4K;  // 4096 bytes
config.logical_unit_size = 8192;  // 2 pages per slot
config.ring_buffer_capacity = 4;  // 4 slots
config.flex_zone_size = 4096;     // 1 page
```

**Resulting layout**:
```
┌────────────────────────────────────────────────────┐
│ Control Zone                                        │
│  - SharedMemoryHeader (4K-aligned)                 │
│  - SlotRWStates                                     │
│  - Checksums                                        │
├────────────────────────────────────────────────────┤
│ Flexible Zone (4096 bytes, 4K-aligned)             │
│  - Must fit: sizeof(FlexZoneMetadata) <= 4096      │
├────────────────────────────────────────────────────┤
│ Ring Buffer (4 slots × 8192 bytes = 32K total)     │
│  ┌──────────────────────────────────────────────┐  │
│  │ Slot 0 (8192 bytes)                          │  │
│  │  - Must fit: sizeof(Message) <= 8192         │  │
│  ├──────────────────────────────────────────────┤  │
│  │ Slot 1 (8192 bytes)                          │  │
│  ├──────────────────────────────────────────────┤  │
│  │ Slot 2 (8192 bytes)                          │  │
│  ├──────────────────────────────────────────────┤  │
│  │ Slot 3 (8192 bytes)                          │  │
│  └──────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────┘
```

---

## What About `transaction_context.hpp` Using `cfg.ring_buffer.slot_bytes`?

**Location**: `transaction_context.hpp:393-399`

```cpp
void validate_schema()
{
    // ...
    size_t slot_size = cfg.ring_buffer.slot_bytes;  // ← THIS IS WRONG!
    if (slot_size < sizeof(DataBlockT))
    {
        throw std::runtime_error(...);
    }
}
```

**This is a BUG in the code!** It references a non-existent field.

**What it SHOULD be**:
```cpp
void validate_schema()
{
    // ...
    size_t slot_size = cfg.effective_logical_unit_size();  // ← CORRECT!
    if (slot_size < sizeof(DataBlockT))
    {
        throw std::runtime_error(...);
    }
}
```

---

## Summary Table: Alignment Rules

| Region | Size Field | Alignment Requirement | Valid Examples |
|--------|-----------|----------------------|----------------|
| **FlexZone** | `flex_zone_size` | **MUST be 0 or multiple of 4096** | 0, 4096, 8192, 16384 |
| **Slot** | `logical_unit_size` | **MUST be 0 or multiple of `physical_page_size`** | Depends on physical_page_size |

### If `physical_page_size = 4K`:
- FlexZone: 0, 4096, 8192, 16384, ... ✅
- Slot: 0, 4096, 8192, 16384, ... ✅

### If `physical_page_size = 256 bytes`:
- FlexZone: 0, 4096, 8192, 16384, ... ✅ (always 4K!)
- Slot: 0, 256, 512, 1024, 2048, 4096, ... ✅ (multiple of 256)

---

## Action Items

1. ✅ Fix `transaction_context.hpp` to use `cfg.effective_logical_unit_size()` instead of `cfg.ring_buffer.slot_bytes`
2. ✅ Fix all examples to use correct API (`ring_buffer_capacity`, not `ring_buffer.num_slots`)
3. ✅ Update documentation to clarify alignment rules
4. ✅ Remove all references to non-existent `ring_buffer` nested struct

These are critical bugs that need immediate fixing!
