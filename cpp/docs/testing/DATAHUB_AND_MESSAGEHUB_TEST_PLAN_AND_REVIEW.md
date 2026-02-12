# DataHub Protocol Tests & MessageHub Review

**Purpose:** (1) Plan the tests required for the DataBlock/DataHub implementation and protocol before moving to higher-level design. (2) Review `message_hub.cpp` for C++20 practice, abstraction, reuse, and correctness for DataHub integration.

**Cross-platform:** Test plan and review **integrate cross-platform by default.** All tests must be runnable and meaningful on every supported platform (Windows, Linux, macOS, FreeBSD). Platform-specific paths (e.g. platform shm_*, DataBlockMutex, is_process_alive) must have coverage and documented semantics; avoid “skip on platform X” unless justified and documented. See DATAHUB_TODO “Cross-Platform and Behavioral Consistency” and critical review §2.3.

---

## Part 0: Foundational APIs Used by DataBlock (lower-layer modules)

DataBlock and related code depend on these lower-layer modules. **Their correctness must be covered by tests before relying on DataBlock behavior.** Rationale: memory allocation, spinlock behavior, checksum calculation/validation, and initialization are foundational; failures there are hard to diagnose if only tested indirectly via DataBlock.

| Foundation | Used by DataBlock for | Current test coverage | Gap / action |
|------------|------------------------|------------------------|--------------|
| **Platform (plh_platform.hpp / platform.cpp)** | | | |
| get_pid(), get_native_thread_id() | Writer/reader lock ownership, heartbeat | Yes: test_layer0_platform/test_platform_core.cpp | — |
| monotonic_time_ns(), elapsed_time_ns() | Timeouts, clock skew protection | Yes: test_platform_core.cpp (incl. clock skew) | — |
| is_process_alive(pid) | Zombie lock reclaim, consumer health | Yes: test_platform_core.cpp (current, invalid, POSIX/Windows) | — |
| **shm_create, shm_attach, shm_close, shm_unlink** | DataBlock segment creation/attach, diagnostic handle | Yes: test_layer0_platform/test_platform_shm.cpp | Create, attach same process, read/write, close, unlink; portable names (POSIX /name, Windows name). |
| **Backoff (utils/backoff_strategy.hpp)** | Writer/reader acquisition spin loops | Yes: test_layer2_service/test_backoff_strategy.cpp | — |
| **Crypto (utils/crypto_utils)** | BLAKE2b for checksums, header layout hash, random secret | Yes: test_layer2_service/test_crypto_utils.cpp + workers | — |
| **SharedSpinLock (utils/shared_memory_spinlock)** | Header spinlock states, flexible zone locking | Yes: test_layer2_service/test_shared_memory_spinlock.cpp | try_lock_for, lock/unlock, timeout, recursion, guards; two-thread mutual exclusion with state in shm. Zombie reclaim covered by platform is_process_alive tests; multi-process spinlock can be added later. |
| **Lifecycle** | sodium_init (via crypto module) for checksums/schema hash | Yes: test_lifecycle*.cpp, crypto workers | — |
| **Schema BLDS / header layout** | SchemaInfo, validate_header_layout_hash | Yes: test_layer3_datahub/test_schema_blds.cpp | Extend for validate_header_layout_hash as needed in Phase A. |

**Recommended order:** (1) Add **platform `shm_*`** tests in Layer 0. (2) Add **SharedSpinLock** tests (Layer 2 or dedicated target using utils + shared memory). Then proceed with Part 1 (DataBlock protocol tests).

**Part 0 – Cross-platform:** Platform `shm_*` and SharedSpinLock tests must run on all supported platforms (Windows, Linux, macOS, FreeBSD). Document any platform-specific assertions (e.g. shm name format, timeout granularity). Do not add platform-only skips without a documented reason.

---

## Part 1: Test Plan for DataBlock/DataHub “This Part of the Code”

### Scope of “this part”

- **DataBlock core:** Shared memory layout, header (SharedMemoryHeader), slot coordination (SlotRWState), writer/reader acquisition and commit/release, flexible zone layout and access (undefined until definition/agreement).
- **Producer/Consumer API:** Create/find (with/without MessageHub), `flexible_zone_span` (producer, consumer, slot handles), checksum update/verify (flexible zone + slot), acquire/release write/consume slots.
- **Protocol and agreement:** Producer config (flexible_zone_configs, ring capacity, unit_block_size, checksum), consumer expected_config and schema validation, shared secret discovery.
- **Integrity and diagnostics:** Diagnostic handle (open_datablock_for_diagnostic), checksum verify/repair (current full path; lighter path is designed but not yet implemented).
- **MessageHub:** Connect/disconnect, send_message/receive_message, register_producer, discover_producer, register_consumer (stub). Used by DataBlock create/find when broker is used.

Tests should be layered so that protocol and correctness are assured before adding more integration.

---

### 1.1 Unit / API tests (single process, no broker, no real shm where avoidable)

| Area | What to test | Priority | Notes |
|------|--------------|----------|--------|
| **Flexible zone semantics** | `flexible_zone_span` returns empty when no zones defined (producer/consumer/slot handle); non-empty when zones defined and index valid. | P1 | Critical for “undefined ⇒ no access”. |
| **Flexible zone checksum** | `update_checksum_flexible_zone` / `verify_checksum_flexible_zone` return false when zone index out of range or no zones; true when zone valid and checksum matches. | P1 | Same semantics as design. |
| **Schema (BLDS)** | Already in `test_schema_blds.cpp`. Extend as needed for SchemaVersion, hash, validate_header_layout_hash. | P1 | Already planned in CODEBASE_REVIEW. |
| **DataBlockConfig** | `total_flexible_zone_size()`, `flexible_zone_configs` ordering; config used to build FlexibleZoneInfo (offset/size) on producer and consumer. | P2 | Ensures agreement basis. |
| **Slot handle span APIs** | SlotWriteHandle/SlotConsumeHandle `buffer_span()`, `flexible_zone_span(index)` with 0 and 1 zone; empty when index >= size. | P1 | Direct coverage of recent impl. |
| **Producer/Consumer flexible_zone_span** | DataBlockProducer::flexible_zone_span, DataBlockConsumer::flexible_zone_span (same rules as slot handles). | P1 | Just implemented. |
| **MessageHub (no broker)** | connect with invalid endpoint/key (fail); disconnect idempotent; send_message/receive_message when not connected return nullopt; parse errors in register_* / discover_* (e.g. invalid JSON, missing keys). | P2 | Requires either mock broker or “no broker” code paths. |

**Dependencies:** GTest, test_framework, pylabhub::utils. For tests that need a real DataBlock, use in-process producer + consumer (same process, no MessageHub) or with a dummy/in-memory broker if we add one.

---

### 1.2 Integration tests (real shared memory, optional broker)

| Area | What to test | Priority | Notes |
|------|--------------|----------|--------|
| **Producer create (no broker)** | Not currently in public API; create via factory that doesn’t require MessageHub, or test via “with broker” path. | P2 | If we add create_datablock_producer(name, config) without hub, test it. |
| **Consumer find with expected_config** | Attach to existing block (producer in same process or pre-created); expected_config matches header (flexible_zone_size, ring capacity, etc.); consumer gets non-empty flexible_zone_span when zones defined. | P1 | Validates agreement and access. |
| **Consumer find with schema** | Producer stores schema hash/version; consumer with same schema succeeds; consumer with different schema hash fails (schema_mismatch_count, returns nullptr). | P1 | P9.2 schema validation. |
| **Consumer without expected_config** | find_datablock_consumer(..., no config); flexible_zones_info empty; flexible_zone_span returns empty. | P1 | “No definition ⇒ no access”. |
| **Slot write/commit and read** | Acquire write slot, write buffer, commit; acquire consume slot, read buffer; same data. | P1 | Core protocol. |
| **Checksum (flexible zone + slot)** | Enable checksum in config; update on producer (slot handle or producer API); verify on consumer; after tampering (if possible in test), verify fails. | P1 | Integrity mechanism. |
| **Header layout hash** | Producer and consumer with same header ABI; validate_header_layout_hash passes; if possible, simulate different ABI and expect SchemaValidationException. | P2 | Protocol/ABI check. |
| **Diagnostic handle** | open_datablock_for_diagnostic(name) returns valid handle; header() and slot_rw_state(i) usable; no producer/consumer lifecycle. | P2 | Lighter integrity path prep. |
| **MessageHub with real broker** | Start broker (or use existing test broker); connect, register_producer, discover_producer, then create producer and find consumer via hub; one write/read. | P2 | Full integration; depends on broker availability. |

---

### 1.3 Concurrency and multi-process tests (existing plan, high value)

| Area | What to test | Priority | Notes |
|------|--------------|----------|--------|
| **Writer acquisition** | Timeout when readers hold slot; backoff; eventual success when readers drain. | P1 | From CODEBASE_REVIEW. |
| **Reader TOCTTOU** | Slot state change between first check and reader_count increment; reader gets NOT_READY and retries. | P1 | Critical for correctness. |
| **Reader wrap-around** | validate_read fails when slot reused (generation mismatch). | P1 | From CODEBASE_REVIEW. |
| **Concurrent readers** | Multiple consumers read same slot; all see same data; reader_count and release correct. | P1 | From CODEBASE_REVIEW. |
| **Zombie writer reclaim** | Writer process dies while holding write_lock; new writer detects zombie PID and reclaims. | P2 | Platform (is_process_alive). |
| **DataBlockMutex** | Cross-process exclusion (existing or planned test in datablock_management_mutex). | P2 | From CODEBASE_REVIEW. |

---

### 1.4 Test implementation order (recommended)

1. **Phase A – Protocol and API correctness (no broker)**  
   - Flexible zone: empty span when no zones; valid span when zones defined (producer, consumer, slot handles).  
   - Checksum: false when zone undefined; true when valid and matching.  
   - Config and agreement: consumer with expected_config gets zones; consumer without does not.  
   - Schema: producer stores hash/version; consumer validates; mismatch fails.

2. **Phase B – Slot protocol in one process**  
   - Single process: create producer (e.g. via a test-only factory or with a dummy broker), create consumer (attach by name + secret + config).  
   - Acquire write slot, write + commit; acquire consume slot, read; checksum update/verify.  
   - Optional: diagnostic handle open and header/slot_rw_state access.

3. **Phase C – MessageHub and broker**  
   - MessageHub unit behavior (connect/disconnect, send/receive when disconnected, parse errors).  
   - With broker: register_producer, discover_producer, then full create/find and one write/read.

4. **Phase D – Concurrency and multi-process**  
   - Concurrent readers; writer timeout; TOCTTOU and wrap-around; zombie reclaim; DataBlockMutex.

**Part 1 – Cross-platform:** Run Phase A–D tests on every supported platform. Integration and multi-process tests (B–D) must cover Windows and at least one POSIX (Linux or macOS). Document platform-specific behavior (e.g. DataBlockMutex abandoned-owner, shm naming) in test or design docs; avoid silent skips.

---

### 1.5 Test infrastructure needs

- **DataBlockTestFixture (or equivalent):**  
  - Create producer with given DataBlockConfig (and optional schema) in test process.  
  - Create consumer with same name, shared secret, expected_config (and optional schema).  
  - Optionally: use MessageHub + broker when available.  
  - Cleanup: ensure shm is unlinked / handles released (e.g. producer destroyed first so consumer doesn’t hold last reference in a way that complicates cleanup).

- **Broker for tests:**  
  - In-process mock or a small broker executable that implements REG_REQ / DISC_REQ and returns JSON with shm_name, schema_hash, schema_version so that register_producer / discover_producer can be tested end-to-end.

- **Schema tests:**  
  - test_schema_validation.cpp is present but commented out in CMake; enable when DataBlock + MessageHub are stable and add tests for validate_header_layout_hash and consumer schema mismatch.

---

## Part 2: MessageHub Code Review

**File:** `cpp/src/utils/message_hub.cpp` (and `message_hub.hpp`)

### 2.1 API / header vs implementation mismatch

- **Header** declares:
  - `send_message(const std::string &channel, const std::string &message, int timeout_ms = 5000)`
- **Implementation** defines:
  - `send_message(const std::string &message_type, const std::string &json_payload, int timeout_ms)`
- **Usage:** `register_producer` and `discover_producer` call `send_message("REG_REQ", request_payload.dump())` and `send_message("DISC_REQ", request_payload.dump())`, i.e. first argument is **message type**, second is **JSON payload**. The parameter name `channel` in the header is misleading; the channel is sent *inside* the JSON (`channel_name`), not as the first frame.
- **Recommendation:** Align header with implementation: e.g. `send_message(const std::string &message_type, const std::string &json_payload, int timeout_ms = 5000)`. If the public API is intended to be “channel + message”, then the implementation should be refactored so that the first frame is channel and the second is message (and internally build message_type from channel or vice versa). As it stands, the header does not match the implementation.

### 2.2 C++20 and modern C++

- **Good:** Uses `std::optional` for optional returns; pImpl; `std::lock_guard`; `std::chrono::milliseconds` for timeout; `std::atomic<bool>` with explicit memory ordering (`memory_order_acquire` / `memory_order_release`) where relevant.
- **Improvements:**  
  - Use `std::scoped_lock` if more than one mutex is ever used (C++17, good practice).  
  - Consider `[[nodiscard]]` on `send_message`, `receive_message`, `discover_producer`, `connect`, `register_producer` so callers don’t ignore results.  
  - JSON parse in `register_producer` / `discover_producer`: `nlohmann::json::parse(response_str.value())` can throw; consider `parse` in a try/catch or use a non-throwing API and return false / nullopt on parse failure instead of letting exceptions propagate.  
  - `response_json["message"].get<std::string>()` can throw if `"message"` is missing or not a string; same for `shm_name`, `schema_hash`, `schema_version` in `discover_producer`. Defensive checks or structured parsing would make the code safer and more C++20-friendly (e.g. use `.contains()` and `.value()` with defaults where appropriate).

### 2.3 Abstraction and reuse

- **Good:** MessageHub abstracts ZMQ context/socket and CurveZMQ keys; lifecycle (GetLifecycleModule, sodium_init) is centralized; protocol (two-frame: type + payload) is consistent.
- **Gaps:**  
  - **Duplicate receive logic:** `send_message` (wait for response) and `receive_message` both: poll, recv_multipart, check size >= 2, take last frame as string. This could be a private helper (e.g. `recv_response_frame(int timeout_ms)`) to avoid duplication and to centralize “expected at least 2 frames” and error logging.  
  - **JSON building/parsing:** Register and discover build JSON by hand and parse with ad-hoc `.get<>()`. Consider small helper structs or free functions (e.g. `registry_request_to_json`, `registry_response_from_json`) so that protocol shape lives in one place and is easier to test and change.  
  - **Z85 validation:** `is_valid_z85_key` is a simple length check; no character set validation. Acceptable for now but document or tighten if broker assumes strict Z85.

### 2.4 Logic and correctness

- **connect:** Recreates socket if `!pImpl->m_socket` (e.g. after close). Correct.  
- **disconnect:** Closes socket and sets `m_is_connected = false`; catch on close is good.  
- **send_message:** Requires connected; sends two frames (message_type, json_payload); waits for response; returns last frame only. Logic is consistent with “request/response” usage.  
- **receive_message:** Passive receive; returns nullopt when not connected or timeout; same 2-frame expectation as send path.  
- **register_producer:** Builds REG_REQ payload with channel and ProducerInfo; parses response; checks `status == "success"`. Missing: timeout for `send_message` is not passed (uses default 5000 ms) — acceptable.  
- **discover_producer:** Builds DISC_REQ with channel; returns ConsumerInfo (struct is “info the consumer needs about the producer” — naming is a bit confusing but usage is correct). No timeout override.  
- **register_consumer:** Stub; always returns true. Document that it’s not yet implemented so DataHub doesn’t rely on broker-side consumer registration until the protocol is defined.  
- **get_instance:** Singleton; destructor of static object can run at process exit. If other static destructors use MessageHub, order could be an issue; for normal use (explicit connect/send/disconnect during app life) this is fine.

### 2.5 DataHub integration readiness

- **Needed for DataBlock create/find:**  
  - `register_producer` and `discover_producer` are used; they need a running broker that speaks the same REG_REQ/DISC_REQ and returns JSON with `shm_name`, `schema_hash`, `schema_version`.  
  - Consumer discovery currently returns one `ConsumerInfo`; if multiple producers can be on the same channel, protocol may need to clarify (e.g. “one producer per channel” or “return list”).  
- **Recommendations for “next level” design:**  
  - Fix header/implementation parameter naming (message_type vs channel) and consider non-throwing JSON parsing and field access.  
  - Extract receive path into a shared helper; add `[[nodiscard]]` and optional timeout parameter for register_* / discover_* if needed.  
  - Document the exact broker contract (message types, JSON keys, success/error shape) in one place (e.g. HEP or a short protocol doc) so tests and broker impl stay in sync.  
  - Once broker protocol for consumer registration is defined, implement `register_consumer` and add tests that use it.

---

## Summary

- **Tests:** Prioritize (1) flexible zone and checksum semantics (no access when undefined), (2) producer/consumer agreement and schema validation, (3) slot write/commit and read with checksums, (4) MessageHub behavior with or without broker, (5) concurrency and multi-process. Implement in phases: API correctness → single-process slot protocol → MessageHub/broker → concurrency/multi-process.
- **MessageHub:** Align header with implementation (send_message parameters); harden JSON parsing and field access; reduce duplication in receive path; document broker contract and register_consumer stub; small C++20/nodiscard and error-handling improvements will make it more robust for DataHub integration.
