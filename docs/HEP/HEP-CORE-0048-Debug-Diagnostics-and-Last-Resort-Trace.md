# HEP-CORE-0048: Debug Diagnostics and the Last-Resort Trace

| Property         | Value                                                    |
| ---------------- | -------------------------------------------------------- |
| **HEP**          | `HEP-CORE-0048`                                          |
| **Title**        | Debug Diagnostics and the Last-Resort Trace              |
| **Author**       | Quan Qing, AI assistant                                  |
| **Status**       | Partially implemented — see Implementation status        |
| **Category**     | Core                                                     |
| **Created**      | 2026-07-27                                               |
| **C++-Standard** | C++20                                                    |

---

## Implementation status

The diagnostics primitives (`PLH_PANIC`, `PLH_DEBUG`, backtrace emission) are
implemented in `src/include/utils/debug_info.hpp` and `src/utils/core/debug_info.cpp`.

The **last-resort trace** specified in this HEP is currently implemented in the
lifecycle subsystem rather than here — see `src/utils/service/lifecycle_impl.hpp`
(`LifecycleTrace`) and `src/utils/service/lifecycle_helpers.cpp`. Relocating it
to this module, and wiring `panic()` to drain it, is the outstanding work. Until
that lands, the buffer is emitted only when a lifecycle phase runs to completion,
and an abnormal exit leaves it recoverable only from a debugger or core file.

| File | Layer | Description |
|------|-------|-------------|
| `src/include/utils/debug_info.hpp` | L1 (public) | `PLH_PANIC`, `PLH_DEBUG`, `panic()`, `debug_msg()` |
| `src/utils/core/debug_info.cpp` | impl | backtrace emission |
| `src/utils/service/lifecycle_helpers.cpp` | impl | last-resort trace (to be relocated here) |

---

## Abstract

This HEP specifies the process-wide diagnostics module: the panic path, the
compile-gated debug-message path, and a **last-resort trace** — a small global
buffer that preserves information which an abnormal exit would otherwise
destroy.

---

## Motivation

A process that dies abnormally takes its explanation with it.

Two mechanisms in this codebase make that concrete, and both were observed
together in a single failure:

1. **Narration accumulated in a local buffer is lost with its frame.** Code
   that gathers a description of what it is doing and emits it at the end
   emits nothing at all if it never reaches the end.
2. **`PLH_DEBUG` does not exist outside Debug builds.** It expands to
   `do {} while (0)` unless `PYLABHUB_ENABLE_DEBUG_MESSAGES` is defined, and
   `cmake/ToplevelOptions.cmake` defines it only for Debug. Diagnostics routed
   through it are silent in every shipped configuration.

Combined, a release binary can wedge during teardown and die under a harness
timeout without emitting one word about where it stopped. The last-resort trace
exists to make that impossible.

---

## Scope — what this is, and what it must never become

**This is a last-resort buffer, not a log.**

| | Last-resort trace | Logger (HEP-CORE-0004) |
|---|---|---|
| Purpose | Survive an abnormal exit | Record ordinary runtime events |
| Written from | Shutdown / exit / panic paths only | Anywhere |
| Levels, sinks, rotation, filtering | None, by design | Yes |
| Capacity | Small, fixed, capped | Rotating files |
| Depends on Logger | **No** | — |

The independence from Logger is not incidental: Logger is itself a module that
gets torn down, so it cannot be the sink for diagnostics about teardown.

**Two consequences follow, and both are load-bearing.**

- **Capacity is not a concern, because the write span is bounded.** Nothing
  appends during normal operation, so the buffer holds one teardown's worth of
  narration rather than a process lifetime's. A fixed cap with
  truncate-on-overflow is therefore adequate; no ring or eviction policy is
  required.
- **Do not "just add a line" from ordinary code.** Every unrelated writer
  dilutes the one thing the buffer is for. Filled with general progress
  messages it stops being a last-resort record and becomes an unfiltered log
  that merely happens to survive a crash — burying the signal it was built to
  preserve under noise it was never meant to carry. Ordinary information goes
  to the Logger.

---

## Design

### The rule: append incrementally, never summarise

Each participant appends **each step as it happens**. A held string dies with
its owner, and during teardown owners are routinely abandoned: a shutdown
worker that overruns its deadline is detached, and the process itself may be
killed mid-phase. Writing immediately means no later event can retract the
record.

A module that appended `draining queue` and nothing after did not merely hang
— it hung while draining the queue. That inference is only available if the
line was written before the hang, not gathered for a report that never came.

### Storage

| Property | Rule |
|---|---|
| Location | Process-global, static, preallocated |
| Address | Fixed — a wedged or crashed process still holds the contents where a debugger or core file can recover them |
| Allocation | Never; appending must be safe where the allocator's state is suspect |
| Overflow | Capped; dropped bytes are counted so truncation is self-declaring |
| Concurrency | Appends serialised. Readers take a private snapshot — never a view into live storage |
| Lifetime | **Never destroyed.** A detached worker may still append after static destruction has begun; tearing the buffer down underneath it would make the diagnostic aid a use-after-free on the unhappy path. Every member is therefore trivially destructible and constant-initialised, which also removes any static-initialisation-order hazard. |
| Locking | An `atomic_flag` spinlock, **not** `std::mutex` — a non-trivial destructor would drag the buffer back into static-destruction order and defeat the lifetime rule. |

### Writer identity

Every appended line carries the native thread id.

This is not decoration. One buffer receives writes from the finalize thread and
from every shutdown worker, and a worker detached at its deadline keeps writing
while the *next* module tears down — so lines from different subsystems
genuinely interleave. Unattributed interleaving is worse than no record: it
invites a confident wrong conclusion. A worker's entry line is what binds a
thread id to the subsystem it was tearing down.

### Emission

**Printing drains the buffer.** This single property is what makes the emergency
path correct without coordination: whoever prints does not need to know whether
anyone printed already. Drained → a later panic prints nothing. Died early →
the panic prints the remainder. No duplication, no bookkeeping.

Two entry points over one buffer:

| Path | Caller | Behaviour |
|---|---|---|
| Normal | End of a lifecycle phase | **Always drains.** Emits only when there is something worth saying — an anomaly, or the debug channel for the verbose narrative. |
| Emergency | `panic()`, fatal-signal handler | **Always drains and always emits**, unconditionally to stderr. |

**Drain and emit are separate decisions.** A normal path that both always drains
*and* always writes to stderr would make every ordinary CLI invocation dump a
teardown narrative at the operator. Clean runs stay quiet; the record is still
consumed.

**The emergency path cannot be the normal path.** Invoked from a signal handler
it must not use `fmt`, must not allocate, and **must not take the lock** — if
the signal interrupted a thread mid-append, acquiring that lock deadlocks the
handler and loses everything. It writes with `write(2)` directly from the fixed
array and accepts a possibly torn tail, because a torn tail beats silence when
the process is dying. The storage rules above are what make this possible.

### Panic ordering

```
[PANIC] <location> -- <message>     ← what went wrong
<last-resort trace contents>        ← how we got here
<backtrace>                         ← where we are now
```

The trace precedes the backtrace: breadcrumbs explain the path, the backtrace
describes the instant.

---

## Entry format contract

The buffer is a single blob of bytes. Without a guaranteed record separator a
dump is an unreadable run-on, so **the debug module enforces the envelope** and
the caller supplies the content.

**Enforced by the API — the caller cannot omit or corrupt these:**

| Element | Why the framework owns it |
|---|---|
| **Thread id** | One buffer receives writes from many threads, and a worker detached at its deadline keeps writing while the next subsystem tears down. Lines genuinely interleave; without the writer's identity they cannot be demultiplexed, and an unattributed interleaving invites a confident wrong conclusion. A caller cannot cheaply obtain this. |
| **Timestamp** | Ordering and gap-spotting — "entered, then nothing for four seconds" is the diagnosis. Monotonic (`steady_clock`), **not** wall-clock: these order events inside one teardown and must not jump if the system clock is stepped mid-shutdown. |
| **Newline termination** | Record integrity. If a caller forgets, one is appended, so two entries can never run together into a single unparseable line. |

**Supplied by the caller — the framework does not guess at identity it does not
own:** what the entry is *about*. Task, module, request id, source. Callers
follow the project log convention (`event=<Verb> key='value'`, see
`docs/IMPLEMENTATION_GUIDANCE.md`) so trace lines stay greppable alongside
ordinary log output.

Resulting shape:

```
[trace|t<tid>|<monotonic_us>us] <caller-supplied content>\n
```

Subsystems are expected to build **thin wrappers** over the raw append so their
own context is added consistently rather than restated at every call site — see
Clients below.

## API

| Symbol | Caller | Purpose |
|---|---|---|
| `append(text)` | Subsystem wrappers | Record one entry. Stamps thread id + timestamp, guarantees newline termination. Never allocates, never throws. |
| `dump()` | Normal path | Drain; emit per the anomaly rule. |
| `emergency_dump()` | `panic()`, signal handler | Drain and emit unconditionally; async-signal-safe. |
| `dropped()` | Readers | Bytes lost to truncation; absent if the buffer could not be read — "nothing was dropped" and "could not find out" must not look alike. |

---

## Clients

Subsystems should not call `append` scattered through their code. Each wraps it
once, adding the context that only that subsystem knows — the wrapper is where
"which module" or "which request" is attached, so every entry from that
subsystem is shaped alike and no call site can forget.

**The lifecycle subsystem** (HEP-CORE-0001) is the first and, at present, only
client, and is the reference example of that pattern: it exposes its own
narration wrapper that its asynchronous shutdown workers call directly, adding
the module name the debug module has no way to know. It appends phase boundaries and per-module dispatch outcomes, its
shutdown workers append their own entry and exit, and it calls the normal dump
at the end of each phase. See HEP-CORE-0001 §"Lifecycle trace — the shared
narration pool" for how the lifecycle uses this facility and what its narration
looks like.

**Logger** (HEP-CORE-0004) is the intended next client. Its per-step shutdown
probes are currently `PLH_DEBUG` and therefore absent from released binaries;
Logger is also the module observed to exceed its shutdown deadline in the open
stall documented there, so moving those probes onto this facility is where the
diagnostic payoff is.

New clients must satisfy the Scope section above: shutdown, exit and panic
paths only.

---

## Testing implications

The contract is observable, so it is testable without mocks:

- A module whose shutdown callback overruns its deadline must produce an entry
  line with no matching exit line, naming the module — pinned by
  `LifecycleTest.ShutdownDeadlineOverrun_IsNarratedByTheDetachedWorker`.
- The anomaly emission must reach stderr in builds where `PLH_DEBUG` is
  compiled out. A Debug-only assertion passes for the wrong reason, since the
  debug channel is live there; the pin that bites is a non-Debug build.
- Drain-on-print means a second dump after a first must emit nothing.

---

## Related work

- **HEP-CORE-0001** — the lifecycle subsystem; first client.
- **HEP-CORE-0004** — the async logger; the intended next client, and the
  source of the open shutdown-stall investigation this facility instruments.
- **HEP-CORE-0020** — the interactive signal handler; the natural home for the
  fatal-signal emergency dump.
