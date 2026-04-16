# Tech Draft: broker_request_comm + role_communication_channel

**Status**: Draft (2026-04-07)
**Branch**: `feature/lua-role-support`
**Relates to**: HEP-CORE-0007 (Protocol), HEP-CORE-0018 (Producer/Consumer)
**Replaces**: `Messenger` module (src/utils/ipc/messenger.cpp)

---

## 1. Problem Statement

The current `Messenger` module entangles two distinct communication concerns:

1. **Role ↔ Broker protocol** — registration, heartbeat, discovery, metrics,
   presence queries. Uses a single DEALER socket to the broker's ROUTER.

2. **Producer ↔ Consumer direct communication** — peer join/leave, peer-dead
   detection, broadcast data frames, custom messages. Uses separate ROUTER/DEALER
   + XPUB/SUB sockets created per channel.

These are mixed into one class with one worker thread, one command queue,
and shared callback infrastructure. This causes:

- Layer violations: role hosts reach into Messenger internals for socket
  handles (`peer_ctrl_socket_handle`, `data_zmq_socket_handle`)
- Embedded mode: a workaround where the Messenger creates sockets but
  doesn't poll them, forcing the role host to poll manually
- Heartbeat ownership confusion: Messenger has its own periodic timer,
  role suppresses it and takes over
- Difficult to reason about thread safety: one worker thread handles both
  broker protocol and channel factory lifecycle

**Solution**: Split into two independent modules, each with a single clear
responsibility, a single socket system, and a single dedicated thread.

---

## 2. Module 1: broker_request_comm

### 2.1 Purpose

Handles all communication between the role and the broker. The broker is
a central registry that manages channel lifecycle, liveness, and routing.

### 2.2 Transport

One **DEALER** socket connected to the broker's **ROUTER** endpoint.
Optional CurveZMQ encryption (server pubkey + client keypair).

### 2.3 Wire protocol

Outbound (DEALER → ROUTER):
```
['C'] [msg_type_string] [json_body]
```

Inbound (ROUTER → DEALER, broker-initiated notifications):
```
['C'] [msg_type_string] [json_body]
```

All payloads are JSON. The broker dispatches by `msg_type` string.

### 2.4 Message catalog

**Request-reply (role sends, broker replies):**

| Request | Reply | Purpose |
|---------|-------|---------|
| REG_REQ | REG_ACK | Register producer channel (returns endpoints) |
| DISC_REQ | DISC_ACK | Discover channel (consumer gets producer endpoints) |
| CONSUMER_REG_REQ | CONSUMER_REG_ACK | Register consumer on channel |
| DEREG_REQ | DEREG_ACK | Deregister producer channel |
| CONSUMER_DEREG_REQ | CONSUMER_DEREG_ACK | Deregister consumer |
| ROLE_PRESENCE_REQ | ROLE_PRESENCE_ACK | Check if role UID is alive |
| ROLE_INFO_REQ | ROLE_INFO_ACK | Get inbox connection info for a role |
| CHANNEL_LIST_REQ | CHANNEL_LIST_ACK | List all registered channels |
| SHM_BLOCK_QUERY_REQ | SHM_BLOCK_QUERY_ACK | Query SHM block info |
| SCHEMA_REQ | SCHEMA_ACK | Query channel schema |
| METRICS_REQ | METRICS_ACK | Query aggregated metrics |
| ENDPOINT_UPDATE_REQ | ENDPOINT_UPDATE_ACK | Update endpoint after bind |

**Fire-and-forget (role sends, no reply):**

| Message | Purpose |
|---------|---------|
| HEARTBEAT_REQ | Liveness signal + optional metrics payload |
| METRICS_REPORT_REQ | Consumer metrics report |
| CHANNEL_NOTIFY_REQ | Send event to target channel members |
| CHANNEL_BROADCAST_REQ | Broadcast to all channel members |
| CHECKSUM_ERROR_REPORT | Report slot checksum error |

**Broker-initiated (unsolicited, received by the role):**

| Notification | Purpose |
|-------------|---------|
| CHANNEL_CLOSING_NOTIFY | Channel is shutting down (grace period) |
| FORCE_SHUTDOWN | Grace period expired, shut down immediately |
| CONSUMER_DIED_NOTIFY | A consumer on the channel timed out |
| CHANNEL_ERROR_NOTIFY | Channel error event |
| CHANNEL_BROADCAST_NOTIFY | Broadcast relayed from another role |
| ROLE_REGISTERED_NOTIFY | A new role appeared on a channel |
| ROLE_DEREGISTERED_NOTIFY | A role left a channel |

### 2.5 Threading

One dedicated thread managed by RoleAPIBase's thread manager. This thread:

1. **Polls** the DEALER socket (non-blocking, ~100ms interval)
2. **Sends** outbound requests from a thread-safe command queue
3. **Receives** broker replies and unsolicited notifications
4. **Dispatches** notifications to `core_.enqueue_message()` (same pattern
   as current `wire_event_callbacks`)
5. **Sends** periodic HEARTBEAT_REQ at the configured interval, including
   `snapshot_metrics_json()` in the payload
6. **Invokes** `engine->invoke("on_heartbeat")` via ThreadEngineGuard
7. **Detects** hub-dead via ZMQ socket monitor (ZMQ_EVENT_DISCONNECTED)

This thread replaces BOTH the old Messenger worker thread AND the old
role-level ctrl_thread_. The heartbeat is owned entirely by this thread —
no suppress/unsuppress dance.

### 2.6 API (thread-safe, callable from any thread)

```cpp
class BrokerRequestComm
{
public:
    struct Config
    {
        std::string broker_endpoint;
        std::string broker_pubkey;       // empty = no CurveZMQ
        std::string client_pubkey;
        std::string client_seckey;
        int         heartbeat_interval_ms{5000};
        bool        report_metrics{false};
    };

    /// Connect to broker. Called once during setup.
    bool connect(const Config &cfg);

    /// Disconnect and clean up.
    void disconnect();

    // ── Request-reply (blocks until reply or timeout) ────────────────

    /// Register a producer channel. Returns REG_ACK payload.
    std::optional<nlohmann::json> register_channel(const nlohmann::json &opts,
                                                    int timeout_ms = 5000);

    /// Discover a channel. Returns DISC_ACK payload (endpoints, pubkey, etc.).
    std::optional<nlohmann::json> discover_channel(const std::string &channel,
                                                    const nlohmann::json &opts,
                                                    int timeout_ms = 5000);

    /// Register consumer. Returns CONSUMER_REG_ACK payload.
    std::optional<nlohmann::json> register_consumer(const nlohmann::json &opts,
                                                     int timeout_ms = 5000);

    /// Deregister producer channel.
    bool deregister_channel(const std::string &channel, int timeout_ms = 5000);

    /// Deregister consumer.
    bool deregister_consumer(const std::string &channel, int timeout_ms = 5000);

    /// Query role presence.
    bool query_role_presence(const std::string &uid, int timeout_ms = 5000);

    /// Query role info (inbox endpoint, etc.).
    std::optional<nlohmann::json> query_role_info(const std::string &uid,
                                                   int timeout_ms = 5000);

    /// List all channels.
    std::vector<nlohmann::json> list_channels(int timeout_ms = 5000);

    /// Query SHM info.
    std::optional<nlohmann::json> query_shm_info(const std::string &channel,
                                                  int timeout_ms = 5000);

    // ── Fire-and-forget (non-blocking, enqueued) ─────────────────────

    void send_heartbeat(const nlohmann::json &metrics);
    void send_metrics_report(const std::string &channel,
                             const std::string &uid,
                             const nlohmann::json &metrics);
    void notify_channel(const std::string &target,
                        const std::string &event,
                        const std::string &data);
    void broadcast_channel(const std::string &target,
                           const std::string &msg,
                           const std::string &data);
    void report_checksum_error(const nlohmann::json &report);
    void update_endpoint(const std::string &channel,
                         const std::string &key,
                         const std::string &endpoint);

    // ── Notification callbacks ───────────────────────────────────────

    using NotificationCallback = std::function<void(const std::string &type,
                                                     const nlohmann::json &payload)>;

    /// Set the callback for broker-initiated notifications.
    /// Called from the broker thread — must be thread-safe.
    void on_notification(NotificationCallback cb);

    /// Set hub-dead callback (ZMQ monitor disconnect).
    void on_hub_dead(std::function<void()> cb);
};
```

### 2.7 Relationship to RoleAPIBase

RoleAPIBase owns a `BrokerRequestComm` instance (in Pimpl). It:

- Calls `connect()` during infrastructure setup
- Registers notification callback that routes to `core_.enqueue_message()`
- Registers hub-dead callback that sets StopReason::HubDead + request_stop
- Uses the thread manager to run the broker thread:
  `spawn_thread("broker", [&] { broker_channel_.run_poll_loop(); })`
- The poll loop handles periodic heartbeat + on_heartbeat internally
- On shutdown: `disconnect()` in teardown

RoleAPIBase API methods (`notify_channel`, `broadcast_channel`, `list_channels`,
`request_shm_info`, `wait_for_role`) forward directly to BrokerRequestComm.

---

## 3. Module 2: role_communication_channel

### 3.1 Purpose

Handles direct producer ↔ consumer communication for a channel. This is
the peer-to-peer data path for broadcast messages and control signals
between roles that have established a channel through the broker.

### 3.2 Transport

**Producer side** (binds):
- ROUTER socket (ctrl) — consumers connect to this
- XPUB socket (data, PubSub pattern) — consumers subscribe to this

**Consumer side** (connects, endpoints from DISC_ACK):
- DEALER socket (ctrl) — connects to producer's ROUTER
- SUB socket (data, PubSub) — connects to producer's XPUB

CurveZMQ encryption: producer generates a keypair per channel, registers
the pubkey with the broker. Consumer gets pubkey from DISC_ACK and generates
a client keypair for each connection.

### 3.3 What flows on these sockets

**Ctrl sockets (ROUTER ↔ DEALER):**

| Direction | Frame format | Purpose |
|-----------|-------------|---------|
| Consumer → Producer | HELLO frame | Consumer announces presence |
| Consumer → Producer | BYE frame | Consumer leaves |
| Consumer → Producer | Custom message | `api.send_to(producer, data)` |
| Producer → Consumer | Typed ctrl frame | `api.send_to(consumer, data)` |

**Data socket (XPUB → SUB):**

| Direction | Frame format | Purpose |
|-----------|-------------|---------|
| Producer → Consumer(s) | `['A'] [payload]` | `api.broadcast(data)` — one-to-many |

### 3.4 Threading

One dedicated thread per role (managed by RoleAPIBase's thread manager).
This thread:

**Producer side:**
1. Polls the ROUTER (ctrl) socket
2. Dispatches HELLO → `on_consumer_joined` callback
3. Dispatches BYE → `on_consumer_left` callback
4. Dispatches custom messages → `on_consumer_message` callback
5. Detects peer-dead (consumer timeout) → `on_peer_dead` callback
6. Drains outbound ctrl queue (typed ctrl frames to specific consumers)

**Consumer side:**
1. Polls both DEALER (ctrl) and SUB (data) sockets
2. Dispatches ctrl frames → `on_producer_message` callback
3. Dispatches data frames → `on_data` callback (→ core_.enqueue_message)
4. Detects peer-dead (producer timeout) → `on_peer_dead` callback
5. Drains outbound ctrl queue

**Processor**: has both a producer-side and consumer-side channel. Each has
its own thread, managed by the thread manager.

All callbacks route to `core_.enqueue_message()` (thread-safe).

### 3.5 API

```cpp
class RoleCommunicationChannel
{
public:
    struct ProducerConfig
    {
        std::string channel_name;
        std::string ctrl_endpoint;   // "tcp://127.0.0.1:0" (OS-assigned)
        std::string data_endpoint;   // "tcp://127.0.0.1:0" (OS-assigned)
        std::string pubkey;          // CurveZMQ server key
        std::string seckey;
        int peer_dead_timeout_ms{30000};
    };

    struct ConsumerConfig
    {
        std::string channel_name;
        std::string ctrl_endpoint;   // from DISC_ACK
        std::string data_endpoint;   // from DISC_ACK
        std::string server_pubkey;   // from DISC_ACK
        int peer_dead_timeout_ms{30000};
    };

    /// Create and bind producer-side sockets. Returns actual endpoints.
    struct BindResult { std::string ctrl_endpoint; std::string data_endpoint; };
    std::optional<BindResult> bind_producer(const ProducerConfig &cfg);

    /// Connect consumer-side sockets using endpoints from broker DISC_ACK.
    bool connect_consumer(const ConsumerConfig &cfg);

    // ── Send (thread-safe, enqueued) ─────────────────────────────────

    /// Broadcast data to all consumers (XPUB). Producer only.
    bool broadcast(const void *data, size_t size);

    /// Send to a specific peer (ctrl socket). Both sides.
    bool send_to(const std::string &identity, const void *data, size_t size);

    /// Send typed ctrl frame.
    bool send_typed_ctrl(const std::string &type,
                         const void *data, size_t size,
                         const std::string &identity = {});

    // ── Callbacks (called from comm thread) ──────────────────────────

    void on_consumer_joined(std::function<void(const std::string &identity)> cb);
    void on_consumer_left(std::function<void(const std::string &identity)> cb);
    void on_consumer_message(std::function<void(const std::string &identity,
                                                 std::span<const std::byte> data)> cb);
    void on_producer_message(std::function<void(std::string_view type,
                                                 std::span<const std::byte> data)> cb);
    void on_data(std::function<void(std::span<const std::byte> data)> cb);
    void on_peer_dead(std::function<void()> cb);

    // ── Lifecycle ────────────────────────────────────────────────────

    /// Start polling. Called by thread manager's thread body.
    void run_poll_loop();

    /// Signal shutdown. run_poll_loop() will return.
    void stop();

    /// Accessor for connected consumer identities.
    std::vector<std::string> connected_consumers() const;
};
```

### 3.6 Relationship to RoleAPIBase

RoleAPIBase owns one or two `RoleCommunicationChannel` instances:
- Producer: one (producer-side)
- Consumer: one (consumer-side)
- Processor: two (one consumer-side for input, one producer-side for output)

Thread manager spawns dedicated threads:
- `spawn_thread("comm_out", [&] { out_comm_.run_poll_loop(); })`
- `spawn_thread("comm_in", [&] { in_comm_.run_poll_loop(); })`

RoleAPIBase API methods (`broadcast`, `send`, `connected_consumers`) forward
to the appropriate channel instance.

### 3.7 Relationship to broker_request_comm

The broker provides discovery:
1. Producer calls `broker.register_channel(opts)` → gets confirmation
2. Consumer calls `broker.discover_channel(name, opts)` → gets endpoints + pubkey
3. Consumer uses endpoints to call `comm.connect_consumer(cfg)`

After connection, P2C communication is direct — no broker involvement.

---

## 4. Threading Summary

### Per role (new design):

| Thread | Owner | Purpose |
|--------|-------|---------|
| Data loop | RoleAPIBase (owner thread) | `run_data_loop()` — acquire, invoke, commit |
| Broker | Thread manager ("broker") | `BrokerRequestComm::run_poll_loop()` — broker protocol + heartbeat + on_heartbeat |
| Comm (out) | Thread manager ("comm_out") | `RoleCommunicationChannel::run_poll_loop()` — producer P2C sockets |
| Comm (in) | Thread manager ("comm_in") | `RoleCommunicationChannel::run_poll_loop()` — consumer P2C sockets |
| Queue | QueueReader/QueueWriter internal | ZmqQueue recv/send thread (if ZMQ transport) |

Producer: data_loop + broker + comm_out (+ queue if ZMQ) = 3-4 threads
Consumer: data_loop + broker + comm_in (+ queue if ZMQ) = 3-4 threads
Processor: data_loop + broker + comm_out + comm_in (+ queues) = 4-6 threads

Each thread has ONE purpose. No socket sharing across threads.

### vs old design:

| Thread | Old (embedded mode) | New |
|--------|--------------------|----|
| Messenger worker | Broker protocol + heartbeat timer | Eliminated |
| ctrl_thread_ | ZmqPollLoop: P2C sockets + heartbeat + on_heartbeat | Eliminated |
| broker thread | (new) | Broker protocol + heartbeat + on_heartbeat |
| comm thread(s) | (new) | P2C socket polling |
| Data loop | Same | Same |
| Queue | Same | Same |

### Why one thread per module?

Both `broker_request_comm` and `role_communication_channel` use ZMQ
sockets. ZMQ sockets are NOT thread-safe — a socket must be used from a
single thread only. Each module owns its socket(s) and must poll them from
its own dedicated thread. This is not a design choice — it's a ZMQ
constraint.

The broker thread additionally handles heartbeat timing and on_heartbeat
script callback because these are coupled to the broker protocol:
- Heartbeat is a broker message (HEARTBEAT_REQ)
- The interval must be enforced even when the data loop is blocked
- on_heartbeat fires at the same cadence

The comm thread(s) handle P2C socket events because:
- HELLO/BYE and peer-dead detection require active polling
- Broadcast receive (`on_data`) requires polling the SUB socket
- These events must be delivered promptly regardless of data loop state

---

## 5. Shared Abstractions

### 5.1 ZmqPollLoop — redesigned with inproc wake-up

The current `ZmqPollLoop` has three problems:
1. Hardcoded 5ms poll interval (wastes CPU for low-frequency threads)
2. No wake-up — enqueued commands wait up to 5ms for no reason
3. HeartbeatTracker gates on iteration count (wrong for broker thread)

**Redesign** — the poll loop adds an inproc PAIR socket for immediate wake-up:

```
MonitoredQueue::push(cmd):
    lock, push to queue
    signal_socket.send(1 byte)      ← wake the poll loop immediately

ZmqPollLoop::run():
    poll_items = [dealer_socket, signal_socket, ...]
    while running:
        zmq_poll(items, next_timer_deadline_ms)
        if signal_socket readable:
            recv and discard           ← just a wake-up signal
            drain command queue → send on owned socket(s)
        for each data socket with POLLIN:
            dispatch callback
        for each periodic task:
            task.tick_time()           ← time-based, not iteration-based
```

Benefits:
- **Immediate wake-up** when commands enqueued (zero latency vs 5ms)
- **Poll timeout = next timer deadline** (no wasted CPU)
- **Time-based periodic tasks** (heartbeat fires on schedule regardless
  of data loop state)

`MonitoredQueue<T>` gains a `set_signal_socket(void *socket)` method.
When set, `push()` sends a 1-byte signal after enqueuing. The poll loop
includes this socket in its pollitem array.

### 5.2 HeartbeatTracker — dual mode (iteration-gated + time-based)

The existing HeartbeatTracker gates heartbeat on iteration count:

```cpp
void tick(uint64_t current_iteration)
{
    if (current_iteration == last_iter) return;  // no progress → no heartbeat
    last_iter = current_iteration;
    if (now - last_sent >= interval) { action(); last_sent = now; }
}
```

**Why iteration-gating matters**: If a script enters a dead loop inside
a callback (e.g., infinite loop in `on_produce`), the data loop stalls —
`iteration_count` never advances. The heartbeat stops. The broker detects
the timeout and declares the channel dead. This is the correct behavior:
a stuck script IS a dead role from the broker's perspective.

**Bug fix history** (2026-03-02): Idle loops (consumer waiting for data,
processor waiting for input) must still advance `iteration_count` even when
no data arrives — otherwise idle roles are falsely declared dead. The fix:
`core.inc_iteration_count()` is called every cycle in the shared data loop
frame (Step F), regardless of whether data was acquired.

**For the new design**, the broker thread needs access to `iteration_count`
from `RoleHostCore` (already available via `core->iteration_count()`).
The HeartbeatTracker adds a time-only mode for future periodic tasks that
should fire regardless of loop progress:

```cpp
struct PeriodicTask
{
    std::function<void()>                 action;
    std::chrono::milliseconds             interval;
    std::chrono::steady_clock::time_point last_fired;

    // Optional iteration gate — set to nullptr for time-only mode.
    std::function<uint64_t()>             get_iteration;
    uint64_t                              last_iter{0};

    void tick()
    {
        if (get_iteration)
        {
            uint64_t iter = get_iteration();
            if (iter == last_iter) return;   // no progress → skip
            last_iter = iter;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_fired >= interval)
        {
            action();
            last_fired = now;
        }
    }
};
```

The broker thread's heartbeat task uses iteration-gated mode
(`get_iteration = [core] { return core->iteration_count(); }`).
The metrics report task uses time-only mode (`get_iteration = nullptr`).

### 5.3 MonitoredQueue\<T\> — command queue for cross-thread enqueue

Already exists at `src/utils/hub/hub_monitored_queue.hpp`. Thread-safe
bounded queue with drop-oldest overflow and metrics. Used by both modules:

- `BrokerRequestComm`: API threads push broker commands → broker thread
  drains and sends on DEALER
- `RoleCommunicationChannel`: API threads push P2C messages → comm thread
  drains and sends on ROUTER/XPUB

Extended with `set_signal_socket()` for ZmqPollLoop wake-up integration.

### 5.4 Other reusable abstractions

| Abstraction | File | Used by |
|---|---|---|
| `get_zmq_context()` | `zmq_context.hpp` | Both modules — shared ZMQ context for socket creation |
| `crypto_utils` | `crypto_utils.hpp` | Both — CurveZMQ keypair generation |
| `net_address` | `net_address.hpp` | Both — endpoint validation |
| `RoleHostCore` | `role_host_core.hpp` | Both — shutdown state, message queue, iteration count |

---

## 6. Heartbeat Design — Iteration-Gated Liveness

### 6.1 The invariant

**Heartbeat fires only when the data loop makes progress.** If the script
is stuck, the role stops heartbeating, the broker detects the timeout, and
the channel is declared dead.

### 6.2 How it works

```
Data loop (owner thread):
    each cycle:
        acquire → invoke → commit → core.inc_iteration_count()

Broker thread:
    each poll cycle:
        heartbeat_task.tick():
            iter = core.iteration_count()
            if iter == last_iter: return       ← stuck → no heartbeat
            if now - last_sent >= interval:
                send HEARTBEAT_REQ(metrics)
                invoke on_heartbeat

Broker service:
    each poll cycle:
        check_heartbeat_timeouts():
            if now - last_heartbeat > channel_timeout:
                send CHANNEL_CLOSING_NOTIFY     ← role declared dead
```

### 6.3 Edge cases (already handled)

- **Idle consumer/processor** (no data arriving): The shared data loop
  frame calls `core.inc_iteration_count()` every cycle (Step F) even when
  `read_acquire` returns nullptr. Iteration advances on timeout cycles.
  Idle roles stay alive.

- **Script dead loop**: `invoke_produce/consume/process` never returns →
  the data loop stalls → `inc_iteration_count()` never called → heartbeat
  stops → broker timeout → CHANNEL_CLOSING_NOTIFY.

- **MaxRate with no data**: Same as idle — iteration advances each cycle.

### 6.4 Configuration

- `heartbeat_interval_ms`: Role config (default 5000ms). How often the
  role sends HEARTBEAT_REQ when making progress.
- `channel_timeout`: Broker config (default 30s). How long the broker waits
  before declaring a channel dead.

---

## 7. Current Implementation Status

### Already done (committed + pushed):
- Phase 1: `wire_event_callbacks()` — unified callback wiring
- Phase 2: `run_data_loop()` + `RoleCycleOps` — unified data loop frame
- `set_engine()` + `drain_inbox_sync()` on RoleAPIBase
- File migration: 5 files from pylabhub-scripting → pylabhub-utils

### Done (uncommitted, compiles, 1323/1323 pass):
- Thread manager: `spawn_thread()`, `join_all_threads()`, `thread_count()`
- `start_ctrl_thread(CtrlConfig)` — centralized in RoleAPIBase, uses
  current ZmqPollLoop (to be replaced by redesigned version)
- Old `run_ctrl_thread_()` removed from all 3 role hosts
- Role hosts use `api_->start_ctrl_thread()` / `api_->join_all_threads()`

### Next steps:
1. Commit the uncommitted thread manager + ctrl thread centralization
2. Redesign ZmqPollLoop (inproc wake-up, time-based PeriodicTask)
3. Implement `broker_request_comm` using redesigned ZmqPollLoop +
   MonitoredQueue
4. Replace `start_ctrl_thread()` internals with `broker_request_comm`
5. Implement `role_communication_channel`
6. Remove Messenger

---

## 8. Migration Strategy

### Phase A: Commit + redesign ZmqPollLoop

1. Commit the uncommitted thread manager + ctrl thread centralization
2. Redesign `ZmqPollLoop`: inproc wake-up, `PeriodicTask` (dual mode),
   generalized shutdown predicate
3. Extend `MonitoredQueue` with `set_signal_socket()` for wake-up
4. Update tests

### Phase B: broker_request_comm

1. Create `src/include/utils/broker_request_comm.hpp` +
   `src/utils/ipc/broker_request_comm.cpp`
2. DEALER socket + MonitoredQueue + redesigned ZmqPollLoop
3. Implement all broker protocol messages (§2.4)
4. Iteration-gated heartbeat (§6)
5. Hub-dead detection via ZMQ socket monitor
6. Wire into RoleAPIBase: replace `set_messenger()` and
   `start_ctrl_thread()` with `BrokerRequestComm`
7. Update role hosts

### Phase C: role_communication_channel

1. Create `src/include/utils/role_communication_channel.hpp` +
   `src/utils/ipc/role_communication_channel.cpp`
2. P2C socket creation (ROUTER/XPUB for producer, DEALER/SUB for consumer)
3. MonitoredQueue for outbound P2C messages
4. Redesigned ZmqPollLoop for poll + dispatch
5. Wire into RoleAPIBase: spawn comm threads via thread manager
6. Update Producer/Consumer to use comm channel

### Phase D: Remove Messenger

1. Delete `src/utils/ipc/messenger.cpp`, `messenger_protocol.cpp`,
   `messenger_internal.hpp`, `channel_handle.cpp`, `channel_handle.hpp`
2. Delete `src/include/utils/messenger.hpp`
3. Update CMakeLists.txt
4. Remove all `Messenger &` parameters from Producer/Consumer
5. Remove `start_embedded()` / `start()` modes
6. Remove `handle_*_events_nowait()` methods
7. Update HEP-CORE-0007, HEP-CORE-0011, HEP-CORE-0018

---

## 9. ZMQ API Standard: cppzmq Everywhere

### 9.1 Decision

**cppzmq is the project standard for all ZMQ code.** No raw C API (`zmq.h`)
in new code. Existing raw C code migrates as its module is touched.

### 9.2 Rationale

- **Type safety**: `zmq::socket_t`, `zmq::socket_ref` instead of `void*`
- **RAII**: socket/context destruction automatic on scope exit
- **Exception-based errors**: `zmq::error_t` instead of `zmq_errno()` checks
- **Header-only**: zero build cost, already included
- **Modern C++ syntax**: consistent with C++20 codebase
- **Multipart helpers**: `zmq_addon.hpp` for send/recv_multipart

### 9.3 API mapping

| Operation | Standard (cppzmq) | Forbidden (raw C) |
|-----------|-------------------|-------------------|
| Socket creation | `zmq::socket_t(ctx, zmq::socket_type::X)` | `zmq_socket()` |
| Socket options | `socket.set(zmq::sockopt::X, val)` | `zmq_setsockopt()` |
| Send/recv | `socket.send(msg)` / `socket.recv(msg)` | `zmq_send()` / `zmq_recv()` |
| Multipart | `zmq::send_multipart()` / `zmq::recv_multipart()` | manual frame loops |
| Polling | `zmq::poll(items, timeout)` | `zmq_poll()` |
| Context | `get_zmq_context()` (shared singleton) | `zmq_ctx_new()` |
| Non-owning ref | `zmq::socket_ref(zmq::from_handle, h)` | raw `void*` |
| Error handling | `try/catch zmq::error_t` | `zmq_errno()` checks |

### 9.4 Current violations and migration plan

| File | Violation | When to fix |
|------|-----------|-------------|
| `hub_zmq_queue.cpp` | Raw C throughout, local context | Separate task (data transport sprint) |
| `hub_inbox_queue.cpp` | Raw C throughout, local context | Separate task (data transport sprint) |
| `broker_service.cpp` | Local `zmq::context_t` | Phase D (Messenger removal) |
| `zmq_poll_loop.hpp` | Was raw C, now cppzmq | This commit |
| `hub_monitored_queue.hpp` | ZMQ-free (callback pattern) | No change needed |

### 9.5 Socket lifetime management

- **Process-level** (ZMQ context): Lifecycle module via `GetZMQContextModule()`
- **Module-level** (channel sockets): `zmq::socket_t` RAII in owning class
  (`BrokerRequestComm`, `RoleCommunicationChannel`). Destructs when
  module destructs. No Lifecycle needed.
- **Reference passing** (poll loop): `zmq::socket_ref` (non-owning). The
  owning module holds `zmq::socket_t`; the poll loop holds `zmq::socket_ref`.

---

## 10. Resolved Questions

1. **Request-reply pattern**: Standard async command queue (MonitoredQueue)
   + callback dispatch from poll loop. Same pattern as Logger, existing
   Producer/Consumer ctrl queues. No promise/future needed.

2. **Processor dual-broker**: Two `BrokerRequestComm` instances, same
   as current two `Messenger` instances. Self-contained objects, no special
   design needed.

3. **Hub-dead detection**: ZMQ socket monitor (ZMQ_EVENT_DISCONNECTED) on
   the broker DEALER socket. Carried over from current Messenger design.

4. **Channel lifecycle notifications**: Dispatch directly to
   `core_.enqueue_message()` from broker thread. The comm channel doesn't
   need to know about broker lifecycle events.
