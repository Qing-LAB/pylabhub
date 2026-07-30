# HEP-CORE-0048: Debug Diagnostics and the Last-Resort Trace

| Property         | Value                                                    |
| ---------------- | -------------------------------------------------------- |
| **HEP**          | `HEP-CORE-0048`                                          |
| **Title**        | Debug Diagnostics and the Last-Resort Trace              |
| **Author**       | Quan Qing, AI assistant                                  |
| **Status**       | Implemented                                              |
| **Category**     | Core                                                     |
| **Created**      | 2026-07-27                                               |
| **C++-Standard** | C++20                                                    |

---

## Implementation status

Implemented. The trace lives in the debug module, where `panic()` can reach it;
`lifecycle` is its first client and owns no storage of its own.

| File | Layer | Description |
|------|-------|-------------|
| `src/include/utils/debug_info.hpp` | L1 (public) | `trace_add` / `trace_print` / `trace_clear`, `PLH_PANIC`, `PLH_DEBUG`, `panic()` |
| `src/utils/core/debug_info.cpp` | impl | the buffer, its three atomics, backtrace emission |
| `src/include/utils/lifecycle.hpp` | L1 (public) | `LifecycleManager::critical_report` — lifecycle's single wrapper |
| `cmake/ToplevelOptions.cmake` | build | `PLH_DEBUG_TRACE_BYTES` (default 16384) |

### A note on how this HEP was first written

The first revision of this document specified a 64 KiB buffer, a `copy_out()`
accessor handing bytes to callers, an "anomaly" flag inside the buffer gating
whether it printed, and a chunked drain. None of that was designed — the size
was picked arbitrarily, and each of the other three existed only to manage a
problem the size created: 64 KiB is too large to print in one write, too noisy
to emit on a clean run, and `copy_out` forced a SECOND 64 KiB array
(`static thread_local`) in every thread that dumped.

It also named this module as the trace's future home while the implementation
went into `lifecycle`, above `panic()` — so `panic()` could not reach the buffer
it was supposed to print, and "relocate it" became a follow-up task for a
layering error that should never have been introduced.

Recorded because the failure is instructive and not obvious in the result: an
invented constant can manufacture an entire subsystem of machinery to service
it, and every piece of that machinery looks locally justified.

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
| Concurrency | **Lock-free.** A writer reserves a byte range with an atomic compare-exchange, then fills only its own slice. |
| Lifetime | **Never destroyed.** A detached worker may still append after static destruction has begun; tearing the buffer down underneath it would make the diagnostic aid a use-after-free on the unhappy path. Every member is therefore trivially destructible and constant-initialised, which also removes any static-initialisation-order hazard. |
| Locking | **None, and this is required rather than clever.** A lock — of any kind — can be held by a worker that is wedged or killed mid-append, and every later reader would then block on the corpse. The reader that matters most is the one at the end of teardown whose entire job is to explain that wedge. A bounded-retry spin "solves" this by giving up and printing nothing, which loses exactly the record being sought. Reserve-then-fill has no holder to die. |
| Torn reads | A print can catch a slice that is reserved but not yet filled; the array is zero-initialised, so that reads as NULs, not garbage. A torn tail is the accepted price of never blocking. |

### Writer identity

Every appended line carries the native thread id.

This is not decoration. One buffer receives writes from the finalize thread and
from every shutdown worker, and a worker detached at its deadline keeps writing
while the *next* module tears down — so lines from different subsystems
genuinely interleave. Unattributed interleaving is worse than no record: it
invites a confident wrong conclusion. A worker's entry line is what binds a
thread id to the subsystem it was tearing down.

### Emission

**Printing drains the buffer.** This single property is what makes every printer
correct without coordination: whoever prints does not need to know whether
anyone printed already. Drained → a later panic prints nothing. Died early →
the panic prints the remainder. No duplication, no bookkeeping.

**It drains by FREEZING, not by zeroing.** One atomic exchange takes the current
length and parks the counter at a sentinel no append can fit into. For the
duration of the write, every `trace_add` therefore sees a full buffer and takes
its ordinary drop path.

The two simpler shapes are both wrong, in opposite directions:

| Reset | Failure |
|---|---|
| Length zeroed **before** the write | A concurrent writer sees `len == 0`, restarts at offset 0, and overwrites the bytes being emitted — corrupting the report actually being printed. |
| Length zeroed **after** the write | A concurrent writer appends past the printed region, and the reset then discards it. The entry is lost with nothing said about it. |
| **Freeze, emit, unfreeze** | Emitted bytes cannot be touched, and refused entries land in the dropped-byte counter — so the loss is bounded, reported, and never silent. |

The reserve loop needs no special case for the transition: a writer that already
read a stale length simply loses its compare-exchange to the freeze and refreshes
on retry, at which point it drops correctly.

Two details this forces, both easy to get wrong:

- **The sentinel must be distinct from the capacity**, not a reuse of it. "The
  buffer is legitimately full" and "a print is in progress" have to be
  distinguishable, or two printers — `finalize()` on the main thread while the
  SIGTERM watcher fires — would both see "full" and both emit the whole buffer.
  A second caller seeing the sentinel returns instead.
- **The reserve guard must be written as a subtraction** (`used > kTraceBytes -
  old`), not an addition. With the sentinel at `SIZE_MAX`, `old + used` wraps and
  yields a false "there is room".

**Dropped bytes are subtracted, not zeroed.** `trace_print()` reports the count
it read and then subtracts exactly that many. Entries refused *during* the write
are counted while it is still emitting; a blind reset would throw those counts
away, so the loss would happen and nobody would ever learn of it. What remains is
reported by the next print.

**One entry point, and the decision lives inside it.** `trace_print()` always
drains, and decides for itself whether to emit:

| Report state | Debug build | Release build |
|---|---|---|
| **dirty** — something went wrong | prints | **prints** |
| clean | prints | silent, buffer left intact |

A dirty report is never suppressed in any build — that is the case the facility
exists for. A clean one prints only where `PLH_DEBUG` is compiled in, because
that switch already means "this build is for someone working on the code", and
that is exactly who benefits from seeing what a healthy exit looks like. An
operator running `plh_hub --list` gets nothing.

On the silent path the content is **left in the buffer** rather than dropped: if
something later marks dirty, its report carries this context too, and there is
nothing to gain by clearing storage in a process on its way out.

### The dirty latch

`trace_mark_dirty()` is a **one-way latch**. Any number of reporters may set it;
**nothing** clears it — not `trace_clear()`, not `trace_print()`.

The asymmetry is the design. A clearable flag would let a subsystem that tore
down cleanly *after* someone else's failure erase the record of that failure,
which is precisely the loss this buffer exists to prevent. The accident already
happened; no later phase completing normally makes it un-happen.

It is owned by **this module**, not by any client. The debug module depends on
nothing and is usable on its own, so a program that never touches lifecycle can
still declare its exit unclean and have the report survive. An earlier revision
put the flag in lifecycle, which denied it to every other user and tied a
general facility to one caller.

Callers never consult the latch before printing. `panic()` and the fatal-signal
path simply **mark dirty first** — arriving there is itself the thing that went
wrong — and then print. That keeps one rule in one place instead of a
build-conditional repeated at every print site.

**Startup is out of scope.** This is an EXIT life line: it exists so a process
that fails to *quit* can still say how far it got. A startup failure is a
different situation — the logger is coming up or already up, and the caller
reports it directly. Writing startup steps here would spend a fixed, scarce
budget on the phase that does not need it and could push out the teardown record
that does. `initialize()` therefore uses `PLH_DEBUG` and never touches the
buffer, which also removes a path by which startup could clear an async
unload's timeout record.

**Signal handlers are not a special case.** Per HEP-CORE-0020 §4.1 the
`signal()` handler sets an atomic flag and writes one byte to a self-pipe; all
I/O runs on the watcher thread. So `trace_print()` is only ever called from
ordinary threads and needs no async-signal-safe variant. It uses `write(2)`
rather than stdio regardless, because stdio takes a lock and may allocate, and
on a panic path either can be the reason we are there.

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
[t<tid>|<monotonic_us>us] <caller-supplied content>\n
```

Subsystems are expected to build **thin wrappers** over the raw append so their
own context is added consistently rather than restated at every call site — see
Clients below.

## API

Four functions. There is no class, no accessor object and no handle: the buffer
is process-global state and pretending otherwise only adds ceremony.

| Symbol | Caller | Purpose |
|---|---|---|
| `debug::trace_add(msg)` | Subsystem wrappers | Record one entry. Stamps thread id + timestamp, guarantees newline termination. Never allocates, never throws, never blocks. |
| `debug::trace_print()` | `finalize()`, SIGTERM watcher, `panic()` | Write everything to stderr and reset, in one atomic step. Emits nothing when empty. |
| `debug::trace_mark_dirty()` | Any reporter, on any failure | Latch the report as dirty. Set-only; nothing clears it. |
| `debug::trace_is_dirty()` | Diagnostics, tests | Read the latch. Callers do NOT need this before printing. |
| `debug::trace_clear()` | Rare — a caller that knows its entries were unremarkable | Discard entries and the dropped count. Does **not** reset the latch. |
| `debug::trace_write_stderr(p, n)` | `panic()` only | Raw `write(2)`, so the panic message goes out the same door as the trace. Not for general use. |

Dropped bytes are **not** exposed as a query. Both loss modes — an entry longer
than `kTraceEntryBytes`, and a pool with no room left — accumulate into one
counter that `trace_print()` reports as a trailing `[trace] dropped=N bytes`
line. One counter rather than two, because two would need reconciling; reported
rather than queryable, because the only useful moment to know is when the record
is being read.

Sizes are two constants and no more: `PLH_DEBUG_TRACE_BYTES` (cmake, default
16384) for the pool, and `kTraceEntryBytes` (256) bounding the per-entry stack
buffer each caller formats into. The default was sized against the measured
write volume of one full teardown — roughly 30 framework markers at 60-90 bytes
for a role process, plus module-internal markers — not chosen by feel.

---

## Clients

Subsystems should not call `trace_add` scattered through their code. Each wraps
it once, adding the context only that subsystem knows, so every entry from it is
shaped alike and no call site can forget.

**Lifecycle** (HEP-CORE-0001) is the reference client. It exposes one wrapper —
`LifecycleManager::critical_report(step)` — used by its own teardown phases and
by the async shutdown workers it spawns. It owns **no buffer, no counter, no
lock and no flag**; the reporting is not threaded through the teardown call
chain as a parameter either. It records phase boundaries, per-module dispatch,
worker entry/exit, deadline overruns and detaches, marks dirty on each of those
failures, and calls `trace_print()` unconditionally at the end of `finalize()` —
making no build-conditional decision of its own.

The wrapper is deliberately **not** called `narrate`. This is a life line, not a
progress report, and a name that invites casual use is how a forensic buffer
becomes an unfiltered log.

**Logger** (HEP-CORE-0004) writes to `debug::trace_add` **directly**, not via
lifecycle's wrapper — it must not depend on `pylabhub::utils`, and the buffer
belongs to the debug module regardless. Its shutdown brackets the two steps that
can block: the worker-thread join and the callback-dispatcher shutdown. This is
the case that motivated the facility: `Logger::Impl::shutdown` cannot use
`LOGGER_*` (it is joining the thread that would drain the queue), and its step
markers were `PLH_DEBUG` only — compiled out of Release — which is why the hang
recorded as open #93/#242 has never been diagnosable from a release log.

**ZMQContext** brackets `ctx->shutdown()` and `zmq_ctx_term()`, both of which
block indefinitely if a foreign thread still holds a socket. The module's only
log line came after both, so a wedge left no record of which one.

**ThreadManager** (HEP-CORE-0031) **mirrors** its unclean-drain report into the
trace rather than moving it. `LOGGER_ERROR` remains right for anyone reading
logs afterwards, but a detach means threads are still running that we gave up
waiting for; if teardown then wedges, that message dies unflushed in the
logger's queue — losing the record of the accident exactly when the accident is
what happened.

**ZapPumpThread** brackets the jthread join, whose only log line likewise ran
after the join returned.

### The rule for new clients

> Would I still need this if the process never finished shutting down?
> **Yes → `trace_add`. No → `LOGGER_*`.**

The test is *not* "is the logger up". During teardown the logger usually is up,
because dynamic modules are torn down before static ones. But `LOGGER_*` is
asynchronous: it enqueues, and a worker drains later. A process that wedges or
is killed never drains that queue. Durability, not availability, is the question.

---

## Testing implications

The contract is observable, so it is testable without mocks:

- A module whose shutdown callback overruns its deadline must produce an entry
  line with no matching exit line, naming the module — pinned by
  `LifecycleTest.ShutdownDeadlineOverrun_IsNarratedByTheDetachedWorker`.
- Emission must reach stderr in builds where `PLH_DEBUG` is compiled out. A
  Debug-only assertion passes for the wrong reason, since the debug channel is
  live there; the pin that bites is a non-Debug build.
- Print-and-drain means a second `trace_print()` after a first emits nothing.
  This is the property the whole no-coordination design rests on, so it is the
  one worth pinning directly.
- A clean `finalize()` must emit nothing at all, since it clears before
  printing. A test that asserts "some output appeared" would pass on a broken
  implementation that never clears.

---

## Related work

- **HEP-CORE-0001** — the lifecycle subsystem; reference client and owner of the
  `LifecycleManager::critical_report` wrapper.
- **HEP-CORE-0004** — the async logger. Both a client (its shutdown brackets the
  two blocking steps) and the subject of the open stall this facility exists to
  make diagnosable.
- **HEP-CORE-0020** — the interactive signal handler. Its **watcher thread**
  calls `trace_print()` after the Logger state dump: state first, then the
  sequence that led into it. The `signal()` handler itself does no I/O, which is
  why no async-signal-safe variant of the trace is required.
- **HEP-CORE-0031** — ThreadManager; mirrors its unclean-drain report here.
