# HEP-CORE-0010: Actor Thread Model Redesign and Unified Script Interface

| Property      | Value                                                           |
|---------------|-----------------------------------------------------------------|
| **HEP**       | `HEP-CORE-0010`                                                 |
| **Title**     | Actor Thread Model Redesign and Unified Script Interface        |
| **Status**    | Phase 1 complete (2026-02-24) — Phase 2 complete (2026-02-24) — Phase 3 complete (2026-02-24) — ActorScriptHost (2026-02-28) |
| **Created**   | 2026-02-24                                                      |
| **Area**      | Actor Framework (`pylabhub-actor`)                              |
| **Depends on**| HEP-CORE-0007 (DataHub Protocol), HEP-CORE-0008 (LoopPolicy), HEP-CORE-0011 (ScriptHost Framework) |
| **Supersedes**| HEP-CORE-0005 (partially, on threading and callback model)      |

---

## 0. Implementation Status

**Phase 1 complete (2026-02-24).** Unified script interface and GIL-race fix are live.
**Phase 2 complete (2026-02-24).** True 2-thread-per-role model; embedded-mode API on hub::Producer/Consumer.
**Phase 3 complete (2026-02-24).** Application-level heartbeat: `zmq_thread_` sends `HEARTBEAT_REQ` only when `iteration_count_` advances.
**ActorScriptHost complete (2026-02-28).** Dedicated interpreter thread via `PythonScriptHost`; `PyConfig.install_signal_handlers=0`; `main_thread_release_` GIL handoff. See §3.7 and HEP-CORE-0011.

### Phase 1 — complete

- `actor_dispatch_table.hpp` deleted; decorator API removed from `actor_module.cpp`
- `on_iteration(slot, flexzone, messages, api)` unified callback; `on_init(api)` / `on_stop(api)` by name
- `incoming_queue_` (mutex + condvar) routes ZMQ callbacks — GIL race eliminated
- `loop_trigger` config (`shm` / `messenger`); `run_loop_shm()` / `run_loop_messenger()` in both workers
- `api.set_critical_error()` latch; `api.trigger_write()` removed
- Script: `"script": {"module": "...", "path": "..."}` (object only; string throws)
- 66/66 Layer 4 actor unit tests pass

### Phase 2 — complete (2026-02-24)

True 2-thread-per-role: `zmq_thread_` + `loop_thread_` per role. 501/501 tests pass.

**Embedded-mode API added to `hub::Producer` and `hub::Consumer`:**

| Method | Purpose |
|--------|---------|
| `start_embedded()` | Sets `running=true` without launching peer/data/ctrl threads |
| `peer_ctrl_socket_handle()` | Returns raw ZMQ ROUTER socket handle for `zmq_poll` (Producer) |
| `handle_peer_events_nowait()` | Drains ctrl queue + non-blocking recv on ROUTER socket |
| `data_zmq_socket_handle()` | Returns raw ZMQ SUB socket handle (Consumer; nullptr for Bidir) |
| `ctrl_zmq_socket_handle()` | Returns raw ZMQ DEALER socket handle (Consumer) |
| `handle_data_events_nowait()` | Non-blocking recv on SUB data socket |
| `handle_ctrl_events_nowait()` | Drains ctrl queue + non-blocking recv on DEALER ctrl socket |

**Actor host changes:**
- `ProducerRoleWorker` and `ConsumerRoleWorker` each gain `zmq_thread_` + `iteration_count_`
- `zmq_thread_` launches **before** `call_on_init()` — mirrors old behaviour where peer/ctrl threads were running before on_init
- `stop()` guard updated: joins zmq_thread_ even when `running_=false` (handles api.stop() in on_init)
- `acquire_write_slot()` timeout: was hardcoded 100ms → now `interval_ms` (scheduled) or `5ms` (max-rate)
- `acquire_consume_slot()` timeout: was hardcoded 100ms → now `timeout_ms` (timed), `5ms` (max-rate), `5000ms` (indefinite)
- Consumer `timeout_ms > 0`: now fires `on_iteration(None, ...)` when no slot acquired within `timeout_ms` (was previously silent)

**Thread count per role (Phase 2):**
- Producer: `zmq_thread_` + `loop_thread_` = **2 threads** (+ Messenger's internal worker, shared process-wide)
- Consumer: `zmq_thread_` + `loop_thread_` = **2 threads** (+ Messenger's internal worker, shared process-wide)

### Phase 3 — complete (2026-02-24): application-level heartbeat via zmq_thread_

Previous heartbeats were sent by the **Messenger's internal worker thread** (`HEARTBEAT_REQ` at a fixed 2-second interval). This was a *connection-level* health signal — it proved the TCP socket was alive, but not that the application loop was progressing.

**Phase 3 design:** `zmq_thread_` takes over per-channel heartbeat responsibility for producer roles.

**Messenger API additions** (`src/include/utils/messenger.hpp`):
```cpp
/// Suppress or restore the periodic heartbeat for one channel (thread-safe, fire-and-forget).
/// When suppressed, the actor's zmq_thread_ is responsible for heartbeats.
void suppress_periodic_heartbeat(const std::string &channel, bool suppress = true) noexcept;

/// Enqueue an immediate HEARTBEAT_REQ for one channel (thread-safe, fire-and-forget).
/// Called by zmq_thread_ when iteration_count_ has advanced.
void enqueue_heartbeat(const std::string &channel) noexcept;
```

Both methods push a command to the Messenger worker's queue. The worker processes them in FIFO order; socket access remains single-threaded.

**Actor host changes** (`src/actor/actor_host.cpp`):

In `ProducerRoleWorker::start()`, after `start_embedded()`:
1. `messenger_.suppress_periodic_heartbeat(role_cfg_.channel)` — disables the 2s periodic timer for this channel
2. `messenger_.enqueue_heartbeat(role_cfg_.channel)` — sends one immediate heartbeat to keep the channel alive during `on_init`

In `ProducerRoleWorker::run_zmq_thread_()`:
- Computes `hb_interval`:
  - `heartbeat_interval_ms > 0` → use it
  - `heartbeat_interval_ms = 0`, `interval_ms > 0` → `10 × interval_ms`
  - otherwise → `2000 ms` (matches old Messenger default)
- After each `zmq_poll` cycle: if `iteration_count_` has advanced **and** `hb_interval` has elapsed → `messenger_.enqueue_heartbeat(channel)`
- Initialised to `now - hb_interval` so the **first** iteration advance fires a heartbeat immediately

**Consumer roles**: consumers do not own channels and do not send heartbeats. `ConsumerRoleWorker::run_zmq_thread_()` is unchanged (no heartbeat logic).

**Result**: A stalled producer loop (GIL deadlock, SHM full, Python exception consuming `on_iteration` budget) now stops heartbeats even if TCP is alive. The broker's existing heartbeat-timeout path (`CHANNEL_CLOSING_NOTIFY`) provides the application-level liveness enforcement without any broker protocol changes.

---

## 1. Motivation

### 1.1 Current Gaps in the Actor Layer

The current actor thread model was designed around Producer and Consumer objects
that have their own internal threading:

| Object | Internal threads | Actor-context status |
|--------|-----------------|----------------------|
| `hub::Producer` | `peer_thread` (HELLO/BYE), `write_thread` (slot writes) | `write_thread` is **idle** — actor drives writes itself |
| `hub::Consumer` | `data_thread` (ZMQ data), `ctrl_thread` (ZMQ ctrl), `shm_thread` (SHM in Queue mode) | `shm_thread` is **idle** — actor drives SHM reads itself |
| `ProducerRoleWorker` | `loop_thread_` | Bypasses `write_thread` entirely |
| `ConsumerRoleWorker` | `loop_thread_` | Bypasses `shm_thread` entirely |

This leaves 2-3 unnecessary sleeping threads per role. The ZMQ-facing threads
(Producer's `peer_thread`, Consumer's `data_thread`/`ctrl_thread`) are still active and
handle network I/O, but their callbacks race with the actor's own loop and require
extra synchronization.

Additional gaps:

1. **Fragmented script interface**: Separate decorator callbacks (`on_write`, `on_read`,
   `on_data`, `on_message`) make it impossible to write a script that naturally reacts
   to *either* a new SHM slot *or* an incoming ZMQ message in one unified iteration.

2. **No unified timing model**: ZMQ messages arrive asynchronously and may arrive
   during the SHM write path, causing latency spikes or dropped messages.

3. **script_path is a file path**: requires the script to be a single `.py` file,
   cannot accommodate Python packages with multiple modules.

4. **Broker connection not wired**: `role.broker` is parsed but `Messenger::connect()`
   is never called from actor workers (Phase B3 in MESSAGEHUB_TODO).

---

## 2. Goals

1. Reduce per-role thread count from 4-5 to exactly 2: **ZMQ thread** + **loop thread**.
2. Provide a single unified iteration callback: `on_iteration(slot, flexzone, messages, api)`.
3. Require Python module/package format for scripts (not single-file paths).
4. Wire `role.broker` so each role worker connects to its own broker endpoint.
5. Move heartbeat responsibility to the ZMQ thread (iteration-count–based).
6. Define a clean graceful-exit sequence that always releases SHM before broker BYE.
7. Support `loop_trigger = "shm"` (SHM-primary) or `loop_trigger = "messenger"` (ZMQ-primary).

### 2.1 Non-Goals

- LuaJIT integration (future).
- External Python process mode (future).
- Actor-level metrics aggregation across roles (future, HEP-CORE-0008 Pass 3+).
- Dynamic role reconfiguration at runtime.
- Priority-ordered outbound message queue (FIFO is sufficient for current use cases).

---

## 3. Thread Architecture

> **Implementation note**: §3.1–3.4 describe the full target architecture.
> The table below maps each feature to implementation phase:
>
> | Feature | Phase |
> |---------|-------|
> | `zmq_thread_` + `loop_thread_` (2 threads per role) | Phase 2 ✅ |
> | `start_embedded()` / `handle_*_events_nowait()` on hub::Producer/Consumer | Phase 2 ✅ |
> | `iteration_count_` (atomic, incremented by loop thread) | Phase 2 ✅ |
> | SHM acquire timeout: policy-derived (`interval_ms` / `timeout_ms`) | Phase 2 ✅ |
> | Consumer `timeout_ms > 0`: `on_iteration(None,...)` on slot miss | Phase 2 ✅ |
> | Heartbeat via `zmq_thread_` (iteration-count–based) | Phase 3 ✅ |
> | Messenger per-channel heartbeat disabled after zmq_thread_ takes over | Phase 3 ✅ |
> | `wake_send` / `wake_recv` inproc PAIR (sub-5ms outbound latency) | Phase 4 (deferred) |
> | `outbound_queue_` (loop-initiated sends, e.g. `api.broadcast`) | Phase 4 (deferred) |

### 3.1 Two Threads Per Role

Each role worker owns exactly two threads:

```
Role Worker
├── zmq_thread_   — peer/consumer network I/O; Phase 3: heartbeat, outbound queue
└── loop_thread_  — SHM slot acquisition, script execution, incoming message drain
```

The ZMQ thread absorbs the responsibilities of the former `peer_thread` (Producer),
`data_thread` and `ctrl_thread` (Consumer). The idle `write_thread` and `shm_thread`
are not launched — `start_embedded()` is used instead of `start()`.

### 3.2 ZMQ Thread

The ZMQ thread owns all ZMQ sockets for its role and runs a `zmq_poll` loop:

```
# Phase 2 (current implementation):
Loop:
  zmq_poll([peer_ctrl_sock], timeout = messenger_poll_ms)   # Producer
  # OR
  zmq_poll([data_sock, ctrl_sock], timeout = messenger_poll_ms)   # Consumer

  if peer_ctrl readable (Producer):
      producer->handle_peer_events_nowait()
        → drains ctrl_send_queue (outbound HELLO ACKs etc.)
        → non-blocking recv: HELLO → incoming_queue_ notify; BYE → incoming_queue_; other → incoming_queue_

  if data_sock readable (Consumer):
      consumer->handle_data_events_nowait()
        → non-blocking recv: ZMQ data frame → incoming_queue_ notify

  if ctrl_sock readable (Consumer):
      consumer->handle_ctrl_events_nowait()
        → drains ctrl_send_queue
        → non-blocking recv: ctrl frame → incoming_queue_ notify

  cur = iteration_count_.load(relaxed)
  if cur != last_iter:
      last_iter = cur
      if now() - last_heartbeat >= hb_interval:          # Phase 3 ✅ — throttled
          messenger_.enqueue_heartbeat(channel)
          last_heartbeat = now()

  check running_ flag — exit if false

# Phase 4 additions (deferred):
#   zmq_poll items expand to include: wake_recv (inproc PAIR)
#   if wake_recv readable: drain outbound_queue_ and send (sub-5ms outbound latency)
```

**Socket ownership**: the ZMQ thread is the **sole owner** of all ZMQ sockets for
its role. The loop thread never touches ZMQ sockets directly; it communicates via
`incoming_queue_` (messages in) and `incoming_cv_` (wake for messenger mode).

**Wake mechanism (Phase 4 — deferred)**: an inproc `PAIR` socket pair (`wake_send` / `wake_recv`).
When implemented, the loop thread writes 1 byte to `wake_send` at the end of each iteration
to signal the ZMQ thread that outbound messages may be waiting in `outbound_queue_`.
This would reduce outbound ZMQ send latency from up to `messenger_poll_ms` to sub-5ms.

**Current (Phase 3 complete)**: The ZMQ thread reads `iteration_count_` directly on each
`zmq_poll` timeout cycle (every `messenger_poll_ms`, default 5 ms). When `iteration_count_`
has advanced **and** `hb_interval` has elapsed, `messenger_.enqueue_heartbeat(channel)` is
called (fire-and-forget to the Messenger's internal worker thread).

### 3.3 Loop Thread (Main Script Execution)

The loop thread is the primary execution unit. It owns the SHM acquire/release path
and all Python interpreter calls.

**Iteration structure:**

```
Phase 1 — primary blocking call (determined by loop_trigger config):
  shm (default): acquire_slot(timeout = interval_ms)
      → slot may be non-null (new data) or null (timeout)
  messenger: incoming_cv_.wait_for(messenger_poll_ms)
      → no SHM slot; any slot arg to script will be None

Phase 2 — secondary non-blocking drain:
  drain incoming_queue_ up to N messages (non-blocking)

Build messages list for script call.

Call on_iteration(slot, flexzone, messages, api):
  slot:     ctypes struct view (or None on timeout / no-SHM)
  flexzone: ctypes struct (persistent, may be None if no schema)
  messages: list of (sender_str, data_bytes) tuples (may be empty)
  api:      persistent ActorRoleAPI object

if producer and return value is True: commit slot
if producer and return value is False/None: discard slot

release SHM slot if held

increment iteration_count_ (atomic, relaxed)
# Phase 4 (deferred): write 1 byte to wake_send for sub-5ms outbound latency
# Phase 3 ✅: ZMQ thread reads iteration_count_ each poll cycle for heartbeat
```

### 3.4 Shared State Between Threads

All cross-thread state is either:
- **Atomic**: no lock required for single reads/writes.
- **Mutex-protected**: explicit lock; document the owning thread.

| Variable | Type | Written by | Read by | Notes |
|----------|------|-----------|---------|-------|
| `iteration_count_` | `std::atomic<uint64_t>` | loop thread | ZMQ thread | Monotonically increasing; ZMQ checks for change |
| `running_` | `std::atomic<bool>` | both (init by host, cleared by either) | both | Exit flag; set false to begin graceful stop |
| `critical_error_` | `std::atomic<bool>` | loop thread (via api) | ZMQ thread, host | Latch; never cleared |
| `zmq_thread_healthy_` | `std::atomic<bool>` | ZMQ thread | host/supervisor | ZMQ thread sets true on start, false on error |
| `incoming_queue_` | bounded deque + mutex | ZMQ thread (push) | loop thread (pop) | `incoming_cv_` for loop-wait notification |
| `outbound_queue_` | deque + mutex | loop thread (push) | ZMQ thread (pop after wake) | FIFO (Phase 4 deferred) |
| `wake_send` / `wake_recv` | inproc PAIR | loop thread writes | ZMQ thread reads | 1-byte write per iteration (Phase 4 deferred) |
| `incoming_cv_` | `condition_variable` | ZMQ thread notifies | loop thread waits | Used only when `loop_trigger = "messenger"` |

### 3.5 Initialization Sequence and Thread Start Order

The following sequence ensures correctness with respect to broker ACK timing and HELLO delivery:

```
Main thread                            Messenger worker thread      Broker / Network
──────────────────────────────────────────────────────────────────────────────────

[Producer init]
messenger_.connect(broker_endpoint)  ──────────────────────────────> CONNECT

Producer::create(messenger_, opts)
  → messenger.create_channel(...)
    [blocks until future received]  ───> REG_REQ ────────────────> broker
                                         <── REG_ACK ─────────────  broker
                                         HEARTBEAT_REQ (immediate)→ broker (marks channel Ready)
  ← create() returns                    (Messenger now sends periodic HEARTBEAT_REQ)

create_from_parts(): SHM created; ROUTER ctrl socket BOUND

start_embedded()          [running_ ← true; NO threads launched yet]
[build flexzone view — GIL]
running_.store(true)
zmq_thread_ starts        ← launched BEFORE on_init to mirror old peer_thread timing
  → begins polling ROUTER ctrl socket
call_on_init()            [script on_init() runs; ZMQ sends from on_init are processed]
loop_thread_ starts

[Consumer init — identical pattern; key difference:]
Consumer::connect(messenger_, opts)
  → messenger.connect_channel(...)
    [blocks until future received]  ───> DISC_REQ ───────────────> broker
                                         <── DISC_ACK ────────────  broker (has producer info)
                                         CONSUMER_REG_REQ ────────> broker
                                         <── CONSUMER_REG_ACK ───   broker
  ← connect() returns

connect_from_parts():
  HELLO sent directly on DEALER ctrl socket [running_=false → direct send, not queued]
    → buffered in Producer's ROUTER socket until Producer's zmq_thread_ polls it

start_embedded()          [running_ ← true; fn_send_ctrl now queues sends]
zmq_thread_ starts        ← polls SUB data + DEALER ctrl sockets
call_on_init()
loop_thread_ starts
```

**Key invariants:**
1. All broker ACKs (REG_ACK, CONSUMER_REG_ACK) complete inside `create()`/`connect()` **before** `start_embedded()` is called. No broker interaction is needed during thread startup.
2. Consumer HELLO is sent with `running_=false` (direct send path) in `connect_from_parts()`, before any threads exist. The HELLO is buffered in the Producer's ROUTER socket and delivered within the first `messenger_poll_ms` poll cycle of the Producer's `zmq_thread_`.
3. `zmq_thread_` launches **before** `call_on_init()`. This mirrors the old design where `peer_thread` / `ctrl_thread` were running before `on_init` was called.
4. HEARTBEAT_REQ responsibility is transferred to `zmq_thread_` in Phase 3 (complete). On `start()`, the Messenger's periodic heartbeat for this channel is suppressed (`suppress_periodic_heartbeat()`), and one immediate heartbeat is enqueued to keep the channel alive during `on_init`. Subsequent heartbeats are sent by `zmq_thread_` only when `iteration_count_` advances (see §0 Phase 3 for full details).

### 3.6 Thread Interaction Diagram (Phases 2 + 3)

```mermaid
sequenceDiagram
    participant LT as Loop Thread
    participant IQ as incoming_queue_
    participant ZT as ZMQ Thread
    participant PC as Producer/Consumer<br/>(embedded mode)
    participant NET as ZMQ Network

    Note over LT,ZT: start() — zmq_thread_ launches before call_on_init()
    Note over LT,ZT: Messenger periodic heartbeat suppressed (Phase 3); one immediate HB enqueued
    Note over LT: call_on_init() under GIL
    Note over LT,ZT: loop_thread_ launches after on_init completes

    loop Each SHM iteration (loop_trigger=shm)
        LT->>PC: acquire_slot(timeout — policy-derived)
        PC-->>LT: slot_handle or null
        LT->>IQ: drain_incoming_queue_()
        IQ-->>LT: messages[]
        Note over LT: acquire GIL
        LT->>LT: on_iteration(slot, fz, messages, api)
        Note over LT: release GIL
        LT->>PC: commit / release_slot
        LT-->>ZT: iteration_count_.fetch_add(1) [atomic relaxed]
    end

    loop ZMQ poll (messenger_poll_ms = 5 ms default)
        ZT->>PC: zmq_poll(peer/data/ctrl socket, messenger_poll_ms)
        NET-->>ZT: POLLIN event
        ZT->>PC: handle_*_events_nowait()
        PC-->>IQ: push IncomingMessage (mutex)
        IQ-->>LT: incoming_cv_.notify_one()
        ZT->>ZT: iteration_count_ advanced AND hb_interval elapsed?
        ZT->>NET: messenger_.enqueue_heartbeat() [producer only — Phase 3 ✅]
    end

    Note over LT,ZT: stop() — running_=false, cv.notify_all()
    LT->>LT: finish iteration; call_on_stop(api)
    ZT->>ZT: zmq_poll wakes (≤messenger_poll_ms timeout); exit loop
    Note over LT,ZT: stop() joins loop_thread_ then zmq_thread_
    Note over LT,ZT: producer_->stop(); producer_->close() — BYE + DEREG via Messenger
```

### 3.7 ActorScriptHost — Interpreter Thread Owner

`ActorScriptHost` (`src/actor/actor_script_host.hpp/.cpp`) is the actor's concrete
`PythonScriptHost` subclass (see **HEP-CORE-0011** for the abstract `ScriptHost`
base class and `PythonScriptHost` threading model). It owns the CPython interpreter
lifetime and drives all role workers from a single dedicated interpreter thread.

#### 3.7.1 Class Responsibilities

| Responsibility | How |
|---|---|
| Own `py::scoped_interpreter` | Interpreter thread; `do_python_work()` holds it for full lifetime |
| Drive `ActorHost` lifecycle | `load_script()` → `start()` → wait → `stop()` inside `do_python_work()` |
| Propagate external shutdown | `signal_shutdown()` → `ActorHost::signal_shutdown()` |
| Report results to main thread | `script_load_ok_` / `has_active_roles_` set before `signal_ready_()` |
| Support diagnostic modes | `set_validate_only()`, `set_list_roles()` — exit early with report |

```cpp
class ActorScriptHost : public scripting::PythonScriptHost {
public:
    void set_config(ActorConfig config);
    void set_shutdown_flag(std::atomic<bool>* flag) noexcept;
    void set_validate_only(bool v) noexcept;
    void set_list_roles(bool v) noexcept;

    void startup_() noexcept;   // spawns interpreter thread; blocks until ready
    void shutdown_() noexcept;  // sets stop_, joins interpreter thread; idempotent
    void signal_shutdown() noexcept; // signal handler path → ActorHost::signal_shutdown()

    bool script_load_ok() const noexcept;    // available after startup_()
    bool has_active_roles() const noexcept;  // available after startup_()

protected:
    void do_python_work(const std::filesystem::path& script_path) override;
};
```

#### 3.7.2 Interpreter Thread Lifecycle

```
actor_main.cpp:                          Interpreter Thread (spawned by PythonScriptHost):
  actor_script.set_config(config);
  actor_script.set_shutdown_flag(&g_shutdown);
  actor_script.startup_();            ─── spawns thread ──────────────────────────────────>
                                                           Py_Initialize (py::scoped_interpreter)
                                                           PyConfig: install_signal_handlers=0 ①
                                                           PYTHONHOME from exe/../opt/python
                                                           ActorHost::load_script() [GIL held]
                                                           ActorHost::start()
                                                             emplace main_thread_release_ ② → GIL released
                                                             role worker loop_threads launched
                                                           signal_ready_()  ③
  <── startup_() returns ─────────────── unblocked by ③
  check script_load_ok() / has_active_roles()
  wait loop on g_shutdown ────────────────────────────────> wait (stop_ || g_shutdown)
                                                           [loop_threads run; GIL acquired per on_iteration]
  SIGINT / api.stop() ─────────────────> g_shutdown=true  signal_shutdown() ④
  actor_script.shutdown_() ──────────── stop_=true         ActorHost::stop():
                                          wakes wait          reset main_thread_release_ ⑤ → GIL re-acquired
                                                              join loop_threads (py::gil_scoped_release ⑥)
                                                              call_on_stop() for each role
                                                           Py_Finalize (py::scoped_interpreter destructor)
  <── shutdown_() returns (join)
```

**Annotations:**
- ① `PyConfig.install_signal_handlers = 0` — preserves C++ SIGINT/SIGTERM handlers;
  Python's default handler would intercept `SIGINT` before the C++ handler.
- ② `main_thread_release_.emplace()` — `std::optional<py::gil_scoped_release>` member in
  `ActorHost`; releasing the GIL here allows role `loop_thread_`s to acquire it.
- ③ `signal_ready_()` — `PythonScriptHost` base sets `ready_ = true` and notifies
  `init_promise_`; `startup_()` on the main thread unblocks from `init_future_.get()`.
- ④ `signal_shutdown()` — called from the C++ signal handler (`SIGINT`/`SIGTERM`) or
  from `ActorRoleAPI::stop()` inside a Python script.
- ⑤ `reset()` on `main_thread_release_` — re-acquires the GIL before `stop()` joins
  the role workers; the GIL must be held to call Python callbacks in `on_stop`.
- ⑥ `py::gil_scoped_release` around each `join()` — the `loop_thread_` may still hold
  the GIL during its current `on_iteration`; the interpreter thread must not hold it
  while waiting for the join to avoid deadlock.

#### 3.7.3 Signal Handler Safety

`PyConfig.install_signal_handlers = 0` is set inside `PythonScriptHost::do_initialize()`
before `Py_Initialize`. This is required because:

1. CPython installs a `SIGINT` handler that raises `KeyboardInterrupt` — this would
   prevent the C++ SIGINT handler (`g_shutdown_ = true`) from running.
2. The actor uses `std::signal(SIGINT, ...)` in `actor_main.cpp` to set the shutdown
   flag. If Python's handler fires first, the C++ handler never runs.
3. Python scripts that need `KeyboardInterrupt` can register their own handler via
   `signal.signal()` after initialization — the default `SIG_DFL` is preserved.

---

## 4. Script Interface

> **Authoritative reference: HEP-CORE-0014** (Actor Framework Design) §3 covers the
> complete Python script interface: module format, callback signatures (`on_init`,
> `on_iteration`, `on_stop`), `ActorRoleAPI` methods, `loop_trigger` strategy,
> `LoopTimingPolicy`, `RoleMetrics`, and `SharedSpinLockPy`.
>
> This HEP (§3) owns the threading model: how the loop thread acquires the GIL and calls
> Python callbacks, how `zmq_thread_` routes events to `incoming_queue_`, and how the
> 2-thread-per-role model works internally. HEP-CORE-0014 §3 owns the developer-facing
> API contract.
---

## 5. Message Model

### 5.1 Message Classification

| Source | Handling | Queued? | Notes |
|--------|----------|---------|-------|
| Broker | Inline in ZMQ thread → atomic flags | No | Channel ACK/NACK, config push |
| HELLO / BYE (peer) | Inline in ZMQ thread → atomic map | No | Updates `connected_peers_` map |
| Peer user data | Bounded queue, drop on overflow | Yes | Delivered to script via `messages` list |
| Peer user ctrl | Bounded queue, drop on overflow | Yes | Delivered to script via `messages` list |
| Shutdown signal | Atomic flag only | No | External SIGINT/SIGTERM |
| `api.set_critical_error()` | Atomic flag → exit loop | No | Script-initiated |

Broker messages and HELLO/BYE frames are **never queued** — they are handled inline
by the ZMQ thread without involving the script or the incoming_queue_.

### 5.2 Incoming Queue

- **Type**: `std::deque<std::pair<std::string, std::vector<std::byte>>>`
- **Bound**: configurable (default 256 messages per role)
- **Overflow policy**: drop oldest with a `LOGGER_WARN` log entry; no back-pressure
- **Lock**: `std::mutex incoming_mu_`
- **Notification**: `std::condition_variable incoming_cv_` (used by loop thread
  when `loop_trigger = "messenger"`)

The loop thread drains the queue in **Phase 2** of each iteration (non-blocking).
In `loop_trigger = "messenger"` mode, Phase 1 blocks on `incoming_cv_.wait_for()`
which is also notified when a new message arrives.

### 5.3 Outbound Queue

- **Type**: `std::deque<OutboundMessage>` where `OutboundMessage = {identity, data, is_ctrl}`
- **Lock**: `std::mutex outbound_mu_`
- **Drain**: ZMQ thread drains the queue whenever it receives a wake signal from loop thread
- **Order**: strict FIFO

The loop thread enqueues outbound messages (via `api.broadcast()`, `api.send()`,
`api.send_ctrl()`) and the ZMQ thread sends them.

---

## 6. Broker Integration

### 6.1 Wiring `role.broker`

Currently `role.broker` is parsed but never used for connection. In this design,
each role worker creates its own `hub::Messenger` (value member, not singleton)
and calls `messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey)` in `start()`.

```cpp
// In ProducerRoleWorker::start() — before ZMQ thread launch:
if (!role_cfg_.broker.empty())
{
    if (!messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey))
        LOGGER_WARN("[actor] Role '{}': broker connect failed; running degraded",
                    role_name_);
    // Graceful degradation: SHM channel still works without broker
}
```

`broker_pubkey` (Z85, 40 chars) enables CurveZMQ server-side verification.
Empty `broker_pubkey` = plain TCP (no CURVE auth).

### 6.2 Init Protocol

The init sequence is **synchronous and blocking** in the role worker's `start()` call:

```
1. ZMQ thread starts → broker connect (Messenger::connect)
2. create_channel / discover_producer called (existing Messenger API)
   → Messenger's discover_producer() already retries on CHANNEL_NOT_READY
   → For consumer: broker ACK confirms SHM is ready before connect_channel() returns
3. Python interpreter prepared (if not already done by ActorHost)
4. Script module imported (synthetic name to avoid sys.modules collision)
5. on_init(api) called (if function exists)
   → If on_init raises: log exception, send BYE/DEREG, role stops → start() returns false
6. HELLO sent to producer (consumer roles only; producer does not send HELLO)
7. Loop thread starts
8. start() returns true
```

HELLO is sent **only after** `on_init` completes successfully. This ensures the
producer knows that the consumer is fully initialized before it sees data flowing.

### 6.3 Heartbeat

The ZMQ thread sends heartbeat frames to the **broker only** (not to peers). The
heartbeat mechanism is based on `iteration_count_` — a monotonically increasing
`std::atomic<uint64_t>` incremented by the loop thread at the end of each iteration.

```
Heartbeat condition: iteration_count_ changed since ZMQ thread's last check
Heartbeat interval: messenger_poll_ms polling naturally detects this at zmq_poll wakeup
Config check: warn if heartbeat_interval_ms < interval_ms × 10 (no benefit in faster heartbeats)
```

Heartbeat frame format (JSON):
```json
{
  "ts":    1740384000000,
  "actor": "ACTOR-Sensor-3F2A1B0E",
  "role":  "raw_out",
  "iter":  12345
}
```

If `iteration_count_` has **not changed** since the last ZMQ wakeup, no heartbeat
is sent — this signals stall to the broker (stale actor detection).

---

## 7. Graceful Exit Sequence

Exit is triggered by any of:
- `api.set_critical_error()` (script)
- External `SIGINT` / `SIGTERM` (host)
- ZMQ thread error (unrecoverable broker or peer socket failure)

Sequence:

```
1. running_ = false  (atomic store)
2. incoming_cv_.notify_all()  (wake loop thread if blocked in Phase 1)
3. Loop thread: complete current iteration (including on_iteration call if in progress)
4. Loop thread: release SHM slot if held (commit or discard)
5. Loop thread: call on_stop(api) if defined
6. Loop thread: write 1 byte to wake_send  (signal ZMQ thread)
7. Loop thread: exits
8. ZMQ thread: receives wake, checks running_ = false
9. ZMQ thread: sends BYE + DEREG to broker (DEREG is functionally equivalent to BYE)
10. ZMQ thread: closes all ZMQ sockets
11. ZMQ thread: exits
12. Role worker: joins both threads
13. Role worker: destroys Messenger
14. Role worker: signals ActorHost (decrement running role count)
```

**Key invariant**: SHM is **always released** (step 4) before broker BYE is sent
(step 9). This prevents the broker from closing the channel while a slot is still
held, which would cause `DRAINING` to stall.

The 100 ms `acquire_slot` timeout on shutdown is acceptable — if a slot acquisition
is in progress when `running_` is cleared, it will complete or time out within 100 ms
before the loop exits.

---

## 8. Configuration

> **Authoritative reference: HEP-CORE-0014** (Actor Framework Design) §2 covers the
> complete configuration reference: actor directory layout, `actor.json` field reference,
> per-role fields, `LoopTrigger` enum values, `validation` sub-block, and slot schema
> field types.
>
> Threading-relevant config fields (`loop_trigger`, `messenger_poll_ms`,
> `heartbeat_interval_ms`) are described in HEP-CORE-0014 §2.3; the threading behavior
> they control is specified in §3 of this HEP.
---

## 9. Removed / Replaced Items

| Old item | Replacement | Reason |
|----------|-------------|--------|
| `@actor.on_write("role")` decorator | `def on_iteration(...)` in module | Unified callback |
| `@actor.on_read("role")` decorator | `def on_iteration(...)` in module | Unified callback |
| `@actor.on_data("role")` decorator | `messages` param in `on_iteration` | Inline delivery |
| `@actor.on_message("role")` decorator | `messages` param in `on_iteration` | Inline delivery |
| `@actor.on_init("role")` decorator | `def on_init(api)` by name | Convention over decoration |
| `@actor.on_stop("role")` decorator | `def on_stop(api)` by name | Convention over decoration |
| `api.trigger_write()` | `loop_trigger` config + `incoming_cv_` | Config-driven trigger |
| `script_path` (file path) | `script_module` + `script_path` (dir) | Module format |
| Shared `Messenger` singleton in actors | Per-role owned `Messenger` value | Per-role broker |
| `GetLifecycleModule()` in `actor_main.cpp` | Removed from actor (stays for HubShell) | Not needed in actor |
| `hub::Producer::write_thread` (actor use) | Loop thread drives writes directly | Already bypass |
| `hub::Consumer::shm_thread` (actor use) | Loop thread drives reads directly | Already bypass |
| `hub::Producer::peer_thread` (actor use) | ZMQ thread | Consolidated |
| `hub::Consumer::data_thread` (actor use) | ZMQ thread | Consolidated |
| `hub::Consumer::ctrl_thread` (actor use) | ZMQ thread | Consolidated |

---

## 10. Files to Change

| File | Change summary |
|------|---------------|
| `src/actor/actor_config.hpp` | Add `LoopTrigger` enum; add `loop_trigger`, `messenger_poll_ms`, `heartbeat_interval_ms`, `script_module` fields to `RoleConfig`; deprecate top-level `script_path` in `ActorConfig` |
| `src/actor/actor_config.cpp` | Parse new fields; backward-compat for old `"script"` format; validation rules |
| `src/actor/actor_host.hpp` | Replace decorator-based constructor params with module lookup; add `zmq_thread_` and shared state members; per-role `Messenger` ownership |
| `src/actor/actor_host.cpp` | Implement ZMQ thread loop; wire new init protocol; new graceful exit; per-role Messenger connect |
| `src/actor/actor_api.hpp` | Remove `trigger_write()`, `trigger_fn_`; add `set_critical_error()` and `critical_error()` |
| `src/actor/actor_api.cpp` | Implement `set_critical_error()` |
| `src/actor/actor_module.cpp` | Remove decorator registrations (`on_write`, `on_read`, `on_data`, `on_message`); keep module bindings that remain valid (`ActorRoleAPI`, `SharedSpinLockPy`) |
| `src/actor/actor_main.cpp` | Remove `GetLifecycleModule()` and `Messenger::get_instance()` from setup |
| `docs/todo/MESSAGEHUB_TODO.md` | Close B3 (role.broker now wired) |
| `docs/tech_draft/ACTOR_DESIGN.md` | Update to reflect new thread model and script interface |

---

## 11. Risk Analysis and Conflict Detection

### 11.0 Calm-Herding-Koala Plan Already Complete

The plan in `docs/plans/calm-herding-koala.md` (per-role Messenger ownership, broker_pubkey,
removal of `GetLifecycleModule()` from actor_main.cpp) is **already fully implemented as of
2026-02-22** (MESSAGEHUB_TODO B3 closed). This HEP builds on that baseline. No re-work
of the Messenger ownership model is needed.

### 11.1 GIL Serialization

The Python GIL serializes all Python execution across the process. With multiple
roles, `on_iteration` calls from different roles cannot overlap. The loop threads
must acquire the GIL via `py::gil_scoped_acquire` before each Python call and
release it (via `py::gil_scoped_release`) during the blocking Phase 1 (SHM acquire).

**Risk**: if one role's `on_iteration` is slow (e.g. heavy NumPy computation),
other roles miss their deadlines.

**Mitigation**: documented as a known constraint; users are advised to minimize
Python work in time-critical roles. Future option: multi-process actor federation.

### 11.2 `hub::Producer` / `hub::Consumer` Internal Threads Still Running

The current actor code (actor_host.cpp) calls:
- `producer_->start()` — starts `peer_thread` (ROUTER ctrl polling) + `write_thread` (idle)
- `consumer_->start()` — starts `data_thread` (SUB polling) + `ctrl_thread` (DEALER polling) + `shm_thread` (idle)

These callbacks are wired **before** `start()` and fire from background threads directly into Python:
- `producer_->on_consumer_message(...)` → `peer_thread` acquires GIL and calls `py_on_message_()` directly
- `consumer_->on_zmq_data(...)` → `data_thread` acquires GIL and calls `py_on_data_()` directly

**Current GIL safety issue**: `py_on_message_` and `py_on_data_` are called from background
threads *concurrently* with the loop_thread calling `py_on_write_` / `py_on_read_`. The GIL
serializes execution, but there is **no ordering guarantee** — a message callback can preempt
a write/read callback mid-Python-call (GIL switches between Python bytecodes). This is a
latent correctness risk in the current design.

**New design fixes this** by routing all callbacks through `incoming_queue_`:
- ZMQ callbacks push to `incoming_queue_` without acquiring GIL or calling Python
- Only the loop thread calls Python, in a well-defined sequence

**Thread reduction conflict**: if the ZMQ thread also polls `peer_ctrl` / `peer_data` directly
(the `zmq_poll` approach in §3.2), it would conflict with `peer_thread` / `data_thread` /
`ctrl_thread` that are still running.

**Resolution (two viable approaches):**

**A. Callback-routing approach (pragmatic, Phase 1)**:
- Keep `producer_->start()` / `consumer_->start()` as-is
- Route callbacks to `incoming_queue_` instead of direct Python dispatch
- The loop thread drains `incoming_queue_` in Phase 2 of each iteration
- Heartbeat: add a lightweight heartbeat via `messenger_.send_heartbeat()` at the end of each loop iteration
- Thread count per role: peer_thread + data_thread + ctrl_thread + loop_thread = 4 (write_thread + shm_thread idle)
- Eliminates the GIL race and the direct-Python-from-thread model

**B. Full ZMQ-thread approach (clean, Phase 2)**:
- Do NOT call `producer_->start()` / `consumer_->start()`
- Add an "embeddable mode" API to Producer/Consumer: `get_poll_items() + handle_poll_events()`
- Actor's ZMQ thread does `zmq_poll` over all sockets (broker + peer)
- True 2-thread-per-role model

**Recommended**: implement Phase 1 first (delivers the unified script interface and fixes the GIL race), then Phase 2 (thread consolidation). Both phases are backward-compatible at the script level.

### 11.3 Messenger Singleton vs. Per-Role Messenger

The current `actor_main.cpp` does not call `GetLifecycleModule()` for Messenger, and
`hub::Messenger` is already a per-role owned value in `ProducerRoleWorker` and
`ConsumerRoleWorker` (as of the plan in `calm-herding-koala.md`). This aligns with
the new design.

**Note**: HubShell continues to use `Messenger::GetLifecycleModule()` as a singleton.
The actor never uses the singleton path.

### 11.4 `actor_module.cpp` Decorator Registry

The current `pylabhub_actor` Python module exposes `on_write`, `on_read`, `on_data`,
`on_message` as decorator-registration functions that populate a dispatch table.
These must be removed. The new lookup is `getattr(module_obj, "on_iteration", None)`.

The existing `ActorRoleAPI` bindings and `SharedSpinLockPy` bindings are preserved.

### 11.5 `trigger_write()` Removal

`api.trigger_write()` is currently used by scripts that operate in event-driven mode
(`interval_ms = -1`). This is replaced by `loop_trigger = "messenger"` — the loop
blocks on `incoming_cv_.wait_for()` which is notified when ZMQ messages arrive,
giving the same effect without an explicit API call.

Any existing scripts using `api.trigger_write()` must be updated when migrating
to the new API.

### 11.6 Consumer SHM Race During Init

The consumer's init depends on the producer having its SHM channel registered with
the broker. The existing `Messenger::discover_producer()` already retries on
`CHANNEL_NOT_READY`. No additional synchronization is needed — the blocking
`discover_producer()` call in step 2 of the init protocol (§6.2) handles this.

### 11.6b `exec_script_file` Helper Must Be Replaced

The current `load_script()` in `ActorHost` calls the private `exec_script_file()` helper
(line ~75-98 in actor_host.cpp) which:
1. Reads the file as a string
2. Uses `py::exec(code, globals)` to run it in a dict namespace
3. Wraps the result in a pseudo-module

This approach:
- Cannot find `on_iteration` / `on_init` / `on_stop` by name (they exist in the dict, not a proper module)
- Doesn't support Python packages (multi-file modules)
- Pollutes `sys.modules` (the wrapped pseudo-module is not added to `sys.modules`)

**Resolution**: Replace `exec_script_file` with proper module import:
```python
import importlib
import sys

# Add script_path to sys.path if not present
sys.path.insert(0, role_cfg.script_path)

# Use synthetic name to avoid sys.modules collision between roles
import importlib.util
spec = importlib.util.spec_from_file_location(synthetic_name, module_name)
module = importlib.util.module_from_spec(spec)
sys.modules[synthetic_name] = module
spec.loader.exec_module(module)
```

The C++ side uses `py::module_::import(synthetic_name)` after calling the above,
or does `getattr(module_obj, "on_iteration", py::none())` directly.

### 11.7 Heartbeat Interval Validation

Config warning: `heartbeat_interval_ms < interval_ms × 10`.

If `interval_ms = 0` (as-fast-as-possible), the ratio check is skipped.
If `interval_ms = -1` (event-driven), the ratio check is skipped.
`heartbeat_interval_ms = 0` (auto) → computed as `max(interval_ms × 10, 1000)`.

---

## 12. Test Plan

### 12.1 Existing Tests

The current 479 tests must continue to pass. No changes to:
- `tests/test_pylabhub_utils/` — hub API tests
- `tests/test_pylabhub_corelib/` — basic utility tests

### 12.2 Actor Layer Tests (`tests/test_layer4_actor/`)

Existing actor tests cover the decorator-based interface. These must be **replaced**
with tests for the new module-based interface:

| Test | Scenario |
|------|----------|
| `on_iteration_shm_trigger` | SHM-primary loop: producer writes, consumer reads, both via on_iteration |
| `on_iteration_messenger_trigger` | Messenger-primary loop: ZMQ message arrives → on_iteration called with messages |
| `on_iteration_timeout` | SHM acquire timeout: on_iteration called with slot=None |
| `on_init_failure` | on_init raises → role aborts, no HELLO sent |
| `critical_error_latch` | api.set_critical_error() in on_iteration → role stops after current iteration |
| `graceful_exit_shm_released` | SHM slot always released before BYE (even on error) |
| `incoming_queue_overflow` | Messages dropped on queue overflow, warning logged |
| `heartbeat_stall_detection` | iteration_count not advancing → no heartbeat to broker |
| `multi_role_gil_serialization` | Two roles in one actor: Python calls serialized |
| `loop_trigger_config_validation` | loop_trigger="shm" + has_shm=false → startup denied |

### 12.3 Smoke Test

```bash
# Build:
cmake --build build --target pylabhub-actor pylabhub-hubshell

# Run all tests:
ctest --test-dir build --output-on-failure -j$(nproc)

# Manual: actor with no broker (degraded mode):
#   Log should show "broker connect failed; running degraded"
#   Actor should continue running (SHM channel still works)

# Manual: actor with broker at role.broker:
#   Log should show successful channel registration
#   Heartbeat frames should appear in broker log
```

---

## 13. Migration Guide (Script Authors)

### Old decorator-based script:

```python
import pylabhub_actor as actor

@actor.on_init("raw_out")
def raw_out_init(flexzone, api):
    flexzone.device_id = 42
    api.update_flexzone_checksum()

@actor.on_write("raw_out")
def write_raw(slot, flexzone, api) -> bool:
    slot.ts = time.time()
    slot.value = read_sensor()
    return True

@actor.on_message("raw_out")
def raw_out_ctrl(sender: str, data: bytes, api):
    handle_command(data)
```

### New module-based script (`raw_out.py`):

```python
import time

def on_init(api):
    # api.flexzone only available if has_shm=true
    # ... initialization code (no flexzone param — access via api if needed)
    api.log("info", "raw_out initialized")

def on_iteration(slot, flexzone, messages, api):
    # Handle incoming ZMQ messages
    for sender, data in messages:
        handle_command(data)

    # Write SHM slot (slot is None on timeout)
    if slot is not None:
        slot.ts = time.time()
        slot.value = read_sensor()
        return True  # commit

    return False  # nothing to write this iteration

def on_stop(api):
    api.log("info", "raw_out stopping")
```

Config change:
```json
// Old:
{ "script": "raw_out.py" }

// New:
{ "script": { "module": "raw_out", "path": "/path/to/scripts" } }
```
