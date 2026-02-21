# Messenger TODO

**Purpose:** Track Messenger integration, broker protocol development, and channel management for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`
**Implementation:** `src/utils/messenger.cpp`, `src/utils/zmq_context.cpp`
**Header:** `src/include/utils/messenger.hpp`, `src/include/utils/zmq_context.hpp`
**Design Review:** `docs/IMPLEMENTATION_GUIDANCE.md` § Messenger code review

---

## Current Status

**Overall**: ✅ HubShell all 6 phases complete + pylabhub-actor implemented — 426/426 tests passing (2026-02-21)

### HubShell 6-Phase Plan

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 1 | ✅ Complete (2026-02-20) | `HubConfig` lifecycle module + layered JSON config (`hub.default.json` / `hub.user.json`); `config/hub.default.json.in`; non-destructive cmake staging |
| Phase 2 | ✅ Complete (2026-02-20) | CMake Python env: `PYLABHUB_PYTHON_LOCAL_ARCHIVE` + `PYLABHUB_PYTHON_WHEELS_DIR` (offline); `prepare_python_env` target (stamp-based idempotency); `share/scripts/python/requirements.txt` |
| Phase 3 | ✅ Complete (2026-02-20) | Remove `src/broker/` standalone (fold into hubshell); deleted `src/broker/CMakeLists.txt` + `src/broker/broker_main.cpp` |
| Phase 4 | ✅ Complete (2026-02-20) | `PythonInterpreter` lifecycle module + pybind11 bindings (`pylabhub_module.cpp`); `pylabhub::third_party::pybind11_embed` CMake target |
| Phase 5 | ✅ Complete (2026-02-20) | Admin ZMQ shell: `AdminShell` C++ lifecycle module (127.0.0.1, optional token auth, exec()+JSON, persistent namespace); `share/scripts/python/hubshell_client.py` REPL client |
| Phase 6 | ✅ Complete (2026-02-20) | Rewrite `src/hubshell.cpp` — full LifecycleGuard, BrokerService from HubConfig, channels() wired, startup script execution, double-SIGINT fast-exit |

**Phase 1 files created:**
- `src/include/utils/hub_config.hpp` — `HubConfig` class with Pimpl, typed getters, `GetLifecycleModule()`
- `src/utils/hub_config.cpp` — layered load (hub.default.json → merge hub.user.json); binary-relative path discovery; env var overrides
- `config/hub.default.json.in` — canonical defaults template (staged as `hub.default.json` + initial `hub.user.json`)
- `cmake/StageIfMissing.cmake` — copies file only if destination absent (used for hub.user.json)
- `src/include/plh_datahub.hpp` — added `#include "utils/hub_config.hpp"`
- `src/utils/CMakeLists.txt` — added `hub_config.cpp` to UTILS_SOURCES
- `CMakeLists.txt` — added `stage_hub_config` target (always updates hub.default.json; non-destructive for hub.user.json)

**Broker health and notification layer complete (previous sprint):**
424/424 tests passing (Tests 423–424: RAII stress tests `DatahubStressRaiiTest`)

`Messenger` (renamed from `MessageHub`) provides ZeroMQ-based async communication with a broker:
- Producer registration (fire-and-forget, async worker thread)
- Producer discovery (synchronous via std::future/promise)
- Consumer registration (stub — protocol not yet defined)
- Consumer coordination

`BrokerService` (`src/include/utils/broker_service.hpp`, compiled into `pylabhub-utils`):
- Full Pimpl ABI-stable API; `BrokerServiceImpl` hides `ChannelRegistry`, ZMQ sockets, private handlers
- `pylabhub-broker` executable links against `pylabhub::utils` (no raw source duplication)
- CurveZMQ keypair generated at startup; public key logged for clients
- REG_REQ / DISC_REQ / DEREG_REQ / HEARTBEAT_REQ / CONSUMER_REG_REQ / CONSUMER_DEREG_REQ handled
- CHANNEL_CLOSING_NOTIFY pushed to registered consumers on heartbeat timeout

`ZMQContext` module: standalone `zmq::context_t` lifecycle module; also initialized automatically by `GetLifecycleModule()` (DataExchangeHub).

DataBlock factory functions (`create_datablock_producer_impl`, `find_datablock_consumer_impl`, `attach_datablock_as_writer_impl`) **no longer** accept or call `Messenger` — the coupling is fully removed. Broker registration is now caller-initiated.

---

## Current Focus

### Broker Protocol Definition
**Status**: ✅ Implemented (REG/DISC/DEREG)

- [x] **Broker message format** – JSON schema in HEP §6.2; REG_REQ/DISC_REQ/DEREG_REQ implemented
- [x] **Authentication** – CurveZMQ keypair generated at startup; public key logged
- [x] **Error handling** – Standardized ERROR responses (SCHEMA_MISMATCH, CHANNEL_NOT_FOUND, NOT_REGISTERED)
- [ ] **Version negotiation** – Deferred; not in HEP §6.2 scope

### Consumer Registration
**Status**: ✅ Complete (2026-02-18)

- [x] **CONSUMER_REG_REQ / CONSUMER_REG_ACK** – Broker validates channel exists; stores ConsumerEntry (pid + hostname)
- [x] **CONSUMER_DEREG_REQ / CONSUMER_DEREG_ACK** – Broker removes consumer by pid; returns NOT_REGISTERED on mismatch
- [x] **DISC_ACK consumer_count** – DISC_ACK now includes `consumer_count` field (additive, non-breaking)
- [x] **Messenger::register_consumer()** – Implements fire-and-forget CONSUMER_REG_REQ; pid filled from `platform::get_pid()`
- [x] **Messenger::deregister_consumer()** – New public method; fire-and-forget CONSUMER_DEREG_REQ
- [x] **ChannelEntry::consumers** – Vector of ConsumerEntry in ChannelRegistry; 3 new methods (register/deregister/find_consumers)
- [ ] **Consumer heartbeat to broker** – Keep broker informed of live consumers (deferred: zombie detection)
- [ ] **Naming conventions** – Use `logical_name()` per NAME_CONVENTIONS.md

---

## Backlog

### Phase C - MessageHub Integration

#### Core Protocol
- [ ] **Producer registration** – `register_producer` with full metadata
- [ ] **Producer discovery** – `discover_producer` with schema validation
- [ ] **Schema registry** – Broker stores and serves schema information
- [ ] **Channel lifecycle** – Create, destroy, list channels via broker

#### Advanced Features
- [ ] **Consumer groups** – Multiple consumers for load balancing
- [ ] **Priority channels** – Different QoS for different channels
- [ ] **Broker reconnection** – Handle broker restarts gracefully
- [ ] **Connection pooling** – Reuse ZeroMQ sockets efficiently

#### Monitoring and Diagnostics
- [ ] **Broker health check** – Detect broker unavailability
- [ ] **Connection metrics** – Track broker communication stats
- [ ] **Message tracing** – Debug protocol interactions

### Pluggable Slot-Processor and Messenger Access
**Status**: ✅ Complete (2026-02-19) — 424/424 tests passing
**HEP**: `docs/HEP/HEP-CORE-0006-SlotProcessor-API.md`

Two processing modes (selected implicitly by API call):
- **Queue mode**: `push<F,D>(fn)` (async) / `synced_write<F,D>(fn, timeout)` (sync) for
  producer; `pull<F,D>(fn, timeout)` for consumer.
- **Real-time mode**: `set_write_handler<F,D>(fn)` / `set_read_handler<F,D>(fn)` —
  framework thread drives loop continuously; handler hot-swappable at runtime.
  stop() notifies CV so sleeping threads wake immediately.

Handler receives fully-typed `WriteProcessorContext<F,D>` / `ReadProcessorContext<F,D>`
bundling: typed `FlexZoneT&` / `const FlexZoneT&`, `is_stopping()`,
peer messaging (`broadcast`, `send_to`, `send_ctrl`), `Messenger&` for broker access.

**Files changed:**
- `src/include/utils/hub_producer.hpp` — `push`, `synced_write`, `set_write_handler`, `shm_processing_mode`, `WriteProcessorContext<F,D>`, `ProducerMessagingFacade`, `InternalWriteHandlerFn`
- `src/include/utils/hub_consumer.hpp` — `pull`, `set_read_handler`, `shm_processing_mode`, `ReadProcessorContext<F,D>`, `ConsumerMessagingFacade`, `InternalReadHandlerFn`
- `src/utils/hub_producer.cpp` — dual-mode write_thread; facade wiring; helper methods
- `src/utils/hub_consumer.cpp` — dual-mode shm_thread; facade wiring; CV notify in stop(); helper methods
- `tests/.../datahub_hub_api_workers.cpp` — all tests updated to new typed context API

### No-Broker Fallback
**Status**: ✅ Partially complete

- [x] **Connect/disconnect idempotence** – Works without broker
- [x] **Send/receive when not connected** – Fails gracefully
- [x] **Register/discover with no broker** – Returns false/nullopt
- [ ] **File-based discovery** – Alternative discovery without broker
- [ ] **Embedded broker mode** – Run broker in same process for testing

---

## Design Decisions

### Singleton Pattern
**Current**: `Messenger::get_instance()` returns singleton

**Rationale**:
- One ZeroMQ context per process is recommended
- Single broker connection shared across all DataBlocks
- Simplifies lifecycle management

**Trade-offs**:
- Testing complexity (singleton state persists)
- No support for multiple brokers (not a requirement)

### JSON Message Format
**Current**: Use nlohmann::json for all broker messages

**Rationale**:
- Human-readable for debugging
- Easy schema evolution
- Well-supported library

**Trade-offs**:
- Slower than binary formats
- Larger message size
- Acceptable for control plane (not data plane)

### Error Handling Strategy
**Current**: Non-blocking, graceful degradation

**Pattern**:
- Broker unavailable → log warning, DataBlock creation succeeds
- Discovery fails → return nullopt, caller decides fallback
- Never crash due to broker issues

---

## Testing

### Current Coverage (Phase C groundwork)
- [x] Lifecycle-only tests (init/shutdown)
- [x] No-broker path tests
- [x] Connect/disconnect idempotence
- [x] Send/receive when not connected
- [x] Register/discover when broker unavailable
- [x] JSON parse failure paths

### Phase C Integration Tests — ✅ Complete (2026-02-18)
- [x] **ChannelRegistryOps** – Pure ChannelRegistry unit test (register, re-register, schema-mismatch, deregister, list)
- [x] **RegDiscHappyPath** – Full REG/DISC round-trip via Messenger ↔ real BrokerService (CurveZMQ)
- [x] **SchemaMismatch** – Re-register same channel with different schema_hash → ERROR SCHEMA_MISMATCH (raw ZMQ)
- [x] **ChannelNotFound** – Discover unknown channel → Messenger returns nullopt (CHANNEL_NOT_FOUND)
- [x] **DeregHappyPath** – Register → discover (found) → DEREG_REQ (correct pid) → discover → nullopt
- [x] **DeregPidMismatch** – DEREG_REQ with wrong pid → NOT_REGISTERED; channel still discoverable (raw ZMQ)

### Consumer Registration Tests — ✅ Complete (2026-02-18)
- [x] **ChannelRegistryConsumerOps** – Pure ChannelRegistry CRUD for ConsumerEntry (no ZMQ)
- [x] **ConsumerRegChannelNotFound** – CONSUMER_REG_REQ for unknown channel → CHANNEL_NOT_FOUND
- [x] **ConsumerRegHappyPath** – Messenger register_consumer → ACK; DISC_ACK consumer_count ≥ 1
- [x] **ConsumerDeregHappyPath** – Register consumer, deregister (correct pid) → success; count drops to 0
- [x] **ConsumerDeregPidMismatch** – Wrong pid → NOT_REGISTERED; consumer still registered
- [x] **DiscShowsConsumerCount** – consumer_count tracks 0→1 (register) →0 (deregister) via DISC_ACK

### End-to-End Multi-Process Test — ✅ Complete (2026-02-18)
- [x] **ProducerToConsumerViaRealBroker** – Real producer process writes 5 slots; real consumer process discovers, attaches, reads, verifies; real BrokerService mediates discovery; CurveZMQ encryption end-to-end

### ChannelHandle Phase 6 Tests — ✅ Complete (2026-02-18)
- [x] **CreateNotConnectedReturnsNullopt** – `create_channel` returns nullopt when Messenger not connected
- [x] **ConnectNotFoundReturnsNullopt** – `connect_channel` returns nullopt for unknown channel (timeout)
- [x] **PipelineDataExchange** – Pipeline `create_channel` + `connect_channel` + `send` + `recv`
- [x] **PubSubDataExchange** – PubSub exchange with retry loop for subscription propagation
- [x] **ChannelHandleIntrospection** – `channel_name()`, `pattern()`, `has_shm()`, `is_valid()`, move semantics, `invalidate()`

### BrokerService Library Integration — ✅ Complete (2026-02-18)
- [x] **BrokerService moved into pylabhub-utils** – Pimpl ABI-stable API; `BrokerServiceImpl` hides all ZMQ + ChannelRegistry internals
- [x] **ChannelPattern canonical header** – Single definition in `utils/channel_pattern.hpp`; broker namespace uses type alias
- [x] **pylabhub-broker executable** – Links `pylabhub::utils`; only compiles `broker_main.cpp`
- [x] **Private ChannelRegistry** – In `src/utils/`; not installed; tests compile own copy for white-box unit tests

### Hub Producer/Consumer Active API Tests — ✅ Complete (2026-02-19)

**9 original tests (hub::Producer + hub::Consumer unified API):**
- [x] **ProducerCreatePubSub** – Producer::create(PubSub, no SHM); is_valid, channel_name, close()
- [x] **ProducerCreateWithShm** – Producer::create(has_shm); synced_write slot-processor; push async
- [x] **ConsumerConnect** – Producer + Consumer via unified API; ZMQ send/recv end-to-end
- [x] **ConsumerHelloTracked** – After Consumer::connect(), connected_consumers() contains the identity
- [x] **ActiveProducerConsumerCallbacks** – start(); on_zmq_data fires with correct data
- [x] **PeerCallbackOnConsumerJoin** – on_consumer_joined fires from peer_thread when consumer sends HELLO
- [x] **NonTemplateFactory** – Non-template create/connect; SHM works; ZMQ works
- [x] **ManagedProducerLifecycle** – start()/stop()/close() sequencing; is_running() correct
- [x] **ConsumerShmSecretMismatch** – Wrong shm_shared_secret → consumer.shm() is nullptr; ZMQ still works

**5 module-level behavioral tests (2026-02-19):**
- [x] **ConsumerByeTracked** – Consumer::close() sends BYE; on_consumer_left fires; connected_consumers empties
- [x] **ConsumerShmReadE2E** – push (async) → set_read_handler fires with correct data
- [x] **ConsumerReadShmSync** – synced_write (sync) → pull (sync) data fidelity round-trip
- [x] **ProducerConsumerIdempotency** – start()/stop()/close() each called twice is safe; correct return values
- [x] **ProducerConsumerCtrlMessaging** – consumer->send_ctrl triggers on_consumer_message; producer->send_ctrl triggers on_producer_message

### Broker Health and Notification Tests — ✅ Complete (2026-02-19)

**5 broker health tests (DatahubBrokerHealthTest, tests 418–422):**
- [x] **ProducerGetsClosingNotify** – Cat 1: heartbeat timeout (1s) → producer's on_channel_closing fires
- [x] **ConsumerAutoDeregisters** – Consumer::close() sends CONSUMER_DEREG_REQ; broker consumer_count drops to 0
- [x] **ProducerAutoDeregisters** – Producer::close() sends DEREG_REQ; same channel re-registered immediately
- [x] **DeadConsumerDetected** – Cat 2: consumer _exit(0) (no clean deregister); broker liveness check (1s) detects dead PID → producer's on_consumer_died fires
- [x] **SchemaMismatchNotify** – Cat 1: second Messenger tries conflicting schema_hash on same channel; broker rejects + sends CHANNEL_ERROR_NOTIFY to original producer

**Implementation completions tied to these tests:**
- [x] **ChannelRegistry::producer_zmq_identity** – Stored on REG_REQ; enables broker→producer push
- [x] **ChannelRegistry::find_channel_mutable / all_channels** – For broker liveness iteration
- [x] **BrokerService** – stores producer ZMQ identity; sends CHANNEL_CLOSING_NOTIFY to producer; Cat 1 CHANNEL_ERROR_NOTIFY on schema mismatch; Cat 2 check_dead_consumers (is_process_alive PID check); CONSUMER_DIED_NOTIFY to producer; CHECKSUM_ERROR_REPORT handler stub
- [x] **Messenger per-channel callbacks** – `on_channel_closing(channel, cb)`, `on_consumer_died(channel, cb)`, `on_channel_error(channel, cb)` with unordered_map storage; `unregister_channel()` sends DEREG_REQ; `report_checksum_error()` fire-and-forget
- [x] **Producer auto-wire** – `create_from_parts` auto-registers 3 per-channel Messenger callbacks; `close()` clears them + calls `unregister_channel`
- [x] **Consumer auto-wire** – `connect_from_parts` auto-registers 2 per-channel Messenger callbacks + sends HELLO with consumer_pid JSON; `close()` clears callbacks + calls `deregister_consumer` before BYE
- [x] **Producer HELLO pid tracking** – `pid_to_identity` map populated from consumer HELLO body; CONSUMER_DIED_NOTIFY handler removes dead consumer from `consumer_identities`

### Needed Tests (backlog)
- [ ] **Broker restart tests** – Graceful reconnection
- [ ] **Concurrent access tests** – Multiple threads using Messenger
- [ ] **Error injection tests** – Simulate broker failures

### Python SDK — Layer 2 (future major phase)
- [ ] **Non-template Python-friendly Producer/Consumer API** – Design needed; `push<F,D>` template can't be called from Python; needs a bytes-based write path
- [ ] **pybind11 extension module** (`.so`/`.pyd`) – Exposes `Producer`, `Consumer`, `BrokerHandle` as Python classes; installable as `pylabhub` pip package
- [ ] **GIL management in callbacks** – write_handler/read_handler require `py::gil_scoped_acquire` when called from C++ threads
- [ ] **DataBlock schema Python binding** – BLDS schema description from Python; numpy dtype compatibility
- [ ] **Shared memory Python bridge** – `FlexZone` → Python bytes/memoryview/numpy array
- [ ] **Python test suite** – pytest-based tests for the Python SDK

---

## Protocol Messages (Draft)

### Producer Registration
```json
{
  "type": "PRODUCER_REGISTER",
  "channel": "sensor_data",
  "schema_hash": "abc123...",
  "schema_version": "1.0.0",
  "config": {
    "policy": "RingBuffer",
    "capacity": 10,
    "page_size": 4096
  }
}
```

### Consumer Registration
```json
{
  "type": "CONSUMER_REGISTER",
  "channel": "sensor_data",
  "consumer_id": "consumer_001",
  "expected_schema_hash": "abc123..."
}
```

### Discovery Request
```json
{
  "type": "DISCOVERY_REQUEST",
  "channel": "sensor_data"
}
```

### Discovery Response
```json
{
  "type": "DISCOVERY_RESPONSE",
  "channel": "sensor_data",
  "shm_name": "/pylabhub_sensor_data",
  "schema_hash": "abc123...",
  "schema_version": "1.0.0",
  "status": "active"
}
```

---

## Integration Points

### With DataBlock
- **DataBlock factory functions are fully decoupled from Messenger** — no hub parameter
- Broker registration is caller-initiated: after `create_datablock_producer_impl()` returns,
  caller calls `Messenger::get_instance().register_producer(channel, info)`
- Consumer discovery is caller-initiated: caller calls `discover_producer(channel)` to get
  the `shm_name`, then passes it to `find_datablock_consumer_impl()`
- Schema validation uses broker registry (when protocol is defined)

### With Schema System
- Schema info sent to broker on producer creation
- Schema validation on consumer attach uses broker data
- Schema versioning managed by broker

### With Lifecycle
- Messenger initialized via `GetLifecycleModule()` (DataExchangeHub module name)
- ZMQ context managed as separate `GetZMQContextModule()` ("ZMQContext"); DataExchangeHub depends on it
- `g_messenger_instance` (raw pointer) created in `do_hub_startup`, destroyed in `do_hub_shutdown`
  before `zmq_context_shutdown()` — guarantees socket closed before context destroyed
- Broker connection is NOT established during startup; caller must call `connect()` explicitly
- `zmq_context_shutdown()` is idempotent — `nullptr` guard prevents double-delete

---

## Related Work

- **Testing** (`docs/todo/TESTING_TODO.md`) – Phase C integration tests
- **API** (`docs/todo/API_TODO.md`) – Consumer registration API gaps
- **Platform** (`docs/todo/PLATFORM_TODO.md`) – ZeroMQ cross-platform consistency

---

## Recent Completions

### 2026-02-20
- ✅ **Phase 3** — Deleted `src/broker/` standalone executable; BrokerService remains in `pylabhub-utils`; BrokerService now started directly from `hubshell.cpp` main()
- ✅ **Phase 4** — `PythonInterpreter` lifecycle module (`src/hub_python/python_interpreter.hpp/cpp`); `PYBIND11_EMBEDDED_MODULE(pylabhub, m)` (`pylabhub_module.cpp`); `pylabhub::third_party::pybind11_embed` CMake INTERFACE target; `Python::Development` removed from both pybind11 targets (CMake 3.29+ compatibility)
- ✅ **Phase 5** — `AdminShell` C++ lifecycle module (`src/hub_python/admin_shell.hpp/cpp`); ZMQ REP socket on `admin_endpoint`; optional token auth via `HubConfig::admin_token()`; `BrokerService::list_channels_json_str()` with `m_query_mu` mutex (thread-safe snapshot); `share/scripts/python/hubshell_client.py` interactive REPL client with `--exec`/`--file`/`--endpoint`/`--token` flags; `:channels`, `:config`, `:help` shortcuts
- ✅ **Phase 6** — `src/hubshell.cpp` full rewrite: all 9 lifecycle modules registered, BrokerService from HubConfig in background thread, `channels()` callback wired to `list_channels_json_str()`, Python startup script execution, double-SIGINT fast-exit, graceful shutdown via `pylabhub.shutdown()` or signal
- ✅ **HubConfig::admin_token()** — new getter; reads from `hub.user.json["admin"]["token"]`; empty = no auth
- ✅ **HubConfig::admin_endpoint()** — already existed; now also used by AdminShell lifecycle
- ✅ **All 424/424 tests pass** — no regressions from Phases 3-6

**Architecture note (Python SDK):**
The embedded `pylabhub` module (Phase 4) is for admin/control only. User-facing Python producer/consumer bindings (`pylabhub` pip package with `pybind11::module`) are a separate future phase requiring non-template API design and GIL management for C++ thread callbacks.

### 2026-02-19
- ✅ **Broker health and notification layer** – Cat 1/Cat 2 error taxonomy implemented end-to-end; ChannelRegistry+BrokerService+Messenger+Producer+Consumer all updated; 5 new DatahubBrokerHealthTest tests; 422/422 passing
- ✅ **Messenger per-channel callback maps** – Replaced single global `on_channel_closing` with per-channel `m_channel_closing_cbs`, `m_consumer_died_cbs`, `m_channel_error_cbs`; global callback kept for backward compat under `m_global_channel_closing_cb`
- ✅ **Producer/Consumer auto-wire** – Messenger per-channel callbacks registered automatically in `create_from_parts`/`connect_from_parts`; cleared in `close()`; safe via pImpl lifetime (close clears before destroy)
- ✅ **Producer::close() deregisters channel** – Sends DEREG_REQ via `unregister_channel()`; new channel can be created immediately without 10s heartbeat timeout
- ✅ **Consumer::close() deregisters from broker** – Sends CONSUMER_DEREG_REQ via `deregister_consumer()` before BYE; broker consumer_count correctly tracks live consumers
- ✅ **CONSUMER_DIED_NOTIFY** – Broker liveness check (configurable interval, default 5s) uses `is_process_alive()`; sends CONSUMER_DIED_NOTIFY to producer; Producer removes dead consumer from `consumer_identities` + fires `on_consumer_died` callback
- ✅ **CHANNEL_ERROR_NOTIFY** – Cat 1 schema mismatch: broker sends CHANNEL_ERROR_NOTIFY to existing producer; Messenger routes to `on_channel_error` callback on ProducerImpl
- ✅ **Producer `on_consumer_died` + `on_channel_error`** – New public callbacks on `hub::Producer`
- ✅ **Consumer `on_channel_error`** – New public callback on `hub::Consumer`
- ✅ **Consumer HELLO includes consumer_pid** – JSON body `{"consumer_pid": N}` enables ProducerImpl to populate `pid_to_identity` map for CONSUMER_DIED_NOTIFY→identity removal
- ✅ **`hub::Producer` active service** – `ProducerOptions`, `Producer` class with peer_thread, write_thread, `ManagedProducer`
- ✅ **`hub::Consumer` active service** – `ConsumerOptions`, `Consumer` class with data_thread, ctrl_thread, shm_thread; `ManagedConsumer`
- ✅ **`DatahubHubApiTest`** – 15 tests (403–417) all passing
- ✅ **`DatahubBrokerHealthTest`** – 5 tests (418–422) all passing
- ✅ **422/422 full test suite passes** — no regressions
- ✅ **Static analysis: `bool closed` → `std::atomic<bool>`** — `ProducerImpl::closed` and `ConsumerImpl::closed` were plain `bool`, causing a data race: main thread writes in `close()`, Messenger worker thread reads in lambda callbacks. Fixed to `std::atomic<bool>` in `hub_producer.cpp:56` and `hub_consumer.cpp:51`. All implicit bool conversions at read sites remain correct.
- ✅ **Static analysis: callback try-catch in `process_incoming()`** — `messenger.cpp` invoked user callbacks (on_channel_closing, on_consumer_died, on_channel_error) inside `catch(json::exception)` blocks; non-JSON exceptions from user callbacks killed the worker thread. Each callback invocation now has its own `try-catch(std::exception)` + `catch(...)` guard with LOGGER_ERROR.
- ✅ **RAII stress tests** — `DatahubStressRaiiTest` (tests 423–424); `MultiProcessFullCapacityStress` (500 × 4KB, ring=32, 2 racing consumers, random delays) + `SingleReaderBackpressure` (100 × 4KB, ring=8, 0–20ms consumer delay); 424/424 passing

**Deferred:**
- BrokerService health_thread (multi-threaded broker I/O)
- ChecksumRepairPolicy::Repair (needs WriteAttach-based slot repair path)
- Consumer heartbeat timeout detection in Producer's peer_thread
- Consumer receives CHANNEL_EVENT_NOTIFY callback (infra ready; add later)

### 2026-02-18
- ✅ **Consumer registration protocol** – CONSUMER_REG_REQ/ACK and CONSUMER_DEREG_REQ/ACK in BrokerService; ConsumerEntry + consumers vector in ChannelRegistry; register/deregister/find_consumers methods; consumer_count field in DISC_ACK; Messenger::register_consumer() implemented (was stub); Messenger::deregister_consumer() new public method; DeregisterConsumerCmd added to MessengerCommand variant
- ✅ **DatahubBrokerConsumerTest** – 6 tests (391–396): ChannelRegistryConsumerOps, ConsumerRegChannelNotFound, ConsumerRegHappyPath, ConsumerDeregHappyPath, ConsumerDeregPidMismatch, DiscShowsConsumerCount
- ✅ **DatahubE2ETest** – 1 test (397): ProducerToConsumerViaRealBroker — orchestrator spawns producer+consumer subprocesses; real broker; CurveZMQ; 397/397 tests passing
- ✅ **Phase C broker integration tests** – `DatahubBrokerTest` (6 tests, 390/390 total passing); `start_broker_in_thread` helper with `on_ready` callback; `raw_req` helper supporting optional CurveZMQ; `BrokerService::Config::on_ready` added for dynamic port assignment in tests
- ✅ **`BrokerService::Config::on_ready` callback** – Called from `run()` after `bind()` with (bound_endpoint, server_public_key); enables tests to use `tcp://127.0.0.1:0` dynamic port assignment without sleep() hacks; bound endpoint now logged instead of config endpoint
- ✅ **pylabhub-broker implemented** – `src/broker/` directory: `ChannelRegistry`, `BrokerService`, `broker_main.cpp`; standalone `pylabhub-broker` executable; links against `pylabhub::utils`
- ✅ **CurveZMQ server** – Keypair generated via `zmq_curve_keypair` at construction; public key logged at startup; clients use it for `Messenger::connect()`
- ✅ **REG_REQ handler** – Validates schema_hash; allows re-registration on producer restart; returns SCHEMA_MISMATCH error on hash mismatch
- ✅ **DISC_REQ handler** – Looks up channel; returns DISC_ACK with shm_name/schema_hash/schema_version/metadata; or CHANNEL_NOT_FOUND
- ✅ **DEREG_REQ handler** – Removes channel entry if producer_pid matches; returns NOT_REGISTERED on mismatch
- ✅ **384/384 tests still pass** – No regressions from broker binary addition

### 2026-02-17
- ✅ **MessageHub → Messenger rename** – Clean rename; no compat shims; v1.0 design
- ✅ **Async command queue** – Worker thread owns ZMQ socket; fire-and-forget `register_producer`; synchronous `discover_producer` via `std::future/promise`
- ✅ **ZMQContext lifecycle module** – `GetZMQContextModule()` for standalone use; automatically initialized by `GetLifecycleModule()` (DataExchangeHub)
- ✅ **DataBlock decoupling** – Removed `Messenger &hub` parameter from all DataBlock factory functions; removed `register_with_broker()` and `discover()` methods; broker registration is now caller-initiated
- ✅ **message_hub.hpp/cpp deleted** – `messenger.hpp/cpp` and `zmq_context.hpp/cpp` replace them
- ✅ **All 21+ test worker files updated** – hub param removed from factory calls
- ✅ **in-process broker test (with_broker_happy_path)** – Full round-trip: `register_producer` + `discover_producer` + write/read
- ✅ **Test class renamed** – `DatahubMessageHubTest` → `DatahubMessengerTest`
- ✅ **Use-after-free fix in send** – `zmq::buffer(std::string(...))` dangling pointer bug fixed in `RegisterProducerCmd` and `DiscoverProducerCmd` handlers
- ✅ **ZMQ lifecycle ownership fix** – `Messenger::get_instance()` is NO LONGER a function-local static; `g_messenger_instance` raw pointer managed by `do_hub_startup` / `do_hub_shutdown`; static destruction order hazard eliminated; socket always closed before context destroyed
- ✅ **Idempotent `zmq_context_shutdown()`** – `nullptr` guard prevents double-delete if both lifecycle modules registered
- ✅ **Assert message corrected** – `get_instance()` assert reads "called before registration and initialization through Lifecycle"
- ✅ **HEP-CORE-0002 updated** – Section 2.1 diagram, Section 6.1 characteristics, Section 6.5 (full rewrite: async queue design + Mermaid diagrams), Section 7 code examples; all `MessageHub`/`register_with_broker` references replaced with current Messenger API
- ✅ **raii_layer_example.cpp fixed** – Removed stale `message_hub.hpp` include; removed hub parameter from factory calls

### 2026-02-12
- ✅ MessageHub Phase C groundwork (no-broker paths)
- ✅ JSON safety (parse failures handled gracefully)
- ✅ Lifecycle integration tests

### 2026-02-11
- ✅ MessageHub code review completed
- ✅ Design compliance with C++20 patterns verified
- ✅ JSON message handling hardened

### 2026-02-10
- ✅ MessageHub ZeroMQ integration fixed (recv_multipart warnings)
- ✅ Send_message signature aligned

---

## Notes

### Broker Dependencies

Messenger functionality is limited until broker provides:
1. **Protocol specification** – Message formats and flows
2. **Schema registry** – Store and serve schema information  
3. **Consumer coordination** – Track consumer registrations
4. **Discovery service** – Map channels to shared memory names

### Graceful Degradation

Design principle: **DataHub works without broker**
- Producer creation succeeds even if broker is down
- Consumer find falls back to direct shared memory access
- Schema validation uses local config if broker unavailable
- Applications can function with reduced features (no discovery)

### ZeroMQ Patterns

**Current**: DEALER/ROUTER pattern for broker communication (async worker thread)

**Considerations**:
- Simple request/response model
- Synchronous, blocking calls
- Sufficient for control plane
- Could use DEALER/ROUTER for async if needed

**Connection management**:
- Single socket per Messenger instance (lifecycle singleton)
- Connection is manual: caller calls `connect(endpoint, server_key)`
- Reconnection with exponential backoff (when implemented)

### Open Questions

- Should broker connection be required or optional?
- How to handle broker version mismatches?
- Should we support multiple brokers for HA?
- What's the discovery fallback strategy without broker?
