# HEP-CORE-0045: Broker SHM Channel Observer

| Property | Value |
|---|---|
| **HEP** | `HEP-CORE-0045` |
| **Title** | Broker SHM Channel Observer — Real-Time Metrics via Authenticated Attach |
| **Status** | 🚧 **DESIGN AUTHORITATIVE; IMPLEMENTATION IN FLIGHT.**  D1 primitives + D2 producer-side storage + C.2.a broker keypair emission + C.2.b producer verify path all shipped.  Remaining: C.2.c (PeerDeathWatcher) + C.2.d (broker dial + fd cache) + D5 opt-out + C.3 metrics_source + C.4 L4 tests + C.5 status sync.  Promoted from `docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md` (2026-07-08). |
| **Transport scope** | **SHM data plane ONLY** (Linux `memfd_create` + `SCM_RIGHTS`; other-OS backends deferred to #259/#260/#261).  Broker→producer connection over the producer's `shm_capability_endpoint` (Unix socket).  No ZMQ equivalent is specified. |
| **Created** | 2026-07-08 (content dates back to 2026-07-03 tech draft) |
| **Depends on** | HEP-CORE-0040 §5 (KeyStore), HEP-CORE-0041 §5 (SHM capability transport + `shm_capability_endpoint`), HEP-CORE-0043 §6 (SMS asymmetric box), HEP-CORE-0044 (AttachProtocol — this HEP uses `role_type="observer"`) |
| **Related** | HEP-CORE-0036 §6.5 (`ChannelAccessIndex` — distinct trust anchor; observer path deliberately does NOT go through the consumer allowlist), HEP-CORE-0002 (DataBlock header layout — observer reads atomic counters from the header page) |
| **Trackers** | task #317 (umbrella).  Shipped slices: Phase A `b3d5e36d`, Phase B `da2a5e76`, D1 slice A `d6f5d621`, D2 slice `f7d3a51e`, C.2.a `029bbe31`, C.2.b `ce956972`.  Remaining slices tracked in §10. |

---

## 0. Status + scope

### 0.1 What this HEP covers

This HEP specifies how the broker attaches to a producer's SHM channel **as a metrics observer** — a distinct role from a data consumer — using the same underlying `AttachProtocol` handshake (HEP-CORE-0044) but with a distinct trust anchor and a distinct downstream capability (a header-page-only memfd, no data-plane access).

Coverage:

- The trust chain — how the broker proves it is the broker to a producer (§3).
- The observer capability — what the broker gets to see once attached (§4).
- The broker's dial timing and fd cache (§5).
- Producer teardown when the observer disconnects (§6).
- Operator opt-out (§7).
- Metrics wiring on the broker's admin surface (§8).
- Test coverage plan (§9).
- Slice-level implementation map (§10).

### 0.2 What this HEP does NOT cover

- **The AttachProtocol wire.**  Frames 1/2/3, MAC verification, mutual auth — all in HEP-CORE-0044.
- **The SHM capability transport.**  `memfd_create`, `SCM_RIGHTS`, `shm_capability_endpoint` binding — all in HEP-CORE-0041 §5.
- **The consumer path.**  Consumers use a distinct trust anchor (broker's `ChannelAccessIndex`).  Observer path is deliberately separate.
- **Federation / hub-to-hub metrics probing.**  Any federation observer story would be HEP-CORE-0035 amendment territory.

### 0.3 Where the code lives (as-shipped + planned)

| Piece | File | Status |
|---|---|---|
| `KeyStore::generate_and_add_identity(name) → std::string` | `src/utils/security/key_store.cpp` | ✅ Shipped `d6f5d621` |
| Broker generates + registers observer keypair at startup | `src/utils/ipc/broker_service.cpp` | ✅ Shipped `029bbe31` |
| `PRODUCER_REG_ACK.broker_observer_pubkey_z85` | `src/utils/ipc/broker_service.cpp` (emitter), `src/utils/service/role_api_base.cpp` (extractor) | ✅ Shipped `029bbe31` + `f7d3a51e` |
| `RoleAPIBase::set_broker_observer_pubkey_z85(...)` | `src/utils/service/role_api_base.cpp` | ✅ Shipped `f7d3a51e` |
| Producer `AttachProtocolAcceptor` observer verify path (`role_type="observer"` branch) | `src/utils/security/attach_protocol.cpp` (`run_producer_handshake`) | ✅ Shipped `ce956972`; preserved through 2026-07-07 refactor at `run_producer_handshake` |
| `DataBlockObserverHandle` (header-only mmap) | `src/utils/shm/data_block.cpp` | ✅ Shipped `da2a5e76` |
| `datablock_get_metrics_from_fd(int fd, DataBlockMetrics*)` | `src/utils/shm/data_block_recovery.cpp` | ✅ Shipped `b3d5e36d` |
| `PeerDeathWatcher` interface + Linux backend | `src/utils/os/` (bikeshed §6.5) | ⏳ C.2.c |
| Broker dial worker + fd cache map | `src/utils/ipc/broker_service.cpp` (observer_worker) | ⏳ C.2.d |
| Producer `startup.shm_metrics_observer` opt-out | `src/include/utils/config/startup_config.hpp` + role hosts | ⏳ D5 |
| `collect_shm_info` fd lookup + `metrics_source` field | `src/utils/ipc/broker_service.cpp:5866` | ⏳ C.3 |
| L4 tests (e2e, opt-out, crash-safety) | `tests/test_layer4_plh_hub/` | ⏳ C.4 |

---

## 1. Plain-language overview

### 1.1 The problem in one paragraph

Before HEP-CORE-0041, the broker read per-channel SHM metrics by calling `datablock_get_metrics(channel_name)` — the primitive did `shm_open("/pylabhub_...")` under the hood, mapped the header page, read atomic counters, unmapped.  The pattern was "**broker opens by name, on demand**."  It was cheap, stateless, and real-time by construction — the broker read the same live memory the producer was writing.

HEP-CORE-0041 deleted the well-known name.  The SHM segment is now created via `memfd_create` (anonymous) and delivered to consumers as a bare file descriptor over an authenticated Unix socket.  There is no name to open.  If the broker still wants to observe metrics, it must acquire an fd through the same authenticated handshake consumers use.

### 1.2 The design in one paragraph

The broker becomes an **observer** — a distinct role type that goes through `AttachProtocol` (HEP-CORE-0044) with `role_type="observer"`, but with two things unlike a data consumer: (1) its trust anchor is not the channel allowlist but an ephemeral pubkey the broker generated at startup and published on `PRODUCER_REG_ACK`; (2) the memfd it receives is wrapped in a `DataBlockObserverHandle` that maps only the header page (no data-plane access).  The broker dials once per SHM channel at producer-registration time, caches the observer fd, and reads metrics via `datablock_get_metrics_from_fd(cached_fd, ...)` — same real-time property as the pre-HEP-0041 path.

### 1.3 Worked example

Concrete scenario: broker starts up, a temperature-sensor producer registers a channel `"lab.temp"`, and later an operator queries `BROKER_SHM_INFO_REQ` to see live metrics.

```mermaid
sequenceDiagram
    autonumber
    participant B as Broker
    participant KS as SMS KeyStore<br/>(inside broker process)
    participant P as Sensor (Producer)
    participant O as Operator / Dashboard

    Note over B,KS: Startup
    B->>KS: secure().keys().generate_and_add_identity("broker.observer")
    KS-->>B: broker_observer_pubkey_z85

    Note over B,P: Producer registers channel "lab.temp" (data_transport=shm)
    P->>B: REG_REQ { channel_name: "lab.temp", role_uid: "prod.sensor01", ... }
    Note over B: REG_ACK builder includes broker_observer_pubkey_z85
    B-->>P: REG_ACK { ..., broker_observer_pubkey_z85 }
    Note over P: RoleAPIBase.set_broker_observer_pubkey_z85(pubkey)<br/>Stashed as producer's observer trust anchor

    Note over B,P: (later — asynchronously) broker's observer worker dials producer
    B->>P: connect to shm_capability_endpoint (Unix socket)
    Note over B: run_consumer_handshake with:<br/>self.own_seckey_name = "broker.observer"<br/>self.role_type = "observer" (§9 HEP-0044)
    B->>P: Frame 2 (role_type="observer", pubkey_z85=<broker.observer pub>)
    Note over P: run_producer_handshake sees role_type="observer".<br/>Compares Frame 2's pubkey against ObserverPubkeyAccessor's return.<br/>Matches → standard MAC verify per HEP-0044 §4 → OK.
    P->>B: (memfd via SCM_RIGHTS — header-page-mapping only)
    Note over B: Wrap in DataBlockObserverHandle;<br/>cache fd in unordered_map<channel_name, ObserverEntry>

    Note over B,O: Operator query — real-time metrics read from cached fd
    O->>B: BROKER_SHM_INFO_REQ (admin plane)
    Note over B: datablock_get_metrics_from_fd(cached_fd, &m)<br/>Reads atomic counters directly.<br/>No wire hop, no heartbeat wait.
    B-->>O: { slot_count, commit_index, throughput, ..., metrics_source: "attached" }

    Note over B,P: Producer eventually goes away
    P--xB: TCP/Unix-socket closes (crash OR clean DISC)
    Note over B: PeerDeathWatcher (Linux epoll EPOLLHUP) fires<br/>OR presence FSM transitions producer to Disconnected —<br/>whichever comes first
    Note over B: teardown_observer(entry): CAS torn_down flag,<br/>close fd, remove map entry
```

### 1.4 Key property preserved

Real-time observation.  Broker reads `slot_rw_state[i]` atomics directly from the mmap'd memfd — same code path a same-machine dashboard uses under HEP-CORE-0019.  A `BROKER_SHM_INFO_REQ` completes in the same wall-clock as pre-HEP-0041.  No heartbeat interval, no producer round-trip, no serialization through a role process.

---

## 2. Design decisions

Five load-bearing decisions.  Each has a small options table + the choice + rationale.  Options text is preserved verbatim from the 2026-07-03 conversation record; verdicts are ✅ next to the chosen row.

### 2.1 D1 — trust anchor for the observer handshake

Producer's `AttachProtocolAcceptor` must verify that the party requesting an observer attach IS the broker.

| Option | Pubkey source | Ephemeral | Wire change |
|---|---|---|---|
| a | Broker's ZMQ identity keypair | No (permanent) | none — already on REG_ACK |
| b | Separate operator-provisioned observer keypair | No | new REG_ACK field + operator burden |
| c | Ambient `SO_PEERCRED.uid` + `role_type="observer"` | N/A | none, but weak (same-uid squat) |
| **d** ✅ | **Broker-generated at each startup** | **Yes** | **new REG_ACK field (`broker_observer_pubkey_z85`), zero operator burden** |
| e | Per-channel ephemeral keypair | Yes, per channel | Overkill for one broker |

**Decision: (d).**  Broker generates a fresh Curve25519 keypair at startup via `secure().keys().generate_and_add_identity("broker.observer")`.  Broker echoes the pubkey on `PRODUCER_REG_ACK` as `broker_observer_pubkey_z85`.  Producer stashes it (D2) and uses it as the trust anchor when verifying observer handshakes (§3).

**Blast-radius analysis.**  If the broker's observer seckey ever leaks, an attacker can impersonate the broker to producers **as an observer** — obtaining a header-page-only mmap of the channel's metrics.  Not a data-plane compromise (data pages are not mapped by `DataBlockObserverHandle`).  Rotation on every broker startup limits the compromise window to the broker's lifetime.  The observer keypair is separate from the broker's ZMQ identity keypair — separate blast radius, independent rotation.

### 2.2 D2 — producer-side storage of the broker observer pubkey

| Option | Location | Update semantics on broker restart |
|---|---|---|
| a | Constructor arg on `AttachProtocolAcceptor` | Static — stuck with stale key after broker restart |
| **b** ✅ | **Settable field on `RoleAPIBase`** | **Setter fires on every `REG_ACK` — auto-refresh** |
| c | `KnownRolesStore`-shaped lookup | Overkill for one entry |

**Decision: (b).**  Same shape as `set_strict_abi_mismatch` (#327) and `set_shm_require_mutual_auth` (#262).  Producer's `register_producer_channel` extracts `broker_observer_pubkey_z85` from the `REG_ACK` response, calls `set_broker_observer_pubkey_z85(...)`.  `AttachProtocolAcceptor` reads it via `ObserverPubkeyAccessor` callback at handshake time (HEP-CORE-0044 §9.2) — no rewiring on broker restart.

**Backward compatibility.**  Empty pubkey (pre-#317 broker) means the observer path stays disabled — `AttachProtocolAcceptor` rejects `role_type="observer"` cleanly.  No legacy behaviour change.

### 2.3 D3 — broker dial timing

The old model: open by name on demand, per query.  Under the new model this requires a full authenticated handshake per query — prohibitively expensive.

| Option | Trigger | Cost per query | Cost per channel lifetime |
|---|---|---|---|
| **a** ✅ | **Immediately after producer `REG_REQ` succeeds** | **~0 (fd already cached)** | **1 handshake** |
| b | Lazy — first `BROKER_SHM_INFO_REQ` | Full handshake (10s of ms) | 1 handshake |
| c | Per query (mimics old) | Full handshake every query | N queries |

**Decision: (a).**  The pre-HEP-0041 code's INTENT was "broker always able to observe channel metrics."  Its MECHANISM was "open by name."  We preserve the intent using the mechanism the new authenticated model demands (persist the connection).

**Concrete flow.**  Broker's poll thread picks up the successful producer `REG_REQ` for a SHM channel.  Enqueues an observer-attach task on a **detached worker thread** (not the poll thread — see §2.6 Risk 1).  The worker:

1. Dials producer's `shm_capability_endpoint` (Unix socket).
2. Runs `initiate_consumer_handshake` variant with `role_type="observer"` (HEP-CORE-0044 §7 — the transport-agnostic helper handles it once we teach it about the observer trust anchor injection).
3. Receives the memfd via `SCM_RIGHTS`.
4. Stashes the fd + `DataBlockObserverHandle` in the broker-side `unordered_map<channel_name, ObserverEntry>`.

Handshake failures log `event=BrokerObserverAttachFailed` — the map entry stays absent — `collect_shm_info` falls back to `metrics_source="unavailable"` OR `"heartbeat"` (see §8).

### 2.4 D4 — teardown timing (cross-platform)

Broker holds a persistent connection per SHM channel; the fd must be closed when the channel ends.  End cases:

- Producer deregisters cleanly.
- Producer crashes (SIGKILL, seg fault).
- Producer partitions (no signal to broker).
- Broker shuts down.

Producer clean deregister is already handled by the presence FSM.  Producer death without notification is the hard case.

| Option | Trigger | Latency | Correctness | Portability |
|---|---|---|---|---|
| a | Heartbeat-timeout sweep (existing FSM) | 10s–30s | Always eventually closes | Portable (Linux + non-Linux) |
| b | `EPOLLHUP` on observer socket | ms | Immediate | Linux-only primitive |
| **c** ✅ | **Both — whichever fires first** | **ms on Linux, seconds elsewhere** | **Belt-and-suspenders** | **Portable via abstraction** |

**Decision: (c) with cross-platform abstraction.**  See §6 for the `PeerDeathWatcher` interface + per-platform backend matrix.

### 2.5 D5 — producer opt-out knob

**Decision.**  `producer.shm_metrics_observer: true|false` under `startup` config, default `true`.

When `false`, producer's `AttachProtocolAcceptor` refuses observer handshakes with a clear diagnostic; broker's `PeerDeathWatcher` fires immediately (socket closed by producer); broker falls back to `metrics_source="heartbeat"` or `"unavailable"`.

Same wire shape as `startup.strict_abi_mismatch` (#327) and `startup.shm_require_mutual_auth` (#262).  Implementation is mechanical, four-site pattern:

- Parse in `parse_startup_config`.
- Setter on `RoleAPIBase`.
- Wire in the three role hosts (producer, processor, plus consumer for future-proofing).
- Read at handshake time (feed into the observer path of `AttachProtocolAcceptor`).

### 2.6 Risks (flagged during self-review, mitigations locked)

1. **Broker poll thread synchronous handshake latency.**  Under D3(a), the broker dials during producer `REG` handling.  If a producer registers N SHM channels in a burst, the poll thread runs N handshakes sequentially before servicing the next RPC.  Each handshake is ~10 ms locally, ~100 ms cross-node worst case.  **Mitigation: run the observer-attach on a detached worker thread; poll thread just enqueues "please observe channel X" work.  Ship default: worker thread.**  If load ever exceeds even that, sibling task #282 (BRC poll thread offload) applies.

2. **Partial-handshake fd leak.**  If the observer-attach fails between "socket connected" and "fd received via SCM_RIGHTS," the connect fd stays open until scope-exit RAII closes it.  **Mitigation: wrap the observer-attach in a scope guard** (`std::unique_ptr<int, void(*)(int*)>` or the existing `FdGuard` pattern) so any early return / throw closes the connect fd.  The memfd (once received via SCM_RIGHTS) has its own guard.

3. **Broker shutdown ordering.**  Observer connections hold references into `secure().keys().with_seckey("broker.observer", ...)` callback scope.  If KeyStore is torn down before observer connections, the callback fires against a dead reference.  **Mitigation: broker's dtor tears down observer connections FIRST** (close all fds, unwatch all `PeerDeathWatcher` entries, join the worker thread), THEN releases KeyStore.  Follow the RAII pattern of the existing BRC teardown.

4. **Multiple channels per producer.**  A producer with multiple SHM channels shares one identity keypair but has one `AttachProtocolAcceptor` per channel.  The observer pubkey the producer trusts (`broker_observer_pubkey_z85`) is set ONCE on `RoleAPIBase` (per role instance).  All channels trust the SAME broker.  This IS what we want — one broker, one observer key.  Verified consistent with D2.

5. **Interface placement.**  The `PeerDeathWatcher` interface is an OS abstraction, not a security primitive.  Recommended home: `src/utils/os/` or `src/utils/lifecycle/`.  Decision on placement lands with the impl slice (C.2.c).

6. **Non-Linux teardown fallback.**  On platforms without an `EPOLLHUP`-equivalent primitive shipped yet, `PeerDeathWatcher` is a no-op — the callback never fires.  Correctness is preserved by the heartbeat-timeout safety net (D4 option a).  The observable effect: crash/partition teardown latency degrades from milliseconds to the heartbeat interval.  Clean deregister still fires teardown via the presence FSM path regardless of platform.

7. **Observer socket vs consumer's data attach socket.**  Both connect to the producer's `shm_capability_endpoint`.  The producer's accept loop already spawns one handshake state machine per accept, so simultaneous consumer + observer connections work by construction (HEP-CORE-0041 §5 accept loop model, HEP-CORE-0044 §5 per-connection state machine).

---

## 3. Trust chain

### 3.1 Broker startup — keypair generation

At broker bringup, immediately after SMS is `Initialized`:

```
broker_observer_pubkey_z85 = secure().keys().generate_and_add_identity("broker.observer")
```

`KeyStore::generate_and_add_identity(name)` (shipped `d6f5d621`) mints a fresh Curve25519 keypair inside the KeyStore's LockedKey region, returns the Z85-encoded pubkey.  The seckey never leaves the KeyStore — subsequent AttachProtocol runs cite it by name (`"broker.observer"`) and SMS reads the bytes inside a scoped callback (HEP-CORE-0040 §8.5.2, HEP-CORE-0043 §6, HEP-CORE-0044 §4.1).

Broker holds the Z85 pubkey in its own state for emission on subsequent `PRODUCER_REG_ACK`s.

### 3.2 REG_ACK emission

Every `PRODUCER_REG_ACK` for a `data_transport="shm"` channel gains the field:

```
broker_observer_pubkey_z85: "<40 Z85 chars, broker's ephemeral observer pubkey>"
```

Absent for non-SHM channels.  Empty allowed only during rolling upgrade of a pre-#317 broker (producer treats empty as "observer path disabled").

### 3.3 Producer-side storage + trust

`RoleAPIBase::register_producer_channel` extracts the field from the `REG_ACK` response, calls `set_broker_observer_pubkey_z85(pubkey)`.  Setter is thread-safe (shared_mutex).

`AttachProtocolAcceptor` on the producer's channel accepts an `ObserverPubkeyAccessor` callback at construction — a `std::function<std::string()>` returning the current stashed pubkey, or empty if none.  The accessor closes over `RoleAPIBase::get_broker_observer_pubkey_z85()`.

### 3.4 Handshake verification

Per HEP-CORE-0044 §7.2 step 5 + §9.3:

1. `run_producer_handshake` sees Frame 2's `role_type == "observer"`.
2. Invokes `observer_pubkey_accessor()`.  Null callable or empty return → reject with `role_type="observer" received but no broker observer pubkey known yet`.
3. Constant-time compares Frame 2's `pubkey_z85` against the accessor's return.  Mismatch → reject with `role_type="observer" hello pubkey_z85 does not match the broker observer pubkey the producer trusts`.
4. On match, fall through to the standard MAC verify (HEP-CORE-0044 §4.2).  The initiator must ALSO hold the seckey corresponding to that pubkey (otherwise the MAC won't verify) — squatter with stolen pubkey but no seckey fails at step 4 even if step 3 somehow passed.

Both checks are load-bearing.  The identity check binds the observer to "this broker's current instance"; the crypto verify binds the observer to "the entity that holds this broker's observer seckey."  Only the broker itself passes both.

---

## 4. Observer capability

### 4.1 What the observer receives

The producer's fd handoff step (post-handshake) sends a memfd via `SCM_RIGHTS` — same primitive as the consumer path (HEP-CORE-0041 §5).  The KEY difference is how the receiver wraps it:

- **Consumer receiver:** `DataBlock::from_fd` → maps the full segment → read/write access to slot pages.
- **Observer receiver:** `DataBlockObserverHandle::from_fd` (`da2a5e76`) → maps ONLY the header page.  No slot pages.  No writes.

### 4.2 Header-only capability

`DataBlockObserverHandle` is a header-only, read-only view of a `DataBlock`.  It exposes:

- Metrics reads via `datablock_get_metrics_from_fd(fd, DataBlockMetrics *out)` — the C API primitive shipped `b3d5e36d`.
- No slot enumeration.
- No commit / read operations.
- No mutations.

The producer cannot restrict which pages the receiver `mmap`s once the fd is handed over — anyone with the fd can `mmap(PROT_READ, MAP_SHARED, fd, 0, full_size)`.  The observer discipline is enforced by CONVENTION at the broker code side, not by the kernel.  This is acceptable for a same-machine, same-operator threat model where the broker is a trusted process.  A hostile broker with the observer fd could in principle map the full segment; but a hostile broker with the observer fd could already do far more damage — the operator has bigger problems.

### 4.3 Real-time property

Metrics reads via `datablock_get_metrics_from_fd(cached_fd, &m)` execute against the live mmap'd atomics — same read path a same-machine dashboard uses under HEP-CORE-0019.  No wire hop, no heartbeat serialization, no round-trip through the producer's data loop.

---

## 5. Broker dial + fd cache (§C.2.d)

### 5.1 Trigger

Broker's `handle_reg_req` runs the standard REG processing.  On successful REG for a `data_transport="shm"` channel, before returning REG_ACK, enqueue an observer-attach work item:

```
struct ObserverAttachWork {
    std::string channel_name;
    std::string shm_capability_endpoint;   // from REG_REQ payload
    // producer identity / channel key omitted — the handshake carries them
};
broker_observer_worker_.enqueue({channel_name, endpoint});
```

The observer-attach happens on a detached worker thread (Risk 1 mitigation).  REG_ACK is not blocked on the observer-attach's completion.

### 5.2 Worker flow

```
for each ObserverAttachWork:
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)
    connect(fd, endpoint)                     // FdGuard on scope
    channel = ShmAttachChannel(fd, "broker.observer")
    try:
        // HEP-CORE-0044 §7 helper — same crypto flow as consumer
        run_consumer_handshake(channel, self, producer_pubkey_z85,
                               deadline, require_mutual_auth=false)
        // Above throws on any failure
    except:
        log event=BrokerObserverAttachFailed reason=...
        continue     // FdGuard closes fd
    // On success, receive memfd via SCM_RIGHTS on the same fd
    memfd = recv_scm_rights(fd)
    // Wrap in observer handle
    obs_handle = DataBlockObserverHandle::from_fd(memfd)
    // Cache under channel lock
    observer_map_[channel_name] = ObserverEntry{
        .fd = memfd,
        .observer_handle = obs_handle,
        .connect_fd = fd,        // kept alive for PeerDeathWatcher registration
        .torn_down = false,
    };
    peer_death_watcher_.watch(fd, [this, channel_name]() {
        teardown_observer_by_channel(channel_name);
    });
```

`self` for the broker observer is:

```
ConsumerAuthMaterial self;
self.role_uid = "broker.observer";
self.pubkey_z85 = broker_observer_pubkey_z85_;  // stored at startup
self.own_seckey_name = "broker.observer";       // KeyStore entry name
// role_type="observer" is set inside run_consumer_handshake variant
//   (see HEP-CORE-0044 §7.3 for the signature; observer variant TBD if
//    we need a distinct entry point or just parameterize role_type)
```

### 5.3 State

`BrokerServiceImpl` grows one map:

```
struct ObserverEntry {
    int fd{-1};                                 // memfd
    std::unique_ptr<DataBlockObserverHandle> observer_handle;
    int connect_fd{-1};                         // AF_UNIX fd, kept for PeerDeathWatcher
    std::atomic<bool> torn_down{false};
};

std::unordered_map<std::string, ObserverEntry> observer_map_;   // key = channel_name
mutable std::shared_mutex observer_map_mtx_;
```

Read path (§8) takes shared lock.  Insert/erase takes exclusive lock.  Idempotent teardown per §6.4.

---

## 6. Producer teardown — `PeerDeathWatcher` (§C.2.c)

### 6.1 Interface

```cpp
class PeerDeathWatcher {
public:
    virtual ~PeerDeathWatcher() = default;

    /// Register: fires `on_peer_gone` exactly once when the OS notices
    /// the peer has closed the socket / process died.  Idempotent —
    /// safe to unwatch even if the callback has already fired.
    virtual void watch(int fd_or_handle,
                       std::function<void()> on_peer_gone) = 0;

    virtual void unwatch(int fd_or_handle) = 0;
};
```

Placement: `src/utils/os/` recommended (not `security/` — it is not a crypto primitive; it is an OS peer-lifecycle abstraction that will be reused by other subsystems).  Header-final placement is a lightweight bikeshed for the C.2.c slice.

### 6.2 Backend matrix

| Platform | Primitive | Ships in |
|---|---|---|
| Linux | `epoll_wait` + `EPOLLHUP` | #317 C.2.c |
| Non-Linux fallback | no-op — callback never fires; heartbeat safety net (D4a) covers correctness | #317 C.2.c |
| FreeBSD | `kqueue` + `EVFILT_READ` + `EV_EOF` | #260 |
| macOS | `kqueue` (same as FreeBSD) | #261 |
| Windows | `WSAPoll` + `POLLHUP` (Windows 10+ AF_UNIX) OR `WaitForMultipleObjects` on named pipes | #262 |

Broker code registers via the interface, no `#ifdef __linux__` in broker paths.  On unsupported platforms the callback simply never fires; the heartbeat-timeout safety net closes the fd within one heartbeat interval.  Correctness preserved everywhere; latency degrades to "heartbeat interval" on non-Linux until each platform's backend lands.

### 6.3 Callback contract

`on_peer_gone` fires exactly once per registered fd.  Broker's callback invokes `teardown_observer_by_channel(channel_name)` (§6.4).

### 6.4 Idempotent teardown

Both paths call the same teardown function — the callback AND the presence-FSM `on_producer_removed` handler.  Whichever fires first wins the CAS on `ObserverEntry::torn_down`; the second no-ops.

```cpp
void teardown_observer_by_channel(const std::string &channel_name) {
    std::unique_lock lock(observer_map_mtx_);
    auto it = observer_map_.find(channel_name);
    if (it == observer_map_.end()) return;                // never attached
    ObserverEntry &e = it->second;
    bool expected = false;
    if (!e.torn_down.compare_exchange_strong(expected, true))
        return;                                            // other path won
    peer_death_watcher_.unwatch(e.connect_fd);
    if (e.observer_handle) e.observer_handle.reset();      // munmap
    if (e.fd >= 0)          ::close(e.fd);
    if (e.connect_fd >= 0)  ::close(e.connect_fd);
    observer_map_.erase(it);
    // LOG event=BrokerObserverTornDown channel=...
}
```

`on_producer_removed` (existing broker path, fires on heartbeat timeout + explicit DEREG) also calls `teardown_observer_by_channel(channel_name)`.  Whichever path completes first releases resources; the other is a no-op.

---

## 7. Producer opt-out (§D5)

### 7.1 Config knob

```yaml
startup:
  shm_metrics_observer: true    # default
```

When `false`:
- Producer's `AttachProtocolAcceptor` REFUSES `role_type="observer"` handshakes with a clear diagnostic message.
- Broker's observer-worker sees the handshake close on the wire.
- Broker's `PeerDeathWatcher` fires immediately (socket closed).
- Broker's next metrics query for this channel falls back to `metrics_source="heartbeat"` or `metrics_source="unavailable"` (see §8).

### 7.2 Implementation shape

Mirrors `startup.strict_abi_mismatch` (#327) and `startup.shm_require_mutual_auth` (#262):

- Parse in `parse_startup_config` (`src/utils/config/`).
- Setter on `RoleAPIBase`.
- Read in each role host's `run_data_loop` initialization or handshake wiring.
- Feed into `AttachProtocolAcceptor` construction — if opt-out, install an `ObserverPubkeyAccessor` that always returns empty (which HEP-CORE-0044 §9.3 already handles as "observer path disabled").

Total scope: ~40 LOC across config + role hosts.

---

## 8. Broker metrics_source wiring (§C.3)

### 8.1 Response field

`BROKER_SHM_INFO_REQ`'s response gains a per-channel `metrics_source` field with a small enum:

| Value | Meaning |
|---|---|
| `"attached"` | Broker holds a live observer fd; metrics are read directly from mmap'd atomics.  Real-time. |
| `"heartbeat"` | Broker does not hold an observer fd, but the producer's heartbeats are recent — coarse metrics estimated from the presence FSM. |
| `"unavailable"` | Broker has no observer fd and no recent heartbeat — reporting `null` metrics honestly. |

### 8.2 Producer liveness in the response

Per Risk 6 in §2.6 and the pre-promotion draft §10.5, the response for each channel also carries:

```json
{
  "producer_state": "Live" | "HeartbeatMissed" | "Dead",
  "producer_last_heartbeat_ms_ago": 42
}
```

Dashboards / operators distinguish live-metrics from stale-metrics based on state, not on presence-of-metrics.  A `"Live"` reading is trustworthy; anything else is explicitly marked.

### 8.3 collect_shm_info

Broker's `collect_shm_info` (currently `broker_service.cpp:5866`) is rewritten to:

1. Take shared lock on `observer_map_`.
2. If channel present + not torn_down: `datablock_get_metrics_from_fd(entry.fd, &m)`, populate response with `metrics_source="attached"`.
3. If channel absent + producer state is `Live`: fall back to heartbeat-derived coarse metrics, `metrics_source="heartbeat"`.
4. Otherwise: `metrics_source="unavailable"`, null metrics fields.

---

## 9. Test coverage (§C.4)

### 9.1 L2 unit tests

- **Observer keypair generation:** `secure().keys().generate_and_add_identity("broker.observer")` returns a valid Z85 pubkey; entry is present under that name; `pubkey(name)` round-trips.  (Already covered by `test_key_store.cpp::GenerateAndAddIdentity_Roundtrip`.)
- **REG_ACK emission:** broker's `handle_reg_req` output for a SHM channel contains `broker_observer_pubkey_z85` with the current broker startup pubkey.
- **Producer stashing:** `RoleAPIBase::set_broker_observer_pubkey_z85(...)` followed by `get_broker_observer_pubkey_z85()` round-trips; setter is thread-safe under parallel writers.
- **`AttachProtocolAcceptor` observer verify path:** already covered structurally by HEP-CORE-0044 §7.2 + §9.3 — the observer branch is the same code path as the consumer branch after the trust-anchor check.  Explicit observer tests (mismatch pubkey rejected, empty stash rejected) are C.4 additions.
- **`PeerDeathWatcher` Linux backend:** watch a pipe fd, close writer, callback fires.  Watch, unwatch before close, callback does not fire.  Watch twice, unwatch once — one fire, one silence.

### 9.2 L4 end-to-end

- **Happy path:** broker starts, producer registers SHM channel, admin calls `BROKER_SHM_INFO_REQ`, response carries live metrics with `metrics_source="attached"`.
- **Crash safety:** producer is SIGKILL'd; within `PeerDeathWatcher` latency (Linux: ms; non-Linux: heartbeat interval), broker's observer entry is torn down; subsequent `BROKER_SHM_INFO_REQ` reports `metrics_source="unavailable"` with `producer_state="Dead"`.
- **Opt-out:** producer starts with `startup.shm_metrics_observer: false`; broker's observer-attach fails cleanly; `BROKER_SHM_INFO_REQ` reports `metrics_source="heartbeat"` with `producer_state="Live"`.
- **Broker restart:** broker restarts mid-session; new observer pubkey published on next REG_ACK; but producer's channel is already registered — needs to handle a fresh REG_REQ path (producer re-REGs after seeing broker come back, per existing HEP-CORE-0023 presence FSM).  New pubkey stashed, new observer connection established.
- **Multi-producer:** broker observes N producers' SHM channels in parallel; observer_map_ concurrency stress test.

### 9.3 L3 broker unit tests

- Broker's `observer_worker` under fault injection: handshake times out, socket unreachable, fd receive fails, etc.  All produce `event=BrokerObserverAttachFailed` and leave the observer_map_ absent for that channel; do not disrupt other channels.
- Broker shutdown ordering: observer connections torn down before KeyStore released (Risk 3 mitigation).

---

## 10. Impl slice map (task #317)

Following commits, in order:

| Slice | Scope | Status |
|---|---|---|
| Phase A | `datablock_get_metrics_from_fd` C API + L2 test | ✅ `b3d5e36d` 2026-07-03 |
| Phase B | `DataBlockObserverHandle` header-only observer attach | ✅ `da2a5e76` 2026-07-03 |
| D1 slice A | `KeyStore::generate_and_add_identity` primitive | ✅ `d6f5d621` 2026-07-03 |
| D2 slice | `RoleAPIBase::set_broker_observer_pubkey_z85` + `PRODUCER_REG_ACK` extraction (dormant until C.2.a lights it) | ✅ `f7d3a51e` 2026-07-03 |
| C.2.a | Broker startup generates observer keypair; REG_ACK builder emits `broker_observer_pubkey_z85` | ✅ `029bbe31` 2026-07-03 |
| C.2.b | Producer `AttachProtocolAcceptor` observer verify path (`role_type="observer"` branch replaces old "not yet implemented" throw) | ✅ `ce956972` 2026-07-04.  Preserved through the 2026-07-07 refactor in `run_producer_handshake` (HEP-CORE-0044 §7.2 + §9.3). |
| **C.2.c** | **`PeerDeathWatcher` interface + Linux `epoll` backend + non-Linux fallback + L2 tests** | ⏳ **Next** |
| **C.2.d** | **Broker-side observer dial worker + fd cache map + teardown + scope-guard for partial-attach** | ⏳ (depends on C.2.c) |
| **D5** | **Producer `startup.shm_metrics_observer` opt-out config knob** | ⏳ (mechanical; can slot alongside C.2.b close or C.2.d) |
| **C.3** | **`collect_shm_info` fd lookup + `metrics_source` field + producer liveness fields** | ⏳ (depends on C.2.d) |
| **C.4** | **L4 tests: e2e, opt-out, crash-safety, broker restart, multi-producer** | ⏳ (depends on C.2.d + C.3) |
| **C.5** | **HEP-CORE-0043 §9.3 status sync + HEP-CORE-0041 §10.5 pointer refresh** | ⏳ (docs only, after C.4) |

Rough sizing: C.2.c ~150 LOC (interface + Linux backend + fallback + basic L2 tests); C.2.d ~200 LOC (worker thread + fd cache map + teardown + scope-guard); D5 ~40 LOC; C.3 ~50 LOC; C.4 ~200 LOC + fixture work.  Remaining scope: **~640 LOC + tests + docs.**

---

## 11. Cross-references

- **HEP-CORE-0044** — AttachProtocol primitive.  This HEP uses `role_type="observer"` on Frame 2 + the `ObserverPubkeyAccessor` injection point at `AttachProtocolAcceptor` construction (HEP-CORE-0044 §9).
- **HEP-CORE-0041 §5** — SHM capability transport.  Broker's observer connection reuses `shm_capability_endpoint` + `SCM_RIGHTS` — the observer connects on the same producer socket the consumers use.
- **HEP-CORE-0041 §10.5** — This HEP supersedes the observer content that was previously in §10.5.  §10.5 remains as a pointer.
- **HEP-CORE-0040 §5** — KeyStore.  The broker's observer keypair lives here as an ephemeral identity entry (`"broker.observer"`).
- **HEP-CORE-0043 §6** — SMS asymmetric box.  The observer handshake's crypto goes through `box_encrypt_using` / `box_decrypt_using` via HEP-0044.
- **HEP-CORE-0036 §4.1** — `ChannelAccessIndex`.  Explicitly NOT the observer's trust anchor — the observer path bypasses the consumer allowlist because it operates on a different trust model (D1).
- **HEP-CORE-0002** — DataBlock header layout.  Observer reads atomic counters from the header page defined here.
- **HEP-CORE-0019** — Per-slot observability.  The observer path is how the broker regains access to this observability under HEP-0041's authenticated model.
- **HEP-CORE-0023 §2.5** — Heartbeat cadence.  Sets the fallback-teardown latency on non-Linux platforms (D4 option a).

---

## 12. Change log

- **2026-07-08 — HEP promoted.**  Content authoritative from `docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md` (dated 2026-07-03).  Structure reorganized: plain-language overview + worked example moved to §1; D1-D5 decisions kept as §2 with verdicts intact; trust chain / observer capability / broker dial / teardown / opt-out / metrics wiring split into §3-§8 for clean single-source lookup.  Test coverage plan §9 added.  Slice map §10 preserves the tech-draft ordering.  Tech draft archived under `docs/archive/transient-2026-07-08/`.
