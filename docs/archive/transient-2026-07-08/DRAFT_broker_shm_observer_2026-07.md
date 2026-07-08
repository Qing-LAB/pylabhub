# Broker SHM channel observer — design decisions

> **Status:** DRAFT, 2026-07-03.  Captures the D1–D4 designer
> decisions taken during #317 C.2 planning.  D5 pending; not yet a
> HEP amendment.  Once #317 C.2/C.3 ship end-to-end, this doc
> promotes to a HEP-CORE-0041 §D1(d) amendment + §10.5 update.
> **Scope:** the broker's persistent per-channel observation
> mechanism that replaces the pre-#316 `datablock_get_metrics(name)`
> path retired under HEP-CORE-0041's memfd + SCM_RIGHTS model.

## Background

Pre-#316, the broker read SHM channel metrics by calling
`datablock_get_metrics(channel_name)` at
`broker_service.cpp:6098`.  That primitive did a name-based
`shm_open("/pylabhub_...")`, mapped the header page, read the
metrics struct, unmapped, closed.  Per query, cheap, no state.
The pattern was "**broker opens by name, on demand**" — call it
option (b-old) in the D3 tradeoff table below.

HEP-CORE-0041 Phase 1i deleted the well-known name.  The SHM
segment is now created via `memfd_create` (anonymous) and handed
to consumers as a bare fd over a per-channel authenticated Unix
socket.  The broker has no way to open the segment by name —
there IS no name.  Metrics observation must acquire an fd through
the same authenticated handshake consumers use.

Task #317 is the reintroduction of broker observation on the new
model.  Phase A (`b3d5e36d`) shipped the C API primitive
`datablock_get_metrics_from_fd(int fd, DataBlockMetrics*)`.
Phase B (`da2a5e76`) shipped `DataBlockObserverHandle`.  Phase
C.1 (`48b12cc5`) added the `role_type` wire hook.  Everything is
in place for the broker to hold observation fds; the task's job
is to actually put fds in the broker's hands.

## Decisions

### D1 — trust anchor for the observer handshake

Producer's `AttachProtocolAcceptor` must verify that the party
requesting an observer attach IS the broker.  Options considered:

| Option | Pubkey source | Ephemeral | Wire change |
|---|---|---|---|
| a | Broker's ZMQ identity keypair | No (permanent) | none — already on REG_ACK |
| b | Separate operator-provisioned observer keypair | No | new REG_ACK field + operator burden |
| c | Ambient SO_PEERCRED.uid + role_type=observer | N/A | none, but weak (same-UID squat) |
| **d** ✅ | **Broker-generated at each startup** | **Yes** | **new REG_ACK field (broker_observer_pubkey_z85), zero operator burden** |
| e | Per-channel ephemeral keypair | Yes, per channel | overkill for one broker |

**Decision: (d).**  Broker generates a fresh CURVE keypair at
startup via `keystore.generate_and_add_identity("broker.observer")`
(the primitive shipped in `d6f5d621`).  Broker echoes the pubkey
on REG_ACK as `broker_observer_pubkey_z85`.  Producer stashes it
and uses it as the trust anchor when verifying observer
handshakes.

**Compromise scope if broker observer seckey leaks.**  Attacker
can impersonate broker to producers AS AN OBSERVER (read-only fd
handoff, header-only metrics).  Not a data-plane compromise.
Rotation on every broker startup limits window to broker's
lifetime.  Not tied to broker's ZMQ identity — separate blast
radius.

**Primitive shipped.**  `KeyStore::generate_and_add_identity(name)`
in commit `d6f5d621`.  Design notes:
`docs/tech_draft/DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md`.

### D2 — producer-side storage of broker observer pubkey

| Option | Location | Update semantics on broker restart |
|---|---|---|
| a | Constructor arg on `AttachProtocolAcceptor` | Static — stuck with stale key after broker restart |
| **b** ✅ | **Settable field on `RoleAPIBase`** | **Setter fires on every REG_ACK — auto-refresh** |
| c | KnownRolesStore-shaped lookup | Overkill for one entry |

**Decision: (b).**  Same shape as `set_strict_abi_mismatch`
(#327) and `set_shm_require_mutual_auth` (#262).  Producer's
`register_producer_channel` extracts `broker_observer_pubkey_z85`
from the REG_ACK response, calls
`set_broker_observer_pubkey_z85(...)`.
`AttachProtocolAcceptor` reads it via accessor callback at
handshake time — no rewiring on broker restart.

**Shipped in `f7d3a51e`.**  Storage + extraction wired.  Setter
is thread-safe (shared_mutex).  Extraction is backward-compatible
(empty pubkey means pre-#317 broker — observer path stays "not
yet implemented" and legacy behaviour holds).

### D3 — broker dial timing

The old model (b-old): open by name on demand, per query.  Under
the new model this requires a full authenticated handshake per
query — prohibitively expensive.

| Option | Trigger | Cost per query | Cost per channel lifetime |
|---|---|---|---|
| **a** ✅ | **Immediately after producer REG_REQ succeeds** | **~0 (fd already cached)** | **1 handshake** |
| b | Lazy — first BROKER_SHM_INFO_REQ | Full handshake (10s of ms) | 1 handshake |
| c | Per query (mimics old) | Full handshake every query | N queries |

**Decision: (a).**  The old code's INTENT was "broker always
able to observe channel metrics."  Its MECHANISM was "open by
name."  We preserve the intent using the mechanism the new
authenticated model demands (persist the connection).

**Concrete flow.**  Broker's poll thread picks up the successful
PRODUCER REG_REQ for a SHM channel (not consumer — the producer
is the one creating the memfd; observer path shadows the consumer
path but the trigger fires earlier, at producer-registration time
so we're ready when consumers/queries arrive).  It spawns an
observer-attach task that:
1. Dials producer's `shm_capability_endpoint` (Unix socket).
2. Runs the 2-frame or 3-frame handshake with `role_type=observer`.
3. Receives the memfd via `SCM_RIGHTS`.
4. Stashes the fd in a broker-side `unordered_map<channel_name, ObserverEntry>`.

Handshake failures are logged (`event=BrokerObserverAttachFailed`)
and the map entry stays absent — `collect_shm_info` falls back to
`metrics_source="unavailable"` OR `"heartbeat"` (§C.3 to define).

### D4 — teardown timing (cross-platform)

Broker holds a persistent connection per SHM channel; the fd
must be closed when the channel ends.  End cases:

- Producer deregisters cleanly.
- Producer crashes (SIGKILL, seg fault).
- Producer partitions (no signal to broker).
- Broker shuts down.

Producer clean deregister is already handled by the presence
FSM.  Producer death without notification is the hard case.

| Option | Trigger | Latency | Correctness | Portability |
|---|---|---|---|---|
| a | Heartbeat-timeout sweep (existing FSM) | 10s–30s | Always eventually closes | Portable (Linux + non-Linux) |
| b | EPOLLHUP on observer socket | ms | Immediate | Linux-only primitive |
| **c** ✅ | **Both — whichever fires first** | **ms on Linux, seconds elsewhere** | **Belt-and-suspenders** | **Portable via abstraction** |

**Decision: (c) with cross-platform abstraction.**

**Cross-platform peer-death primitive.**  EPOLLHUP is Linux-only.
FreeBSD/macOS need `kqueue` (`EVFILT_READ` + `EV_EOF`); Windows
needs `WSAPoll` (`POLLHUP`) or `WaitForMultipleObjects` on the
pipe handle.  All four have the same abstract shape: "wake me
when the peer socket/pipe disappears."

Introduce a `PeerDeathWatcher` interface in
`src/utils/security/` (or beside `shm_capability_channel/`):

```cpp
class PeerDeathWatcher {
public:
    virtual ~PeerDeathWatcher() = default;
    /// Register: fires `on_peer_gone` exactly once when the OS
    /// notices the peer has closed the socket / process died.
    /// Idempotent — safe to unwatch even if the callback has
    /// already fired.
    virtual void watch(int fd_or_handle,
                       std::function<void()> on_peer_gone) = 0;
    virtual void unwatch(int fd_or_handle) = 0;
};
```

**Backends and rollout:**

| Platform | Backend | Ships with |
|---|---|---|
| Linux | `epoll_wait` (piggyback on broker poll thread if fits) | #317 C.2 |
| Non-Linux fallback | no-op — never fires callback, teardown falls to heartbeat safety net | #317 C.2 |
| FreeBSD | `kqueue` + `EVFILT_READ` + `EV_EOF` | #260 (backend task) |
| macOS | `kqueue` (same as FreeBSD) | #261 (backend task) |
| Windows | `WSAPoll` + `POLLHUP` (Windows 10+ AF_UNIX) OR `WaitForMultipleObjects` on named pipes | #262 (backend task) |

Broker code registers via the interface, no `#ifdef __linux__`
in broker paths.  On unsupported platforms the callback simply
never fires; the heartbeat-timeout safety net (option (a))
closes the fd within seconds.  Correctness preserved everywhere;
latency degrades to "heartbeat interval" on non-Linux until each
platform's backend lands.

**Race between clean deregister and peer-death callback.**  Both
paths call the same teardown function.  Teardown is idempotent
via `std::atomic<bool> torn_down` on the ObserverEntry:

```cpp
struct ObserverEntry {
    int fd{-1};
    std::atomic<bool> torn_down{false};
};

void teardown_observer(ObserverEntry &e) {
    bool expected = false;
    if (!e.torn_down.compare_exchange_strong(expected, true))
        return;  // already torn down by the other path
    if (e.fd >= 0) ::close(e.fd);
    e.fd = -1;
    // ... erase map entry under broker's channel lock
}
```

Whichever of {presence FSM → Disconnected, `PeerDeathWatcher`
callback} fires first wins the CAS; the second no-ops.

### D5 — producer opt-out knob (in scope, pending impl)

**Decision (agreed earlier in conversation):**
`producer.shm_metrics_observer: true|false` under `startup`
config, default `true`.  When `false`, producer's
`AttachProtocolAcceptor` refuses observer handshakes with a
clear diagnostic; broker's `PeerDeathWatcher` fires immediately
(socket closed by producer); broker falls back to
`metrics_source="heartbeat"` or `"unavailable"`.

Same wire shape as `strict_abi_mismatch` and
`shm_require_mutual_auth` in `startup_config.hpp`.
Implementation is mechanical — same 4-site pattern as those:
- Parse in `parse_startup_config`.
- Setter on `RoleAPIBase`.
- Wire in the three role hosts.
- Read at handshake time.

Deferred to slice C.2.d.

## Concrete impl slice map for #317 C.2

Following commits, in order:

| Slice | Scope | Ship gate |
|---|---|---|
| D1 slice A ✅ | KeyStore `generate_and_add_identity` primitive | `d6f5d621` |
| D2 slice ✅ | RoleAPIBase field + REG_ACK extraction (dormant — no ACK carries the field yet) | `f7d3a51e` |
| C.2.a | Broker startup generates observer keypair; REG_ACK builder emits `broker_observer_pubkey_z85` | Lights up D2 extraction |
| C.2.b | Producer's `AttachProtocolAcceptor` replaces "not yet implemented" throw with observer verification path | Requires C.2.a on wire |
| C.2.c | `PeerDeathWatcher` interface + Linux epoll backend + non-Linux fallback | Depends on nothing new; can slot before C.2.a |
| C.2.d | Broker-side observer dial + fd cache map + teardown | Requires C.2.a + C.2.b + C.2.c |
| D5 | Producer `startup.shm_metrics_observer` config knob | Requires C.2.b |
| C.3 | `collect_shm_info` fd lookup + `metrics_source` field | Requires C.2.d |
| C.4 | L4 tests (e2e, opt-out, crash-safety) | Requires C.2.d + C.3 |
| C.5 | HEP-0041 §10.5 status sync | Docs only |

Rough sizing: C.2.a ~50 LOC, C.2.b ~80 LOC (mirror consumer path),
C.2.c ~150 LOC (interface + Linux backend + fallback + basic
L2 tests), C.2.d ~200 LOC (worker thread + fd cache map +
teardown + scope-guard for partial-attach), D5 ~40 LOC, C.3
~50 LOC, C.4 ~200 LOC + fixture work.  Total ~800 LOC + tests.

Sizing incorporates risks-1/2/3 mitigations from the review
above.  Risk-1 (worker thread for observer-attach) adds ~30 LOC
to C.2.d over the naive impl; worth it to keep broker poll
thread responsive.

## Risks flagged during self-review

1. **Broker poll thread synchronous handshake latency.**  Under
   D3(a), the broker dials during producer REG handling.  If a
   producer registers N SHM channels in a burst, the poll thread
   runs N handshakes sequentially before servicing the next RPC.
   Each handshake is ~10 ms locally, ~100 ms cross-node worst
   case.  Mitigation: run the observer-attach on a detached
   worker thread (not the poll thread), broker just enqueues
   "please observe channel X" work.  If load ever exceeds even
   that, sibling task #282 (BRC poll thread offload) applies.
   **Ship default: worker thread.**

2. **Partial-handshake fd leak.**  If the observer-attach fails
   between "socket connected" and "fd received via SCM_RIGHTS,"
   the connect fd stays open until scope-exit RAII closes it.
   Mitigation: wrap the observer-attach in a scope guard
   (`std::unique_ptr<int, void(*)(int*)>` or the existing FdGuard
   pattern) so any early return / throw closes the connect fd.
   The memfd (received via SCM_RIGHTS) has its own guard.

3. **Broker shutdown ordering.**  Observer connections hold
   references into `keystore.with_seckey("broker.observer", ...)`
   callback scope.  If KeyStore is torn down before observer
   connections, the callback fires against a dead reference.
   Mitigation: broker's dtor tears down observer connections
   FIRST (close all fds, unwatch all PeerDeathWatcher entries,
   join the worker thread) THEN releases KeyStore.  Follow the
   RAII pattern of the existing BRC teardown for reference.

4. **Multiple channels per producer.**  A producer with multiple
   SHM channels shares one identity keypair but has one
   `AttachProtocolAcceptor` per channel.  The observer pubkey
   the producer trusts (`broker_observer_pubkey_z85`) is set
   ONCE on `RoleAPIBase` (per role instance).  All channels
   trust the SAME broker.  This IS what we want — one broker,
   one observer key.  Verified consistent with D2.

5. **Interface placement (bikeshed).**  Doc puts
   `PeerDeathWatcher` under `src/utils/security/`.  It's not
   really a security primitive; it's an OS abstraction.  Better
   home: `src/utils/os/` or `src/utils/lifecycle/`.  Decide when
   the code lands.  Not blocking.

6. **Fallback backend on non-Linux — clean deregister still
   works.**  My concrete flow said "no-op — never fires
   callback."  That's teardown-via-heartbeat only for the
   crash/partition case.  Clean deregister STILL fires teardown
   via the presence FSM path.  So on non-Linux the actual gap
   is only "producer crashes without deregister → heartbeat
   timeout window before broker closes fd," not "broker never
   closes fd."  This is acceptable degradation.

7. **Observer socket vs consumer's data attach socket.**  Both
   connect to the producer's `shm_capability_endpoint`.  The
   acceptor already spawns one thread per accept, so
   simultaneous consumer + observer connections work by
   construction.  Verified consistent with the existing
   `AttachProtocolAcceptor` shape.

## Related decisions elsewhere

- **D1 primitive → HEP promotion path.**  When script crypto API
  (#247) lands, the `generate_and_add_identity` primitive
  formalizes as HEP-CORE-0043 (or HEP-CORE-0040 §8.6 amendment).
  See `DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md`.
- **D4 `PeerDeathWatcher` → cross-platform SHM backends.**  Ships
  as a reusable primitive; #259/#260/#261 (FreeBSD/macOS/Windows
  SHM backends) also need peer-death detection for their own
  paths.  If those tasks land first, they can reuse this
  interface directly.
- **D3 dial timing → BRC poll thread offload (#282).**  If the
  observer-attach RPC turns out to block the broker's poll thread
  under load, #282's offload work applies here too.

## Change log

- **2026-07-03 (initial draft):** captures D1–D4 conversation
  decisions plus cross-platform PeerDeathWatcher abstraction.
  D5 known but not fully sketched — mechanical, awaits impl.
