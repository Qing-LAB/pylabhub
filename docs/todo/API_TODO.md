# API TODO

**Purpose:** Track public API refinements, documentation improvements, and API surface enhancements for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`  
**API Reference:** `src/include/utils/data_block.hpp`
**Examples:** `examples/`

---

## Current Focus

### HEP-0024: Role Directory Service (NEW — 2026-03-12)

**Design**: `docs/HEP/HEP-CORE-0024-Role-Directory-Service.md`
**Motivation**: Formalises the implicit role directory layout convention as a public API;
enables custom role binary development with correct security defaults; eliminates
triplication of path/hub-resolution logic across the three role config classes.

**Phase 1** — `RoleDirectory` class ✅ DONE 2026-03-12
- [x] `src/include/utils/role_directory.hpp` — public header
- [x] `src/utils/config/role_directory.cpp` — implementation; `create()` sets `vault/` to 0700 on POSIX
- [x] `src/utils/CMakeLists.txt` — added `config/role_directory.cpp`
- [x] 18 L2 tests: `tests/test_layer2_service/test_role_directory.cpp`

**Phase 2** — `role_cli.hpp` (header-only, public) ✅ DONE 2026-03-12
- [x] `src/include/utils/role_cli.hpp` — `RoleArgs`, `parse_role_args()`, `resolve_init_name()`, `is_stdin_tty()`, `read_password_interactive()`, `get_role_password()`, `get_new_role_password()`
- [x] `role_main_helpers.hpp` updated to delegate to `role_cli.hpp`; 8 L2 tests added
- Total: 26 new L2 tests (18 + 8)

**Phases 3+4** — Migrate `Config::from_directory()` + hub-reference resolution ✅ DONE 2026-03-12
- [x] `producer_config.cpp`, `consumer_config.cpp`, `processor_config.cpp` — use `RoleDirectory::open()`, `config_file()`, `resolve_hub_dir()`, `hub_broker_endpoint()`, `hub_broker_pubkey()`
- [x] `load_broker_from_hub_dir()` in `processor_config.cpp` refactored to use `RoleDirectory`
- [x] `warn_if_keyfile_in_role_dir()` called from all three `from_directory()` — security warning when vault is inside role dir ✅ DONE 2026-03-12 (+4 L2 tests; 22 total)

**Phase 5** — Migrate script-path resolution in script hosts ⚪ DEFERRED
- [ ] `producer_script_host.cpp`, `consumer_script_host.cpp`, `processor_script_host.cpp` — use `role_dir_.script_entry(config_.script_path, config_.script_type)` (requires storing `RoleDirectory` in script host)

**Phase 6** — Migrate `do_init()` and `main()` arg parsing in all 3 binaries ✅ DONE 2026-03-12
- [x] `producer_main.cpp`, `consumer_main.cpp`, `processor_main.cpp` — use `RoleDirectory::create()`, `role_cli::parse_role_args()`, `role_cli::resolve_init_name()`, `role_cli::get_new_role_password()`

**Phase 7** — Docs ⚪ DEFERRED
- [ ] `docs/README/README_EmbeddedAPI.md` — add section on using `RoleDirectory` + `role_cli.hpp` in custom role binaries
- [ ] `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` — reference `role_cli.hpp` as standard CLI builder

**Phase 8** — Tests ⚪ DEFERRED
- [ ] L4 CLI tests: extend `test_producer_cli.cpp`, `test_consumer_cli.cpp`, `test_processor_cli.cpp` to verify `default_keyfile` path + `has_standard_layout()` after `--init`

**Open Design Question (2026-03-12)** — ✅ RESOLVED
- `api.role_dir()`, `api.logs_dir()`, `api.run_dir()` implemented in all 3 APIs (ProducerAPI, ConsumerAPI, ProcessorAPI) with pybind11 bindings.
  Vault/security operations are NOT exposed to scripts (as planned).

---

### Design Gap Fixes (2026-03-09, ✅ CLOSED 2026-03-10)

**Formal review**: `docs/code_review/REVIEW_DesignAndCode_2026-03-09.md` — all 6 items triaged.

- [x] **P1**: Formal REVIEW_DesignAndCode_2026-03-09.md created ✅ 2026-03-10
- [x] **P1**: HEP-0022 peer UID validation — already implemented (`broker_service.cpp:2137–2157`, handle_hub_peer_hello rejects unknown hub_uid) ✅ PRE-FIXED
- [x] **P1**: hub callback thread-safety — already implemented (callbacks_mu in hub_producer.cpp:71, copy-under-lock pattern) ✅ PRE-FIXED
- [x] **P1**: METRICS_REQ SHM merge gap — implemented 2026-03-10 (`broker_service.cpp` handle_metrics_req now calls `query_shm_blocks()` and appends `shm_blocks` key) ✅ FIXED
- [x] **P2**: Script config type defaults silently to Python — ⚪ DEFERRED (Python is only type; revisit when second type added)
- [x] **P2**: Consumer registry dedup under reconnect — fixed 2026-03-09 (zmq_identity dedup key in `channel_registry.cpp`) ✅ FIXED
- [x] **P2**: Messenger API redundancy — ⚪ DEFERRED (legacy surface, not harmful)

### Consumer Inbox + ShmQueue Tests (DONE 2026-03-10)

- [x] Consumer/Processor `inbox_thread_` (ROUTER socket — receiving side): ConsumerConfig + ConsumerScriptHost fields; mirrors ProducerScriptHost implementation; `on_inbox()` callback ✅ 2026-03-10
- [x] 9 new L3 ShmQueue test scenarios (`tests/test_layer3_datahub/test_datahub_hub_queue.cpp`): multiple consumers, flexzone round-trip, ref factories, latest_only policy, ring wrap, destructor safety, last_seq monotonic, capacity/policy_info, verify_checksum mismatch ✅ 2026-03-10 — 988/988

### Recently Completed — Producer Transport Overhaul (✅ 2026-03-08/09)

All phases done. Highlights:
- [x] Transport enum in ProducerConfig; unified run_loop_(); InboxQueue (ROUTER) + InboxClient (DEALER); ROLE_PRESENCE/INFO protocol; transport arbitration (TRANSPORT_MISMATCH); ctrl_thread_ rename; schema_spec_to_zmq_fields shared; write_discard rename; InboxHandle + open_inbox/wait_for_role in all 3 APIs
- [x] QueueReader/QueueWriter split; unified ConsumerScriptHost::run_loop_(); ConsumerAPI spinlock/last_seq/verify_checksum/in_capacity/in_policy/overrun_count
- [x] Code review REVIEW_DataHubInbox_2026-03-09.md: 13 items fixed + closed + archived (2026-03-09)
- [x] §7.4 deferred: ZmqQueue PUSH→DEALER ACK+retry (requires socket type change)
- [x] **HR-02** — CLOSED: `ConsumerAPI::reader_` is already `std::atomic<const hub::QueueReader*>` (`consumer_api.hpp:163`). Confirmed 2026-03-10 by REVIEW_DeepStack_2026-03-10.md audit.
- [x] **HR-03** — ✅ FIXED (pre-existing): `last_rcvtimeo` and `last_acktimeo` caching in `InboxQueueImpl`; `zmq_setsockopt(ZMQ_RCVTIMEO)` only called when value changes. Comment `// HR-03` confirmed at `hub_inbox_queue.cpp:445`. ✅ PRE-FIXED (confirmed 2026-03-10)
- [x] **HR-05** — ✅ FIXED (pre-existing): `py::gil_scoped_release release` at `producer_api.cpp:322` wraps `query_role_info()` call in `open_inbox()`. ✅ PRE-FIXED (confirmed 2026-03-10)

**HIGH (from REVIEW_FullStack_2026-03-10.md):**
- [x] **FS-01** — FALSE POSITIVE: all `on_consumer_*` callbacks only call `enqueue_message()` (C++ mutex-protected queue); Python is called from `loop_thread_` via `drain_messages()` only. No GIL fix needed. (Confirmed 2026-03-10)
- [x] **FS-02** — FIXED 2026-03-10: ConsumerConfig `zmq_packing` now throws on invalid value (was warn+default); `inbox_buffer_depth>0` check added; `inbox_schema_json` type check (string|object) added in both ProducerConfig and ConsumerConfig; 8 new tests → 996/996

**MEDIUM:**
- [x] **MR-02** — ✅ FIXED 2026-03-10: `InboxQueueImpl` now uses `unordered_map<string, uint64_t> expected_seq_per_sender_` keyed by sender_id. Same fix as A11/A18 in REVIEW_FullStack2_2026-03-10.md.
- [x] **MR-05** — FALSE POSITIVE: `ConsumerAPI::in_capacity()` already wraps `r->capacity()` in try/catch (consumer_api.cpp:268-269). No fix needed. (Confirmed 2026-03-10)
- [x] **MR-09** — ✅ FIXED 2026-03-12: `ShmQueue::is_running()` override added; returns false when `pImpl==nullptr` (moved-from). Test `DatahubShmQueueTest.ShmQueueIsRunning` added.
- [x] **MR-10** — FALSE POSITIVE: `send_stop_` guard already present at `hub_zmq_queue.cpp:847-848` (comment "MR-10"). No fix needed. (Confirmed 2026-03-10)
- [x] **IC-04** — ✅ FIXED 2026-03-12: `QueueReader::last_seq()` docstring in `hub_queue.hpp` clarified: ShmQueue returns ring slot index (0-indexed); ZmqQueue returns monotone wire seq. Cross-references confirmed in `hub_shm_queue.hpp` and `hub_zmq_queue.hpp`.
- [x] **MR-01** — ✅ FIXED 2026-03-12: Wire-format helpers extracted to `src/utils/hub/zmq_wire_helpers.hpp` (internal, `wire_detail` namespace). Both `hub_zmq_queue.cpp` and `hub_inbox_queue.cpp` now use shared `kFrameMagic`, `WireFieldDesc`, `compute_field_layout`, `max_frame_size`, `pack_field`, `unpack_field`; ~260 lines of duplication removed.

**LOW:**
- [x] **LR-04** — CLOSED: `ProducerAPI::stop()` already uses `memory_order_release` (`producer_api.cpp:46`). Fix was applied in REVIEW_DataHubInbox_2026-03-09.md (LR-04 ✅). Confirmed 2026-03-10 by REVIEW_DeepStack audit.
- [x] **LR-05** — ✅ FIXED (pre-existing): `parse_on_produce_return()` returns `is_err=true` on wrong type; caller at `producer_script_host.cpp:616` calls `api_.increment_script_errors()` when `is_err`. Confirmed by code read 2026-03-10.
- [x] **MR-08** — ✅ FIXED 2026-03-12: stale `run_loop_shm_` reference removed from `consumer_script_host.hpp` file header comment.
- [ ] **LOW-2 (gemini)** — `DataBlockProducer`/`DataBlockConsumer` lack a `flexzone_size()` accessor. `DataBlockConfig::flexible_zone_size` exists but is not exposed via the C++ API objects directly. Low priority — `get_metrics()` provides indirect access via `DataBlockMetrics`. Add `size_t flexzone_size() const noexcept` to both classes when a diagnostics round is done.

### Code & Docs Review (2026-03-14) — `REVIEW_CodeAndDocs_2026-03-14.md` ✅ CLOSED

All items resolved 2026-03-14; 1166/1166 tests pass.

- [x] **CX-01** — ✅ FIXED: `get_binary_dir()` now delegates to `platform::get_executable_name(true)`.
- [x] **CX-02** — ✅ FIXED: POSIX mutex `try_lock_for()` now uses `CLOCK_MONOTONIC` retry loop with 500 ms `CLOCK_REALTIME` chunks — immune to NTP clock jumps.
- [x] **CX-03** — ✅ ACCEPTED: Guard destructors throw by design.
- [x] **CX-04** — ✅ FIXED: `timeout_ms` → `slot_acquire_timeout_ms` in README_Deployment.md.
- [x] **CX-05** — ✅ FIXED: test count updated to 1160+.
- [x] **CX-06** — ✅ ACCEPTED: different enum types (`processor::OverflowPolicy` vs `hub::OverflowPolicy`).
- [x] **CX-07** — ✅ ACCEPTED: startup-wait duplication acceptable.
- [x] **CX-08** — ✅ ACCEPTED: inbox-thread shutdown duplication acceptable.
- [x] **CX-09** — ✅ FIXED: removed dead `PyExecResult::result_repr` field.
- [x] **CX-10** — ✅ FIXED: removed unused `config_filename` parameter from `RoleDirectory::create()`.
- [x] **CX-11** — ✅ FIXED: README_DirectoryLayout.md updated to match actual staging layout.
- [x] **RC-01** — ✅ FIXED: aligned `python_venv` docstrings across all 3 configs.

### Code Review + Bug Fixes (2026-03-08)

Applied 8 bug fixes across InboxQueue/ZmqQueue/ProducerConfig after static review:
- IQ-1: Removed infinite-timeout reset before ACK frame-1 recv (recv timeout stays active across multi-part ACK)
- IQ-2: `inbox_buffer_depth` now correctly sets `ZMQ_RCVHWM` on ROUTER socket (was unused dead config)
- IQ-3: `zmq_packing` field added to `ProducerConfig`; `push_to()` passes it instead of hardcoded `"aligned"`
- IQ-4: Header doc corrected: DEALER receives ACK `["", ack_byte]` not `[ack_byte]`
- IQ-5: `zmq_recv()` truncation comment corrected (returns actual size, not bytes copied)
- IQ-6: `kInboxMagic` cross-reference comment added
- IQ-7: `frame_recv_buf_` moved from per-call stack alloc to persistent `InboxQueueImpl` member
- IQ-8: `.cpp` file header comment fixed (DEALER receives `["", ack_byte]`, not just `[ack_byte]`)
- "natural" packing → renamed to "aligned" everywhere (100+ occurrences across src/tests/share/docs)
- 3 new `ZmqPacking_*` tests → **966/966** passing

### API Documentation Gaps

- [x] **Consumer registration to broker** – ✅ Fully implemented 2026-02-18; CONSUMER_REG/DEREG handshake
- [ ] **stuck_duration_ms in diagnostics** – `SlotDiagnostic::stuck_duration_ms` requires timestamp on acquire
- [ ] **DataBlockMutex documentation** – Factory vs direct constructor, exception vs optional/expected
- [ ] **Flexible zone initialization** – Document when flexible_zone_info is populated

### API Consistency
**Status**: 🟢 Ready

- [x] **release_write_slot** – Documented return values and idempotent behavior
- [x] **Slot handle lifetime** – Contract documented in data_block.hpp
- [x] **Recovery error codes** – All codes documented in recovery_api.hpp
- [ ] **Error code consistency** – Review all APIs for consistent error reporting

### Code Review Open Items (2026-02-21)

- [x] **ChannelPattern duplicate string conversion** ✅ FIXED 2026-02-22 — Shared
  `channel_pattern_to_str()` / `channel_pattern_from_str()` moved to `channel_pattern.hpp`.
  Removed duplicate `pattern_to_wire/from_wire` in `messenger.cpp` and
  `pattern_to_str/from_str` in `broker_service.cpp`.
  - Source: code_review_utils_2025-02-21.md item 8

- [ ] **Logger header two comment blocks** — Logger public header has two separate comment blocks covering overlapping topics. Merge into one coherent doc block. Low priority.
  - Source: REVIEW_cpp_src_hep_2026-02-20.md item 3

---

## Public API Surface

### Core DataBlock API
```cpp
// Factory functions
std::unique_ptr<DataBlockProducer> create_datablock_producer(...)
std::unique_ptr<DataBlockConsumer> find_datablock_consumer(...)

// Producer API
class DataBlockProducer {
    std::unique_ptr<SlotWriteHandle> acquire_write_slot(int timeout_ms);
    bool release_write_slot(SlotWriteHandle& handle);
    void update_heartbeat();
    SharedSpinLock get_spinlock(size_t index);  // For flexible zones
    size_t spinlock_count() const noexcept;
    // ... metrics, diagnostics
};

// Consumer API
class DataBlockConsumer {
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(int timeout_ms);
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(uint64_t slot_id, int timeout_ms);
    bool release_consume_slot(SlotConsumeHandle& handle);
    DataBlockSlotIterator slot_iterator();
    // ... metrics, diagnostics
};

// Handles
class SlotWriteHandle;   // RAII, destroyed before producer
class SlotConsumeHandle; // RAII, destroyed before consumer
```

### Recovery and Diagnostics API
```cpp
// Diagnostics
DataBlockDiagnosticHandle open_datablock_for_diagnostic(const std::string& name);
class SlotDiagnostics;
class IntegrityValidator;

// Recovery
class SlotRecovery;
class HeartbeatManager;
bool datablock_is_process_alive(uint64_t pid);
RecoveryResult datablock_validate_integrity(...);
```

---

## Recent Completions

### LoopTimingPolicy rename (2026-03-10) ✅
Renamed `FixedPace`→`FixedRate`, `Compensating`→`FixedRateWithCompensation`; added `MaxRate` (explicit, enforces period=0).
Shared header `src/include/utils/loop_timing_policy.hpp` (pylabhub namespace) with cross-field validation in `parse_loop_timing_policy()`.
Updated: producer_config, consumer_config, producer_script_host, consumer_script_host, data_block_policy.hpp.
Added 10 new loop timing tests (6 producer, 4 consumer) → 1021/1021.
Docs updated: HEP-0008, 0009, 0015, 0017, 0018.

### hub::Queue + hub::Processor Layer 3 API (2026-03-01) ✅

**Files**:
- `src/include/utils/hub_queue.hpp` — abstract `Queue` base class, `OverflowPolicy`
- `src/include/utils/hub_shm_queue.hpp` + `src/utils/hub/hub_shm_queue.cpp` — `ShmQueue` wrapping `DataBlockConsumer`/`DataBlockProducer` directly
- `src/include/utils/hub_zmq_queue.hpp` + `src/utils/hub/hub_zmq_queue.cpp` — `ZmqQueue` (PULL/PUSH); `recv_thread_` for read mode; fire-and-forget send
- `src/include/utils/hub_processor.hpp` + `src/utils/hub/hub_processor.cpp` — `Processor` with demand-driven `process_thread_`; typed `ProcessorContext<InF,InD,OutF,OutD>`; hot-swap `ProcessorHandlerFn` via `PortableAtomicSharedPtr`; `in_received`/`out_written`/`out_drops` counters
- `src/include/plh_datahub_client.hpp` — updated with 4 new includes (`hub_queue`, `hub_shm_queue`, `hub_zmq_queue`, `hub_processor`)
- `tests/test_layer3_datahub/test_datahub_hub_queue.cpp` + workers — 9 `DatahubShmQueueTest` tests
- `tests/test_layer3_datahub/test_datahub_hub_processor.cpp` + workers — 9 `DatahubHubProcessorTest` tests

**Test count**: 595 → 613 (+18)

**Key design decisions**:
- No protocol in Queue (no ZMQ peer channels, no HELLO/BYE)
- `ShmQueue` wraps DataBlock primitives directly — no `hub::Consumer`/`hub::Producer` involved
- Templates only in `set_process_handler<InF,InD,OutF,OutD>()` — Queue API is all `void*`
- `ConsumerSyncPolicy::Latest_only` requires write-one-read-one interleaving for sequential ordering

---

## Current Focus — Producer and Consumer Binaries

**Architectural decision (2026-03-01):** `pylabhub-actor` eliminated. Standalone
`pylabhub-producer` and `pylabhub-consumer` replace it. Design: HEP-CORE-0018.

### Step 1: Remove actor from build ✅ 2026-03-02

- [x] Remove `pylabhub-actor` from `src/CMakeLists.txt` → replaced with `producer` + `consumer` subdirs
- [x] Remove `test_layer4_actor` from `tests/CMakeLists.txt` (actor tests removed from build)
- [x] `src/actor/` deleted from disk (2026-03-02)
- [x] `tests/test_layer4_actor/` deleted from disk (2026-03-02)
- [x] `share/demo/` replaced: new flat `producer.json` + `consumer.json` with `script/python/__init__.py`
- [x] `share/scripts/python/examples/` — all actor configs/scripts replaced with flat producer/consumer format

### Step 2: Implement `pylabhub-producer` (`src/producer/`) ✅ 2026-03-01

- [x] `uid_utils` — add `generate_producer_uid()` + `has_producer_prefix()`
- [x] `ProducerConfig` — `from_json_file()` + `from_directory()` (mirrors `ProcessorConfig`)
- [x] `ProducerAPI` + `PYBIND11_EMBEDDED_MODULE(pylabhub_producer, m)`
- [x] `ProducerScriptHost : PythonScriptHost` — `zmq_thread_` + `loop_thread_`; GIL pattern
- [x] `producer_main.cpp` — CLI: `--init` / `<dir>` / `--validate` / `--keygen`
- [x] `CMakeLists.txt` — builds `pylabhub-producer`; links `pylabhub::utils` + `pylabhub::scripting`

### Step 3: Implement `pylabhub-consumer` (`src/consumer/`) ✅ 2026-03-01

- [x] `uid_utils` — add `generate_consumer_uid()` + `has_consumer_prefix()`
- [x] `ConsumerConfig` — mirrors `ProducerConfig` (read side, no slot_count, no update_checksum)
- [x] `ConsumerAPI` + `PYBIND11_EMBEDDED_MODULE(pylabhub_consumer, m)` (read-only — no broadcast/spinlock)
- [x] `ConsumerScriptHost : PythonScriptHost` — demand-driven loop, `zmq_thread_`, GIL pattern
- [x] `consumer_main.cpp` — CLI mirrors producer CLI
- [x] `CMakeLists.txt` — builds `pylabhub-consumer`
- [x] Build + 524/524 tests pass (630 − 106 actor tests = 524)

### Step 4: Demos and examples ✅ 2026-03-02

- [x] `share/demo/producer/` — new flat `producer.json` + `script/python/__init__.py` (PROD-COUNTER)
- [x] `share/demo/consumer/` — new flat `consumer.json` + `script/python/__init__.py` (CONS-LOGGER)
- [x] Updated `share/demo/run_demo.sh` — uses `pylabhub-producer` + `pylabhub-consumer` (was `pylabhub-actor`)
- [x] Replaced `share/scripts/python/examples/` — all actor configs/scripts → flat producer/consumer format
- [x] Deleted `src/actor/` + `tests/test_layer4_actor/` (actor residue fully removed from disk)
- [x] Fixed script path bugs: `script_path{"./script"}` → `{"."}` in ProducerConfig + ConsumerConfig
- [x] Fixed `processor_main.cpp --init`: creates `script/python/` (was flat `script/`), adds `vault/`
- [x] Renamed `stop_on_python_error` → `stop_on_script_error` throughout all three binaries + JSON files
- [x] MEMORY.md corrected: `"path": "."` is the correct default (not `"./script"`)

### Step 5: Tests ✅ 2026-03-02

- [x] Layer 4 producer tests: `tests/test_layer4_producer/` — 8 config + 6 CLI tests
- [x] Layer 4 consumer tests: `tests/test_layer4_consumer/` — 6 config + 6 CLI tests
- [x] Integration test: full pipeline round-trip (hubshell + producer + processor + consumer) — `test_pipeline_roundtrip.cpp`

## Backlog

### C++ Pipeline Demo — ✅ Processor Template Done (2026-03-03)

**Status**: Core processor pipeline template complete; additional templates deferred.

**Completed:**
- [x] `examples/cpp_processor_template.cpp` — Self-contained processor pipeline:
  Producer (SHM) → Processor (SHM-to-SHM) → Consumer (SHM) with typed handler.
  Shows: `PYLABHUB_SCHEMA_BEGIN/MEMBER/END`, `LifecycleGuard`, `BrokerService`,
  `hub::Producer::create<F,D>()`, `hub::Consumer::connect<F,D>()`, `ShmQueue::from_*_ref()`,
  `Processor::create()` + `set_process_handler<>()`, `synced_write` + `pull` with
  `WriteProcessorContext`/`ReadProcessorContext`, clean ordered shutdown.
- [x] `examples/CMakeLists.txt` — builds 7 executables (6 pre-existing + processor pipeline)
- [x] `cmake/ToplevelOptions.cmake` — `PYLABHUB_BUILD_EXAMPLES` option (default OFF)
- [x] Top-level `CMakeLists.txt` — conditional `add_subdirectory(examples)`

**Remaining (deferred):**
- [x] Fix pre-existing example build errors — `0xBAD5ECRET` → `0xBAD5EC` (2026-03-03)
- [ ] `examples/README.md` — document opt-in build, example inventory

---

### ~~README Documentation Update — Application-oriented User Guide~~ ✅ DONE (2026-03-03)

Root `README.md` created with overview, quick start, architecture, four binaries, config model,
two dev paths, pipeline topologies, getting started walkthrough, further reading.
`share/demo/README.md` updated: stale actor references replaced with producer/consumer/processor.

**Goal**: Clean up and expand project README files to create application-oriented, user-friendly
documentation that helps new developers understand the infrastructure and build on top of it.

**Motivation**: The current README files are accurate but fragmented. A developer new to the project
cannot easily answer: "How do I use this? What binary do I run? How do I configure it? Can I use C++
directly or do I have to use Python?" This task unifies that story.

**What to document:**
1. **API Layers** — Clear explanation of L0 (C ABI), L1 (C++ primitives), L2 (RAII DataBlock),
   L3a (hub::Queue/Processor), L3b (hub::Producer/Consumer + BrokerService), with the umbrella
   include header for each level.
2. **Four standalone binaries** — What each binary does, how to configure it (`producer.json` etc.),
   what Python callbacks it supports, how they interoperate.
3. **Configuration model** — Config file fields, UID format, script directory layout,
   `--init` / `--keygen` / `--validate` CLI.
4. **Two development paths** — Python scripting (`on_produce/on_consume/on_process`) vs.
   C++ RAII API (`with_transaction`, `hub::Processor`) — when to use which.
5. **Getting started** — Minimal producer + consumer example walkthrough (from JSON config to
   running data flow), both Python-script path and C++ path.

**Files to update/create:**
- `docs/README/README_overview.md` (new or update) — top-level overview; links to detailed READMEs
- Update existing `docs/README/README_utils.md` — align with current L3a/L3b umbrella split
- Update `docs/README/README_testing.md` — note examples are now opt-in via `PYLABHUB_BUILD_EXAMPLES`
- Possibly update root `README.md` if it exists

**Cross-reference**: HEP-CORE-0011 (ScriptHost), HEP-CORE-0017 (Pipeline Architecture),
HEP-CORE-0018 (Producer/Consumer Binaries), HEP-CORE-0015 (Processor Binary)

**Priority**: Backlog — after Layer 4 tests, Schema Registry, and Processor binary tests.

---

### request_structure_remap / commit_structure_remap stub documentation

These two public methods in `data_block.hpp` throw `std::runtime_error("not implemented")` at runtime.
They need doc comments: `///< NOT IMPLEMENTED — deferred, see tech_draft/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md`.
Blocked on broker-controlled remapping design; low priority.

### ManagedProcessor for LifecycleGuard integration (Phase 2)

`hub::Processor` currently has no `ModuleDef` / `GetLifecycleModule()`. A future
`ManagedProcessor` wrapper could own `LifecycleGuard` startup/shutdown to integrate
with the lifecycle topology without manual `start()`/`stop()` calls.

### ZmqQueue integration tests

`ZmqQueue` is implemented but untested (requires endpoint coordination between two
processes). Add integration tests once the test harness supports dynamic port allocation
for PUSH/PULL endpoints.

### Pure C++ Producer/Consumer (no Python)

**Goal**: Provide a header-only CRTP/virtual template so users can implement high-performance
producer/consumer roles in C++ without Python, while reusing the same `producer.json` /
`consumer.json` config, `hub::Producer` / `hub::Consumer` services, and RAII slot lifecycle.

**Motivation**: Python scripting adds GIL overhead (~150 ns/iter uncontested + CPython call dispatch
~3–5 µs/iter). At >10 kHz loop rates or when squeezing max throughput, a pure C++ path eliminates
this overhead entirely while keeping the same deployment model (same hub, same channels, same config).

**Design sketch:**
```cpp
// User writes only this:
struct SensorSlot {
    double timestamp;
    float  value;
    static constexpr SchemaDesc schema() {
        return {{"timestamp","float64"}, {"value","float32"}};
    }
};

class MySensor : public pylabhub::CppProducer<SensorSlot>
{
    bool on_iteration(SensorSlot& slot) override
    {
        slot.timestamp = platform::now_s();
        slot.value     = read_adc();
        return true; // commit
    }
};

int main()
{
    pylabhub::CppActorHost host("actor.json"); // same actor.json, same hub.json
    host.add_producer<MySensor>("data_out");
    host.run(); // SIGINT + graceful shutdown included
}
```

**What's already available (no new infra needed):**
- `hub::Producer` / `hub::Consumer` full API — connect, acquire, commit, close
- `SlotIterator` / `TransactionContext` RAII acquire/release with LoopPolicy
- `hub::Messenger` — ZMQ peer-to-peer, heartbeat
- Signal handling pattern (established by GIL fixes)
- `RoleConfig` / `ActorConfig` parser — reads `actor.json`, `hub_dir`, channel config

**What to write:**
- `CppRoleHost` header-only template (CRTP or virtual base) — run loop without Python/GIL
- `CppActorHost` wrapper — SIGINT, graceful shutdown, LoopPolicy pacing (mirrors ActorHost)
- Compile-time schema descriptor (`SchemaDesc::schema()`) → runtime schema validation at connect
- Optional: `CppConsumer<SlotType>` mirror

**Design questions to resolve at implementation time:**
1. Schema declaration — `static constexpr SchemaDesc schema()` vs external JSON reuse
2. Flexzone — typed template parameter or `void*` + size
3. ZMQ messaging — typed `send<T>()` / `recv<T>()` or just wrap `Messenger` directly
4. Config — reuse `RoleConfig` directly (broker endpoint, channel name, interval_ms all there)

**Estimated effort:** 2–3 days. Not urgent — Python scripting covers all current use cases.

---

### Header / Include Layering Refactor

**Goal**: Establish a clean, layered include structure across the entire codebase so that each
header is self-contained at exactly its abstraction level, C-API users are never exposed to
C++ internals, and compilation time is proportional to what a TU actually uses.

**Motivation** (observed 2026-02-20):
- `data_block.hpp` (and related DataBlock headers) mix C-API primitives, C++ template
  abstractions, and implementation details in a single file. A C-API consumer sees the full
  C++ RAII layer; a high-level C++ user sees raw C-API structs. Neither gets a clean view.
- Compilation is dominated by the weight of transitive includes — `plh_datahub.hpp` pulls in
  all of DataBlock, Messenger, BrokerService, JsonConfig, and pybind11 in some TUs that only
  need one or two of these.
- Mix of C-API and C++ in the same file makes it harder to audit which symbols are stable ABI
  vs. which are internal C++ helpers.
- The `actor_schema.hpp` ↔ `nlohmann::json` coupling (fixed 2026-02-20 with `json_fwd.hpp`)
  is a symptom of the same problem: headers carrying dependencies they don't structurally need.

**Target structure** (sketch):

```
Layer 0 — C ABI (extern "C", no C++ constructs)
  slot_rw_coordinator.h      ← C consumers only need this
  data_block_c_api.h         ← raw acquire/release/checksum (no templates)

Layer 1 — C++ primitives (no pybind11, no nlohmann, no libzmq)
  shared_memory_spinlock.hpp
  data_block_config.hpp      ← DataBlockConfig, enums, constants only
  data_block_handles.hpp     ← SlotWriteHandle, SlotConsumeHandle

Layer 2 — C++ high-level (full DataBlock, Messenger — not pybind11)
  data_block.hpp             ← DataBlockProducer, DataBlockConsumer, templates
  messenger.hpp

Layer 3 — Integration (broker, hub API, JsonConfig)
  broker_service.hpp
  hub_producer.hpp / hub_consumer.hpp
  hub_config.hpp

Umbrella headers (convenience only, include what they promise):
  plh_datahub.hpp            ← all of Layer 3 + 2 + 1
  plh_service.hpp            ← Layer 2 minus DataBlock
```

**Key rules to enforce**:
1. A header at layer N may not include a header from layer > N.
2. C-API headers (`extern "C"`) may not include any C++ header.
3. Internal `.cpp`-only types (Pimpl `Impl`, static helpers) go in `*_internals.hpp` /
   `*_private.hpp` beside the `.cpp` — never in the installed public header.
4. Forward declarations (`*_fwd.hpp`) are preferred over full includes in peer headers.
5. CMake `target_include_directories` should enforce the boundary: C-API targets see only
   Layer 0 includes; `pylabhub-utils` consumers see Layer 1–3.

**Phase A complete (2026-02-26) — 550/550 tests passing:**
- [x] `data_block_policy.hpp` — 6 policy enums (DataBlockPolicy, ConsumerSyncPolicy,
      DataBlockOpenMode, ChecksumType, ChecksumPolicy, LoopPolicy)
- [x] `data_block_metrics.hpp` — ContextMetrics struct only
- [x] `data_block_config.hpp` — DataBlockConfig + DataBlockPageSize + TIMEOUT_* constants;
      includes data_block_policy.hpp
- [x] `data_block_fwd.hpp` — forward declarations (DataBlockProducer/Consumer/SlotHandles)
- [x] `src/include/nlohmann/json_fwd.hpp` shim — REMOVED 2026-02-27 (was a no-op; json_config.hpp
      needs full json.hpp in template bodies anyway; no compile-time saving possible without
      a deeper restructure). Public headers now use `<nlohmann/json.hpp>` directly.
- [x] `messenger.hpp`, `hub_producer.hpp`, `hub_consumer.hpp` → `<nlohmann/json.hpp>` (direct)

**Full audit completed 2026-02-26 (explore agent). Findings:**

**Critical entanglements to resolve (in priority order):**

**[x] P1 — Bool-disguise enum cleanup ✅ DONE 2026-02-27:**
- Removed 7 two-value enums that were bool-disguises:
  `LockMode`, `ResourceType` (file_lock.hpp), `ShmProcessingMode` (deleted),
  `CommitDecision` (json_config.hpp), `Sink::WRITE_MODE` (sink.hpp),
  `ValidationPolicy::OnFail`, `ValidationPolicy::OnPyError` (actor_config.hpp)
- Replaced with plain `bool` parameters and `bool` struct fields with named comments
- API change: `FileLock(path, is_directory, blocking)`, `Sink::write(msg, sync_flag)`,
  `ValidationPolicy::skip_on_validation_error`, `ValidationPolicy::stop_on_python_error`,
  `Producer/Consumer::has_realtime_handler()`
- Updated all 20+ call sites across src/ and tests/
- Added extensive doc comments to all remaining (non-bool-disguise) enums
- Updated HEP-CORE-0009 policy reference document
- 550/550 tests pass

**[x] P2 — Split `data_block.cpp` (3969L → 4 files + private header) ✅ DONE 2026-02-27 — 550/550 tests:**
```
shm/data_block_internal.hpp   — private header: detail:: inline helpers + internal:: constants
                                 + coordination function declarations (~310L)
shm/data_block_slot_ops.cpp   — acquire_write/read/release, state machine transitions (~180L)
shm/data_block_c_api.cpp      — all extern "C" functions from slot_rw_coordinator.h (~270L)
shm/data_block_schema.cpp     — schema info + layout checksum (BLAKE2b, no DataBlock dep) (~135L)
shm/data_block.cpp            — DataBlockLayout, DataBlock class, impl structs, public API,
                                 factory functions (~2894L; was 3969L)
```
Private header pattern (same as lifecycle_impl.hpp): detail::/internal:: namespaces shared
across slot_ops, c_api, and data_block.cpp; coordination functions declared in
namespace pylabhub::hub (no internal:: sub-namespace, matching public header convention).

**[x] P3 — `src/utils/` subdirectory restructure ✅ DONE 2026-02-27 — 550/550 tests:**
```
core/       — format_tools.cpp, debug_info.cpp, platform.cpp, uuid_utils.cpp
service/    — lifecycle.cpp, file_lock.cpp, crypto_utils.cpp, hub_vault.cpp
logging/    — logger.cpp + logger_sinks/ (moved from utils root)
config/     — json_config.cpp, hub_config.cpp
shm/        — data_block.cpp, data_block_mutex.cpp, data_block_recovery.cpp,
               shared_memory_spinlock.cpp, slot_diagnostics.cpp, slot_recovery.cpp,
               integrity_validator.cpp
ipc/        — zmq_context.cpp, messenger.cpp, heartbeat_manager.cpp,
               channel_handle.cpp, channel_registry.cpp, broker_service.cpp
hub/        — hub_producer.cpp, hub_consumer.cpp
```
Private headers moved alongside their .cpp (channel_registry.hpp, channel_handle_factory.hpp
→ ipc/; channel_handle_internals.hpp → hub/; uid_utils.hpp → config/).
portable_atomic_shared_ptr.hpp stays at src/utils/ root (cross-group).
Added PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} to target_include_directories in CMakeLists.txt.
test_layer3_datahub CMakeLists.txt: added src/utils/ipc to PRIVATE includes (for channel_registry.hpp).
test_layer3_datahub CMakeLists.txt: channel_registry.cpp source path updated to ipc/channel_registry.cpp.

**[x] P4 — Extract ZMQ protocol marshaling from `messenger.cpp` (1707L) ✅ DONE 2026-02-27 — 550/550 tests:**
- `ipc/messenger_internal.hpp` (275L): private header — `internal::` constants + inline helpers
  (`kFrameTypeControl`, `kDefaultRegisterTimeoutMs`, `hex_encode/decode_schema_hash`, etc.),
  all command structs, `HeartbeatEntry`, `MessengerImpl` class declaration (inline: dtor/enqueue/start_worker)
- `ipc/messenger_protocol.cpp` (665L): all broker protocol `handle_command` overloads
  (RegisterProducer, RegisterConsumer, DeregisterConsumer, UnregisterChannel, ChecksumErrorReport,
  DiscoverProducer, CreateChannel, ConnectChannel), `send_disc_req`, `send_immediate_heartbeat`
- `ipc/messenger.cpp` reduced to ~811L: worker loop, process_incoming, send_heartbeats,
  ConnectCmd/DisconnectCmd/StopCmd/SuppressHeartbeatCmd/HeartbeatNowCmd handlers, public API, lifecycle

**P5 — `plh_datahub.hpp` → `json_fwd.hpp` [1 h]:**
- Line 16: `#include <nlohmann/json.hpp>` → `#include <nlohmann/json_fwd.hpp>`
- Update TUs that need full json to include it explicitly

**[x] P6 — Split `actor_host.cpp` (1928L) role workers ✅ DONE 2026-02-27 — 550/550 tests:**
- `actor_role_workers.cpp` (1164L): ProducerRoleWorker + ConsumerRoleWorker implementations
- `actor_worker_helpers.hpp` (469L): anonymous-namespace helpers shared by both workers
- `actor_host.cpp` (284L): ActorHost orchestrator only
- `#pragma GCC diagnostic push/pop` suppresses unused-function warnings for helpers
  not used in every TU

**[x] P7 — Split `lifecycle.cpp` (1771L) ✅ DONE 2026-02-27 — 550/550 tests:**
- `lifecycle_impl.hpp` (354L): private header — includes, validate_module_name/timedShutdown
  helpers, lifecycle_internal types, LifecycleManagerImpl class definition
- `lifecycle_topology.cpp` (137L): buildStaticGraph, topologicalSort, printStatusAndAbort
- `lifecycle_dynamic.cpp` (811L): loadModule/Internal, unloadModule, shutdownModuleWithTimeout,
  computeUnloadClosure, dynShutdownThread*, processOneUnloadInThread, waitForUnload,
  getDynamicModuleState, recalculateReferenceCounts
- `lifecycle.cpp` (501L): ModuleDef API, ~LifecycleManagerImpl, register/init/finalize,
  log sink, LifecycleManager public delegation

**[x] P8 — Umbrella header split: client API vs server API ✅ DONE 2026-02-27 — 550/550 tests:**
Rationale: `plh_datahub.hpp` exposes BrokerService (server infra) alongside DataBlock/hub API.
Actors/scripts only need client API; hub server needs full API.

Completed:
- Removed redundant direct `<nlohmann/json.hpp>` from plh_datahub.hpp (transitive via messenger.hpp)
- Created `src/include/plh_datahub_client.hpp` (Layer 3a):
  plh_service.hpp + data_block.hpp + hub_producer.hpp + hub_consumer.hpp
  (no BrokerService, no JsonConfig, no HubConfig, no schema_blds)
  Note: nlohmann/json.hpp still arrives transitively via messenger.hpp — unavoidable
  without restructuring Messenger public API
- Updated docs: README_utils.md umbrella table, README_DirectoryLayout.md §1 (install tree)

**P9 — `hub_config_keys.hpp` [1–2 h, after Phase 5]:**
- Decouple config schema (keys, defaults, field accessors) from lifecycle module registration
- Small consumers can include config keys without dragging in lifecycle/ModuleDef

**Remaining items:**
- [ ] Audit `messenger.hpp` — verify no DataBlock or pybind11 leakage
- [ ] Audit `broker_service.hpp` for unnecessary transitive includes
- [ ] Review `actor_schema.hpp` and `actor_config.hpp` — no json.hpp in public actor headers
- [ ] Add CMake `SYSTEM` / `INTERFACE` include guards to enforce layer boundaries
- [ ] Measure full-rebuild time before/after as validation metric

**Files most affected**: `data_block.cpp`, `data_block.hpp`, `hub_consumer.hpp`,
`plh_datahub.hpp`, `src/utils/CMakeLists.txt`, actor layer

### API Enhancements
- [ ] **Config builder pattern** – Fluent API for DataBlockConfig construction
- [ ] **Error callbacks** – Register callbacks for specific error conditions
- [ ] **Flexible zone by name** – Access flexible zones by string name instead of index
- [ ] **Batch operations** – Read/write multiple values efficiently
- [ ] **Async API** – Non-blocking variants with futures/promises (if use case emerges)

### Configuration API
- [ ] **Config validation helpers** – Pre-validate config before creation
- [ ] **Config templates** – Named configs for common patterns (e.g., "single_writer", "high_throughput")
- [ ] **Config explicit-fail test** – Test that creation throws with invalid config

### Diagnostics API
- [ ] **Structured diagnostics** – Return diagnostic info as structured data, not just logs
- [ ] **Health check API** – Single call to check if DataBlock is healthy
- [ ] **Performance metrics** – Expose throughput, latency, contention metrics

### Recovery API Improvements
- [ ] **Integrity repair path** – Slot-checksum repair in `validate_integrity` uses
  `create_datablock_producer_impl` which reinitialises the header on open (since shm_create
  uses O_CREAT without O_EXCL).  Should use `attach_datablock_as_writer_impl` (WriteAttach mode)
  instead for safe in-place repair without destroying ring state.
- [ ] **Graceful degradation** – API for dealing with partially corrupted blocks
- [ ] **Recovery policies** – Configurable recovery behavior (aggressive vs conservative)

---

## API Design Principles

### 1. Error Handling Strategy
- **C API**: Return error codes, no exceptions
- **C++ API**: Throw for contract violations, return nullptr/false for expected failures
- **Recovery API**: Return `RecoveryResult` enum

### 2. Lifetime and Ownership
- **Factories** return `unique_ptr` (exclusive ownership)
- **Handles** are move-only, destroyed before owner
- **Guards** are move-only, RAII, noexcept destructors

### 3. Thread Safety
- **Producer/Consumer**: Thread-safe (internal mutex)
- **Handles**: Not thread-safe, use from one thread
- **C API**: No locking, caller's responsibility

### 4. Noexcept Marking
- Destructors: Always noexcept
- Simple accessors: noexcept if no throw
- Acquisition: Not noexcept (can fail)
- See IMPLEMENTATION_GUIDANCE.md § Explicit noexcept

---

## Documentation Tasks

### API Reference
- [ ] **Doxygen coverage** – Ensure all public APIs have complete documentation
- [ ] **Parameter descriptions** – Document all parameters, return values, exceptions
- [ ] **Usage examples** – At least one example per major API
- [ ] **Thread safety notes** – Document thread safety for each class

### User Guides
- [ ] **Getting started guide** – Simple producer/consumer example
- [ ] **Configuration guide** – All config options explained
- [ ] **Error handling guide** – How to handle failures at each API level
- [ ] **Migration guide** – From C API to C++, from primitive to transaction API

### Examples
- [ ] **Modernize producer example** – Use latest transaction API
- [ ] **Modernize consumer example** – Use iterator and transaction API
- [ ] **Add recovery example** – Show how to detect and recover from errors
- [ ] **Add flexible zone example** – Show typed flexible zone usage

---

## API Stability

### Breaking Changes (Major Version)
Track breaking changes for future major version bump:
- Removing Layer 1.75 (SlotRWAccess) ✅ Done in v1.0
- Config validation (require explicit parameters) ✅ Done in v1.0
- Structured buffer alignment change (compatibility break) - Planned for v2.0

### Deprecation Candidates
None currently. Maintain stable API for v1.x.

### Experimental APIs
Mark clearly as experimental, subject to change:
- Flexible zone by-name access (when added)
- Async API variants (when added)
- Batch operations (when added)

---

## Related Work

- **RAII Layer** (`docs/todo/RAII_LAYER_TODO.md`) – Transaction API is part of public API
- **Testing** (`docs/todo/TESTING_TODO.md`) – API surface needs comprehensive tests
- **Platform** (`docs/todo/PLATFORM_TODO.md`) – Cross-platform API consistency

---

## Recent Completions

### 2026-02-27 (P4: messenger.cpp split)
- ✅ **P4 — Split `messenger.cpp` (1707L)** — private header + new protocol file, 550/550 tests
  - `ipc/messenger_internal.hpp` (275L): private header with `internal::` constants/inline helpers
    + all command structs + HeartbeatEntry + MessengerImpl class declaration
  - `ipc/messenger_protocol.cpp` (665L): all broker protocol handlers (8 handle_command overloads)
    + send_disc_req + send_immediate_heartbeat
  - `ipc/messenger.cpp` reduced to 811L: worker loop, connection management, public API, lifecycle
  - CMakeLists.txt updated with `ipc/messenger_protocol.cpp`

### 2026-02-27 (HEP-CORE-0002 restructuring)
- ✅ **HEP-CORE-0002 restructured** — 3259L → 2676L:
  - New **§6 RAII Abstraction Layer** (§6.1–§6.10): design guarantees, TransactionContext,
    SlotIterator, SlotRef/ZoneRef, handles, DataBlockProducer/Consumer API tables,
    hub::Producer/Consumer active services, Slot-Processor API cross-ref (HEP-CORE-0006)
  - **§7 Control Plane Protocol** stub → authoritative reference HEP-CORE-0007
  - **§5** retitled "C Interface — Layer 0 (ABI-Stable)"; stale §5.3/§5.4/§5.5 removed;
    §5.6 → §5.3 (Helper Modules); layer overview diagram replaced with table
  - Implementation status block replaced with pointer to TODO_MASTER.md
  - Sections §6–§15 renumbered to §7–§16; all inline cross-refs updated

### 2026-02-27 (P2: data_block.cpp split)
- ✅ **P2 — Split `data_block.cpp` (3969L)** — private header + 3 new source files, 550/550 tests
  - `data_block_internal.hpp` (310L): private header with `detail::` inline helpers, `internal::`
    constants/inlines (promoted from anon namespace), coordination function declarations
  - `data_block_slot_ops.cpp` (180L): acquire_write/commit_write/release_write/acquire_read/
    validate_read_impl/release_read + is_writer_alive — pure state-machine functions, no DataBlock dep
  - `data_block_c_api.cpp` (270L): all extern "C" wrappers from slot_rw_coordinator.h
  - `data_block_schema.cpp` (135L): get_shared_memory_header_schema_info + layout checksum
    (BLAKE2b), no DataBlock dependency — uses only public headers
  - `data_block.cpp` reduced to 2894L (from 3969L); CMakeLists.txt updated with new files

### 2026-02-27 (HEP + README docs audit)
- ✅ **Docs audit** — checked all HEP and README files for consistency with reorganization:
  - HEP-CORE-0001: lifecycle.cpp impl status updated to 3-file split (`service/lifecycle.cpp`,
    `lifecycle_topology.cpp`, `lifecycle_dynamic.cpp` + `lifecycle_impl.hpp`);
    `DynamicModuleStatus` enum corrected (4→7 states, added UNLOADING/SHUTDOWN_TIMEOUT/FAILED_SHUTDOWN)
  - HEP-CORE-0003: impl status path `file_lock.cpp` → `service/file_lock.cpp`
  - HEP-CORE-0004: impl status path `logger.cpp` → `logging/logger.cpp`
  - HEP-CORE-0009 §2.1-2.3, §2.6.2, §5: DataBlockPolicy/ConsumerSyncPolicy/ChecksumPolicy/LoopPolicy
    updated to reference `data_block_policy.hpp` (direct definition) instead of `data_block.hpp`
  - README_utils.md: added `plh_datahub_client.hpp` (Layer 3a) to umbrella headers table + API ref table
  - README_DirectoryLayout.md §4-6: updated Phase 1 status (--init complete), removed
    `hub_identity.hpp/cpp` reference (replaced by `uuid_utils.hpp/cpp`), corrected "What needs attention" table
  - REVIEW_2026-02-26: fixed `connection_policy.hpp` → `channel_access_policy.hpp` in §2.5, §2.6, §4 table
  - CODE_REVIEW.md M21 triage: marked ✅ FALSE POSITIVE (requires(!IsWrite) already in code)
  - CODE_REVIEW.md: added missing M18 to triage table
  - SECURITY_TODO.md: added CurveZMQ secret key erasure to Deferred table
  - MESSAGEHUB_TODO.md: updated impl file paths to subdirectory layout
  - IMPLEMENTATION_GUIDANCE.md: updated source file references to subdirectory paths

### 2026-02-23
- ✅ **LoopPolicy Pass 2: ContextMetrics at DataBlock Pimpl (HEP-CORE-0008)**
  - Added `LoopPolicy` enum + `ContextMetrics` struct to `data_block.hpp`
  - `DataBlockProducerImpl` / `DataBlockConsumerImpl` gain `loop_policy`, `period_ms`,
    `metrics_`, `t_iter_start_`, `t_acquire_done_` fields
  - Timing injected in `acquire_write_slot()` / `acquire_consume_slot()` (Domains 2+3)
  - Work-time measured in `release_write_slot()` / `release_consume_slot()`
  - `set_loop_policy()`, `metrics()`, `clear_metrics()` implemented on both classes
  - `TransactionContext::metrics()` + `now()` const pass-through added
  - `actor_host.cpp`: wires `set_loop_policy()` from `interval_ms`, `clear_metrics()` at role start
  - `actor_api.cpp` + `actor_module.cpp`: `api.metrics()` returns Python dict (Domains 2+3+4)
  — `src/utils/data_block.cpp`, `src/include/utils/data_block.hpp`,
    `src/include/utils/transaction_context.hpp`,
    `src/actor/actor_host.cpp`, `src/actor/actor_api.cpp`, `src/actor/actor_module.cpp`
  **Deferred (Pass 3)**:
  - `SlotIterator::operator++()` sleep logic for RAII FixedRate path
  - `"loop_policy"` + `"period_ms"` in `ProducerOptions` / `ConsumerOptions` (hub_producer/consumer.hpp)
  - Tests for LoopPolicy/ContextMetrics (no tests exist yet — see TESTING_TODO)
  **488/488 passing.**
- ✅ **UAF fix in `timedShutdown()`** — `completed` and `ex_ptr` were stack-allocated in `timedShutdown()`
  but captured by reference in the thread lambda; after `thread.detach()` the function returned and
  destroyed them, leaving the detached thread with dangling references (UAF/UB on write).
  Fix: wrapped both in a `shared_ptr<SharedState>` so the detached thread keeps the allocation alive.
  This was the root cause of the intermittent `LifecycleTest.IsFinalizedFlag` timeout under load.
  — `src/utils/service/lifecycle_impl.hpp` (`timedShutdown()`, anonymous namespace)
  *(File was at `src/utils/lifecycle.cpp` before the 2026-02-27 lifecycle.cpp split.)*

### 2026-02-17 (DataBlock three-mode constructor + WriteAttach)

- ✅ **`DataBlockOpenMode` enum added** — `Create` / `WriteAttach` / `ReadAttach` modes in
  `data_block.hpp`; replaces `bool m_is_creator` in internal `DataBlock` class.
  Creator mode still owns and unlinks the segment; WriteAttach and ReadAttach do not.
  — `src/include/utils/data_block.hpp`, `src/utils/data_block.cpp`
- ✅ **`attach_datablock_as_writer_impl()` implemented** — Mirrors consumer validation
  (shared_secret, header layout hash, config checksum, both schemas).  Returns
  `DataBlockProducer` attached in WriteAttach mode (no unlink on destroy).
  — `src/include/utils/data_block.hpp`, `src/utils/data_block.cpp`
- ✅ **Timeout named constants** — `TIMEOUT_IMMEDIATE` (0), `TIMEOUT_DEFAULT` (100 ms),
  `TIMEOUT_INFINITE` (-1) added to `data_block.hpp` near `DataBlockOpenMode` enum.
  — `src/include/utils/data_block.hpp`
- ✅ **`ScopedDiagnosticHandle` alias** — `using ScopedDiagnosticHandle = std::unique_ptr<DataBlockDiagnosticHandle>`
  added near `open_datablock_for_diagnostic`; documented as canonical RAII pattern.
  — `src/include/utils/data_block.hpp`
- ✅ **4 new WriteAttach tests** — `DatahubWriteAttachTest`: basic roundtrip, secret
  validation, schema validation, segment persistence after writer detach.
  — `tests/test_layer3_datahub/test_datahub_write_attach.cpp`

### 2026-02-17 (docs audit — resolved issues verified)
- ✅ **API_ISSUE_NO_CONFIG_OVERLOAD resolved** — The dangerous no-config template overload
  `find_datablock_consumer<FlexZoneT, DataBlockT>(hub, name, secret)` (schema validated but
  NOT config/sizes) has been removed. The only template overload now requires `expected_config`
  (line 1396, `data_block.hpp`). All template consumers enforce full schema+config validation.
- ✅ **Deprecated single-schema template declarations removed** — Phase 3 single-schema
  templates (`create_datablock_producer<T>` / `find_datablock_consumer<T>` without dual schema)
  are gone; comment at line 1418 is a placeholder with empty section.
- ✅ **Obsolete code (`DataBlockSlotIterator`, `with_next_slot`, `LegacyTransactionContext`)
  removed** — None of these symbols exist in `src/`. Confirmed by grep search.

### 2026-02-17 (DRAINING policy reachability documented and tested)

- ✅ **DRAINING unreachability for ordered policies** — Proved (and verified in code) that
  `SlotState::DRAINING` is structurally unreachable for `Sequential` and `Sequential_sync`:
  ring-full check (`write_index - read_index < capacity`) fires **before** `write_index.fetch_add(1)`;
  if reader holds slot K then `read_index ≤ K`, making the ring-full condition impossible to
  pass. DRAINING is a `Latest_only`-only live mechanism.
  — `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` § 11, `docs/IMPLEMENTATION_GUIDANCE.md` Pitfall 11
- ✅ **2 new policy-barrier tests** — `SingleReaderRingFullBlocksNotDraining` and
  `SyncReaderRingFullBlocksNotDraining` verify `writer_reader_timeout_count == 0` and no slot
  in DRAINING state when a ring-full timeout occurs (7 draining tests total, 358 overall).
  — `tests/test_layer3_datahub/`

### 2026-02-17 (DRAINING state machine implemented)

- ✅ **`SlotState::DRAINING` activated** — `acquire_write()` now enters DRAINING when wrapping
  a COMMITTED slot; drain timeout restores COMMITTED; recovery path restores COMMITTED (not FREE).
  New readers automatically rejected (slot_state != COMMITTED → NOT_READY). Eliminates
  reader-race events on wrap-around. — `src/utils/data_block.cpp`,
  `src/utils/data_block_recovery.cpp`, `src/include/utils/data_block.hpp`
- ✅ **DRAINING tests** — 5 new `DatahubSlotDrainingTest` scenarios verify state machine:
  DRAINING entered, new readers rejected, resolves after release, timeout restores COMMITTED,
  zero reader races on clean wraparound. — `tests/test_layer3_datahub/`
- ✅ **Protocol doc updated** — State machine and producer flow updated with DRAINING transitions.
  — `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`

### 2026-02-17 (all code review items resolved)

- ✅ **[A-6] `high_resolution_clock` inconsistency** — Replaced with
  `platform::monotonic_time_ns()` in `logger.cpp:87` and `format_tools.cpp:100`
- ✅ **[A-7] `SlotState::DRAINING` undocumented** — Comment updated: active semantics documented
  — `src/include/utils/data_block.hpp`
- ✅ **[CONC-1b] `unlock()` clearing order** — Verified: `owner_pid == 0` is the
  authoritative "lock free" signal; ordering invariant documented in code
  — `src/utils/shared_memory_spinlock.cpp`
- ✅ **[A-4] Shutdown timeout no-op** — Verified: redesigned to real detachable threads,
  not `std::async`; comment confirms — `src/utils/service/lifecycle_impl.hpp`
  *(File was at `src/utils/lifecycle.cpp` before the 2026-02-27 lifecycle.cpp split.)*
- ✅ **[A-5] Handle destructors silent errors** — Verified: `LOGGER_WARN` emitted inside
  `release_write_handle()` for checksum failures — `src/utils/data_block.cpp`
- ✅ **[A-9] namespace inside extern "C"** — Verified: namespace placed before `extern "C"`
  block with comment — `src/include/utils/slot_rw_coordinator.h`
- ✅ **[Q-9] Heartbeat helpers not centralized** — Verified: `is_producer_heartbeat_fresh()`
  uses `producer_heartbeat_id_ptr()` / `producer_heartbeat_ns_ptr()` helpers

### 2026-02-17 (code review resolution)

- ✅ **[CONC-1] Zombie lock reclaim CAS** — `SharedSpinLock` zombie reclaim now uses
  `compare_exchange_strong` instead of plain `store` — `src/utils/shared_memory_spinlock.cpp`
- ✅ **[A-2] `flexible_zone_size` size_t → uint32_t** — ABI-fixed in `SharedMemoryHeader`
- ✅ **[A-3] Enum fixed underlying types** — `DataBlockPolicy : uint32_t`,
  `ConsumerSyncPolicy : uint32_t` — `src/include/utils/data_block.hpp`
- ✅ **[pImpl] `SharedSpinLock::m_name` ABI fix** — `std::string` replaced with
  `char m_name[256]` — `src/include/utils/shared_memory_spinlock.hpp`
- ✅ **[C4251/C4324] MSVC export warnings** — Pragmas added in `message_hub.hpp`,
  `data_block.hpp` — Windows compatibility
- ✅ **[Q-10] `update_reader_peak_count` TOCTOU** — Fixed with `compare_exchange_weak` loop

### 2026-02-14
- ✅ Documented all recovery error codes in recovery_api.hpp
- ✅ Unified metrics API with state snapshot fields

### 2026-02-13
- ✅ Documented release_write_slot return values and idempotent behavior
- ✅ Documented slot handle lifetime contract in data_block.hpp
- ✅ Added get_spinlock and spinlock_count to public API

### 2026-02-12
- ✅ Transaction guard API implemented and documented
- ✅ Iterator API refined (try_next, seek_to, seek_latest)

---

## Notes

### API Evolution Strategy

1. **Additive changes only** in minor versions
2. **Deprecation** with at least one minor version notice
3. **Breaking changes** only in major versions
4. **Experimental** APIs clearly marked

### API Review Checklist

Before adding any new public API:
- [ ] Is it necessary? Can existing API cover this?
- [ ] Is the naming consistent with existing APIs?
- [ ] Is the error handling strategy clear?
- [ ] Is thread safety documented?
- [ ] Is lifetime and ownership clear?
- [ ] Are there tests covering the new API?
- [ ] Is it documented with examples?

### Common API Patterns

**Resource acquisition**:
```cpp
// Factory pattern for complex objects
auto obj = create_thing(...);

// Optional for fallible operations
auto opt = try_lock(...);
if (opt.has_value()) { use(*opt); }

// nullptr for expected failures
auto handle = acquire_slot(...);
if (handle) { use(*handle); }
```

**Error reporting**:
```cpp
// Throw for contract violations
if (invalid_config) throw std::invalid_argument("...");

// Return false for expected failures
bool success = operation();

// Return enum for recovery operations
RecoveryResult result = recover();
```
