# P9: Schema Validation - Design Specification
**Date:** 2026-02-07  
**Priority:** MEDIUM-HIGH (Critical for data integrity)  
**Effort:** 1-2 days design, ~150 lines implementation  
**Dependencies:** P6 (Broker Integration)

---

## PROBLEM STATEMENT

### The ABI Mismatch Problem

**Scenario: Producer and Consumer Disagree on Struct Layout**

**Producer (Version 1):**
```cpp
struct SensorData {
    float x, y, z;          // 12 bytes
    uint32_t timestamp;     // 4 bytes
};  // Total: 16 bytes
```

**Consumer (Version 2):**
```cpp
struct SensorData {
    double x, y, z;         // 24 bytes (changed float → double!)
    uint64_t timestamp;     // 8 bytes (changed uint32_t → uint64_t!)
};  // Total: 32 bytes
```

**Result:**
```
Producer writes 16 bytes: [0.1f, 0.2f, 0.3f, 1234u]
Consumer reads 32 bytes: [garbage, garbage, garbage, garbage]
→ SILENT CORRUPTION, NO ERROR
```

**Current State:**
- **No schema enforcement** → Producer and consumer can disagree
- **No version checking** → Breaking changes undetected
- **No type safety** → Wrong struct passed at compile time
- **Silent corruption** → Data looks valid but is garbage

---

## DESIGN GOALS

1. **Detect ABI Mismatches:** Producer and consumer must agree on struct layout
2. **Version Compatibility:** Support compatible schema evolution
3. **Early Failure:** Fail at `attach()` time, not during data processing
4. **Minimal Overhead:** Schema validation once at startup, not per-message
5. **Human-Readable:** Schema description for debugging
6. **Optional Broker Registry:** Centralized schema management

---

## RECOMMENDED APPROACH

### Three-Layer Schema Validation

```
Layer 1: Compile-Time (C++ Templates)
  - Type system prevents wrong struct at compile time
  - Static assertions on size/alignment

Layer 2: Runtime (Schema Hash)
  - BLAKE2b hash of struct layout
  - Validated at attach() time
  - Mismatch → exception/error

Layer 3: Broker Registry (Optional)
  - Centralized schema database
  - Version compatibility rules
  - Migration support
```

See P9_SCHEMA_VALIDATION_DESIGN.md for complete specification.
