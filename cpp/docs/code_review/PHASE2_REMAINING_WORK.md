# Phase 2 Refactoring - Remaining Work Checklist

**Date:** 2026-02-15  
**Priority:** CRITICAL  
**Status:** IN PROGRESS

---

## ✅ COMPLETED (Just Now)

1. ✅ **Fixed `DataBlockLayout::validate()`** - Now validates 4K-aligned memory layout correctly
2. ✅ **Added `detail::PAGE_SIZE` constant** - Single source for 4096 value
3. ✅ **Added layout query APIs** - `has_flex_zone()`, `control_zone_size()`, `control_zone_end()`
4. ✅ **Fixed layout calculations** - Both `from_config()` and `from_header()` use correct 4K alignment

---

## 🔴 CRITICAL - Must Fix Immediately

### 1. Remove Obsolete `flexible_zone_idx` Parameter

**Problem:** All flex zone APIs accept an index parameter, but only 0 is valid (single zone design).

**Files to modify:**
- `data_block.hpp` - Remove parameter from all public APIs
- `data_block.cpp` - Simplify all implementations

**Functions to update:**
```cpp
// DataBlockProducer
bool update_checksum_flexible_zone(size_t flexible_zone_idx) → update_checksum_flexible_zone()
std::span<std::byte> flexible_zone_span(size_t index) → flexible_zone_span()

// DataBlockConsumer  
bool verify_checksum_flexible_zone(size_t flexible_zone_idx) → verify_checksum_flexible_zone()
std::span<const std::byte> flexible_zone_span(size_t index) → flexible_zone_span()

// SlotWriteHandle
std::span<std::byte> flexible_zone_span(size_t flexible_zone_idx) → flexible_zone_span()
bool update_checksum_flexible_zone(size_t flexible_zone_idx) → update_checksum_flexible_zone()

// SlotConsumeHandle
std::span<const std::byte> flexible_zone_span(size_t flexible_zone_idx) → flexible_zone_span()
bool verify_checksum_flexible_zone(size_t flexible_zone_idx) → verify_checksum_flexible_zone()
```

**Implementation note:** Keep default parameter `= 0` temporarily for backward compatibility if needed.

---

### 2. Consolidate Flex Zone Access Logic

**Problem:** 4 nearly identical implementations of `flexible_zone_span()`

**Current locations:**
1. `DataBlockProducer::flexible_zone_span()` - lines ~1716-1732
2. `SlotWriteHandle::flexible_zone_span()` - lines ~2250-2267  
3. `DataBlockConsumer::flexible_zone_span()` - lines ~2454-2469
4. `SlotConsumeHandle::flexible_zone_span()` - lines ~2355-2372

**Solution:** Create single template helper in `detail` namespace:

```cpp
namespace detail {
template<typename ImplT>
inline std::span<std::byte> get_flex_zone_span(ImplT* impl) noexcept {
    if (impl == nullptr || impl->dataBlock == nullptr || impl->flex_zone_size == 0) {
        return {};
    }
    
    char* zone_base = impl->dataBlock->flexible_data_zone();
    if (zone_base == nullptr) {
        return {};
    }
    
    return {reinterpret_cast<std::byte*>(zone_base + impl->flex_zone_offset),
            impl->flex_zone_size};
}

template<typename ImplT>
inline std::span<const std::byte> get_flex_zone_span_const(const ImplT* impl) noexcept {
    if (impl == nullptr || impl->dataBlock == nullptr || impl->flex_zone_size == 0) {
        return {};
    }
    
    const char* zone_base = impl->dataBlock->flexible_data_zone();
    if (zone_base == nullptr) {
        return {};
    }
    
    return {reinterpret_cast<const std::byte*>(zone_base + impl->flex_zone_offset),
            impl->flex_zone_size};
}
} // namespace detail
```

Then all public APIs become one-liners:
```cpp
std::span<std::byte> DataBlockProducer::flexible_zone_span() noexcept {
    return detail::get_flex_zone_span(pImpl.get());
}
```

---

### 3. Consolidate Checksum Validation Logic

**Problem:** Duplicated validation in `update_checksum_flexible_zone_impl()` and `verify_checksum_flexible_zone_impl()`

**Solution:** Extract common validation:

```cpp
namespace detail {
struct FlexZoneChecksumAccess {
    SharedMemoryHeader* header;
    char* zone_ptr;
    size_t zone_size;
    size_t checksum_idx;  // Always 0 for single zone
};

inline std::optional<FlexZoneChecksumAccess> 
validate_flex_zone_for_checksum(DataBlock* block) noexcept {
    if (block == nullptr || block->header() == nullptr) {
        return std::nullopt;
    }
    
    const auto& layout = block->layout();
    if (layout.slot_checksum_size == 0 || !layout.has_flex_zone()) {
        return std::nullopt;
    }
    
    char* zone_ptr = block->flexible_data_zone();
    if (zone_ptr == nullptr) {
        return std::nullopt;
    }
    
    return FlexZoneChecksumAccess{
        .header = block->header(),
        .zone_ptr = zone_ptr,
        .zone_size = layout.flexible_zone_size,
        .checksum_idx = 0  // Single zone always uses index 0
    };
}
} // namespace detail
```

Then simplify both checksum functions:
```cpp
inline bool update_checksum_flexible_zone_impl(DataBlock *block) {
    auto access = detail::validate_flex_zone_for_checksum(block);
    if (!access) {
        return false;
    }
    
    if (!pylabhub::crypto::compute_blake2b(
            access->header->flexible_zone_checksums[access->checksum_idx].checksum_bytes,
            access->zone_ptr,
            access->zone_size)) {
        return false;
    }
    
    access->header->flexible_zone_checksums[access->checksum_idx].valid.store(
        1, std::memory_order_release);
    return true;
}
```

---

## 🟡 HIGH PRIORITY - Cleanup

### 4. Add Missing Public APIs

**Needed for diagnostics and debugging:**

```cpp
class DataBlockProducer {
public:
    /** Query flex zone size (0 if not configured) */
    size_t get_flex_zone_size() const noexcept;
    
    /** Get memory layout information for diagnostics */
    struct LayoutInfo {
        size_t header_size;
        size_t control_zone_size;
        size_t flex_zone_offset;
        size_t flex_zone_size;
        size_t ring_buffer_offset;
        size_t ring_buffer_size;
        size_t total_size;
    };
    LayoutInfo get_layout_info() const noexcept;
};
```

---

### 5. Update All Comments and Documentation

**Files to update:**
- Remove all "multi-zone" references
- Remove "FlexibleZoneInfo" references  
- Update examples to show single zone usage
- Update error messages to remove "zone_index"

---

### 6. Test Coverage

**Required tests:**
1. ✅ Layout validation in debug build
2. ⬜ Layout calculation matches design spec exactly
3. ⬜ 4K alignment verification
4. ⬜ Flex zone access with size 0, 4K, 8K, 12K
5. ⬜ Ring-buffer alignment verification
6. ⬜ Checksum update/verify for flex zone
7. ⬜ Multi-process shared memory layout consistency

---

## 🟢 LOWER PRIORITY - Nice to Have

### 7. Performance Optimizations

- Consider caching layout pointers in impl structures
- Profile pointer arithmetic overhead
- Consider alignment hints for better cache performance

### 8. Error Message Improvements

- Add layout dump function for debugging
- Improve validation error messages to show expected vs actual values
- Add assertions for impossible states

---

## Implementation Order

1. **CRITICAL (Today):**
   - ✅ Fix layout validation
   - ⬜ Remove `flexible_zone_idx` parameter
   - ⬜ Consolidate flex zone span logic
   - ⬜ Test in debug build

2. **HIGH (Tomorrow):**
   - ⬜ Consolidate checksum validation
   - ⬜ Add query APIs
   - ⬜ Update documentation

3. **MEDIUM (This Week):**
   - ⬜ Comprehensive test suite
   - ⬜ Code review
   - ⬜ Performance validation

---

## Success Criteria

- [ ] All debug assertions pass
- [ ] No duplicated layout calculations
- [ ] All flex zone access goes through standardized APIs
- [ ] Test coverage > 90%
- [ ] Documentation updated
- [ ] Code review approved
- [ ] Performance benchmarks meet targets

---

## Notes

- **Breaking API changes:** Removing `flexible_zone_idx` is breaking. Consider deprecation strategy.
- **Testing:** Need multi-process tests to validate shared memory layout consistency.
- **Documentation:** Update HEP-CORE-0002 to reflect single zone design.
