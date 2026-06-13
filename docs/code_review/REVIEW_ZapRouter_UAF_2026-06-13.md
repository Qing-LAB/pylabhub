# REVIEW_ZapRouter_UAF — 2026-06-13

> **Task:** #215 (Slice A — ZapRouter UAF + reentrance + single-pumper).
> **Triggers:** Fresh-eye Reviewer-2 audit during task #214 / Round-3 review.
> **Scope:** `src/utils/security/zap_router.cpp` + reentrance contract on
> `src/include/utils/security/peer_admission.hpp` + threading-model
> docs on `src/include/utils/security/zap_router.hpp` + HEP-CORE-0036
> §7.4.  Pre-AUTH-2 architectural blocker: AUTH-2 (#162) wires
> `pump_one` onto the BRC poll thread, at which point each finding
> below stops being dormant.
>
> **Status:** ✅ FIXED 2026-06-13 (this commit).

---

## Findings

### F1 — UAF window on admission pointer (CRITICAL)

**Source:** `zap_router.cpp:425-444` pre-fix.
```cpp
PeerAdmission *admission = nullptr;
{
    std::shared_lock<std::shared_mutex> lk(impl_->registered_mu);
    if (auto it = impl_->registered.find(domain); it != impl_->registered.end())
        admission = it->second;
}                                       // ← lock released here
const bool ok = admission->is_peer_allowed(...);
```

`admission` is a raw pointer to the `PeerAdmission` that
`register_domain` recorded.  In production this points at a
`ZmqQueue` (registered via `ZmqQueue::start()` at
`src/utils/hub/hub_zmq_queue.cpp:1279`).  After the shared_lock releases on
line 431, a parallel `~ZapDomainHandle` on another thread can run
`unregister_domain_` (unique_lock → erase) → the queue continues
destruction → `admission` dangles → line 443 dereferences freed
memory.

Dormant today because `pump_one` only runs in test harnesses via
`ZapPumpThread`.  AUTH-2 (#162) explicitly wires `pump_one` onto the
BRC poll thread; every handshake-vs-teardown race goes live.

### F2 — Reentrant `register_domain` from inside `is_peer_allowed` (HIGH)

A future `PeerAdmission` implementer that calls back into
`ZapRouter::register_domain` from inside `is_peer_allowed` would
attempt to acquire `registered_mu` in unique mode while the same
thread holds it in shared mode (post-fix from F1).  Per
`std::shared_mutex` semantics, a thread holding shared cannot
acquire shared OR unique without releasing first — undefined
behavior, observable in practice as silent deadlock.

Today's production implementers (`ZmqQueue`, `BrokerCtrlAdmission`)
are lock-free atomic loads + hash-set lookups; they don't trigger
this.  But the surface allows it.

### F3 — Reentrant `~ZapDomainHandle` from inside `is_peer_allowed` (HIGH)

If an admission implementer transitively destroys a `ZapDomainHandle`
during the decision, the destructor's `unregister_domain_` would
take the same unique_lock and self-deadlock — OR, worse, leave a
dangling map entry pointing at admission/queue memory that's about
to be freed.  Unrecoverable: the destructor has no way to react to
a refusal.

### F4 — Single-pumper invariant has no runtime enforcement (HIGH)

`zap_router.hpp:33-38` documents "exactly ONE thread per process at
a time" — but the contract is purely textual.  A second `pump_one`
caller (e.g. an accidentally-spawned `ZapPumpThread` while the BRC
pump is also active) would silently corrupt the libzmq REP socket
FSM.  Subsequent recvs throw EFSM; no logs trace the cause.

---

## Architectural fix shape

**Option (a) + RecursionGuard belt-and-suspenders.**  Codebase doctrine
cited in `zap_router.hpp:27` (already references `RecursionGuard`
for the LifecycleManager near-miss); same pattern used in
`lifecycle_dynamic.cpp:51-60` + `json_config.cpp:69-579`.

| Site | Change |
|---|---|
| `pump_one` | (1) extend shared_lock to span the admission call → closes F1; (2) push `RecursionGuard(this)` before the call → forces F2/F3 violations to land at `register_domain`/`unregister_domain_` instead of hitting `std::shared_mutex` UB; (3) `try/catch (...)` around the call → future throwing implementers get a logged deny instead of crashing the BRC poll thread; (4) RAII `PumpScope` atomic counter (fetch_add on entry, PANIC if post-increment > 1, fetch_sub on exit) → closes F4. |
| `register_domain` | Refuse + log + inactive sentinel handle when `RecursionGuard::is_recursing(this)` → graceful failure (caller can react). |
| `unregister_domain_` | PLH_PANIC when `is_recursing(this)` → loud failure (destructor can't react to refusal; alternative is silent UAF). |
| `peer_admission.hpp::is_peer_allowed` | Doc-comment now spells out the four-part reentrance contract: synchronous-thread, no-register-callback, no-handle-destruction, bounded-time, may-throw. |
| `zap_router.hpp` file-header | Threading-model section extended with the three runtime-enforced invariants. |
| HEP-CORE-0036 §7.4 | New subsection documents the contracts the runtime now enforces + the regression tests + mutation sweep. |

---

## Regression tests

Four scenarios in `tests/test_layer2_service/workers/zap_router_workers.cpp`:

1. `round3_uaf_destructor_blocks_until_admission_returns`: a
   `BlockingAdmission` parks `is_peer_allowed` on a `std::promise`;
   a destroyer thread races `~ZapDomainHandle`; the test asserts
   the `destroyed` flag is **not** set 100 ms after admission entered
   (set only AFTER `~local` returns).  Without the lock-scope
   extension the destructor returns immediately → assertion fires.
2. `round3_reentrant_register_refused`: a
   `ReEntrantRegisterAdmission` calls `ZapRouter::register_domain`
   from inside `is_peer_allowed`; the test asserts the nested handle
   is inactive + the outer admission still succeeds.
3. `round3_reentrant_unregister_panics`: a
   `ReEntrantUnregisterAdmission` triggers `~ZapDomainHandle` from
   inside `is_peer_allowed`; death test asserts non-zero exit +
   PANIC stderr substring.
4. `round3_concurrent_pumpers_panic`: spawns two `ZapPumpThread`
   instances; death test asserts non-zero exit + PANIC stderr
   substring.

---

## Mutation sweep (each test pinned to exactly one bug)

Each mutation reverts ONE guard and confirms that EXACTLY ONE test
fails (the one designed to pin that bug); the other three keep
passing.

| Mutation | Reverted | UAF | Reg-refused | Unreg-panic | Pumpers-panic |
|---|---|:---:|:---:|:---:|:---:|
| M1 | pump_one shared_lock scope | ❌ FAIL (`destroyed=true` at 100ms) | ✅ pass | ✅ pass | ✅ pass |
| M2 | register_domain `is_recursing` check | ✅ pass | ❌ TIMEOUT (60s; `std::shared_mutex` UB → deadlock) | ✅ pass | ✅ pass |
| M3 | unregister_domain_ `is_recursing` check | ✅ pass | ✅ pass | ❌ TIMEOUT (60s; unique_lock self-deadlock) | ✅ pass |
| M4 | pump_one `PumpScope` counter check | ✅ pass | ✅ pass | ✅ pass | ❌ FAIL (worker stays alive instead of aborting) |

Each mutation isolates exactly one guard; each test fails for
exactly the reason it pins.  Confidence in the test→guard mapping:
high.

---

## Status table

| # | Item | Status |
|---|---|---|
| F1 | Shared_lock scope extension across admission call | ✅ FIXED 2026-06-13 |
| F2 | `register_domain` RecursionGuard refuse | ✅ FIXED 2026-06-13 |
| F3 | `unregister_domain_` RecursionGuard PANIC | ✅ FIXED 2026-06-13 |
| F4 | `pump_one` concurrent-pumper PANIC | ✅ FIXED 2026-06-13 |
| Test 1 | UAF destructor-blocks regression | ✅ landed; M1 confirms |
| Test 2 | Reentrant register refused | ✅ landed; M2 confirms |
| Test 3 | Reentrant unregister panics | ✅ landed; M3 confirms |
| Test 4 | Concurrent pumpers panic | ✅ landed; M4 confirms |
| Doc | `peer_admission.hpp::is_peer_allowed` reentrance contract | ✅ landed |
| Doc | `zap_router.hpp` file-header threading-model | ✅ landed |
| Doc | HEP-CORE-0036 §7.4 runtime-enforced contracts | ✅ landed |

All rows closed.  Ready for archive per
`docs/DOC_STRUCTURE.md §2.2` at the next sweep.
