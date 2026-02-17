# Core Structure Change Protocol

**Document ID**: PROT-CORE-001  
**Version**: 1.0.0  
**Date**: 2026-02-15  
**Status**: Active

## Purpose

This document defines the mandatory review process when modifying core data structures in the PyLabHub codebase. Core structures are ABI-sensitive components that affect shared memory layout, serialization, or cross-process communication.

## Core Structures Requiring This Protocol

1. **`SharedMemoryHeader`** (`data_block.hpp`)
   - ABI-critical: 4KB-aligned, shared memory layout
   - Change impact: All producers, consumers, broker registration
   
2. **`DataBlockConfig`** (`data_block.hpp`)
   - API-critical: Factory function parameters
   - Change impact: All create/find functions, validation logic
   
3. **`FlexibleZoneChecksumEntry`** (`data_block.hpp`)
   - Layout-critical: Embedded in SharedMemoryHeader
   
4. **`ConsumerHeartbeat`** (`data_block.hpp`)
   - Layout-critical: Embedded in SharedMemoryHeader
   
5. **BLDS Schema structures** (`schema_validation.hpp`)
   - ABI-critical: Hash computation affects compatibility

## Change Impact Matrix

### When Modifying `SharedMemoryHeader`

| Component | Review Required | Verification Method |
|-----------|----------------|---------------------|
| **Size & Alignment** | ✓ MANDATORY | `static_assert` at 4096 bytes, `alignof` check |
| **Schema Macro** | ✓ MANDATORY | Update `PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS` |
| **Constructor/Initialization** | ✓ MANDATORY | `DataBlock::DataBlock()` in `data_block.cpp` |
| **Producer Registration** | ✓ MANDATORY | `register_with_broker()` schema hash logic |
| **Consumer Discovery** | ✓ MANDATORY | `find_datablock_consumer_impl()` validation |
| **Schema Validation** | ✓ MANDATORY | All `generate_schema_info<SharedMemoryHeader>()` calls |
| **Checksum Logic** | ✓ MANDATORY | `update_checksum_flexible_zone()`, `verify_checksum_flexible_zone()` |
| **Serialization** | ✓ MANDATORY | Any code reading/writing header fields for persistence |
| **Test Coverage** | ✓ MANDATORY | Update `test_data_block.cpp`, `test_schema_validation.cpp` |
| **Documentation** | ✓ MANDATORY | Update memory layout diagrams, API docs |

### When Modifying `DataBlockConfig`

| Component | Review Required | Verification Method |
|-----------|----------------|---------------------|
| **Factory Functions** | ✓ MANDATORY | All `create_datablock_producer*()` signatures |
| **Validation Functions** | ✓ MANDATORY | `validate_config()`, size/alignment checks |
| **Default Values** | ✓ MANDATORY | Ensure defaults are safe/sensible |
| **C++ Template APIs** | ✓ MANDATORY | Template instantiations with new config fields |
| **RAII Layer** | ✓ MANDATORY | `TransactionContext::validate_schema()`, `validate_layout()` |
| **Examples** | ✓ MANDATORY | `raii_layer_example.cpp`, other examples |
| **Test Coverage** | ✓ MANDATORY | Config validation tests |

### When Modifying Schema System (BLDS)

| Component | Review Required | Verification Method |
|-----------|----------------|---------------------|
| **Schema Generation** | ✓ MANDATORY | All `generate_schema_info<T>()` calls |
| **Hash Computation** | ✓ MANDATORY | BLAKE2b input order, field serialization |
| **Version Compatibility** | ✓ MANDATORY | `validate_schema_match()`, version comparison logic |
| **Macro Expansion** | ✓ MANDATORY | `PYLABHUB_SCHEMA_BEGIN/END/MEMBER` usage |
| **Storage Locations** | ✓ MANDATORY | Where schema hashes are stored (header fields) |
| **Validation Points** | ✓ MANDATORY | Where schema validation occurs (producer create, consumer find) |

## Mandatory Review Checklist

### Phase 1: Structure Definition Review

- [ ] **Size Verification**: Does the structure still meet size requirements? (e.g., 4KB for header)
- [ ] **Alignment Verification**: Are alignment requirements preserved?
- [ ] **Padding Calculation**: Is padding correctly calculated to reach target size?
- [ ] **Field Order**: Does field order preserve cache line optimization?
- [ ] **ABI Compatibility**: Are changes backward-compatible, or is this a breaking change?

### Phase 2: Initialization & Construction

- [ ] **Constructor Logic**: Are all new fields initialized?
- [ ] **Default Values**: Are defaults safe for shared memory?
- [ ] **Zeroing**: Are arrays/padding zeroed appropriately?
- [ ] **Atomic Initialization**: Are atomic fields initialized correctly?

### Phase 3: Serialization & Storage

- [ ] **Write Paths**: All locations writing to new/modified fields
- [ ] **Read Paths**: All locations reading from new/modified fields
- [ ] **Schema Macro**: Schema field list updated with correct types
- [ ] **Hash Computation**: Schema hash includes new fields in correct order

### Phase 4: Validation & Error Handling

- [ ] **Size Checks**: `sizeof()` validations updated
- [ ] **Schema Checks**: BLDS validation logic updated
- [ ] **Compatibility Checks**: Version/schema mismatch detection
- [ ] **Error Messages**: Clear error messages for validation failures

### Phase 5: API Surface Review

#### C-Level API (`data_block.c` compatibility layer)
- [ ] **Type-Agnostic**: C API remains typeless (void*, size_t)
- [ ] **Error Codes**: Return codes cover new failure modes
- [ ] **Documentation**: C API docs reflect behavior changes
- [ ] **Examples**: C examples updated if needed

#### C++ API (`data_block.hpp`)
- [ ] **Template Parameters**: Template signatures match new structure
- [ ] **Type Safety**: `static_assert` checks for new constraints
- [ ] **RAII Wrappers**: RAII layer reflects structure changes
- [ ] **Exception Safety**: Exception paths handle new fields
- [ ] **Move/Copy Semantics**: Impl structs handle new fields in move

### Phase 6: Dependent Code Review

- [ ] **Producer Code**: `DataBlockProducer` methods
- [ ] **Consumer Code**: `DataBlockConsumer` methods
- [ ] **Transaction Context**: RAII layer validation functions
- [ ] **Iterator Code**: Slot/zone iteration logic
- [ ] **Heartbeat Code**: Consumer registration/heartbeat updates
- [ ] **Broker Integration**: MessageHub registration logic
- [ ] **Checksum Code**: Flexible zone checksum updates

### Phase 7: Test & Documentation

- [ ] **Unit Tests**: New tests for modified behavior
- [ ] **Integration Tests**: End-to-end tests pass
- [ ] **Performance Tests**: No unexpected performance regression
- [ ] **Examples**: All examples compile and run
- [ ] **API Documentation**: Public API docs updated
- [ ] **Architecture Docs**: Memory layout diagrams updated
- [ ] **Migration Guide**: If breaking, provide migration path

### Phase 8: Obsolete Code Removal

- [ ] **Dead Fields**: Remove fields no longer used
- [ ] **Dead Functions**: Remove functions no longer called
- [ ] **Dead Macros**: Remove macros no longer expanded
- [ ] **Dead Tests**: Remove tests for removed functionality
- [ ] **Deprecated APIs**: Mark deprecated, provide alternatives

## Review Standards by Layer

### C-Level API Standard
**Philosophy**: Typeless, error-code based, minimal assumptions

- Must remain typeless (no template, no C++ types in signature)
- Must use error codes (int return, error out-params)
- Must validate all pointers for NULL
- Must validate all sizes for overflow
- Must document all error conditions
- Must be callable from C (no name mangling)
- Must not throw exceptions (noexcept)

### C++ Abstraction Standard
**Philosophy**: Type-safe, exception-based, RAII-managed

- Must use strong typing (templates, typed references)
- Must use RAII (unique_ptr, guards, contexts)
- Must throw typed exceptions (std::invalid_argument, custom exceptions)
- Must use compile-time checks (static_assert, concepts)
- Must enforce const correctness
- Must support move semantics (noexcept move)
- Must provide clear lifetime contracts (documented in class comments)

## Example: SharedMemoryHeader Change (Phase 4 Dual Schema)

### Change Description
Added `flexzone_schema_hash[32]` and `datablock_schema_hash[32]`, removed `schema_hash[32]`.

### Components Reviewed

1. **Structure Size**: ✓ Recalculated padding (removed `padding_sec` to absorb +32 bytes)
2. **Schema Macro**: ✓ Updated `PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS`
3. **Initialization**: ✓ Updated `DataBlock::DataBlock()` to zero both hashes
4. **Producer Write**: ✓ Updated `create_datablock_producer_impl()` to write both hashes
5. **Consumer Read**: ✓ Updated `find_datablock_consumer_impl()` to validate both hashes
6. **Broker Registration**: ✓ Updated `register_with_broker()` to use `datablock_schema_hash`
7. **API Signatures**: ✓ Added dual-template `create_datablock_producer<FlexZoneT, DataBlockT>()`
8. **Validation Logic**: ✓ Added dual schema validation in `find_datablock_consumer_impl()`
9. **Examples**: ✓ Updated `raii_layer_example.cpp` to use dual-schema API
10. **Documentation**: ✓ Created `PHASE4_DUAL_SCHEMA_API_DESIGN.md`

### Issues Found During Review
- ❌ Old `schema_hash` field references in `data_block.cpp` (lines 967, 2000)
- ❌ Schema macro still listed single `schema_hash` field
- ❌ Padding calculation incorrect (4120 vs 4096)
- ❌ Consumer heartbeat registration called before consumer construction

### Lessons Learned
- Always grep for old field names after removal
- Always verify `static_assert` passes after size changes
- Always check both write and read paths for field access
- Always construct objects before calling their methods

## Automation Opportunities

### Static Analysis Hooks
- Pre-commit hook: Check `static_assert` for SharedMemoryHeader size
- CI: Verify schema macro matches actual struct fields
- CI: Run clang-tidy with strict checks on modified files

### Review Tooling
- Script to extract all references to a field name
- Script to verify padding calculations
- Script to check API signature consistency

## Approval Requirements

### Minor Changes (Non-Breaking)
- Self-review using this protocol
- Update tests
- Update documentation
- Peer review recommended

### Major Changes (Breaking ABI)
- Full protocol compliance mandatory
- Peer review required
- Architecture review required
- Migration guide required
- Version bump required

---

**Document Control**  
Last Modified: 2026-02-15  
Maintained by: Core Architecture Team  
Review Cycle: Quarterly or on any core structure change
