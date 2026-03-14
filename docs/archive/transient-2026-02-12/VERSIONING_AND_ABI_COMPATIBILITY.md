# SharedMemoryHeader Versioning and ABI Compatibility Rules

**Status**: MANDATORY - All developers must follow these rules
**Date Created**: 2026-02-09
**Applies To**: All shared memory structures (DataBlock, MessageHub, etc.)

---

## 1. NO MAGIC NUMBERS Rule

**STRICT REQUIREMENT**: Never use hardcoded numeric literals for structure sizes, array bounds, or layout constants.

### ❌ FORBIDDEN
```cpp
// BAD: Hardcoded magic numbers
for (size_t i = 0; i < 8; ++i) { ... }
uint8_t reserved_header[2867];
consumer_heartbeats[8];
```

### ✅ REQUIRED
```cpp
// GOOD: Named constants with version association
for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i) { ... }
uint8_t reserved_header[HEADER_RESERVED_SIZE_V1_0];
consumer_heartbeats[detail::MAX_CONSUMER_HEARTBEATS];
```

---

## 2. Version-Specific Constants

All structure layout constants MUST be defined in the `detail` namespace with clear version association:

```cpp
namespace pylabhub::hub::detail {
    // Version 1.0 layout constants
    inline constexpr uint16_t HEADER_VERSION_MAJOR = 1;
    inline constexpr uint16_t HEADER_VERSION_MINOR = 0;

    // Fixed pool sizes (changing these breaks ABI compatibility)
    inline constexpr size_t MAX_SHARED_SPINLOCKS = 8;
    inline constexpr size_t MAX_CONSUMER_HEARTBEATS = 8;

    // Compile-time size verification
    static_assert(sizeof(SharedMemoryHeader) == 4096, "Header must be exactly 4KB");
}
```

---

## 3. ABI Breaking Changes

Changing these constants requires **incrementing MAJOR version**:
- `MAX_SHARED_SPINLOCKS`
- `MAX_CONSUMER_HEARTBEATS`
- Any array sizes in SharedMemoryHeader
- Field order or alignment in SharedMemoryHeader

### Migration Process for Breaking Changes

1. **Create new version constants**:
   ```cpp
   // Version 2.0 layout constants (NEW)
   inline constexpr uint16_t HEADER_VERSION_MAJOR_V2 = 2;
   inline constexpr size_t MAX_SHARED_SPINLOCKS_V2 = 16; // Increased from 8
   ```

2. **Deprecate old constants** (keep for migration period):
   ```cpp
   // Version 1.0 layout constants (DEPRECATED)
   [[deprecated("Use V2 constants")]]
   inline constexpr size_t MAX_SHARED_SPINLOCKS_V1 = 8;
   ```

3. **Add version detection logic**:
   ```cpp
   if (header->version_major == 1) {
       // Use V1 layout
   } else if (header->version_major == 2) {
       // Use V2 layout
   }
   ```

---

## 4. Static Assertions

Every structure layout constant MUST have a corresponding static_assert:

```cpp
// Field size assertions
static_assert(sizeof(SharedSpinLockState) == 32, "SharedSpinLockState must be 32 bytes");

// Array size assertions
static_assert(MAX_SHARED_SPINLOCKS == 8, "V1.0 requires exactly 8 spinlocks");
static_assert(MAX_CONSUMER_HEARTBEATS == 8, "V1.0 requires exactly 8 consumer heartbeat slots");

// Total size assertion
static_assert(sizeof(SharedMemoryHeader) == 4096, "Header must be exactly 4KB");

// Alignment assertions
static_assert(alignof(SharedMemoryHeader) >= 4096, "Header must be page-aligned");
```

---

## 5. Size Calculation Documentation

All padding/reserved fields MUST include calculation comments:

```cpp
uint8_t reserved_header[2867]; // Calculated:
                                // ID = 16 (magic + version + size)
                                // Security = 128 (secret + hash + version + padding)
                                // Config = 25 (policy + sizes + flags)
                                // State = 28 (indices + counter)
                                // Metrics = 264 (slot + error + heartbeat + perf)
                                // Heartbeats = 512 (8 * 64 bytes)
                                // Spinlocks = 256 (8 * 32 bytes)
                                // Total: 16 + 128 + 25 + 28 + 264 + 512 + 256 = 1229
                                // Padding: 4096 - 1229 = 2867 bytes
```

---

## 6. Code Review Checklist

Before committing changes to shared memory structures:

- [ ] No magic numbers used (grep for hardcoded array sizes)
- [ ] All constants defined in `detail` namespace
- [ ] Static assertions added for all size constraints
- [ ] Size calculation comments updated
- [ ] Version compatibility considered
- [ ] Tests verify structure size and layout

---

## 7. Enforcement

### Automated Checks

Add to CI pipeline:
```bash
# Check for magic numbers in shared memory code
if grep -r "for.*i.*<.*[0-9]" src/include/utils/data_block.hpp src/utils/data_block.cpp | grep -v "MAX_"; then
    echo "ERROR: Magic numbers found in DataBlock code"
    exit 1
fi
```

### Pre-commit Hook

```bash
#!/bin/bash
# Check for magic numbers in staged changes
git diff --cached --name-only | grep -E "(data_block|message_hub)" | while read file; do
    if git diff --cached "$file" | grep -E "^\+.*for.*<.*[0-9]" | grep -v "MAX_"; then
        echo "ERROR: Magic number detected in $file"
        echo "Use named constants from detail:: namespace"
        exit 1
    fi
done
```

---

## 8. Examples of Version Transitions

### Example: Increasing MAX_SHARED_SPINLOCKS from 8 to 16

**Step 1**: Define V2 constants
```cpp
namespace detail {
    // Version 2.0 layout constants
    inline constexpr uint16_t HEADER_VERSION_MAJOR_V2 = 2;
    inline constexpr size_t MAX_SHARED_SPINLOCKS_V2 = 16;
}
```

**Step 2**: Create V2 header struct
```cpp
struct alignas(4096) SharedMemoryHeader_V2 {
    // ... same fields as V1 ...
    SharedSpinLockState spinlock_states[detail::MAX_SHARED_SPINLOCKS_V2]; // Now 16
    uint8_t reserved_header[2611]; // Recalculated: 4096 - 1229 - (16-8)*32 = 2611
};
```

**Step 3**: Add migration code
```cpp
if (header->version_major == 1) {
    auto* v1_header = reinterpret_cast<SharedMemoryHeader_V1*>(header);
    // Use V1 layout
} else if (header->version_major == 2) {
    auto* v2_header = reinterpret_cast<SharedMemoryHeader_V2*>(header);
    // Use V2 layout
}
```

---

## Summary

**Golden Rule**: Every numeric literal in shared memory structure definitions must be a named constant associated with a version number. This is non-negotiable for maintaining ABI stability and tracking changes across versions.

**Rationale**: Shared memory structures are the most critical part of the IPC system. A single byte misalignment can cause crashes, data corruption, or security vulnerabilities across process boundaries. Version-tracked named constants ensure we can:
1. Track what changed between versions
2. Detect incompatibilities at compile time
3. Implement safe migration paths
4. Debug layout issues quickly
5. Document the reasoning behind size choices
