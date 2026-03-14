# pyLabHub C++ API — Custom Role Development Guide

> **Audience**: C++ developers building custom roles directly on `hub::Producer` and
> `hub::Consumer`, beyond the four standard binaries (`pylabhub-producer`,
> `pylabhub-consumer`, `pylabhub-processor`, `pylabhub-hubshell`).
>
> **Related docs**: `README_Deployment.md` (config-driven usage), `HEP-CORE-0017`
> (pipeline architecture), `HEP-CORE-0021` (ZMQ transport).

---

## 1. When to Go Below the Script-Host Layer

The four standard binaries cover the majority of use cases via Python scripts. Go
below that layer when you need:

- Zero-overhead tight loops with no Python GIL or interpreter startup cost
- SHM write handlers that must execute at hardware-clock cadence
- Direct integration of a third-party library (e.g. hardware SDK, codec, database)
  without wrapping it in a Python extension module
- Custom threading topologies not expressible in the script host model
- A watchdog process that observes a channel without consuming data

---

## 2. The Two Operation Modes

`hub::Producer` and `hub::Consumer` support two distinct modes. The choice
determines which threads are created and who owns the ZMQ sockets.

### 2.1 Standalone Mode

Call `start()` after configuration. The object launches its own threads and drives
all polling internally.

**Producer standalone threads:**

| Thread | Responsibilities |
|--------|-----------------|
| `peer_thread` | Polls ROUTER ctrl socket; drains ctrl send queue; fires `on_consumer_joined` / `on_consumer_left` / `on_consumer_message`; runs peer-dead check |
| `write_thread` | Queue mode: sleeps until a job is submitted via `push()`. Real-time mode: drives continuous loop calling the installed write handler. |

**Consumer standalone threads:**

| Thread | Responsibilities |
|--------|-----------------|
| `ctrl_thread` | Polls DEALER ctrl socket; drains ctrl send queue; fires `on_producer_message`; runs peer-dead check |
| `data_thread` | Polls SUB/PULL data socket; fires `on_zmq_data` for each arriving frame |
| `shm_thread` | Queue mode: sleeps until `pull()` is called. Real-time mode: drives continuous loop calling the installed read handler. |

**Lifecycle:**

```
create() / connect()        ← factory; no threads yet
  │
  ├── on_consumer_joined()  ─┐
  ├── on_peer_dead()         │  register callbacks BEFORE start()
  ├── on_channel_closing()  ─┘
  │
  start()                   ← threads launch HERE
  │
  [threads run...]
  │
  stop()                    ← sets stop flag, joins all threads
  close()                   ← deregisters from broker, frees sockets and SHM
```

**When to use**: Long-running services where the calling thread has other work to
do (e.g. main thread runs a UI or serves a REST API). `ManagedProducer` /
`ManagedConsumer` wrap this mode for `LifecycleGuard` integration.

### 2.2 Embedded Mode

Call `start_embedded()` instead of `start()`. This sets the `running` flag to
`true` but launches **no threads**. The caller's thread is responsible for driving
all ZMQ polling by calling the `handle_*_events_nowait()` methods in its own loop.

```
create() / connect()
  │
  ├── on_peer_dead()        ─┐  register callbacks BEFORE the poll loop starts
  ├── on_consumer_joined()  ─┘
  │
  start_embedded()          ← running=true; NO threads launched
  │
  [caller's poll loop]
      handle_peer_events_nowait()      ← Producer: drain queue + recv + peer-dead check
      handle_ctrl_events_nowait()      ← Consumer: drain queue + recv + peer-dead check
      handle_data_events_nowait()      ← Consumer: recv data frames
  │
  stop()                    ← clears running flag
  close()
```

The caller obtains the raw ZMQ socket handles for use in a `zmq_poll()` / `zmq::poll()`
call:

```cpp
// Producer
void *ctrl_h = producer->peer_ctrl_socket_handle();   // ROUTER socket

// Consumer
void *data_h = consumer->data_zmq_socket_handle();    // SUB/PULL socket
void *ctrl_h = consumer->ctrl_zmq_socket_handle();    // DEALER socket
```

These handles are valid ZMQ socket pointers castable to `void *zmq_pollitem_t::socket`.

**When to use**: When the caller already runs a ZMQ event loop (e.g. a hub process,
a multiplexed proxy, or a script host managing multiple channels). The four standard
binaries all use embedded mode internally.

### 2.3 Mode Comparison

| | Standalone | Embedded |
|---|---|---|
| Threads launched | Yes (2–3 per role) | None |
| Who drives ZMQ polling | Internal threads | Caller's thread |
| Socket ownership | Internal threads | Caller's thread |
| `handle_*_events_nowait()` | Do NOT call (races with threads) | Caller MUST call periodically |
| `peer_ctrl_socket_handle()` | Do NOT call (thread-owned) | Caller uses in `zmq::poll()` |
| SHM write/read (standalone) | Via `push()` / `synced_write()` / `set_write_handler()` | N/A |
| SHM read (standalone) | Via `pull()` / `set_read_handler()` | N/A |

---

## 3. Thread Safety Reference

### 3.1 What is safe to call from any thread

All public methods on `Producer` and `Consumer` are thread-safe **unless noted
otherwise**, including:

- `send()`, `send_to()`, `send_ctrl()` — thread-safe (lock-guarded internally)
- `push()`, `synced_write()` — thread-safe (enqueue is lock-guarded)
- `on_consumer_joined()`, `on_peer_dead()`, etc. — thread-safe if called before
  the poll loop starts (the race-free window; see §4)
- `is_running()`, `is_valid()`, `channel_name()` — always safe
- `ctrl_queue_dropped()`, `connected_consumers()` — always safe

### 3.2 What is NOT safe across threads

| Operation | Constraint |
|---|---|
| `handle_peer_events_nowait()` | Embedded mode only; single caller thread (socket not thread-safe) |
| `handle_ctrl_events_nowait()` | Embedded mode only; single caller thread |
| `peer_ctrl_socket_handle()` / `data_zmq_socket_handle()` | Embedded mode only; do not call while the thread that owns the socket is polling |
| `in_backpressure()` on `MonitoredQueue` | Drain-thread only |

### 3.3 Shared state between threads

In standalone mode, three values cross thread boundaries via atomics:

| Value | Writer | Reader | Mechanism |
|---|---|---|---|
| `running` | `start()` / `stop()` (caller) | All internal threads | `std::atomic<bool>` (acq_rel) |
| `write_stop` | `stop()` (caller) | `write_thread` / `shm_thread` | `std::atomic<bool>` |
| `stop_reason` (script host) | Callback thread (peer/hub dead) | Loop thread (checks shutdown) | `std::atomic<int>` |

`peer_ever_seen_` and `last_peer_recv_` are plain (non-atomic) fields accessed
**only** from the thread running `handle_peer_events_nowait()` (embedded) or
`run_peer_thread()` (standalone) — never from any other thread.

---

## 4. Callback Registration Ordering

All `on_*` callbacks registered on `Producer` or `Consumer` are called from the
thread that drives polling (`peer_thread` / `ctrl_thread` in standalone mode, or
the caller's ZMQ thread in embedded mode).

Two protection tiers exist internally:

- **Messenger-thread callbacks** (`on_channel_closing`, `on_force_shutdown`,
  `on_consumer_died`, `on_channel_error`, `on_peer_dead`): stored in the impl
  under `callbacks_mu`. The invocation path copies the callback under `callbacks_mu`
  before calling it, so concurrent registration and invocation are race-free.
- **Poll-thread-only callbacks** (`on_consumer_joined`, `on_consumer_left`,
  `on_consumer_message`, `on_zmq_data`, `on_producer_message`): stored in the impl
  without mutex protection. They are only read from the single polling thread.

**Rule**: Register all callbacks **before** calling `start()` or `start_embedded()`.
Even for the mutex-protected tier, this is the safe window. For poll-thread-only
callbacks, registering after start creates a data race.

```cpp
// CORRECT — register before start()
auto p = Producer::create(messenger, opts).value();
p.on_consumer_joined([](auto &id) { /* ... */ });
p.on_peer_dead([&stop] { stop.store(true); });
p.start();                   // ← peer_thread starts after all registrations

// WRONG — registration races with running peer_thread
p.start();
p.on_peer_dead([&stop] { stop.store(true); });  // data race!
```

---

## 5. Peer-Dead and Hub-Dead Detection

### 5.1 Peer-dead (producer↔consumer P2P silence)

The P2P ctrl socket carries HELLO, BYE, and custom ctrl messages directly between
producer and consumer (not via the broker). Any received P2P message resets the
`last_peer_recv_` timestamp. When the gap since the last received message exceeds
`peer_dead_timeout_ms`, the `on_peer_dead` callback fires once.

**Detection point**: inside `handle_peer_events_nowait()` (embedded) and
`run_peer_thread()` (standalone) for Producer; inside `handle_ctrl_events_nowait()`
(embedded) and `run_ctrl_thread()` (standalone) for Consumer.

Detection is re-armed when the next P2P message arrives (`peer_ever_seen_` is reset
to `false` after firing, then set back to `true` on the next HELLO/BYE).

```cpp
opts.peer_dead_timeout_ms = 30000;  // 30 s
auto p = Producer::create(messenger, opts).value();

std::atomic<bool> shutdown{false};
p.on_peer_dead([&shutdown]() {
    LOGGER_WARN("Producer: peer declared dead — shutting down");
    shutdown.store(true, std::memory_order_release);
});
p.start_embedded();
// ... poll loop checks shutdown flag each iteration
```

### 5.2 Hub-dead (broker TCP silence)

`Messenger` sets ZMQ TCP heartbeat options (`ZMQ_HEARTBEAT_IVL = 5000 ms`,
`ZMQ_HEARTBEAT_TIMEOUT = 30000 ms`) on the broker DEALER socket. Additionally,
`Messenger::on_hub_dead(callback, timeout_ms)` fires the callback when no broker
message has been received within `timeout_ms`. The check runs inside the Messenger
worker thread.

```cpp
messenger.on_hub_dead([&shutdown]() {
    LOGGER_WARN("Hub declared dead — shutting down");
    shutdown.store(true, std::memory_order_release);
}, 60000 /* ms */);
```

Note: True hub-dead recovery (reconnect, re-register) is deferred to HEP-CORE-0023.
Currently hub-dead causes the role to shut down.

### 5.3 Ctrl queue monitoring

The `MonitoredQueue<T>` backing the ctrl send queue has two modes:

- `fire_and_forget = true` (default): ZMQ always accepts sends regardless of peer
  liveness; messages silently drop for dead peers. Queue depth does not accumulate.
  Monitoring callbacks are **never invoked**. Drop count is tracked for metrics.
- `fire_and_forget = false`: For blocking/semi-blocking transports where a stagnant
  queue indicates backpressure. `on_warn` / `on_dead` / `on_cleared` callbacks fire.

The ctrl queue in `hub::Producer` and `hub::Consumer` always uses
`fire_and_forget = true`. Custom queues built with `MonitoredQueue<T>` may use
either mode.

---

## 6. The Data-Plane Modes (Orthogonal to Standalone/Embedded)

The SHM data-plane has its own mode selection, independent of the control-plane
operation mode above.

### 6.1 Producer SHM modes

**Queue mode** (default): the write_thread sleeps until a job is submitted.

```cpp
// Async — returns immediately; write_thread executes job when a slot is available.
producer.push<FZ, DB>([](WriteProcessorContext<FZ, DB> &ctx) {
    ctx.txn.slot().value = 42;
    ctx.txn.publish();
});

// Sync — blocks until job completes.
producer.synced_write<FZ, DB>([](WriteProcessorContext<FZ, DB> &ctx) {
    ctx.txn.slot().value = compute();
    ctx.txn.publish();
});
```

**Real-time mode**: write_thread drives a continuous loop at the slot cadence.

```cpp
producer.set_write_handler<FZ, DB>([](WriteProcessorContext<FZ, DB> &ctx) {
    if (ctx.is_stopping()) return;
    for (auto &slot : ctx.txn.slots(50 /* ms timeout */)) {
        slot.value = read_hardware_register();
    }
});
// Remove handler to return to Queue mode:
producer.set_write_handler<FZ, DB>(nullptr);
```

### 6.2 Consumer SHM modes

**Queue mode** (default): caller acquires slots directly from its own thread.

```cpp
// Blocks until a new slot is available, then calls the job.
consumer.pull<FZ, DB>([](ReadProcessorContext<FZ, DB> &ctx) {
    process(ctx.txn.slot());
});
```

**Real-time mode**: shm_thread drives a continuous loop.

```cpp
consumer.set_read_handler<FZ, DB>([](ReadProcessorContext<FZ, DB> &ctx) {
    if (ctx.is_stopping()) return;
    for (auto &slot : ctx.txn.slots(50)) {
        archive(slot);
    }
});
```

---

## 7. Example: Custom C++ Data Recorder (Consumer, Standalone)

Records every slot from a SHM channel to disk. No Python. Uses standalone mode
with a real-time read handler so the shm_thread drives the recording loop.

```cpp
// custom_recorder.cpp
#include <plh_datahub.hpp>
#include <fstream>
#include <atomic>

struct SensorSlot { uint64_t ts_ns; float temperature; float pressure; };

int main()
{
    // ── Lifecycle + Messenger ──────────────────────────────────────────────
    pylabhub::utils::LifecycleGuard lifecycle;
    hub::Messenger messenger;
    messenger.connect("tcp://127.0.0.1:5570", /*pubkey=*/"", /*client keys=*/"","");

    // ── ConsumerOptions ────────────────────────────────────────────────────
    hub::ConsumerOptions opts;
    opts.channel_name       = "lab.sensors.raw";
    opts.shm_shared_secret  = 0xDEADBEEF00000001ULL;
    opts.consumer_uid       = "CONS-RECORDER-00000001";
    opts.consumer_name      = "DataRecorder";
    opts.peer_dead_timeout_ms = 60000;

    // ── Create Consumer ────────────────────────────────────────────────────
    auto consumer = hub::Consumer::connect<void, SensorSlot>(messenger, opts);
    if (!consumer) { LOGGER_ERROR("Failed to connect"); return 1; }

    // ── Register callbacks BEFORE start() ─────────────────────────────────
    std::atomic<bool> shutdown{false};
    std::ofstream out("recording.bin", std::ios::binary);

    consumer->on_peer_dead([&]() {
        LOGGER_WARN("[recorder] producer silent — stopping.");
        shutdown.store(true, std::memory_order_release);
    });
    consumer->on_channel_closing([&]() {
        LOGGER_INFO("[recorder] channel closing — stopping.");
        shutdown.store(true, std::memory_order_release);
    });

    // ── Install real-time read handler ────────────────────────────────────
    consumer->set_read_handler<void, SensorSlot>(
        [&out, &shutdown](hub::ReadProcessorContext<void, SensorSlot> &ctx) {
            if (ctx.is_stopping() || shutdown.load(std::memory_order_acquire))
                return;
            for (auto &slot : ctx.txn.slots(50)) {
                out.write(reinterpret_cast<const char *>(&slot), sizeof(slot));
            }
        });

    // ── Start (launches ctrl_thread, data_thread, shm_thread) ─────────────
    if (!consumer->start()) { LOGGER_ERROR("start() failed"); return 1; }

    // ── Wait for shutdown signal ───────────────────────────────────────────
    while (!shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    consumer->stop();
    consumer->close();
    return 0;
}
```

---

## 8. Example: Custom C++ Producer with Real-Time Hardware Loop (Standalone)

Reads from a hardware SDK in a tight loop, writes to SHM. Uses standalone Real-time
mode. The write_thread is the hardware acquisition loop.

```cpp
// custom_hw_producer.cpp
#include <plh_datahub.hpp>
#include <atomic>

struct SensorSlot { uint64_t ts_ns; float ch[8]; };

int main()
{
    pylabhub::utils::LifecycleGuard lifecycle;
    hub::Messenger messenger;
    messenger.connect("tcp://127.0.0.1:5570", "", "", "");

    hub::ProducerOptions opts;
    opts.channel_name          = "lab.hw.adc8ch";
    opts.has_shm               = true;
    opts.shm_config.slot_count = 64;
    opts.shm_config.shared_secret = 0xABCD1234ABCD1234ULL;
    opts.peer_dead_timeout_ms  = 0;  // no peer-dead; hardware runs regardless

    auto producer = hub::Producer::create<void, SensorSlot>(messenger, opts);
    if (!producer) return 1;

    std::atomic<bool> shutdown{false};
    producer->on_channel_closing([&]() {
        shutdown.store(true, std::memory_order_release);
    });

    // ── Real-time write handler: called by write_thread on each slot ───────
    producer->set_write_handler<void, SensorSlot>(
        [&shutdown](hub::WriteProcessorContext<void, SensorSlot> &ctx) {
            if (ctx.is_stopping() || shutdown.load(std::memory_order_acquire))
                return;
            for (auto &slot : ctx.txn.slots(50)) {
                slot.ts_ns = hw_timestamp_ns();
                hw_read_channels(slot.ch);   // blocking SDK call
                ctx.txn.publish();
            }
        });

    producer->start();   // launches peer_thread + write_thread

    // Main thread: wait for shutdown
    while (!shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    producer->stop();
    producer->close();
}
```

---

## 9. Example: Custom Embedded-Mode Role (Script-Host Pattern)

This is the pattern used internally by all four standard binaries. Useful when
building a custom multiplexed role that manages multiple channels in one ZMQ poll
loop.

```cpp
// custom_embedded_role.cpp  — manages one producer and one consumer in one poll loop
#include <plh_datahub.hpp>
#include <zmq.hpp>
#include <atomic>

int main()
{
    pylabhub::utils::LifecycleGuard lifecycle;
    hub::Messenger in_messenger, out_messenger;
    in_messenger.connect("tcp://127.0.0.1:5570", "", "", "");
    out_messenger.connect("tcp://127.0.0.1:5571", "", "", "");

    // ── Create producer (output side) ─────────────────────────────────────
    hub::ProducerOptions po;
    po.channel_name      = "lab.b.processed";
    po.has_shm           = true;
    po.shm_config.slot_count = 8;
    po.shm_config.shared_secret = 0x4444444444444444ULL;
    po.peer_dead_timeout_ms = 30000;
    auto producer = hub::Producer::create(out_messenger, po);
    if (!producer) return 1;

    // ── Create consumer (input side) ──────────────────────────────────────
    hub::ConsumerOptions co;
    co.channel_name       = "lab.a.raw";
    co.shm_shared_secret  = 0x3333333333333333ULL;
    co.peer_dead_timeout_ms = 30000;
    auto consumer = hub::Consumer::connect(in_messenger, co);
    if (!consumer) return 1;

    // ── Register callbacks BEFORE start_embedded() ────────────────────────
    std::atomic<bool> shutdown{false};

    producer->on_peer_dead([&]() {
        LOGGER_WARN("[custom] output peer dead — stopping.");
        shutdown.store(true, std::memory_order_release);
    });
    consumer->on_peer_dead([&]() {
        LOGGER_WARN("[custom] input peer dead — stopping.");
        shutdown.store(true, std::memory_order_release);
    });
    consumer->on_channel_closing([&]() {
        shutdown.store(true, std::memory_order_release);
    });

    // ── Embedded mode: no threads launched ────────────────────────────────
    producer->start_embedded();
    consumer->start_embedded();

    // ── Build poll items from raw ZMQ socket handles ──────────────────────
    std::vector<zmq::pollitem_t> items = {
        {producer->peer_ctrl_socket_handle(), 0, ZMQ_POLLIN, 0},
        {consumer->ctrl_zmq_socket_handle(),  0, ZMQ_POLLIN, 0},
        {consumer->data_zmq_socket_handle(),  0, ZMQ_POLLIN, 0},
    };

    // ── Poll loop: this thread owns all sockets ───────────────────────────
    while (!shutdown.load(std::memory_order_acquire))
    {
        zmq::poll(items, std::chrono::milliseconds(50));

        producer->handle_peer_events_nowait();   // drain send queue + recv + peer-dead
        consumer->handle_ctrl_events_nowait();   // drain send queue + recv + peer-dead
        consumer->handle_data_events_nowait();   // recv data frames (SHM relay)

        // Custom data processing: pull from SHM, transform, write to SHM
        consumer->pull<void, InputSlot>([&](auto &ctx) {
            producer->synced_write<void, OutputSlot>([&](auto &wctx) {
                transform(ctx.txn.slot(), wctx.txn.slot());
                wctx.txn.publish();
            });
        });
    }

    producer->stop(); producer->close();
    consumer->stop(); consumer->close();
}
```

**Key invariant**: `handle_peer_events_nowait()`, `handle_ctrl_events_nowait()`,
`handle_data_events_nowait()`, `pull()`, and `synced_write()` are all called from
the **same thread** — the poll loop thread. ZMQ sockets are not thread-safe; this
single-thread ownership model guarantees correctness.

---

## 10. Example: ManagedProducer with LifecycleGuard (Standalone, Recommended for Services)

For long-running services that start and stop with the process, the
`ManagedProducer` / `ManagedConsumer` wrappers integrate with `LifecycleGuard` for
topological init/shutdown ordering.

```cpp
#include <plh_datahub.hpp>

hub::ProducerOptions opts;
opts.channel_name = "lab.sensors.raw";
opts.has_shm = true;
// ... configure ...

hub::Messenger messenger;
hub::ManagedProducer mp(messenger, opts);

// Register the module with LifecycleGuard BEFORE lifecycle.init()
pylabhub::utils::LifecycleGuard lifecycle({ mp.get_module_def() });
lifecycle.init();   // calls Producer::create() + start() in dependency order

hub::Producer &producer = mp.get();
// ... use producer ...

// lifecycle destructor calls stop() + close() in reverse order
```

`ManagedConsumer` mirrors this pattern for the consumer side.

---

## 11. Designing a Custom Role: Checklist

When building a custom role, work through these decisions in order:

1. **Which mode?**
   - Standalone: simple service, one channel, main thread can block or sleep.
   - Embedded: you already have a ZMQ poll loop, or you need to multiplex multiple
     channels in one thread (bridge, relay, multi-channel recorder).

2. **Data transport?**
   - SHM: same host, typed slots, low latency, spinlock-based.
   - ZMQ: cross-machine or cross-hub, schema-encoded, TCP.
   - Mixed: SHM input → ZMQ output (bridge pattern; see `share/py-demo-dual-processor-bridge/`).

3. **SHM data-plane mode?**
   - Real-time: hardware-paced write loop, or subscription that must not miss slots.
   - Queue (`pull` / `synced_write`): demand-driven; simpler to reason about.

4. **Shutdown signals?**
   - Register `on_channel_closing`, `on_force_shutdown` for broker-initiated shutdown.
   - Register `on_peer_dead` if silence of the peer is a fatal condition.
   - Register `messenger.on_hub_dead(cb, ms)` if hub connectivity is required.
   - Use a shared `std::atomic<bool> shutdown` visible to all callbacks and the poll loop.

5. **Callback registration ordering?**
   - All `on_*` callbacks: before `start()` or `start_embedded()`.
   - Messenger `on_hub_dead`: before the Messenger worker is processing (early in main).

6. **Thread ownership?**
   - Standalone: never call `handle_*_nowait()` or get socket handles; the internal
     threads own the sockets.
   - Embedded: one dedicated poll-loop thread owns all sockets exclusively. No other
     thread may call socket-touching methods.

---

## 12. Source File Reference

| File | Purpose |
|------|---------|
| `src/include/utils/hub_producer.hpp` | `Producer`, `ProducerOptions`, `WriteProcessorContext`, `ManagedProducer` |
| `src/include/utils/hub_consumer.hpp` | `Consumer`, `ConsumerOptions`, `ReadProcessorContext`, `ManagedConsumer` |
| `src/utils/hub/hub_producer.cpp` | `ProducerImpl`, standalone threads, embedded polling entry points |
| `src/utils/hub/hub_consumer.cpp` | `ConsumerImpl`, standalone threads, embedded polling entry points |
| `src/utils/hub/hub_monitored_queue.hpp` | `MonitoredQueue<T>` — bounded ctrl queue with optional backpressure monitoring |
| `src/utils/hub/hub_zmq_queue.hpp/cpp` | `ZmqQueue` — PUSH/PULL data transport |
| `src/utils/hub/hub_shm_queue.hpp/cpp` | `ShmQueue` — SHM data transport |
| `src/scripting/python_role_host_base.cpp` | Reference implementation of embedded-mode pattern |
| `src/producer/producer_script_host.cpp` | Reference: producer embedded mode with ZMQ poll loop |
| `src/consumer/consumer_script_host.cpp` | Reference: consumer embedded mode with ZMQ poll loop |
| `share/py-demo-dual-processor-bridge/` | End-to-end demo: cross-hub bridge using mixed SHM+ZMQ transport |
