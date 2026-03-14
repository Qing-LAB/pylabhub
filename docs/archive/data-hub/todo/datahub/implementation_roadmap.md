# Data Exchange Hub Implementation Roadmap

This document outlines the detailed tasks required to implement the Data Exchange Hub design, incorporating the Layer 2 Transaction API (P7), Error Recovery API (P8), and Schema Validation (P9). This roadmap will be continuously updated as work progresses.

## Current Status

-   **P7 (Layer 2 Transaction API):** Design Approved, ready for implementation.
-   **P8 (Error Recovery API):** Design Approved, ready for implementation.
-   **P9 (Schema Validation):** Design Pending.

## TODO List

### 1. Initial Setup & Documentation Sync (Priority: High)

-   [x] **Update `docs/code_review/INDEX.md`**: Mark P7 and P8 as completed in the "Critical Remaining" section and move them to the "Completed" section.
-   [x] **Document Update `HEP-core-0002-data-hub-structured.md`**: All updates specified in `HEP_UPDATE_ACTION_PLAN.md` (Sections 9, 10, 11, 12, 13, 15, 16, 17, 18) are now complete, reflecting the comprehensive design.

### 2. Design P9: Schema Validation (Priority: High)

-   [ ] **Define schema hash computation**: Specify how the schema hash (e.g., BLAKE2b) will be computed (e.g., hash of struct definition, IDL, or a derived metadata string).
-   [ ] **Integrate schema validation into `attach()` flow**: Design how consumers will validate the schema during `find_datablock_consumer()` and `DataBlock::DataBlock(name)` construction.
-   [ ] **Specify broker schema registry (optional)**: Design the broker's role in storing and validating schema hashes during producer registration and consumer discovery.
-   [ ] **Document version compatibility rules**: Define how schema versions are managed and what constitutes a breaking vs. non-breaking change.
-   [ ] **Update `HEP-core-0002.md` for P9**: Add detailed design specifications for P9 in the relevant sections (e.g., Section 10.1 Shared Models and Schemas, Section 9 Data Safety, Integrity, and Crash Recovery).

### 3. Core Refactoring (Priority: Medium)

-   [ ] **Refactor `SharedSpinLock` usage**: Replace direct usage of `SharedSpinLock` with `SharedSpinLockGuardOwning` where appropriate to leverage RAII for improved safety and reduced boilerplate.

### 4. Implement P7: Layer 2 Transaction API (Priority: High)

#### 4.1 Producer Write Transaction
-   [ ] Implement `with_write_transaction` template function for `DataBlockProducer` based on Layer 2 API design in `HEP-core-0002.md` Section 10.3.2.
-   [ ] Implement `WriteTransactionGuard` RAII class for managing write slot lifetimes, based on design in `HEP-core-0002.md` Section 10.3.5.
-   [ ] Add unit tests for `with_write_transaction` and `WriteTransactionGuard`, covering:
    -   Basic successful transaction (commit and release).
    -   Failed acquisition (e.g., timeout).
    -   Early return from lambda.
    -   Exception thrown during lambda execution (ensuring no commit and proper release).
    -   Move semantics for `WriteTransactionGuard`.
-   [ ] Add usage examples for `with_write_transaction` and `WriteTransactionGuard` to the examples directory.

#### 4.2 Consumer Read Transaction
-   [ ] Implement `with_read_transaction` template function for `DataBlockConsumer` based on Layer 2 API design in `HEP-core-0002.md` Section 10.3.2.
-   [ ] Implement `with_next_slot` convenience wrapper for `DataBlockSlotIterator`.
-   [ ] Implement `ReadTransactionGuard` RAII class for managing read slot lifetimes.
-   [ ] Add unit tests for consumer transaction APIs, covering:
    -   Basic successful transaction (release).
    -   Failed acquisition.
    -   Early return from lambda.
    -   Exception thrown during lambda execution (ensuring proper release).
    -   Move semantics for `ReadTransactionGuard`.
-   [ ] Add usage examples for `with_read_transaction`, `with_next_slot`, and `ReadTransactionGuard` to the examples directory.

### 5. Implement P8: Error Recovery API (Priority: High)

#### 5.1 C API - Diagnostics
-   [ ] Implement `datablock_diagnose_slot` to get diagnostic information for a single slot, based on C API in `P8_ERROR_RECOVERY_API_DESIGN.md`.
-   [ ] Implement `datablock_diagnose_all_slots` to get diagnostic information for all slots.
-   [ ] Implement `datablock_is_process_alive` (platform-specific implementation for Windows and POSIX).

#### 5.2 C API - Recovery Operations
-   [ ] Implement `datablock_force_reset_slot` to forcefully reset a single slot with safety checks.
-   [ ] Implement `datablock_force_reset_all_slots` to forcefully reset all slots with safety checks.
-   [ ] Implement `datablock_release_zombie_readers` to release reader locks held by dead processes.
-   [ ] Implement `datablock_release_zombie_writer` to release the write lock held by a dead process.
-   [ ] Implement `datablock_cleanup_dead_consumers` to clean up heartbeat entries for dead consumers.
-   [ ] Implement `datablock_validate_integrity` to perform shared memory integrity checks.

#### 5.3 C++ Wrapper API
-   [ ] Implement `SlotDiagnostics` C++ class providing convenient methods for diagnostic functions.
-   [ ] Implement `SlotRecovery` C++ class providing convenient methods for recovery operations.
-   [ ] Implement `HeartbeatManager` C++ class for managing and cleaning up consumer heartbeats.
-   [ ] Implement `IntegrityValidator` C++ class for validating shared memory integrity.

#### 5.4 CLI Tool and Python Bindings
-   [ ] Develop `datablock-admin` command-line tool using the C API for diagnostics and recovery, based on CLI specification in `P8_ERROR_RECOVERY_API_DESIGN.md`.
-   [ ] Create Python bindings (e.g., using `ctypes` or `pybind11`) for the recovery API functions to enable scripting.

### 6. Integration, Testing & Documentation (Priority: High)

-   [ ] **Integration Tests:**
    -   Add integration tests to verify the interaction between producer/consumer using P7 transaction APIs.
    -   Add integration tests for P8 recovery scenarios, simulating stuck writers, zombie readers, and dead consumers.
    -   Add integration tests for P9 schema validation, verifying correct behavior on match/mismatch.
-   [ ] **Performance Benchmarks:**
    -   Add benchmarks for P7 transaction APIs to confirm negligible overhead.
-   [ ] **Documentation Updates:**
    -   Update API reference documentation for all new P7, P8, and P9 functions and classes.
    -   Add emergency procedures documentation based on P8's recovery capabilities, including `datablock-admin` usage.
    -   Add usage examples for P9 schema negotiation.

---