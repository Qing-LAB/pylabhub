# RAII Layer Redesign — Typed C++ Addon Layer

**Status**: Phase 1 ✅ done (timing unification); Phases 2-5 pending —
real, scheduled work tracked in `docs/todo/API_TODO.md` "Template RAII".
**Updated**: 2026-04-21 (post-role-unification context absorbed; scope
expanded to cover typed wrappers for inbox + band, not just slots).
**Scope**: Header-only, ABI-neutral C++ addon providing compile-time
typed wrappers over `RoleAPIBase` facilities — typed queue wrappers
(SlotIterator, TransactionContext), typed inbox client/handler, typed
band pub-sub, plus a `SimpleRoleHost` template that runs the standard
14-sub-step `worker_main_()` skeleton so C++ users write only their
business logic + per-cycle callable. Goal: parity with the script path
in ergonomics, parity with the manual data loop in timing + metrics,
zero ABI cost.
**Companion HEPs**: HEP-CORE-0002 §17.2 (queue abstraction), HEP-CORE-0008
(LoopPolicy + IterationMetrics), HEP-CORE-0009 §2.6 (policy reference),
HEP-CORE-0024 (role unification — provides `RoleAPIBase` and `RoleHostBase`).

---

## 1. Motivation

The current RAII layer (`SlotIterator`, `TransactionContext`, `SlotRef`)
is tied to `DataBlockProducer*` / `DataBlockConsumer*` directly. Beyond
that, the role framework has grown several typed facilities (inbox
messaging, band pub-sub, custom metrics) that the script path consumes
through `RoleAPIBase` but a C++ user has no first-class typed access
to. Four practical problems:

1. **No ZMQ transport support for the existing RAII path.** The current
   `SlotIterator` constructor takes `DataBlockProducer *` /
   `DataBlockConsumer *`, so it cannot consume from a `ZmqQueue` reader.
   The framework's queue abstraction (`QueueWriter` / `QueueReader` per
   HEP-CORE-0002 §17.2) is bypassed.
2. **Timing parity gap with the manual loop.** `SlotIterator::operator++()`
   implements only simple FixedRate (sleep-to-deadline). The manual data
   loop in `run_data_loop` (used by every role host) supports `MaxRate`,
   `FixedRate`, `FixedRateWithCompensation`, retry-acquire with deadline
   budget, short-timeout backoff, and overrun detection.
3. **No typed wrappers for inbox / band.** `RoleAPIBase` exposes
   `open_inbox_client(uid) → InboxHandle` returning opaque bytes;
   `band_broadcast(channel, json)` taking raw `nlohmann::json`. C++
   users must hand-marshal payloads. Should be `TypedInboxClient<MsgT>`
   / `TypedBand<EventT>` mirroring the typed slot pattern.
4. **No "managed lifecycle" entry point for C++ users.** A script user
   writes `on_produce(tx, msgs, api)` and the framework runs the
   14-sub-step `worker_main_()` skeleton for them. The C++ equivalent
   today requires writing a full `RoleHostBase` subclass (~360 LOC of
   boilerplate). C++ should have a `SimpleRoleHost<MySlot>` template
   that takes a per-cycle lambda + optional hooks and runs the same
   skeleton — symmetric ergonomics with the script path.

A C++ user who wants typed, zero-overhead access to the role framework
should get the same set of facilities as a script-side role:
- Slot iteration with timing parity → `TypedQueueReader/Writer` +
  `SlotIterator` + `TransactionContext` (§3, §5).
- Typed inbox messaging → `TypedInboxClient<MsgT>` + typed receive hook
  (§6.2).
- Typed band pub-sub → `TypedBand<EventT>` + typed receive hook (§6.3).
- Managed lifecycle → `SimpleRoleHost<SlotT>` (§2.3, §7 Phase 5).

All four pieces are header-only addons over the existing `RoleAPIBase`
+ queue ABI — no shared-library symbol changes.

---

## 2. Post-Unification Context (added 2026-04-21)

Since the original draft (2026-03-28), the role framework has consolidated:

- `RoleAPIBase` (`src/include/utils/role_api_base.hpp`) is the single role
  API class. It owns the role's queues via `build_tx_queue(opts)` /
  `build_rx_queue(opts)`. Producer / consumer / processor differ only in
  which side(s) they wire.
- `RoleHostBase` (`src/include/utils/role_host_base.hpp`) is the abstract
  host base; concrete hosts (`ProducerRoleHost`, etc.) override
  `worker_main_()`, the pure-virtual entry point that runs the role's
  full lifecycle on a worker thread (HEP-CORE-0024 §16.3 Step 3, with
  the 14-sub-step skeleton). Producer/consumer/processor `worker_main_()`
  bodies are >80% identical — the per-role variation is small and
  contained.
- Queues are abstract behind `QueueWriter` / `QueueReader`; concrete
  implementations are `ShmQueue` (owns DataBlock) and `ZmqQueue` (owns
  ZMQ sockets) — see HEP-CORE-0002 §17.2.

### 2.1 Layer choice — what the RAII path anchors on

The RAII layer can plausibly anchor at three levels in the role framework
stack. The choice determines what a C++ user has to write.

| Level | Anchor | What user writes | What user gets |
|---|---|---|---|
| **A** — RAII inside `worker_main_()` | `RoleAPIBase` | Their own `RoleHostBase` subclass with full ~360-LOC `worker_main_()`; uses `SlotIterator` only at sub-step 8 | Replaces `run_data_loop(api, core, lcfg, ops)` with `for (auto &slot : ctx.slots())` |
| **B** — RAII replaces `worker_main_()` boilerplate | `RoleHostBase` (extension) | Provides a per-cycle callable + optional `on_init`/`on_stop` hooks; framework provides default `worker_main_()` running the 14-sub-step skeleton + RAII data loop | C++ analog of the script path — write business logic, get managed lifecycle |
| **C** — RAII below the role framework | `QueueWriter` / `QueueReader` only | A bare `main()` that builds queues + RAII iterator from scratch; no broker registration, heartbeat, band, inbox, metrics aggregation | Useful only for one-off tools (e.g. CLI utilities that drain a channel without registering as a role) |

### 2.2 Decision: Level B as primary, Level A as escape hatch

**Primary target: Level B** — symmetric with the script path (script
user writes `on_produce(tx, msgs, api)`; C++ user writes the equivalent
lambda; framework handles lifecycle either way). Earns its keep — the
14 sub-steps in `worker_main_()` are framework boilerplate, not user
customisation surface (verified empirically: producer/consumer/processor
implementations are ~80% identical).

**Escape hatch: Level A** — power users who genuinely need to customise
sub-steps of `worker_main_()` (e.g. inject a different broker-register
sequence, add an unusual lifecycle phase) derive directly from
`RoleHostBase` and override `worker_main_()` themselves; nothing forces
them through the Level B wrapper. The two levels coexist — Level B is
implemented *on top of* `RoleHostBase`, not instead of it.

**Level C is out of scope** for this design. If demand for
"queue-only-no-role" RAII surfaces later, it can be added as a
non-conflicting parallel API.

### 2.3 Level B shape — `SimpleRoleHost`

A new template class lives in the public RAII addon header (Phase 4 work):

```cpp
// User-facing shape:
int main(int argc, char **argv) {
    auto cfg = RoleConfig::load_from_directory(argv[1], "producer",
                                                &parse_my_fields);
    SimpleRoleHost<MyOutSlot> host{std::move(cfg)};
    return host
        .on_init([](RoleAPIBase &api) { /* optional setup */ })
        .on_stop([](RoleAPIBase &api) { /* optional teardown */ })
        .run([](TypedQueueWriter<MyOutSlot> &tx, RoleAPIBase &api) {
            // Per-cycle body. Access role facilities via api:
            //   api.flexzone(ChannelSide::Tx)
            //   api.report_metric("temperature", v)
            //   api.band_broadcast("alerts", json)
            //   api.open_inbox_client("PROC-...")
            auto *slot = tx.acquire();
            slot->value = sample_sensor();
            api.report_metric("temperature", slot->value);
            return InvokeResult::Commit;     // or Discard / Error
        });
}
```

`SimpleRoleHost::run()` internally:
1. Constructs the underlying `RoleHostBase`-derived runner.
2. Runs the 14-sub-step `worker_main_()` skeleton using helpers shared
   with the script-path role hosts (no duplication of the broker-register
   / lifecycle code).
3. At sub-step 8, runs the RAII data loop calling the user's lambda
   each cycle (typed queue + timing parity per §3, §5).
4. Handles all teardown.

The lambda's signature mirrors the script path's `on_produce(tx, msgs, api)`
but with C++ types. `on_init` / `on_stop` hooks are optional (defaults
are no-op).

**The RAII layer does not need to surface band / inbox / metrics /
flexzone APIs** itself — the user already has the `RoleAPIBase &` object
in scope and uses its existing methods. The RAII path's responsibility
is narrowly: **own slot lifecycle (acquire / commit / discard) and
timing discipline** for the data loop body, plus the `worker_main_()`
boilerplate elimination at Level B. Everything else is reachable through
`RoleAPIBase` directly.

### 2.4 Level A shape — for power users

For users who need full control (e.g. they want to inject custom
lifecycle phases or skip a sub-step), Level A is reached by deriving
from `RoleHostBase` directly per HEP-CORE-0024 §16.3, and using
`SlotIterator` / `TransactionContext` (§5) at sub-step 8 instead of
calling `run_data_loop` directly:

```cpp
void MyCustomRoleHost::worker_main_() {
    // Sub-steps 1-7: roll your own custom variants if needed.
    // ...

    // Sub-step 8: RAII data loop (instead of run_data_loop).
    TransactionContext ctx{api_ref, config_.timing()};
    for (auto &tx : ctx.slots<MyOutSlot>()) {
        // Per-cycle body.
        write_to_slot(tx);
        // RAII: commit on normal exit, discard on exception.
    }

    // Sub-steps 9-14: roll your own custom variants if needed.
}
```

---

## 3. Design: Typed Queue Wrappers

A standalone addon header `typed_queue.hpp` provides compile-time typed
wrappers over the existing `QueueWriter` / `QueueReader` `void*`
interface:

```cpp
// typed_queue.hpp — C++ RAII addon, header-only, not part of the
// shared library ABI surface.
#include "utils/hub_queue.hpp"

namespace pylabhub::hub {

template <typename SlotT>
class TypedQueueWriter {
    QueueWriter *q_;
public:
    explicit TypedQueueWriter(QueueWriter &q) : q_(&q) {
        // assert sizeof(SlotT) <= q.item_size() at construction
    }
    SlotT *write_acquire(std::chrono::milliseconds t) {
        return static_cast<SlotT*>(q_->write_acquire(t));
    }
    void write_commit()  { q_->write_commit(); }
    void write_discard() { q_->write_discard(); }
    QueueMetrics metrics() const { return q_->metrics(); }
    size_t item_size() const { return q_->item_size(); }
    // ... forwarding for flexzone, capacity, etc., as needed
};

template <typename SlotT> class TypedQueueReader { /* symmetric */ };

} // namespace pylabhub::hub
```

Properties:
- **Non-owning** (raw pointer to existing queue). Lifetime tied to the
  `RoleAPIBase` / queue owner.
- **Zero virtual dispatch** for the typed path (just a static_cast).
- **No ABI impact** (header-only).
- **Type safety**: `SlotT` is the C struct that matches the role's slot
  schema. A `static_assert(sizeof(SlotT) <= item_size())` catches schema
  mismatches at compile time when the size is fixed.

The typed wrapper takes a `QueueWriter &` / `QueueReader &` — sourced
from the role's API. A RAII user writing a custom role host gets the
queue references via:

```cpp
QueueWriter &tx = api_ref.tx_queue();   // accessor to add on RoleAPIBase
QueueReader &rx = api_ref.rx_queue();   // accessor to add on RoleAPIBase
TypedQueueWriter<MySlot> typed_tx{tx};
```

(Phase 2 may need to add `tx_queue()` / `rx_queue()` const accessors to
`RoleAPIBase` if they're not already exposed for external use; the
internal `build_tx_queue` / `build_rx_queue` already construct them.)

---

## 4. Design: Timing at Queue Level (Phase 1 — DONE)

✅ Implemented 2026-03-29. Verified 2026-04-21:

- `LoopTimingParams` struct in `src/include/utils/loop_timing_policy.hpp:109`.
- `set_loop_policy()` removed from DataBlock (was dead).
- Timing flows: `parse_timing_config()` → `RoleConfig::timing()` →
  `RoleAPIBase::build_*_queue` opts → queue's `set_configured_period()` →
  `ContextMetrics::configured_period_us` reported.

No further work in Phase 1.

---

## 5. Design: SlotIterator Timing (Phase 3)

### 5.1 Construction

`SlotIterator` receives timing params and a typed queue wrapper at
construction (from `TransactionContext`):

```cpp
template <typename SlotT, bool IsWrite>
class SlotIterator {
    using Queue = std::conditional_t<IsWrite,
                                     TypedQueueWriter<SlotT>*,
                                     TypedQueueReader<SlotT>*>;
    Queue queue_;
    LoopTimingParams timing_;
    // ... timing-cycle bookkeeping (cycle_start, prev_deadline)
public:
    SlotIterator(Queue q, LoopTimingParams timing);
    // ...
};
```

No more `DataBlockProducer*` handle. No more reading metrics for timing
inputs.

### 5.2 `operator++()` timing discipline

Matches the manual loop step-by-step (parity is the goal):

```
operator++():
  1. Auto-commit (or discard on exception) the previous slot
  2. Record work_end = now()
  3. If previous deadline exists: check overrun (work_end > deadline_prev)
  4. Compute next_deadline via compute_next_deadline(policy, prev_deadline,
                                                      cycle_start, period)
  5. Compute short_timeout from period * io_wait_ratio
  6. Retry-acquire with short_timeout until deadline budget exhausted
     OR success
  7. If FixedRate / FixedRateWithCompensation and now < deadline:
     sleep_until(deadline)
  8. Record cycle_start = now()
  9. Return Result wrapping the new typed slot pointer
```

Steps 3-7 reuse the same helpers as `run_data_loop` (`compute_next_deadline`,
`compute_short_timeout`, `retry_acquire`) so timing semantics are
guaranteed identical, not "similar".

### 5.3 Overrun reporting

`SlotIterator` tracks overruns internally:

```cpp
uint64_t overrun_count() const noexcept;
```

For role hosts using RAII instead of the manual loop, this is the source
that should populate `RoleHostCore::loop_overrun_count` (currently
populated by `run_data_loop`).

---

## 6. Integration with Role Facilities (added 2026-04-21)

This section answers what the RAII layer needs vs. what the user
accesses through `RoleAPIBase` directly.

### 6.1 Config

The RAII path takes a pre-extracted `LoopTimingParams` (from
`RoleConfig::timing()`) — not the full `RoleConfig`. This keeps the RAII
layer free of config-parsing concerns and free of any role-specific
config knowledge (like `RoleConfig::role_data<T>()`).

```cpp
TransactionContext ctx{api_ref, config_.timing()};
//                                ^^^^^^^^^^^^^^^^
//                                pre-extracted LoopTimingParams
```

### 6.2 Inbox — typed C++ wrapper

**In scope.** The untyped `InboxHandle` returned by
`api_ref.open_inbox_client(uid)` carries opaque bytes; a C++ user with
a known message struct should not have to manually marshal. The addon
provides a typed wrapper (header-only, no ABI cost):

```cpp
template <typename MsgT>
class TypedInboxClient {
    InboxHandle h_;
public:
    explicit TypedInboxClient(InboxHandle h) : h_(std::move(h)) {
        // assert sizeof(MsgT) matches the inbox schema's payload size;
        // schema is registered at build_api time as "InboxFrame" type.
    }
    /// Send a typed payload. Returns true on success, false on
    /// timeout / closed.
    bool send(const MsgT &msg, int timeout_ms = -1) {
        auto *buf = h_.acquire();
        if (!buf) return false;
        std::memcpy(buf, &msg, sizeof(MsgT));
        return h_.send(timeout_ms);
    }
    bool is_ready() const noexcept { return h_.is_ready(); }
    void close() { h_.close(); }
};

// User code:
auto raw = api.open_inbox_client("PROC-XYZ-...");
TypedInboxClient<MyCommand> client{std::move(raw)};
client.send(MyCommand{.opcode = 7, .arg = 42});
```

Receiving (incoming inbox) is surfaced via `SimpleRoleHost::on_inbox<MsgT>`:

```cpp
host.on_inbox<MyCommand>([](const MyCommand &cmd, RoleAPIBase &api) {
    // typed access; framework decoded "InboxFrame" bytes → MyCommand
});
```

Internally this hooks the same `invoke_on_inbox(InvokeInbox)` path the
script engines use (HEP-CORE-0024 §15.3); the typed wrapper sits in
front, casting the typed payload from the InvokeInbox's data pointer.

### 6.3 Band — typed C++ wrapper

**In scope.** Band broadcasts today carry JSON (HEP-CORE-0030); a typed
wrapper deserialises into a struct via nlohmann's automatic conversion:

```cpp
template <typename EventT>
class TypedBand {
    RoleAPIBase *api_;
    std::string channel_;
public:
    TypedBand(RoleAPIBase &api, std::string channel)
        : api_(&api), channel_(std::move(channel)) {}

    bool join()  { return api_->band_join(channel_).ok(); }
    bool leave() { return api_->band_leave(channel_); }

    /// Broadcast a typed event. Internally serialises EventT → JSON
    /// (requires nlohmann::adl_serializer specialisation for EventT,
    /// or EventT defines to_json/from_json free functions).
    void broadcast(const EventT &event) {
        nlohmann::json j = event;   // ADL serialisation
        api_->band_broadcast(channel_, j);
    }

    std::vector<std::string> members() const {
        return api_->band_members(channel_);
    }
};

// User code:
TypedBand<TempAlert> alerts{api, "lab.alerts"};
alerts.join();
alerts.broadcast(TempAlert{.celsius = 88.3, .severity = "high"});
```

Receiving incoming band notifications uses the host hook
`SimpleRoleHost::on_band_message<EventT>`:

```cpp
host.on_band_message<TempAlert>("lab.alerts",
    [](const TempAlert &alert, const std::string &sender_uid,
       RoleAPIBase &api) {
        // typed access
    });
```

Internally this hooks the unsolicited-notification path
(`BrokerRequestComm::on_notify_cb` per `broker_request_comm.cpp:200-202`)
filtered by message type + channel; the typed wrapper deserialises
JSON → `EventT` before calling the user's lambda.

### 6.4 Metrics

**Mostly through `api_ref`, with one addition.**

- **Custom metrics**: user calls `api_ref.report_metric(key, value)` /
  `report_metrics(kv)` inside the loop body. Aggregated into the
  role's snapshot picked up by heartbeat piggyback (HEP-CORE-0019 §3
  + HEP-CORE-0033 §9 query model).
- **Per-cycle metrics from RAII**: the SlotIterator should populate
  `RoleHostCore::loop_overrun_count` (§5.3) and also feed into
  `ContextMetrics` for in_slots / out_slots / drops. Phase 3 needs to
  wire these — currently the manual loop does it; RAII path must
  achieve parity.
- **Metrics hook**: `RoleAPIBase::set_metrics_hook(fn)` lets the role
  host inject role-specific JSON into the snapshot. RAII users use the
  same hook the same way.

### 6.5 Flexzone — typed C++ view

**In scope.** `api_ref.flexzone(ChannelSide::Tx)` returns `void *`; the
addon adds a typed view:

```cpp
template <typename FzT>
class TypedFlexzone {
    void *fz_;
    size_t size_;
public:
    TypedFlexzone(RoleAPIBase &api, ChannelSide side)
        : fz_(api.flexzone(side)),
          size_(api.flexzone_size(side)) {
        // runtime check: size_ >= sizeof(FzT) when fz_ != nullptr
    }
    bool valid() const noexcept { return fz_ != nullptr; }
    FzT       *get()       noexcept { return static_cast<FzT*>(fz_); }
    const FzT *get() const noexcept { return static_cast<const FzT*>(fz_); }
    FzT       &operator*()       { return *get(); }
    const FzT &operator*() const { return *get(); }
    FzT       *operator->()       { return get(); }
    const FzT *operator->() const { return get(); }
    size_t byte_size() const noexcept { return size_; }
};

// User code:
TypedFlexzone<MyFlexLayout> fz{api, ChannelSide::Tx};
if (fz.valid()) {
    fz->sample_count = 1024;
    api.update_flexzone_checksum();
}
```

`api.update_flexzone_checksum()`, `api.sync_flexzone_checksum()`, and
`api.set_verify_checksum(bool)` remain direct calls on `RoleAPIBase` —
no typed wrapping needed (they take no payload).

### 6.6 Spinlocks — RAII guard

**In scope.** `api_ref.get_spinlock(idx, side)` returns `hub::SharedSpinLock`;
`hub::SpinGuard` already exists in the framework for RAII lock/unlock.
The addon provides a typed helper that combines them:

```cpp
class ScopedSpinLock {
    hub::SpinGuard guard_;
public:
    ScopedSpinLock(RoleAPIBase &api, size_t idx,
                   std::optional<ChannelSide> side = std::nullopt)
        : guard_(api.get_spinlock(idx, side)) {}
    // guard_ unlocks on destruction
};

// User code (inside cycle body):
{
    ScopedSpinLock lk{api, 0, ChannelSide::Tx};
    // critical section over SHM-backed data
}
```

### 6.7 Custom metrics — typed registry (optional)

**In scope, v1 optional.** `api.report_metric("key", double)` is
stringly-typed; the addon provides a typed registry for compile-time
known keys:

```cpp
// One declaration per logical metric:
constexpr MetricKey kTempC{"temperature_celsius"};
constexpr MetricKey kLatency{"sample_latency_ms"};

// In the cycle body:
api.report_metric(kTempC, 23.5);
api.report_metric(kLatency, 1.2);
```

`MetricKey` is a thin wrapper around a `const char *` + compile-time
length; `api.report_metric(MetricKey, double)` overload inlines to
`api.report_metric(string_view, double)` at the ABI boundary. Prevents
typos and gives grep-able metric names.

v1 can skip this — users can still call `report_metric("key", v)` —
but for large codebases the typed registry reduces key drift.

### 6.8 Shared-data typed access

**In scope.** `RoleAPIBase::set_shared_data(key, StateValue)` /
`get_shared_data(key)` use a variant-like `StateValue`. Typed wrapper:

```cpp
template <typename T>
class SharedStateSlot {
    RoleAPIBase *api_;
    std::string key_;
public:
    SharedStateSlot(RoleAPIBase &api, std::string key);
    void set(const T &value);      // packs into StateValue
    std::optional<T> get() const;  // unpacks; nullopt on missing / type mismatch
    void remove();
};

// User code:
SharedStateSlot<int64_t> sample_count{api, "sample_count"};
sample_count.set(42);
if (auto n = sample_count.get()) { /* ... */ }
```

Type tag stored alongside the value so get<T> fails cleanly on
mismatch instead of silently returning garbage.

### 6.9 Logging — fmt-style helper

**In scope.** `api.log("info", "formatted message")` is stringly-typed
for level; the addon adds fmt-style wrappers:

```cpp
namespace pylabhub::raii {
template <typename... Args>
void log_info(RoleAPIBase &api, fmt::format_string<Args...> fmt, Args&&... args);
template <typename... Args>
void log_warn(RoleAPIBase &api, fmt::format_string<Args...> fmt, Args&&... args);
// ... debug, error
}

// User code:
using pylabhub::raii::log_info;
log_info(api, "sampled v={:.3f} at t={}", value, t);
```

Internally calls `api.log("info", fmt::format(fmt, args...))`. Keeps
the ABI boundary a plain `std::string` while giving users type-safe
formatting.

### 6.10 Typed notification hook — generic

**In scope.** `SimpleRoleHost` provides a generic hook for any
broker-initiated notification (not just band):

```cpp
host.on_notify("ROLE_REGISTERED_NOTIFY",
    [](const nlohmann::json &payload, RoleAPIBase &api) {
        // unsolicited notification from broker — handle or ignore
    });

// Typed convenience for JSON-deserialisable events:
host.on_notify<ChannelClosingPayload>("CHANNEL_CLOSING_NOTIFY",
    [](const ChannelClosingPayload &p, RoleAPIBase &api) { ... });
```

Internally hooks `BrokerRequestComm::on_notify_cb` with per-msg_type
dispatch. Complements `on_band_message<EventT>` (§6.3); band messages
are a subset of broker notifications. Known notification types today:
`ROLE_REGISTERED_NOTIFY`, `CHANNEL_CLOSING_NOTIFY`, `CHANNEL_BROADCAST`,
`FORCE_SHUTDOWN`, `BAND_BROADCAST_NOTIFY`, `BAND_LEAVE_NOTIFY`.

### 6.10.1 Error handling for notification / event hooks

Notification / band / inbox hooks run on the ctrl thread (not the data
loop). These paths are **low-bandwidth** relative to the cycle loop:
a handful of messages per second at steady state, vs tens of kHz for
slot cycling. Detailed error-handling policy (retry budgets, per-
exception-type dispatch, per-message-type circuit breakers,
drop-vs-log-and-continue per class of fault, etc.) is **not over-
specified for v1** — handlers that throw are caught, logged, and the
ctrl thread moves on. Cycle-loop errors are handled explicitly per
§6.12; notification-hook errors are handled loosely on purpose.

If measurement shows that a specific notification handler's failure
mode needs more nuance (e.g. a band handler that routinely times out
on slow ADL deserialisation), that warrants a targeted knob at that
time — not a framework-wide policy upfront.

### 6.6 What the RAII layer DOES need from the role framework

Just two things:
1. References to the queues built by `RoleAPIBase::build_tx_queue()` /
   `build_rx_queue()`. May need new public accessors `tx_queue()` /
   `rx_queue()` on `RoleAPIBase` — see Phase 2 below.
2. The validated `LoopTimingParams` (from `RoleConfig::timing()`).

For Level B (primary), `SimpleRoleHost` reaches both internally — the
user provides nothing beyond the cycle callable + optional hooks. For
Level A (escape hatch), the user is inside their own `worker_main_()`
and constructs `TransactionContext{api_ref, config_.timing()}` directly.

Everything else the user needs is already on the `RoleAPIBase` reference
passed into the cycle callable (Level B) or available as `api_ref`
(Level A) — band, inbox, metrics, flexzone, spinlocks all work
identically across the script path and both RAII levels.

---

## 6.11 Cycle callable shape — producer / consumer / processor specialisation

The cycle callable signature depends on which sides the role wires.
`SimpleRoleHost` specialises per role shape:

```cpp
// Producer (tx only):
SimpleProducerHost<OutSlotT> host{std::move(cfg)};
host.run([](TypedQueueWriter<OutSlotT> &tx, RoleAPIBase &api)
         -> InvokeResult {
    auto *slot = tx.acquire();
    slot->value = sample_sensor();
    return InvokeResult::Commit;
});

// Consumer (rx only):
SimpleConsumerHost<InSlotT> host{std::move(cfg)};
host.run([](TypedQueueReader<InSlotT> &rx, RoleAPIBase &api)
         -> InvokeResult {
    const auto *slot = rx.acquire();
    consume(*slot);
    return InvokeResult::Commit;    // ignored by consumer loop today
});

// Processor (rx + tx):
SimpleProcessorHost<InSlotT, OutSlotT> host{std::move(cfg)};
host.run([](TypedQueueReader<InSlotT> &rx,
            TypedQueueWriter<OutSlotT> &tx,
            RoleAPIBase &api) -> InvokeResult {
    const auto *in = rx.acquire();
    auto *out = tx.acquire();
    transform(*in, *out);
    return InvokeResult::Commit;
});
```

Three concrete class templates (not one with runtime branches) mirror
the three `CycleOps` classes on the script path (HEP-CORE-0024 §15.2).
Zero runtime overhead; the role identity is captured at compile time.

### 6.11.1 Slots are strongly typed — no escape hatch

**Slots always require a concrete `SlotT` template argument.** The
framework's data-plane contract is that every slot has a declared
schema (HEP-CORE-0016 Named Schema Registry); the typed C++ addon
preserves that invariant at the type level. There is **no**
`ByteSlot` / opaque-bytes specialisation — a role without a defined
slot type is not a valid role.

Where byte-level access is genuinely needed (e.g. a generic passthrough
relay that has no a priori knowledge of its schema), it belongs to
the **flexzone**, not the slot. Flexzone is the TABLE 1 user-managed
coordination region (HEP-CORE-0002 §2.2) and permits typeless access
via `std::span<std::byte>`:

```cpp
// Typeless flexzone access (when the layout is determined at runtime
// or the role is intentionally generic).
std::span<std::byte> bytes = api.flexzone_bytes(ChannelSide::Tx);
std::memcpy(bytes.data(), payload, payload_size);
```

`flexzone_bytes(side)` is the byte-span accessor on `RoleAPIBase` — an
addition sibling to the existing `flexzone(side) → void *` and the
typed `TypedFlexzone<FzT>` wrapper (§6.5). Three levels of flexzone
access, by type specificity:

| Accessor | Return | When to use |
|---|---|---|
| `api.flexzone(side)` | `void *` | Legacy; C/legacy integrations |
| `api.flexzone_bytes(side)` | `std::span<std::byte>` | Typeless but bounded byte access |
| `TypedFlexzone<FzT>(api, side)` | `FzT *` / `operator->` | Typed struct access (canonical for typed C++ users) |

Slots are always typed; if a role genuinely has no slot type to declare,
its purpose belongs in flexzone, metadata, or inbox — not in the
slot schema.

## 6.12 Error handling policy (cycle callable)

When the cycle callable throws an exception, `SimpleRoleHost` applies
this policy (matches the script-path `stop_on_script_error` semantics):

| Config | On uncaught exception from cycle lambda |
|---|---|
| `stop_on_script_error: true` | Log error + stack + `api.set_critical_error()` + exit loop with non-zero; teardown runs normally. |
| `stop_on_script_error: false` (default) | Log error + stack + increment `ContextMetrics::script_error_count` + `write_discard()` the current slot (RAII) + continue loop. |

Explicit shutdown from inside the lambda:
- `api.stop()` — graceful shutdown; loop exits after current cycle
  commits/discards.
- `api.set_critical_error()` — fast shutdown; loop exits immediately
  with error status.

These match the script-path behaviours exactly — same config flag,
same effect. The data loop is the hot path; errors here are explicit
and loud so regressions are caught fast.

## 6.13 Config access inside the cycle lambda

The cycle callable receives `RoleAPIBase &api` but the role's typed
config (`role_data<MyFields>()`) is on `RoleConfig`, not on the API.
Two access options:

**Option A** — capture in the lambda's closure (most common):
```cpp
auto cfg = RoleConfig::load_from_directory(dir, "producer", &parse_my_fields);
const auto &fields = cfg.role_data<MyFields>();
SimpleProducerHost<OutSlotT> host{std::move(cfg)};
host.run([&fields](auto &tx, auto &api) {
    use(fields.sampling_rate_hz);
    ...
});
```

**Option B** — `SimpleRoleHost` exposes a const accessor to the
underlying `RoleConfig` so the cycle body can read typed fields:
```cpp
host.run([&host](auto &tx, auto &api) {
    const auto &fields = host.config().role_data<MyFields>();
    ...
});
```

Both work; (A) is idiomatic for the common case. Document (B) for
users who don't want to carry a separate config reference.

## 6.14 Testing the cycle lambda without a full host

**In scope, Phase 6.** A test-helpers header provides in-process fakes:

```cpp
// tests/test_framework/raii_test_helpers.hpp
template <typename SlotT>
class FakeQueueWriter {
    // Exposes the same interface as TypedQueueWriter<SlotT>
    // but backed by an in-process ring, not ShmQueue/ZmqQueue.
};

class FakeRoleAPI : public RoleAPIBase {
    // Records band_broadcast / report_metric / log calls for inspection
};

// Unit test:
TEST(MyRole, ProducesExpectedValues) {
    FakeRoleAPI api;
    FakeQueueWriter<OutSlotT> tx;
    auto result = my_cycle_callable(tx, api);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(tx.last().value, expected);
}
```

Lets users test cycle logic without subprocess / broker / real queues.

## 7. Migration Path (revised)

### Phase 1: Timing unification ✅ DONE 2026-03-29

(See §4. All items verified against code 2026-04-21.)

### Phase 2: Typed queue wrappers + accessors

- Create `src/include/utils/typed_queue.hpp` per §3.
- Add public `tx_queue()` / `rx_queue()` const accessors to `RoleAPIBase`
  if not already exposed (verify before adding — they may be there for
  internal use already).
- Unit tests (L2): `TypedQueueWriter<MySlot>` over `ShmQueue` and over
  `ZmqQueue`; verify `static_assert` triggers on schema-size mismatch
  for fixed-size schemas.

### Phase 3: SlotIterator + TransactionContext over typed queues

- `SlotIterator` constructor takes `TypedQueue*` + `LoopTimingParams`
  (§5.1).
- `operator++()` implements the 9-step timing discipline (§5.2) using
  the same helpers as `run_data_loop` (`compute_next_deadline`,
  `retry_acquire`, etc.).
- `TransactionContext` constructor takes `RoleAPIBase &` +
  `LoopTimingParams`, internally constructs typed queue wrappers.
- Wire `SlotIterator::overrun_count()` into `RoleHostCore::loop_overrun_count`
  (parity with the manual loop's bookkeeping).
- Wire ContextMetrics (in/out slots, drops) through the RAII path so
  metrics snapshot looks the same regardless of which loop drove them.
- Tests (L2/L3): timing parity with `run_data_loop` (both paths produce
  identical metrics for the same workload); ZMQ-backed RAII loop;
  exception-safe discard.

### Phase 4: Typed wrappers for non-slot facilities

Header-only addons that can land independently of the slot-iteration
work in Phase 3 (different facilities, different code paths).

- **Inbox**: `TypedInboxClient<MsgT>` per §6.2. Wraps `InboxHandle`;
  static-asserts payload size matches inbox schema where determinable;
  runtime check at first `send()` as safety net.
- **Band**: `TypedBand<EventT>` per §6.3. Wraps `band_join` / `leave` /
  `broadcast` / `members`; ADL JSON serialisation requires
  `to_json`/`from_json` on `EventT` or `nlohmann::adl_serializer`
  specialisation.
- **Flexzone**: `TypedFlexzone<FzT>` per §6.5. Wraps
  `api.flexzone(side)` with a runtime size check.
- **Spinlocks**: `ScopedSpinLock` per §6.6. RAII wrapper combining
  `api.get_spinlock()` with existing `hub::SpinGuard`.
- **Shared state**: `SharedStateSlot<T>` per §6.8. Typed get/set over
  the variant-valued `set_shared_data` / `get_shared_data`.
- **Logging**: fmt-style free functions per §6.9.
- **Metrics registry** (optional v1 feature): `MetricKey` compile-time
  keys per §6.7.
- **Tests (L2 / L3)**: typed-roundtrip for each wrapper (send
  structured payload, receive via the matching hook; get/set shared
  state; typed flexzone access).

### Phase 5: `SimpleRoleHost` (Level B) + examples + docs

Three concrete host template classes mirroring the three script-path
cycle shapes (HEP-CORE-0024 §15.2):

- `SimpleProducerHost<OutSlotT>` — single-side output.
- `SimpleConsumerHost<InSlotT>` — single-side input.
- `SimpleProcessorHost<InSlotT, OutSlotT>` — dual-side with input-hold.

Each internally derives from `RoleHostBase` and runs the 14-sub-step
`worker_main_()` skeleton using the helpers already factored out for
the script-path role hosts (no duplicated lifecycle code).

**Builder-style API** (uniform across all three host templates;
optional hooks default to no-op / accept):

| Hook | Purpose |
|---|---|
| `.on_init([](RoleAPIBase &){})` | Called once after queue setup, before the data loop starts. Returns `void`. |
| `.on_stop([](RoleAPIBase &){})` | Called once before teardown, after the data loop exits. Returns `void`. |
| `.on_inbox<MsgT>([](const MsgT &, RoleAPIBase &){})` | Wires typed inbox receive (§6.2) via framework's `invoke_on_inbox`. |
| `.on_band_message<EventT>("channel", [](const EventT &, const std::string &sender, RoleAPIBase &){})` | Wires typed band receive (§6.3). |
| `.on_notify<P>("MSG_TYPE", [](const P &, RoleAPIBase &){})` | Generic typed notification hook (§6.10). |
| `.on_veto<ChannelCloseRequest>([](const ChannelCloseRequest &, RoleAPIBase &) → bool)` | Optional veto hook for select broker operations (parallels hub-side veto hooks in HEP-0033 §11.2). |
| `.run(cycle_callable)` | Entry point. Blocks until shutdown. Returns exit code. Cycle callable signature per §6.11 (producer / consumer / processor specialised). |

**Error handling** per §6.12 (exception → log + discard slot + continue
or exit per `stop_on_script_error` config).

**Builder-call ordering contract**: all `.on_*` hooks must be called
before `.run(...)`. Violations are debug-asserted; release mode
silently ignores late hook registrations (logged as a warning).

**Tests (L3)**:
- Each of the three hosts reaches the data loop, runs N cycles, commits/
  discards per lambda return, updates `ContextMetrics` (in/out slots,
  drops, script errors, overrun_count), deregisters cleanly on SIGTERM.
- Typed inbox / band / notify hooks fire with correctly-cast payloads.
- `on_init` / `on_stop` called exactly once in the correct order.
- Exception from cycle lambda: loop continues (default) or exits
  (`stop_on_script_error: true`).
- `api.stop()` / `api.set_critical_error()` from inside the lambda
  cleanly exits.

**Examples in `docs/README/README_EmbeddedAPI.md`**:
- **Level B** (primary): `SimpleProducerHost<MyOutSlot>` / Consumer /
  Processor for ~10-line `main()`; show all three role variants;
  show typed inbox send + receive; show typed band broadcast + receive.
- **Level A** (escape hatch): a custom `RoleHostBase` subclass that
  uses `TransactionContext` at sub-step 8 — for users who need
  customisation Level B doesn't provide.

**HEP-CORE-0011 rewrite** (tracked in API_TODO SE-03) absorbs the
Level B / Level A distinction into the canonical script-host
abstraction documentation.

### Phase 6: Test helpers for cycle lambdas

- `tests/test_framework/raii_test_helpers.hpp` provides
  `FakeQueueWriter<SlotT>` / `FakeQueueReader<SlotT>` / `FakeRoleAPI`
  so users can unit-test cycle callables without spinning up subprocess /
  broker / real queues (per §6.14).
- `FakeRoleAPI` records `band_broadcast` / `report_metric` / `log` /
  `open_inbox_client` calls for inspection.
- Not part of the library ABI (lives under `tests/`); shipped as a
  header-only helper for downstream projects that link against
  `pylabhub-utils` and want to unit-test their cycle logic.

---

## 8. What Does NOT Change

- `QueueWriter` / `QueueReader` virtual interface (the `void*` API stays;
  RAII is a strictly additive header-only layer).
- `ShmQueue` / `ZmqQueue` implementations.
- Script engine path (continues to use the `void*` typed-invoke
  interface — `InvokeTx`, `InvokeRx`, `InvokeInbox`).
- Manual `run_data_loop` path (canonical for the script-driven hosts;
  remains the primary path for `worker_main_()` per HEP-CORE-0024 §16.3).
- Shared library ABI (`typed_queue.hpp` is header-only, no exports
  added; new `RoleAPIBase` queue accessors return `QueueWriter &` /
  `QueueReader &` pointers — references already cross the ABI elsewhere).

---

## 9. Open Questions

1. **Heartbeat timing inside RAII.**
   Heartbeats are driven on a separate ctrl thread
   (`role_api_base.cpp`'s `on_heartbeat_tick_`), not the data loop.
   RAII does NOT need to handle heartbeats — they continue
   independently. Settled — no design choice needed.
2. **Auto-publish semantics on exception.**
   `SlotWriteHandle::commit()` is the existing RAII commit point.
   With `QueueWriter`, it becomes `write_commit()` / `write_discard()`.
   Exception → `write_discard()` (safe default, matches the
   `cleanup_on_shutdown` contract in the manual `CycleOps` classes
   per HEP-CORE-0024 §15.2). Settled.
3. **Typed inbox / band schema discovery at compile time.**
   `TypedInboxClient<MsgT>` should `static_assert(sizeof(MsgT)
   == inbox_schema_payload_size)` where determinable — but the
   inbox schema is registered at runtime (build_api time). Compile-time
   check is best-effort; runtime check is the safety net. Open question:
   should the addon offer a macro / sizeof-traits helper for users to
   declare their schema↔struct correspondence, or should we trust
   `static_assert(sizeof(MsgT) > 0)` plus runtime size check at first
   `send()`? Likely the latter for v1; macro can come later.
4. **`TypedBand<EventT>` JSON serialisation requirements.**
   `nlohmann::json j = event;` requires either ADL `to_json` /
   `from_json` free functions for `EventT`, or an
   `nlohmann::adl_serializer<EventT>` specialisation. Document this
   prerequisite in the wrapper's docstring. No framework-level
   dispatch needed.

---

## 10. Cross-References

- HEP-CORE-0002 §17.2 (Queue Abstraction — the queue layer the RAII
  path consumes).
- HEP-CORE-0008 (LoopPolicy + IterationMetrics — timing measurement
  model).
- HEP-CORE-0009 §2.6 (LoopTimingPolicy reference values).
- HEP-CORE-0024 §16.3 (Adding a New Role — `worker_main_()` skeleton
  the RAII path lives inside).
- HEP-CORE-0024 §15 (Role Plurality — explains why the manual
  `CycleOps` classes are the script path's equivalent of what RAII is
  doing on the C++ user side).
- `docs/todo/API_TODO.md` "Template RAII" section (where the remaining
  Phase 2-4 work is tracked).
- `src/include/utils/slot_iterator.hpp` (current implementation —
  Phase 3 target).
- `src/include/utils/transaction_context.hpp` (current implementation
  — Phase 3 target).
- `src/include/utils/role_api_base.hpp` (the API class the RAII path
  sources queues + facilities from).
