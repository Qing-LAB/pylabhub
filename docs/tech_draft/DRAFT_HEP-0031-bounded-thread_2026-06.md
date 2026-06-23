# DRAFT: HEP-CORE-0031 amendment — Bounded-Deadline Thread Primitive

**Status:** Tech draft — design exploration (2026-06-23).
**Target:** Amendment to `docs/HEP/HEP-CORE-0031-ThreadManager.md` once finalized.
**Tracked task:** #283.
**Originating discussion:** #272 self-review surfaced the BRC-poll-thread block in `RoleAPIBase::apply_consumer_reg_ack_shm_` (up to ~3.9s worst case).  See #282 for that specific edge.

**Revision history:**
- v1 (2026-06-23 morning): initial draft.
- v2 (2026-06-23): simplified per design feedback — single total timeout (no grace period / no backoff cadence), no per-step enforcement, no loop-level sleep, kernel-timer-based deadline detection.
- v3 (2026-06-23, this draft): replaced kernel timers with a single dedicated sweeper thread inside the `BoundedThreadRegistry` lifecycle module.  Kernel-timer-per-thread was rejected for three reasons: (1) per-thread kernel resource cost (timer slots / fd table pressure) that pins on stuck threads, (2) the hardcoded 50ms abnormality window was arbitrary and too tight for some bodies, (3) concurrent timer fires would race the logger non-deterministically.  A single sweeper thread fixes all three.

---

## 1. Motivation

A pattern recurs in the codebase:

| Site | Body | Today |
|---|---|---|
| Consumer SHM dial (#272) | crypto_box handshake + SCM_RIGHTS recv (~3.9s worst case) | blocks BRC poll thread |
| Producer L2c broker pre-confirm (#280) | `CONSUMER_ATTACH_REQ` sync RPC (~2s budget) | blocks accept thread |
| Future #262 mutual auth | extra crypto_box Frame 3 | will block whichever thread invokes it |
| Future HUB_TARGETED_REQ (#75) | cross-hub sync RPC | will block whichever thread invokes it |
| Future api.crypto.* (#247) | script-invoked crypto | (per-call, bounded by script call) |

Each shares: **named diagnostic + total-time budget + cooperative cancellation + completion result**.  Today each call site re-encodes the budget via per-syscall timeouts and per-retry sleeps; teardown integration is ad-hoc; observability is scattered.

The goal is a single primitive that captures the contract once and lets call sites compose without re-deriving it.

---

## 2. Core design point — cancellation lives in the loop, not the body

The framework provides a **loop thread** managed by `ThreadManager`.  The loop ticks:

1. Call the user-supplied **step function** (which is non-blocking per call).
2. Inspect the outcome (Continue / Done / Failed).
3. Check the stop_token.

The user's body is just a step function — it does NOT thread `stop_token` through itself.  Cancellation honoring is the loop's job.

**Why this matters:**

- Body authors can't forget to check `stop_token` — they don't see it.
- Body authors can't accidentally deadlock the cancellation path — the stop signal arrives on a different thread (the deadline timer), affecting only the loop's view.
- Body authors write a state machine, not async-cancellation plumbing.

**Worst case if body misbehaves** (blocks indefinitely in violation of the non-blocking contract):

- The deadline log is still honored — it fires from a different thread, independent of the body's state.
- The "still alive after stop signal" abnormality log fires once — same guarantee.
- Resource leakage is bounded to that thread's owned captures.
- Process teardown reaps the leaked thread.

The framework GUARANTEES timely signalling + timely observability.  It does NOT guarantee timely termination — termination depends on the body's cooperation.  Naming reflects this: `request_stop_on_deadline`, not "kill_on_deadline."

---

## 3. The state machine

Each bounded thread is tracked in the framework's registry while alive.  Three states; one transition per state.

```
                  spawn_bounded()
                        │
                        ▼
                ┌───────────────┐
                │   Running     │  body returning Continue; within total_timeout
                └───────┬───────┘
                        │ sweeper observes now > deadline:
                        │   1. logs timeout via on_timeout_message_provider
                        │   2. signals stop_token
                        ▼
                ┌────────────────────┐
                │ DeadlineSignalled  │  loop should exit shortly
                └─────────┬──────────┘
                          │ (a) body exits cooperatively (well-behaved)
                          │     → registry pruned, completion fires, done.
                          │ (b) sweeper observes entry STILL in this state
                          │     on the NEXT sweep tick after signalling
                          │     → logs ABNORMALITY ONCE, then framework has
                          │     nothing further to do.  Thread remains in
                          │     registry as a known leak until it eventually
                          │     exits (lucky) or process teardown reaps it.
                          ▼
                  ┌───────────────┐
                  │   Exited      │  terminal — entry removed from registry;
                  └───────────────┘  completion event fires.

Natural completion path also reaches Exited from Running:
- step_fn returns Done   → loop exits → Exited
- step_fn returns Failed → loop exits → Exited
- In both cases: deadline timer + abnormality timer are cancelled before they fire.
```

**Single total_timeout.**  The user specifies ONE timeout at spawn time.  It covers the whole thing — any number of step cycles to get the work done.  No grace period, no escalating cadence, no backoff.  The framework does ONE thing on deadline expiry (log + signal stop) and ONE thing on abnormality detection (log once).  After that, nothing.

**Inspection list = running threads.**  Threads are in the registry while alive; removed at quit (whether by body Done/Failed, or by cooperation with stop signal).  No separate state for completion-callback-running — if the callback is part of the bounded thread's execution (see §5), the thread is still "running" until the callback returns.

**`on_timeout_message_provider` is called LATE.**  The deadline timer invokes it at expiry, so the log captures current context ("dial state X / attempt Y / mid-recv"), not bake-time state.  The body's state machine can be inspected via thread-safe accessors if the provider needs to read it.

---

## 4. The step-function contract

### 4.1 Body signature

```cpp
enum class StepStatus { Continue, Done, Failed };

template <typename R>
struct StepOutcome {
    StepStatus       status;
    std::optional<R> result;  // populated only when status == Done
};

template <typename R>
using StepFn = std::function<StepOutcome<R>()>;
```

The step function takes nothing, returns `StepOutcome<R>`:

- `Continue`: more work to do; loop ticks again immediately.
- `Done`: body completed successfully; `result` carries the value.
- `Failed`: body cannot complete; `result` is ignored.

The body does NOT receive `stop_token`.  The loop checks it between ticks.

### 4.2 Body discipline (programmer contract)

The body MUST be **non-blocking per call** — each call returns quickly so the loop can promptly check `stop_token`.

- I/O uses timeout=0 (non-blocking), or short timeouts (≤5-10ms) inside the body's poll/recv calls.
- Long protocols decompose into a state machine; state persists in captures.
- Body captures are RAII-clean — when the loop exits (any reason), captures destruct → cleanup happens.

**This is a contract with the body author, not a runtime check.**  The framework does NOT measure per-step duration and does NOT enforce a per-step bound.  Violations of the non-blocking contract manifest as a slow reaction to `stop_token` (proportional to the misbehaving step's duration), and in the extreme as a `DeadlineSignalled → "still alive" abnormality log`.  The framework's obligation is to surface those signals; the body author's obligation is to honor the contract.

### 4.3 The loop runs without sleeping

```cpp
// Pseudocode for the bounded thread's loop.
while (!stop_token.stop_requested()) {
    auto outcome = step_fn();
    if (outcome.status == StepStatus::Done) {
        complete(std::move(outcome.result));
        return;
    }
    if (outcome.status == StepStatus::Failed) {
        complete(std::nullopt);
        return;
    }
    // Continue: no sleep at the loop level — step_fn is responsible for its own pacing
    // (typically a short poll(timeout=5ms) inside the step).
}
// stop_requested: loop exits cooperatively
complete(std::nullopt);
```

No per-iteration sleep at the loop level.  Pacing is the step function's responsibility (via its own short non-blocking I/O).  If step_fn returns Continue immediately without any internal pacing, that's a body-contract violation — the loop will hammer it and CPU spins.  The deadline timer still fires at total_timeout regardless, so the worst case is still bounded.

### 4.4 Example: SHM dial as a step function

```cpp
enum class DialPhase {
    Connect, SendHello, RecvChallenge, SendResponse, RecvFd, ActivateQueue
};
struct DialState {
    DialPhase     phase{DialPhase::Connect};
    int           attempt{0};
    int           sock_fd{-1};
    // …
};

auto step = [state = DialState{}, /* captures */]() mutable -> StepOutcome<bool> {
    switch (state.phase) {
    case DialPhase::Connect:
        // non-blocking connect; on EAGAIN → return Continue (try again)
        // on success → advance phase, return Continue
        // on hard fail → return {Failed, {}}
    case DialPhase::SendHello:
        // partial send accumulates in state; non-blocking send
    // …
    case DialPhase::ActivateQueue:
        // local operation; rx_queue.set_shm_capability_fd + start
        return {StepStatus::Done, true};
    }
};
```

Each step advances at most one phase or does one short I/O batch.  The body's own internal pacing (e.g. `poll(timeout=5ms)` in Connect / RecvChallenge / RecvFd) keeps it from busy-looping AND keeps stop_token reaction within ~5-10ms.

---

## 5. Completion modes

Both modes share one spawn surface — caller picks how they observe the result.

### 5.1 Bounded-sync (caller blocks)

```cpp
auto handle = tm.spawn_bounded(
    "RoleAPI:shm_dial:" + uid + ":" + ch,
    total_timeout,
    on_timeout_message_provider,
    step_fn);
std::optional<bool> result = handle.wait_for_completion();
// nullopt = timed out (deadline fired, body returned Failed or didn't return cleanly);
// some(true|false) = body returned Done with result, or Failed.
```

- Caller's thread blocks on `wait_for_completion`.
- No FSM change required at call sites.  This is the migration step that ships first.
- BRC-poll-thread block from #272 persists in this mode — the work happens on a TM-managed thread, but the caller still waits synchronously.

### 5.2 Truly-async (caller returns immediately)

```cpp
tm.spawn_bounded(
    name, total_timeout, msg_provider, step_fn,
    [this, ch](std::optional<bool> outcome) {
        // Runs ON THE BOUNDED THREAD after the loop exits, before the
        // thread quits.  Captures of the step function are still in
        // scope (they destruct AFTER the callback returns).
        if (outcome.value_or(false)) {
            // success → fire Authorized transition
        } else {
            // fail or timeout → fire role teardown
        }
    });
// returns immediately; caller's thread is released
```

- Caller's thread released.  BRC-poll-thread is no longer blocked.
- Requires FSM amendment at the call site to accommodate "REG accepted but dial pending."  For #272 specifically that means adding `RegistrationState::RegAckPending` between `Registered` and `Authorized` and teaching `any_presence_authorized()` to wait for `Authorized` (it already does — the state insertion is mostly additive).
- The completion callback runs on the bounded thread.  The thread is still tracked in the registry until the callback returns + captures destruct.  If the callback itself violates the non-blocking expectation by hanging, it's caught by the same deadline timer (the total_timeout covers spawn → registry removal, including the callback).

---

## 6. Deadline detection — single sweeper thread

A single dedicated sweeper thread inside `BoundedThreadRegistry` wakes up periodically, walks the registry sequentially, and runs the state machine.

### 6.1 Why a single sweeper thread (not per-thread kernel timers)

Per-thread kernel timers (`timer_create(SIGEV_THREAD)` or `timerfd`) were rejected for three reasons:

1. **Kernel resource per bounded thread.**  Each timer holds a kernel slot (timer_create) or fd table entry (timerfd).  N concurrent bounded threads consume N kernel slots.  If any thread sticks (StuckBeyondGrace), its abnormality timer remains armed — kernel resources pinned.  A single sweeper holds ONE kernel resource (its sleep) regardless of how many bounded threads are tracked.
2. **Hardcoded abnormality window is arbitrary.**  A short-fuse abnormality timer at deadline+50ms is too tight for some legitimate bodies (a step mid-syscall returning takes longer).  With a sweeper, the natural grace IS the sweep interval — pick e.g. 100-200ms once, body has at least one full sweep cycle to exit before abnormality fires.
3. **Concurrent fires race the logger.**  Multiple `SIGEV_THREAD` callbacks fire as transient pthreads.  If several deadlines hit nearly simultaneously, N pthreads race the logger mutex non-deterministically; log ordering is undefined; under load they pile up.  A sweeper emits logs serially in one ordered stream.

### 6.2 Sweeper behavior

The sweeper is a managed thread inside the `BoundedThreadRegistry` lifecycle module.  Sleep interval is configurable (proposed default **100ms**).

**Per sweep tick** — walk the registry sequentially.  For each entry:

| Entry state | Condition | Action |
|---|---|---|
| Running | `now > deadline` | Log timeout via `on_timeout_message_provider()`; signal stop_token; mark state=DeadlineSignalled; record `signalled_at = now()`.  Continue to next entry. |
| Running | `now ≤ deadline` | No action.  Continue. |
| DeadlineSignalled | already observed on a PRIOR tick (`signalled_at < this_tick_started`) | Log abnormality ONCE (record `abnormality_logged=true`).  Continue. |
| DeadlineSignalled | signalled THIS tick or `abnormality_logged` already true | No action.  Continue. |

Entries with body completion (Done / Failed / stop_token honored) remove themselves from the registry when the loop exits — the sweeper just doesn't see them on the next tick.  No special "Exited" state needs to be observed by the sweeper.

The sweeper logs in **serial order**.  No two deadline-fires interleave; no two abnormality logs interleave.

### 6.3 Sweep interval is the natural grace window

The body has between **1× and 2× sweep interval** between the sweeper's "stop signal sent" and "abnormality logged."  Concretely with sweep=100ms:
- Best case (stop signalled just before next tick): ~100ms grace.
- Worst case (stop signalled just after a tick): ~200ms grace before abnormality is observed.

If 100ms is too tight for the workloads we have, dial up the sweep interval at the registry's lifecycle-module config.  No per-spawn-site grace tuning — one process-wide sweep cadence.

### 6.4 Lifecycle of the registry entry

- **On spawn_bounded:**  insert entry `{name, deadline = now() + total_timeout, message_provider, completion, state = Running, signalled_at = nullopt, abnormality_logged = false}` under the registry's lock.  Spawn loop thread.
- **Loop thread on natural completion (Done / Failed):**  fire completion (sync wait, async callback), then remove own entry from registry under the lock.  Thread quits.
- **Loop thread on stop_token honored:**  fire completion with nullopt, then remove own entry from registry under the lock.  Thread quits.
- **Loop thread refuses to exit (body breaks the non-blocking contract):**  entry remains in registry indefinitely; subsequent sweeps see DeadlineSignalled state and take no further action.  Process teardown reaps the stuck thread.

The registry's lock is held for short critical sections only — insert, remove, snapshot for sweep.  The sweep itself can iterate over a snapshot without holding the lock for the per-entry log calls.

### 6.5 Why the sweeper thread is a `ThreadManager`-managed thread

The sweeper thread is owned by the `BoundedThreadRegistry` lifecycle module.  It uses the existing `ThreadManager` Shutdown Contract (HEP-CORE-0031 §4.1) for clean teardown:

- `with_active_loop` bracket around the sweep loop.
- Quiescence declared on natural exit (when the registry is torn down).
- `wait_for_quiescence` from the lifecycle-module teardown caller honors the bracket.

On process teardown, the registry module:
1. Signals stop_token on EVERY remaining bounded-thread entry (regardless of state).
2. Waits for the sweeper thread to quit (it observes the lifecycle stop, exits its loop).
3. Existing TM bounded-join handles waiting for any remaining bounded-thread loops to honor their stop_token (or hit the bounded-join timeout, in which case the existing leak-aggregator records the detach).

---

## 7. Lifecycle integration with HEP-CORE-0031

### 7.1 Loop thread is a ThreadManager thread

The loop is just a TM-managed thread.  It honors the existing Shutdown Contract (§4.1):
- Bracket: the entire loop body lives inside `with_active_loop`.
- Quiescence: declared on natural exit (Done / Failed) AND on cancellation.
- `wait_for_quiescence` from the teardown caller works as today.

No new bracket discipline needed; the existing one covers loop threads transparently.

### 7.2 Registry is a process-global lifecycle module

Proposed: `BoundedThreadRegistry` lifecycle module.

- Process-singleton, like `Logger`, `KeyStore`.
- Owns ONE sweeper thread (managed by the module's own `ThreadManager`).
- Owns the registry of bounded-thread entries.
- `spawn_bounded` registers entries; entries are removed by the loop on natural exit.
- Teardown: signal stop_token on EVERY active bounded thread, stop the sweeper thread.  Bounded threads honor stop and exit; existing TM bounded-join handles the wait.

Rationale for process-global (not per-TM): a process can have many ThreadManagers (per role, per queue, plus singletons).  One sweeper thread + one registry is simpler than per-TM sweepers and scales better when many ThreadManagers each have a few bounded threads.

### 7.3 Interaction with the existing Shutdown Contract

Bounded threads add a NEW failure mode to the contract: **stop_token signalled but body not yet exited**.  The existing `context_valid_` beacon (§4.1.0 fallback) covers post-timeout callback safety.  Bounded threads inherit the beacon for free since they're TM-managed.

The abnormality log in §3 is the framework's escape hatch for this failure mode — it surfaces the body-contract violation to operators.

---

## 8. Migration plan

Two passes: first deploy as bounded-sync (no caller-side FSM change), then upgrade to truly-async where it pays off.

### Pass 1 — bounded-sync (no FSM change)

| Site | Current shape | Pass-1 shape | Effort |
|---|---|---|---|
| Consumer SHM dial (#272) | sync loop in `apply_consumer_reg_ack_shm_` | 6-state step function (Connect / SendHello / RecvChallenge / SendResponse / RecvFd / ActivateQueue) | medium |
| Producer L2c broker pre-confirm | sync `broker_query` callback in `ShmAttachOrchestrator::accept_and_serve_one` | 2-state step function (SendReq / RecvAck) | small |
| HUB_TARGETED_REQ (#75) | not yet implemented | designed as bounded-sync from the start | n/a yet |
| api.crypto.* (#247) | not yet implemented | bounded-sync only (script callers can't deal with callbacks) | n/a yet |

After Pass 1: BRC-poll-thread block in #272 PERSISTS but call site code is cleaner; observability unified; teardown integration is consistent.

### Pass 2 — truly-async + FSM amendments

| Site | FSM change | Pays off | When |
|---|---|---|---|
| Consumer SHM dial (#272) | add `RegistrationState::RegAckPending`; teach `any_presence_authorized()` to wait for `Authorized` | BRC poll thread released; concurrent role startup parallelizes | under REVIEW-B (#274) or as part of #262 |
| #262 mutual auth | same RegAckPending state — Frame 3 added to dial step machine | producer-side accept loop can interleave consumers | designed async from day one once Pass 1 lands |
| Producer L2c broker pre-confirm | accept thread services NEXT consumer while previous broker_query is in flight | parallel pre-confirm | optional — complicates the accept-loop FSM, skip until justified |

Pass 2 lifts the BRC-block, but is a real FSM change at each call site.  Sequencing matters: framework lands → Pass 1 migration → REVIEW-B validates → Pass 2 begins.

---

## 9. Open question — implementation-level only

**Sweep interval default.**  Proposed 100ms.  Trade-off:
- Smaller (50ms): faster deadline detection, tighter abnormality grace (~50-100ms), more sweeper CPU.
- Larger (250-500ms): looser deadline detection (deadlines fire up to one interval LATE), more body grace, lower CPU.

For the workloads at hand (consumer dial: total_timeout ~4s; producer pre-confirm: ~2s; future RPCs: similar order), 100-200ms is small relative to the total budgets and gives bodies a comfortable cooperative-exit window.  Probably configurable globally at the lifecycle module; not per spawn site.

**Everything else resolved** in design discussion 2026-06-23:
- ~~Per-step bound enforcement~~ — no, contract with programmer.
- ~~Backoff cadence for stuck threads~~ — no cadence, single total timeout.
- ~~What to do for stuck threads~~ — log once, accept the leak.
- ~~Completion callback as its own state~~ — no, thread is in inspection until it quits (including callback execution).
- ~~Per-tick loop sleep~~ — no, step function provides own pacing.
- ~~Kernel timers vs sweeper~~ — sweeper, for kernel resource bounded-ness + deterministic log ordering + sweep-interval-as-natural-grace.

---

## 10. Out of scope (for this amendment)

- C++20 coroutines as the body primitive.  Step functions can be refactored into coroutines later if the codebase adopts them broadly.  Not needed for the contract.
- Replacing existing TM threads (BRC poll, ZmqQueue recv, etc.) with bounded threads.  Those run for the lifetime of the owner; they're not bounded-deadline work.  Keep them as today.
- A general async/future framework.  This is targeted at bounded-deadline work, not async I/O at large.

---

## 11. Next steps

1. **Designer review** of this v2 draft.  Confirm shape, decide §9 timer implementation flavor.
2. **Promote to HEP-CORE-0031 amendment** as a new section (probably §4.4 "Bounded-Deadline Threads" between current §4.3 process inventory and §5 lifecycle thunk design).
3. **Implement** the primitive (`ThreadManager::spawn_bounded` + `BoundedThreadRegistry` lifecycle module + kernel timers + observability metrics).
4. **Pass 1 migration**: consumer SHM dial (#272) → bounded-sync.  Validates the body-step-function pattern on a concrete protocol.
5. **Pass 2 begin**: under REVIEW-B (#274), evaluate truly-async + FSM amendment for #272.  Decide based on whether the BRC-block has caused operational pain.
