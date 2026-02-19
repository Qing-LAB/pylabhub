# Messenger TODO

**Purpose:** Track Messenger integration, broker protocol development, and channel management for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`
**Implementation:** `src/utils/messenger.cpp`, `src/utils/zmq_context.cpp`
**Header:** `src/include/utils/messenger.hpp`, `src/include/utils/zmq_context.hpp`
**Design Review:** `docs/IMPLEMENTATION_GUIDANCE.md` § Messenger code review

---

## Current Status

**Overall**: ✅ Consumer registration protocol implemented; end-to-end multi-process test passing

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

### Needed Tests (still pending)
- [ ] **Broker restart tests** – Graceful reconnection
- [ ] **Concurrent access tests** – Multiple threads using Messenger
- [ ] **Error injection tests** – Simulate broker failures

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
