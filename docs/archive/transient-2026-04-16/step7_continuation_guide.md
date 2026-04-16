# Step 7 Continuation Guide — Messenger/ChannelHandle Removal

**Date**: 2026-04-12
**Branch**: `feature/lua-role-support` (10 commits ahead of push)
**State**: Source code changes in `git stash`. Pop with `git stash pop`.

---

## 1. Current Architecture (Authoritative — this is how things work NOW)

### 1.1 Communication Facilities

| Facility | Class | Purpose | Transport |
|---|---|---|---|
| **Data streaming** | `ShmQueue` / `ZmqQueue` via `QueueWriter` / `QueueReader` | High-frequency typed binary data (producer → consumer) | SHM (POSIX shm) or ZMQ PUSH/PULL (msgpack typed frames) |
| **Broker communication** | `BrokerRequestComm` | All role ↔ broker protocol: registration, heartbeat, role discovery, band pub/sub | ZMQ DEALER ↔ broker ROUTER, with inproc wake-up via `ZmqPollLoop` |
| **Band pub/sub** | `BandRegistry` (broker-side) + `BrokerRequestComm` methods | JSON coordination messaging between roles | Broker fan-out: `BAND_JOIN/LEAVE/BROADCAST/MEMBERS_REQ` |
| **Inbox** | `InboxQueue` (ROUTER) + `InboxClient` (DEALER) | Schema-enforced P2P messaging | Direct ZMQ ROUTER/DEALER between roles |

### 1.2 What's DELETED (never reference these)

| Deleted | What it was | Replacement |
|---|---|---|
| `Messenger` class | ZMQ DEALER client to broker + channel lifecycle | `BrokerRequestComm` for broker protocol; `Producer::create(opts)` / `Consumer::create(opts)` for local queue setup |
| `ChannelHandle` | P2C ZMQ socket wrapper (PUB/SUB/PUSH/PULL/ROUTER/DEALER) | `ShmQueue` / `ZmqQueue` own their sockets directly |
| `ChannelPattern` enum | Socket topology selector (PubSub/Pipeline/Bidir) | Still in `ChannelRegistry` for broker tracking, but no longer creates sockets |
| HELLO/BYE protocol | P2C consumer handshake | `CONSUMER_REG_REQ` / `CONSUMER_DEREG_REQ` (broker-mediated) |
| `on_zmq_data` callback | Raw bytes from ChannelHandle data socket | `QueueReader::read_acquire()` in data loop |
| `on_consumer_joined/left` | HELLO/BYE tracking | Broker CONSUMER_REG/DEREG + `CONSUMER_DIED_NOTIFY` |
| `on_consumer_message` / `on_producer_message` | P2C ctrl frame exchange | Inbox (schema P2P) or Band (JSON coordination) |
| `on_peer_dead` | ZMQ socket monitor disconnect | `BrokerRequestComm::on_hub_dead()` |
| `Producer::send()` / `send_to()` / `connected_consumers()` | Raw-bytes peer messaging | Deleted entirely — use typed queue or band |
| `ManagedProducer` / `ManagedConsumer` | RAII wrapper storing Messenger* | Deleted — lifecycle managed by role host |
| `start_embedded()` | Start peer_thread/data_thread/ctrl_thread on Producer/Consumer | Deleted — `start_queue()` starts only the queue transport |
| `send_ctrl()` | Typed ctrl frame on ChannelHandle ROUTER | Deleted — use inbox or band |
| `ctrl_queue_dropped` metric | P2C ctrl queue drop count | Returns 0 — metric removed |
| `GetLifecycleModule()` (DataExchangeHub bundle) | Bundled sodium+ZMQ+Messenger init | Split into `GetDataBlockModule()` + `GetZMQContextModule()` + `crypto::GetLifecycleModule()` |
| `lifecycle_initialized()` | Bundle ready flag | `datablock_lifecycle_ready()` — checks DataBlock module only |

### 1.3 Producer/Consumer Factory API (NEW)

```cpp
// OLD (deleted):
auto producer = hub::Producer::create(messenger, opts);  // took Messenger&
auto consumer = hub::Consumer::connect(messenger, opts);  // took Messenger&

// NEW:
auto producer = hub::Producer::create(opts);   // no Messenger, creates local queues only
auto consumer = hub::Consumer::create(opts);   // no Messenger, creates local queues only
```

Producer/Consumer no longer talk to the broker. They create local queue infrastructure
(ShmQueue or ZmqQueue). Broker registration is handled separately by `BrokerRequestComm`
at the role host level.

### 1.4 Lifecycle Modules

```
Logger (root — no deps)
  ├── CryptoUtils (sodium_init)
  │     └── DataBlock (SHM facility — no ZMQ dependency)
  └── ZMQContext (zmq::context_t — library init)
```

Each module is independent. `GetDataBlockModule()` does NOT depend on ZMQ.
`GetZMQContextModule()` does NOT depend on CryptoUtils. Tests should declare
only the modules they actually need.

### 1.5 Thread Model (per role host)

| Thread | What it does | Script engine calls |
|---|---|---|
| **Main (data loop)** | acquire slot → drain msgs → invoke on_produce/consume/process → commit | YES — primary script thread |
| **Broker** | `BrokerRequestComm::run_poll_loop()` — heartbeat, band notifications, request/reply | YES — `invoke("on_heartbeat")` |
| **Inbox** | `InboxQueue::recv_one()` → `invoke_on_inbox()` | YES — `invoke_on_inbox()` |
| **Logger** | Async I/O to sinks | No |

Cross-thread script dispatch: Python (GIL), Lua (thread-local child state),
Native (plugin responsibility).

---

## 2. What's in the Git Stash

Pop with `git stash pop`. Contains 68 files:

### Source code (DONE — compiles for libraries + binaries):
- 9 files deleted (Messenger, ChannelHandle, ChannelPattern)
- `hub_producer.hpp/cpp`, `hub_consumer.hpp/cpp` rewritten
- `role_api_base.hpp/cpp` — Messenger-free
- `zmq_context.cpp` — standalone ZMQContext module
- `data_block.hpp/cpp` — `GetDataBlockModule()` + `datablock_lifecycle_ready()`
- All 3 role host hpp/cpp — Messenger member removed
- `lua_engine.cpp` — `ctrl_queue_dropped` returns 0
- `plh_datahub.hpp`, `plh_datahub_client.hpp` — Messenger include removed
- `schema_registry.hpp/cpp` — `query_from_broker(Messenger&)` removed
- `data_block_internal.hpp` — Messenger include removed
- HEP-0002/0007/0011/0018 updated
- `obsolete_code_replacement.md` updated
- `API_TODO.md` — threading model + diagram tasks tracked

### Tests (NOT DONE — build will fail on L3 tests):

Partial fixes applied (lifecycle rename, some BRC translations). But 13 test
files still reference deleted Messenger/ChannelHandle and need rewriting.

---

## 3. Test Rewrite Plan

### 3.1 Files to DELETE (concept dead — test the old P2C infrastructure):

| File | Why delete |
|---|---|
| `workers/datahub_hub_api_workers.cpp` | 41 old `Producer::create(messenger, opts)` calls across 18 functions. Tests Messenger-mediated factory, HELLO/BYE tracking. Entire concept replaced. |
| `workers/datahub_engine_workers.cpp` | Old factory pattern for engine roundtrip. Needs complete redesign with new Producer/Consumer API. |

Also delete their headers (`datahub_hub_api_workers.h`) and update `test_datahub_hub_api.cpp`
and `test_datahub_engine_roundtrip.cpp` to remove references to deleted workers.

### 3.2 Files to REWRITE from scratch (valid concept, old mechanism):

| File | Concept to test | New mechanism |
|---|---|---|
| `test_datahub_broker_protocol.cpp` | Broker handles REG/DISC/HEARTBEAT correctly | Use `BrokerRequestComm` to send requests; verify broker state via admin API |
| `test_datahub_broker_shutdown.cpp` | Graceful shutdown: CHANNEL_CLOSING_NOTIFY → FORCE_SHUTDOWN | Register via BRC, trigger close, verify notifications arrive |
| `test_datahub_channel_access_policy.cpp` | Access policy enforcement (Open/Required/Verified) | Register via BRC with different policy configs |
| `test_datahub_broker_schema.cpp` | Schema protocol: SCHEMA_REQ/ACK, schema_id validation | Register via BRC with schema_id, query via BRC |
| `test_datahub_hub_federation.cpp` | Multi-broker federation relay | Register via BRC on each broker, verify cross-broker relay |
| `test_datahub_metrics.cpp` | Metrics forwarding: queue metrics, heartbeat piggybacking | Create Producer/Consumer with new factory, send metrics via BRC |
| `test_datahub_zmq_endpoint_registry.cpp` | ZMQ Endpoint Registry (HEP-0021): endpoint updates | Register via BRC, create ZmqQueue, verify endpoint resolution |
| `workers/datahub_channel_workers.cpp` | Data exchange: SHM and ZMQ roundtrip via queue | Use new `Producer::create(opts)` + `Consumer::create(opts)` directly |
| `workers/datahub_broker_health_workers.cpp` | Broker health: heartbeat timeout, dead consumer detection | Register via BRC, stop heartbeat, verify CONSUMER_DIED_NOTIFY |

### 3.3 Files to TRANSLATE (swap setup helper):

| File | Change |
|---|---|
| `test_datahub_messagehub.cpp` | Update worker references |
| `test_datahub_channel.cpp` | Update worker references to rewritten workers |

### 3.4 Shared test helper pattern

All new L3 tests that need a broker should use this pattern:

```cpp
#include "utils/broker_request_comm.hpp"

struct BrcHandle {
    hub::BrokerRequestComm brc;
    std::atomic<bool>      running{true};
    std::thread            thread;

    void start(const std::string &ep, const std::string &pk, const std::string &uid) {
        hub::BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey   = pk;
        cfg.role_uid        = uid;
        ASSERT_TRUE(brc.connect(cfg));
        thread = std::thread([this] { brc.run_poll_loop([this] { return running.load(); }); });
    }

    void stop() {
        running.store(false);
        brc.disconnect();
        if (thread.joinable()) thread.join();
    }

    ~BrcHandle() { if (thread.joinable()) stop(); }
};
```

For data queue tests, use the new factory directly:

```cpp
hub::ProducerOptions opts;
opts.channel_name = "test.channel";
opts.has_shm = true;
opts.shm_config = make_shm_config();
// ... set fields ...
auto producer = hub::Producer::create(opts);
ASSERT_TRUE(producer.has_value());
```

### 3.5 Lifecycle modules in tests

Tests should declare only what they need:

```cpp
// SHM-only test (no ZMQ):
auto guard = LifecycleGuard(MakeModDefList(
    Logger::GetLifecycleModule(),
    crypto::GetLifecycleModule(),
    hub::GetDataBlockModule()));

// Broker test (needs ZMQ):
auto guard = LifecycleGuard(MakeModDefList(
    Logger::GetLifecycleModule(),
    crypto::GetLifecycleModule(),
    hub::GetZMQContextModule()));

// Full role test (SHM + ZMQ):
auto guard = LifecycleGuard(MakeModDefList(
    Logger::GetLifecycleModule(),
    crypto::GetLifecycleModule(),
    hub::GetDataBlockModule(),
    hub::GetZMQContextModule()));
```

---

## 4. Post-Test Tasks

After tests are green:

1. **Directory restructure**: `src/utils/ipc/` → `src/utils/broker/`,
   move `heartbeat_manager.{hpp,cpp}` to `src/utils/shm/`
2. **ChannelRegistry cleanup**: remove dead P2C fields (`zmq_ctrl_endpoint`,
   `zmq_data_endpoint`, `zmq_pubkey`) from `ChannelRegistry::ChannelEntry`
3. **HEP-0011 threading model rewrite**: document current thread model
   (main, broker, inbox, logger) and per-engine dispatch
4. **Archive stale tech drafts**: `channel_redesign.md`,
   `channel_implementation_plan.md`, `broker_and_comm_channel_design.md` →
   `docs/archive/`
5. **Static code review**: full audit of all changes

---

## 5. How to Start the New Session

Paste this as the first message:

```
Continue the Step 7 Messenger/ChannelHandle removal on branch feature/lua-role-support.

Read docs/tech_draft/step7_continuation_guide.md first — it has the complete
current architecture, what's deleted, what's in the git stash, and the test
rewrite plan.

Start by: git stash pop, then execute the test rewrite plan in §3.
Do NOT patch old tests — delete and rewrite from scratch per §3.2.
Use the shared BrcHandle pattern from §3.4.
Use correct lifecycle modules per §3.5.

The source code is done. Only tests need rewriting. Get a green build + full
test pass, then do the post-test cleanup in §4.
```
