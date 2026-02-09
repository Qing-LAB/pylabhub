# Data Exchange Hub - Implementation TODO

**Last Updated:** 2026-02-09
**Priority Legend:** 🔴 Critical | 🟡 High | 🟢 Medium | 🔵 Low

---

## Quick Status

| Phase | Status | Progress | Target |
|-------|--------|----------|--------|
| **P1-P8 Design** | ✅ Complete | 100% | Done |
| **P9 Design** | ✅ Complete | 100% | Done |
| **Core Implementation** | 🟡 In Progress | 30% | Week 2-3 |
| **P9 Implementation** | ⏳ Not Started | 0% | Week 1 |
| **Testing** | ⏳ Not Started | 0% | Week 4 |
| **Deployment** | ⏳ Not Started | 0% | Week 5 |

---

## Phase 0: Code Refactoring and Service Integration

### 🔴 PRIORITY 0.1: Refactor Helper Functions to Service Modules

**Rationale**: Many helper functions in DataHub could be generalized and moved to appropriate service modules for better code reuse, separation of concerns, and independent testing.

#### Platform Service Enhancements (`src/utils/platform.cpp`)

- [ ] **Move PID liveness check** from `SharedSpinLock::is_process_alive()` to `pylabhub::platform::is_process_alive(uint64_t pid)`
  - Current: In `datablock_spinlock.cpp`
  - Target: `platform.cpp` (more general utility)
  - Signature: `PYLABHUB_UTILS_EXPORT bool pylabhub::platform::is_process_alive(uint64_t pid);`
  - Benefits: Reusable by FileLock, other IPC modules

- [ ] **Move monotonic timestamp** from anonymous namespace to platform utilities
  - Current: `now_ns()` in `data_block.cpp`
  - Target: `pylabhub::platform::monotonic_time_ns()`
  - Signature: `PYLABHUB_UTILS_EXPORT uint64_t pylabhub::platform::monotonic_time_ns();`
  - Benefits: Consistent timestamp source across modules

- [ ] **Add platform-specific shared memory utilities** to platform module
  - `shm_create()`, `shm_open()`, `shm_unlink()` wrappers
  - Current: Scattered in `data_block.cpp`
  - Target: `pylabhub::platform::shm_*` family
  - Benefits: Centralized platform abstraction

#### Debug Info Enhancements (`src/utils/debug_info.cpp`)

- [ ] **Add deadlock detector helper**
  - Current: Timeout logic scattered in acquisition functions
  - Target: `pylabhub::debug::detect_potential_deadlock()`
  - Use case: Detect hung locks, readers, spinlocks
  - Benefits: Reusable across all synchronization primitives

- [ ] **Add memory corruption detector**
  - Current: Magic number checks in `data_block.cpp`
  - Target: `pylabhub::debug::validate_magic_number()`
  - Benefits: Reusable for other shared memory structures

#### Crypto Utilities Module (NEW)

- [ ] **Create `src/utils/crypto_utils.cpp`** for cryptographic primitives
  - Move `compute_blake2b()` from anonymous namespace
  - Move `verify_blake2b()` from anonymous namespace
  - Add `generate_random_secret()` for shared secret generation
  - Benefits: Single point for libsodium initialization, reusable checksums

- [ ] **Add header `src/include/utils/crypto_utils.hpp`**
  - Export: `compute_blake2b()`, `verify_blake2b()`, `generate_random_bytes()`
  - Integrate with lifecycle (libsodium init on startup)

#### Backoff Strategy Module (NEW)

- [ ] **Create `src/include/utils/backoff_strategy.hpp`** (header-only)
  - Move `backoff(int iteration)` from anonymous namespace
  - Add configurable strategies: `ExponentialBackoff`, `ConstantBackoff`, `NoBackoff`
  - Benefits: Reusable for all spin loops (FileLock, SharedSpinLock, SlotRWState)

**Estimated Effort**: 2-3 days
**Dependencies**: None (can be done in parallel with other tasks)
**Testing**: Unit tests for each new utility function

---

## Phase 1: P9 Schema Validation (Week 1)

### 🔴 PRIORITY 1.1: BLDS Schema Generation

- [ ] **Define BLDS grammar and type ID mapping**
  - File: `src/include/utils/schema_blds.hpp`
  - Implement: Type ID mapping (f32, f64, i8-i64, u8-u64, arrays, nested structs)
  - Implement: Canonical BLDS string generation

- [ ] **Implement schema registration macros**
  - `PYLABHUB_SCHEMA_BEGIN(StructName)`
  - `PYLABHUB_SCHEMA_MEMBER(member_name)`
  - `PYLABHUB_SCHEMA_END()`
  - Use: Compile-time reflection to generate BLDS string

- [ ] **Implement `SchemaInfo` struct**
  ```cpp
  struct SchemaInfo {
      std::string name;          // "SensorHub.SensorData"
      std::string blds;          // "timestamp_ns:u64;temperature:f32;..."
      std::array<uint8_t, 32> hash; // BLAKE2b-256
      SchemaVersion version;     // {major, minor, patch}
  };
  ```

- [ ] **Implement hash computation**
  - Use BLAKE2b-256 from libsodium (via new crypto_utils module)
  - Personalization string: `"PYLABHUB_SCHEMA_V1"`

**Estimated Effort**: 2 days
**Dependencies**: Phase 0 (crypto_utils module)
**Testing**: Test BLDS generation for various struct layouts

### 🔴 PRIORITY 1.2: Producer Schema Registration

- [ ] **Update `SharedMemoryHeader`** to include schema fields
  - Already done: `schema_hash[32]`, `schema_version`
  - Verify: Padding calculation is correct

- [ ] **Update `create_datablock_producer()` overload with schema**
  ```cpp
  template <typename Schema>
  std::unique_ptr<DataBlockProducer>
  create_datablock_producer(MessageHub& hub, const std::string& name,
                            DataBlockPolicy policy, const DataBlockConfig& config,
                            const Schema& schema_instance);
  ```

- [ ] **Compute and store schema hash in header**
  - Generate BLDS from `Schema` type
  - Compute BLAKE2b-256 hash
  - Store in `SharedMemoryHeader::schema_hash`

- [ ] **Include schema in broker registration**
  - Update `REG_REQ` message format to include `schema_hash`, `schema_version`, `schema_name`
  - Update `MessageHub::register_producer()` to accept schema info

**Estimated Effort**: 1 day
**Dependencies**: Priority 1.1 (BLDS implementation)
**Testing**: Test producer registration with schema

### 🔴 PRIORITY 1.3: Consumer Schema Validation

- [ ] **Update `find_datablock_consumer()` overload with schema**
  ```cpp
  template <typename Schema>
  std::unique_ptr<DataBlockConsumer>
  find_datablock_consumer(MessageHub& hub, const std::string& name,
                          uint64_t shared_secret,
                          const DataBlockConfig& expected_config,
                          const Schema& schema_instance);
  ```

- [ ] **Implement schema validation flow**
  1. Discover producer via broker (retrieve `schema_hash` from `DISC_RESP`)
  2. Compute expected hash from local `Schema` type
  3. Compare hashes (strict equality check)
  4. Attach to shared memory
  5. Verify hash in `SharedMemoryHeader` matches

- [ ] **Throw `SchemaValidationException` on mismatch**
  ```cpp
  class SchemaValidationException : public std::runtime_error {
      std::array<uint8_t, 32> expected_hash;
      std::array<uint8_t, 32> actual_hash;
  };
  ```

- [ ] **Update metrics**: Increment `SharedMemoryHeader::schema_mismatch_count` on failure

**Estimated Effort**: 1 day
**Dependencies**: Priority 1.2 (producer registration)
**Testing**: Test consumer rejection on schema mismatch

### 🟡 PRIORITY 1.4: Broker Schema Registry

- [ ] **Update broker to store schema metadata**
  - Current: Broker stores `shm_name`, `producer_pid`
  - Add: `schema_hash`, `schema_version`, `schema_name`

- [ ] **Update `DISC_RESP` message** to include schema info
  ```json
  {
    "type": "DISC_RESP",
    "shm_name": "datablock_sensor_12345",
    "schema_hash": "a1b2c3d4...",
    "schema_version": "2.0.0",
    "schema_name": "SensorHub.SensorData"
  }
  ```

- [ ] **Add schema query API** (optional, for tooling)
  - `GET_SCHEMA_REQ` / `GET_SCHEMA_RESP` messages
  - Return: Full BLDS string, hash, version

**Estimated Effort**: 1 day
**Dependencies**: Priority 1.3 (consumer validation)
**Testing**: Test broker schema registry operations

### 🟢 PRIORITY 1.5: Schema Versioning Policy

- [ ] **Define version compatibility rules**
  - Current: Strict hash matching (no tolerance)
  - Future: Semantic versioning (major break, minor backward-compatible)

- [ ] **Implement version checker** (optional)
  ```cpp
  bool is_schema_compatible(const SchemaVersion& producer,
                           const SchemaVersion& consumer);
  ```

- [ ] **Document migration procedures**
  - How to evolve schemas without breaking consumers
  - Add to Section 11 of HEP document

**Estimated Effort**: 0.5 days
**Dependencies**: Priority 1.4 (schema registry)
**Testing**: Test version compatibility logic

---

## Phase 2: Core SlotRWCoordinator Implementation (Week 2)

### 🔴 PRIORITY 2.1: C API (Layer 0) - `slot_rw_coordinator.h`

- [ ] **Define C struct for result types**
  ```c
  typedef enum {
      SLOT_ACQUIRED = 0,
      SLOT_TIMEOUT = 1,
      SLOT_NOT_READY = 2,
      SLOT_RACE_DETECTED = 3
  } SlotAcquireResult;
  ```

- [ ] **Implement writer acquisition**
  ```c
  SlotAcquireResult slot_rw_acquire_write(
      SlotRWState* rw,
      SharedMemoryHeader* header,
      uint32_t slot_index,
      int timeout_ms
  );
  ```

- [ ] **Implement writer release**
  ```c
  void slot_rw_release_write(
      SlotRWState* rw,
      SharedMemoryHeader* header,
      uint32_t slot_index,
      bool did_commit
  );
  ```

- [ ] **Implement reader acquisition (with double-check)**
  ```c
  SlotAcquireResult slot_rw_acquire_read(
      SlotRWState* rw,
      SharedMemoryHeader* header,
      uint32_t slot_index,
      int timeout_ms
  );
  ```

- [ ] **Implement reader release**
  ```c
  void slot_rw_release_read(
      SlotRWState* rw,
      SharedMemoryHeader* header,
      uint32_t slot_index
  );
  ```

**Estimated Effort**: 3 days
**Dependencies**: Phase 0 (backoff_strategy, platform utilities)
**Testing**: Unit tests for each acquisition/release function
**Reference**: Section 4.2 of HEP-CORE-0002-DataHub-FINAL.md

### 🟡 PRIORITY 2.2: C++ Template Wrappers (Layer 1.75) - `slot_rw_access.hpp`

- [ ] **Implement `with_typed_write<T>`**
  ```cpp
  template <typename T, typename Func>
  auto with_typed_write(DataBlockProducer& producer, int timeout_ms, Func&& func);
  ```

- [ ] **Implement `with_typed_read<T>`**
  ```cpp
  template <typename T, typename Func>
  auto with_typed_read(DataBlockConsumer& consumer, uint64_t slot_id, int timeout_ms, Func&& func);
  ```

- [ ] **Add type safety checks**
  - Verify `sizeof(T) <= slot_buffer_size`
  - Verify alignment requirements

**Estimated Effort**: 1 day
**Dependencies**: Priority 2.1 (C API)
**Testing**: Test typed access with various struct types
**Reference**: Section 5.3 of HEP-CORE-0002-DataHub-FINAL.md

### 🟡 PRIORITY 2.3: Transaction Guards (Layer 2) - Already Defined

- [ ] **Complete implementation of `WriteTransactionGuard`**
  - Constructor: Acquire write slot
  - Destructor: Release write slot (commit if `commit()` was called)
  - Methods: `commit()`, `abort()`, `slot()`, `operator bool()`

- [ ] **Complete implementation of `ReadTransactionGuard`**
  - Constructor: Acquire read slot
  - Destructor: Release read slot
  - Methods: `slot()`, `operator bool()`

- [ ] **Verify exception safety**
  - Ensure slots are released even if lambda throws

**Estimated Effort**: 1 day
**Dependencies**: Priority 2.2 (template wrappers)
**Testing**: Test exception paths in transaction lambdas
**Reference**: Section 5.4 of HEP-CORE-0002-DataHub-FINAL.md

---

## Phase 3: DataBlock Factory and Lifecycle (Week 2)

### 🔴 PRIORITY 3.1: Producer Factory Implementation

- [ ] **Implement `create_datablock_producer()` (non-schema version)**
  - Create shared memory segment (use platform utilities from Phase 0)
  - Initialize `SharedMemoryHeader` (magic, version, config)
  - Initialize `SlotRWState` array (all FREE)
  - Initialize flexible zones (zero-initialized)
  - Initialize SharedSpinLock states
  - Create `DataBlockMutex` for control zone

- [ ] **Implement schema-aware overload** (depends on P9)
  - Compute schema hash
  - Store in header
  - Include in broker registration

- [ ] **Implement `DataBlockProducer::register_with_broker()`**
  - Send `REG_REQ` to broker via MessageHub
  - Include: `channel_name`, `shm_name`, `schema_hash`, `schema_version`
  - Handle `REG_RESP` (success/failure)

**Estimated Effort**: 2 days
**Dependencies**: Priority 2.1 (SlotRWState C API), Phase 0 (platform utilities)
**Testing**: Test producer creation with various configs

### 🔴 PRIORITY 3.2: Consumer Factory Implementation

- [ ] **Implement `find_datablock_consumer()` (non-schema version)**
  - Send `DISC_REQ` to broker via MessageHub
  - Parse `DISC_RESP` to get `shm_name`
  - Attach to existing shared memory (use platform utilities)
  - Verify magic number, version, config
  - Register heartbeat in `SharedMemoryHeader::consumer_heartbeats`

- [ ] **Implement schema-aware overload** (depends on P9)
  - Retrieve schema hash from broker
  - Compare with expected hash
  - Verify hash in header matches
  - Throw `SchemaValidationException` on mismatch

- [ ] **Implement heartbeat registration**
  - Find free slot in `consumer_heartbeats[8]`
  - Store PID/UUID, timestamp
  - Update heartbeat periodically (background thread or user-called)

**Estimated Effort**: 2 days
**Dependencies**: Priority 3.1 (producer factory), Phase 0 (platform utilities)
**Testing**: Test consumer discovery and attachment

### 🟡 PRIORITY 3.3: DataBlock Lifecycle Integration

- [ ] **Register DataHub module with Lifecycle**
  - Already done in `message_hub.cpp` (`GetLifecycleModule()`)
  - Verify: ZeroMQ context initialization in startup
  - Verify: Cleanup in shutdown

- [ ] **Add lifecycle check in factory functions**
  ```cpp
  if (!pylabhub::hub::lifecycle_initialized()) {
      throw std::runtime_error("DataHub module not initialized");
  }
  ```

- [ ] **Document lifecycle requirements** in API docs
  - User must create `LifecycleGuard` with `GetLifecycleModule()`
  - Example: See Section 7 of HEP document

**Estimated Effort**: 0.5 days
**Dependencies**: None (already partially implemented)
**Testing**: Test factory calls before/after lifecycle init

---

## Phase 4: MessageHub and Broker Protocol (Week 2-3)

### 🔴 PRIORITY 4.1: MessageHub C++ Wrapper

- [ ] **Complete `MessageHub::connect()` implementation**
  - Create ZeroMQ REQ socket
  - Apply CurveZMQ security (if `server_key` provided)
  - Connect to broker endpoint

- [ ] **Complete `MessageHub::send_message()` implementation**
  - Send two-part message: `[channel, message]`
  - Wait for response with timeout
  - Handle ZeroMQ errors (EAGAIN, ETERM)

- [ ] **Complete `MessageHub::register_producer()` implementation**
  - Construct `REG_REQ` JSON message
  - Call `send_message()`
  - Parse `REG_RESP`

- [ ] **Complete `MessageHub::discover_producer()` implementation**
  - Construct `DISC_REQ` JSON message
  - Call `send_message()`
  - Parse `DISC_RESP`
  - Return `ConsumerInfo` struct

**Estimated Effort**: 2 days
**Dependencies**: None (ZeroMQ already linked)
**Testing**: Test broker communication (requires test broker)
**Reference**: Section 6 of HEP-CORE-0002-DataHub-FINAL.md

### 🟡 PRIORITY 4.2: Broker Service (Separate Binary)

- [ ] **Create `src/admin/broker_service.cpp`**
  - Implement minimal broker (3 messages: REG_REQ, DISC_REQ, DEREG_REQ)
  - Use ZeroMQ REP socket
  - Store producer registry in-memory (std::map)

- [ ] **Implement broker message handlers**
  - `handle_reg_req()`: Register producer, return `REG_RESP`
  - `handle_disc_req()`: Lookup producer, return `DISC_RESP`
  - `handle_dereg_req()`: Deregister producer, return `DEREG_RESP`

- [ ] **Add CurveZMQ support** (optional, for production)
  - Generate broker keypair
  - Enforce client authentication

- [ ] **Add broker CLI** (`datablock-broker`)
  - Start/stop broker
  - Query registered producers
  - Set log level

**Estimated Effort**: 2 days
**Dependencies**: Priority 4.1 (MessageHub)
**Testing**: Integration tests with MessageHub
**Reference**: Section 6.2 of HEP-CORE-0002-DataHub-FINAL.md

---

## Phase 5: P8 Error Recovery (Week 3)

### 🟢 PRIORITY 5.1: Diagnostics API Implementation

- [ ] **Implement `datablock_diagnose_slot()`**
  - Attach to shared memory (read-only)
  - Read `SlotRWState` fields
  - Check if slot is stuck (heuristic: writer lock held > 5 seconds)
  - Fill `SlotDiagnostic` struct

- [ ] **Implement `datablock_diagnose_all_slots()`**
  - Loop over all slots
  - Call `datablock_diagnose_slot()` for each

- [ ] **Implement `datablock_is_process_alive()`**
  - Delegate to `pylabhub::platform::is_process_alive()` (from Phase 0)

**Estimated Effort**: 1 day
**Dependencies**: Phase 0 (platform utilities), Priority 2.1 (SlotRWState)
**Testing**: Test diagnostics on stuck slots

### 🟢 PRIORITY 5.2: Recovery Operations Implementation

- [ ] **Implement `datablock_force_reset_slot()`**
  - Check if writer PID is alive (unless `force=true`)
  - Reset `SlotRWState` to FREE
  - Clear reader count
  - Update metrics: `recovery_actions_count`

- [ ] **Implement `datablock_release_zombie_readers()`**
  - Check if any readers are zombies (heuristic: heartbeat timeout)
  - Clear `reader_count` if all zombies (or `force=true`)

- [ ] **Implement `datablock_release_zombie_writer()`**
  - Check if writer PID is dead
  - Clear `write_lock`
  - Transition to FREE

- [ ] **Implement `datablock_cleanup_dead_consumers()`**
  - Scan `consumer_heartbeats[8]`
  - Check if PID is alive
  - Clear dead consumer entries

**Estimated Effort**: 1.5 days
**Dependencies**: Priority 5.1 (diagnostics)
**Testing**: Test recovery on simulated crashes

### 🟢 PRIORITY 5.3: Integrity Validation

- [ ] **Implement `datablock_validate_integrity()`**
  - Verify magic number
  - Verify version compatibility
  - Recompute checksums (if enabled)
  - Compare with stored checksums
  - If `repair=true`: Rewrite corrected checksums

**Estimated Effort**: 1 day
**Dependencies**: Phase 0 (crypto_utils), Priority 5.2 (recovery ops)
**Testing**: Test integrity validation on corrupted data

### 🟡 PRIORITY 5.4: CLI Tool (`datablock-admin`)

- [ ] **Create `src/admin/datablock_admin.cpp`**
  - Command: `datablock-admin diagnose <shm_name>`
  - Command: `datablock-admin force-reset <shm_name> [--slot=N] [--force]`
  - Command: `datablock-admin cleanup-zombies <shm_name>`
  - Command: `datablock-admin validate <shm_name> [--repair]`

- [ ] **Add CLI parsing** (use `fmt` for output formatting)
  - Parse arguments
  - Call recovery API functions
  - Print human-readable output

**Estimated Effort**: 1 day
**Dependencies**: Priority 5.3 (integrity validation)
**Testing**: Manual CLI testing
**Reference**: Section 8.1 of HEP-CORE-0002-DataHub-FINAL.md

---

## Phase 6: Testing (Week 4)

### 🔴 PRIORITY 6.1: Unit Tests

- [ ] **Test SlotRWState coordination** (`test_datablock.cpp`)
  - Single writer, multiple readers
  - Writer timeout
  - Reader TOCTTOU race detection
  - Generation counter wrap-around

- [ ] **Test SharedSpinLock** (`test_datablock.cpp`)
  - Multi-process contention
  - PID reuse detection
  - Recursive locking

- [ ] **Test P9 Schema Validation** (`test_schema_validation.cpp`)
  - Producer registration with schema
  - Consumer validation (matching schema)
  - Consumer rejection (mismatched schema)
  - BLDS generation for various struct types

- [ ] **Test P7 Transaction API** (`test_transaction_api.cpp`)
  - `with_write_transaction()` success path
  - `with_write_transaction()` timeout path
  - `with_read_transaction()` exception safety
  - `with_next_slot()` iterator

- [ ] **Test P8 Recovery API** (`test_recovery_api.cpp`)
  - Diagnose stuck slots
  - Force reset slot
  - Release zombie readers/writers
  - Cleanup dead consumers

**Estimated Effort**: 3 days
**Dependencies**: All previous phases

### 🟡 PRIORITY 6.2: Integration Tests

- [ ] **Test producer-consumer basic flow**
  - Producer writes, consumer reads
  - Verify data integrity
  - Verify metrics updated

- [ ] **Test multi-consumer scenario**
  - 1 producer, 3 consumers
  - Verify heartbeat registration
  - Verify all consumers receive data

- [ ] **Test ring buffer wrap-around**
  - Producer writes > capacity slots
  - Verify `write_index` wraps correctly
  - Verify no data corruption

- [ ] **Test broker discovery**
  - Producer registers
  - Consumer discovers
  - Verify schema validation (P9)

**Estimated Effort**: 2 days
**Dependencies**: Priority 6.1 (unit tests)

### 🟢 PRIORITY 6.3: Stress Tests

- [ ] **High contention test** (`test_benchmarks.cpp`)
  - 1 producer, 10 consumers
  - 10,000 slots/sec
  - Measure latency (p50, p95, p99)

- [ ] **Long-running test**
  - Run for 1 hour
  - Verify no memory leaks
  - Verify no deadlocks
  - Verify metrics consistency

- [ ] **ThreadSanitizer (ARM)**
  - Run all tests with TSan enabled
  - Fix any race conditions detected

**Estimated Effort**: 2 days
**Dependencies**: Priority 6.2 (integration tests)

---

## Phase 7: Deployment and Documentation (Week 5)

### 🟡 PRIORITY 7.1: Python Bindings (Optional)

- [ ] **Create `src/python/pylabhub_datahub.cpp`**
  - Wrap `DataBlockProducer`, `DataBlockConsumer`
  - Wrap recovery API (`diagnose_slot`, `force_reset_slot`)
  - Use pybind11

- [ ] **Add Python examples**
  - Producer example
  - Consumer example
  - Monitoring script (using recovery API)

**Estimated Effort**: 2 days
**Dependencies**: All core implementation complete

### 🟢 PRIORITY 7.2: Prometheus Exporter (Optional)

- [ ] **Create `src/admin/datablock_exporter.cpp`**
  - Periodically query `SharedMemoryHeader::metrics`
  - Expose as Prometheus metrics (HTTP endpoint)
  - Metrics: `datablock_slots_written_total`, `datablock_writer_timeouts_total`, etc.

**Estimated Effort**: 1 day
**Dependencies**: Priority 7.1 (Python bindings, for easier implementation)

### 🟡 PRIORITY 7.3: Documentation

- [ ] **Update API reference**
  - Generate Doxygen docs
  - Add to `docs/api/`

- [ ] **Update emergency procedures**
  - Expand `docs/emergency_procedures.md`
  - Add troubleshooting guide (common errors, how to diagnose)

- [ ] **Update CLAUDE.md** (if needed)
  - Add DataBlock usage examples
  - Update build instructions

**Estimated Effort**: 1 day
**Dependencies**: All implementation complete

---

## Maintenance Tasks

### 🔵 ONGOING: Code Cleanup

- [ ] Remove obsolete TODOs in code
- [ ] Remove unused headers (e.g., `data_header_sync_primitives.hpp` - already deleted)
- [ ] Consolidate duplicate code (e.g., PID checks, timestamp functions)
- [ ] Run clang-tidy and fix warnings
- [ ] Run clang-format on all modified files

### 🔵 ONGOING: Update TODO List

- [ ] Mark completed tasks with ✅
- [ ] Update progress percentages
- [ ] Add new tasks as discovered
- [ ] Remove obsolete tasks

---

## Timeline Summary

| Week | Phase | Key Deliverables |
|------|-------|------------------|
| **Week 1** | Phase 0 + P9 | Helper refactoring, Schema validation complete |
| **Week 2** | Phase 2-3 | SlotRWState API, DataBlock factories |
| **Week 3** | Phase 4-5 | MessageHub, broker, recovery API |
| **Week 4** | Phase 6 | All tests passing, TSan clean |
| **Week 5** | Phase 7 | Python bindings, docs, deployment ready |

---

## Blockers and Risks

### Current Blockers

- None

### Potential Risks

1. **ARM ThreadSanitizer availability**: If ARM CI not available, may miss race conditions
   - Mitigation: Test on x86 TSan + manual ARM testing

2. **Broker service design**: Minimal broker may not scale to 100+ producers
   - Mitigation: Document broker limitations, plan for future refactor if needed

3. **P9 Schema evolution**: Strict hash matching may be too restrictive
   - Mitigation: Document migration procedures, add version compatibility in future

---

## Notes

- **Phase 0 is critical**: Refactoring helper functions will simplify all subsequent phases and improve overall code quality
- **P9 can be implemented in parallel**: Schema validation is mostly independent of core DataBlock implementation
- **Test early and often**: Multi-process IPC bugs are hard to debug; catch them with unit tests
- **Document as you go**: Update HEP document and IMPLEMENTATION_GUIDANCE.md with any design changes

---

**Revision History**:
- **v1.0** (2026-02-09): Initial TODO list created for 5-week implementation timeline
