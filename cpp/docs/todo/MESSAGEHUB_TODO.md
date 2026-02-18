# Messenger TODO

**Purpose:** Track Messenger integration, broker protocol development, and channel management for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`
**Implementation:** `src/utils/messenger.cpp`, `src/utils/zmq_context.cpp`
**Header:** `src/include/utils/messenger.hpp`, `src/include/utils/zmq_context.hpp`
**Design Review:** `docs/IMPLEMENTATION_GUIDANCE.md` § Messenger code review

---

## Current Status

**Overall**: 🟢 Core infrastructure complete — broker protocol features still pending

`Messenger` (renamed from `MessageHub`) provides ZeroMQ-based async communication with a broker:
- Producer registration (fire-and-forget, async worker thread)
- Producer discovery (synchronous via std::future/promise)
- Consumer registration (stub — protocol not yet defined)
- Consumer coordination

`ZMQContext` module: standalone `zmq::context_t` lifecycle module; also initialized automatically by `GetLifecycleModule()` (DataExchangeHub).

DataBlock factory functions (`create_datablock_producer_impl`, `find_datablock_consumer_impl`, `attach_datablock_as_writer_impl`) **no longer** accept or call `Messenger` — the coupling is fully removed. Broker registration is now caller-initiated.

---

## Current Focus

### Broker Protocol Definition
**Status**: 🔴 Blocked - Waiting for broker team

- [ ] **Define broker message format** – JSON schema for all broker messages
- [ ] **Version negotiation** – How client and broker agree on protocol version
- [ ] **Authentication** – Security model for broker connections
- [ ] **Error handling** – Standardize error responses from broker

### Consumer Registration
**Status**: 🔴 Blocked - Protocol not defined

- [ ] **Implement register_consumer** – Currently a stub in messenger.cpp (protocol not defined)
- [ ] **Consumer heartbeat to broker** – Keep broker informed of live consumers
- [ ] **Consumer discovery** – How producers find consumers via broker
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

### Needed Tests (when broker ready)
- [ ] **Full protocol tests** – All message types with live broker
- [ ] **Schema registry tests** – Store, retrieve, validate schemas
- [ ] **Consumer registration tests** – Register, heartbeat, discover
- [ ] **Broker restart tests** – Graceful reconnection
- [ ] **Concurrent access tests** – Multiple threads using MessageHub
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
