# PeerAdmission Architecture — Data-Channel Auth-Gating Design

**Status:** ADOPTED — Phases A/B/C shipped; Phases D–H pending designer
decision on two structural items (see §9 status table + §8 P-Wire,
P-Schema).
**Authors:** Quan Qing (designer), Claude (drafter)
**Date:** 2026-06-02
**Supersedes:** HEP-CORE-0036 §3.3 + §4.1 + §6 layering (existing wire protocol intent
preserved; class layout corrected)
**Related:** HEP-CORE-0035 (file ACL + identity vault), HEP-CORE-0017 (queue
abstractions), HEP-CORE-0037 (federation, TBD; placeholder per task #105)

---

## 1. Why this doc exists

The existing HEP-CORE-0036 design places per-peer allowlists at `ZmqQueue::Options`
and adds methods like `ZmqQueue::add_producer_peer()` directly on the concrete
ZeroMQ implementation (HEP-0017 §3.3 lines 226-316; HEP-0036 §3.3 + §6). That
layering is wrong:

- **Gate is policy.** "Allow K, deny K" is an invariant of the queue concept, not
  of how the queue is wired. Burying it in `ZmqQueue` means SHM, inbox, and
  federation transports all need parallel re-implementations of admission. The
  word "gate" splinters across N implementations.
- **Mechanism is implementation detail.** CURVE+ZAP is one *enforcement
  mechanism* of admission. POSIX uid+gid is another. Token-based attachment is
  a third. None of them IS the gate.
- **The existing pylabhub abstraction already exists.** `QueueReader` and
  `QueueWriter` (`src/include/utils/hub_queue.hpp:171, 335`) are the abstract
  base. They have **zero** admission virtuals today. That's the layer where the
  gate belongs.

This document specifies the corrected architecture: gate at the abstraction;
enforcement per transport. It also identifies open design decisions the
designer must make before implementation starts.

## 2. Threat model (what we're protecting against)

| Threat | Status today | What this design must close |
|---|---|---|
| Any host on the network reads a producer's data stream | OPEN — `ZmqQueue::push_to(endpoint, ...)` binds with no CURVE; anyone running `pull_from(endpoint, ...)` receives data | Producer's data socket admits ONLY consumers whose CURVE pubkey is on the broker-issued allowlist |
| Any host on the network injects data into a consumer | OPEN — `ZmqQueue::pull_from(endpoint, ...)` connects to producer with no `curve_serverkey`; impersonator can stand up a fake producer at the same endpoint | Consumer uses producer's pubkey (broker-delivered) as `curve_serverkey`; mismatched server rejected |
| Role with valid CURVE handshake claims arbitrary identity | OPEN — `RoleIdentityPolicy::Verified` matches `role_name`/`role_uid` strings the client wrote into its own REG_REQ JSON body (`src/include/utils/role_identity_policy.hpp:11-12`) | CURVE pubkey is the identity; `KnownRole` schema gains `pubkey_z85`; ZAP handler on broker ROUTER validates pubkey-vs-allowlist before application-layer dispatch |
| Federation peer impersonation | OPEN — `broker_service.cpp:580-584` sets `curve_serverkey` from `peer_cfg.pubkey_z85` but no ZAP on inbound peer ROUTER | Federation peer ROUTER gets ZAP with federation-peer allowlist |
| Admin token sniffed in cleartext | OPEN — `admin_service.cpp:170-176` REP socket is plain TCP; token sent in cleartext payload | Either CURVE-wrap or hard-enforce loopback-only at bind time |
| SHM segment opened by unauthorized local process | PARTIAL — `shm_shared_secret` (uint64) in role configs is a static pre-shared token; anyone reading the config file gets it | Broker-issued ephemeral shm_secret distributed via REG_ACK only to authorized consumer pubkeys; (optional) file ACL on SHM segment to UID |
| Compromised broker | **OUT OF SCOPE** (HEP-CORE-0036 §I8) | Broker is the sole authority; defenses against a compromised broker are not addressed |

## 3. Current state — file:line evidence

Confirming the gaps before specifying the fix:

| Surface | File:line | What's there |
|---|---|---|
| Abstract queue admission | `hub_queue.hpp:171, 335` | `QueueReader`, `QueueWriter` define schema/checksum/metrics/lifecycle virtuals. **Zero** peer/admission virtuals. Verified by `grep "set_peer\|allowlist\|admission\|peer_pubkey\|allowed_peer\|PeerIdentity\|reject_peer"` → empty |
| ZmqQueue factory keys | `hub_zmq_queue.hpp:148, 186` | `pull_from(endpoint, schema, packing, bind, depth, schema_tag, instance_id)` + `push_to(...)` — no key parameters |
| ZmqQueue socket creation | `hub_zmq_queue.cpp:570-594` | Socket gets `linger` + `sndhwm` only. No `curve_*`, no ZAP setup |
| TxQueueOptions / RxQueueOptions | `role_api_base.hpp:72-121` | No `pubkey_z85` / `secretkey_z85` / `serverkey_z85` / `allowlist` fields |
| RoleIdentityPolicy | `role_identity_policy.hpp:7-44` (self-deprecating doc) + line 120 (`KnownRole` has no pubkey field) | String-name match on self-asserted JSON body |
| Broker REG_REQ handlers | `broker_service.cpp:1145, 1920` | Return success/error JSON with `correlation_id`; **never** include peer allowlist data |
| ZAP handler | grep `zap\|ZAP\|inproc://zeromq.zap.01` | Zero implementation; only design comments in `role_identity_policy.hpp:4` |
| Vault key handoff to ZmqQueue | `role_api_base.cpp:318, 382` | `ZmqQueue::push_to(opts.zmq_node_endpoint, ...)` — no keys passed |
| broker_proto version | `broker_service.cpp:119-160` | Currently 5; CHANNEL_AUTH_UPDATE wire frame not present |
| InboxQueue inheritance | `hub_inbox_queue.hpp:101, 218` | Does **NOT** inherit from `QueueReader`/`QueueWriter`. Standalone ROUTER/DEALER |

## 4. Design — Layer 1: the abstraction

### 4.1 New header: `peer_admission.hpp`

Free-standing header (under `src/include/utils/security/`) defining the cross-
transport admission contract.

```cpp
namespace pylabhub::utils::security
{

/// Identifier of a peer presenting itself to a queue / channel / endpoint.
/// Cross-transport: each transport extracts the kind it cares about.
///
/// kind = "curve" → data is the peer's CURVE public key (40-char Z85).
/// kind = "posix" → data is "<uid>:<gid>" (decimal, colon-separated).
/// kind = "shm"   → data is a hex-encoded broker-issued ephemeral shm_secret.
/// Future kinds: "federation", "script", ...
///
/// Comparison is byte-exact on (kind, data).  PeerIdentity is meant for set
/// membership, not for rich identity-attribute matching.
struct PeerIdentity
{
    std::string kind;
    std::string data;

    bool operator==(const PeerIdentity &o) const noexcept
    { return kind == o.kind && data == o.data; }
    bool operator<(const PeerIdentity &o) const noexcept
    { return std::tie(kind, data) < std::tie(o.kind, o.data); }
};

/// Snapshot semantics for the allowlist.  Set the full intended state;
/// implementations do NOT merge with prior state.  The caller (broker glue)
/// is responsible for computing the union when adding peers without
/// dropping existing ones.
struct PeerAllowlist
{
    std::set<PeerIdentity> peers;

    /// When true, ANY caller is admitted regardless of peers content.
    /// Reserved for explicit test fixtures + an opt-in transitional mode.
    /// NEVER set this in production code paths.
    bool unrestricted{false};
};

/// Abstract gate.  Both QueueReader + QueueWriter inherit from this so each
/// concrete queue implementation overrides the same surface.  Non-queue
/// admission-bearing surfaces (InboxQueue, BrokerService ROUTER, AdminService
/// REP, federation peer ROUTER) also implement this interface directly.
///
/// Thread-safety:
///   - set_peer_allowlist() may be called from ANY thread (typically the
///     broker-control thread inside a role process, or the broker's own
///     handler thread inside plh_hub).
///   - is_peer_allowed() is called from the transport's enforcement context
///     (e.g., the ZAP handler thread).  Implementations MUST be safe under
///     concurrent set + is_allowed.
///   - Recommended impl: copy-on-write std::shared_ptr<const PeerAllowlist>
///     swapped via atomic shared_ptr semantics or shared_mutex.
class PeerAdmission
{
public:
    virtual ~PeerAdmission() = default;

    /// Replace the allowlist.  Snapshot semantics (does NOT merge).
    /// Return true on accept; false if the transport rejected the update
    /// (e.g., admission not supported in this build).
    virtual bool set_peer_allowlist(PeerAllowlist allowlist) = 0;

    /// Inspect.  Returns nullopt if admission not supported by this
    /// instance (e.g., a local-only inproc queue with no policy concept).
    [[nodiscard]] virtual std::optional<PeerAllowlist>
    peer_allowlist_snapshot() const = 0;

    /// Test admission for a candidate PeerIdentity.  Called by the
    /// transport-specific enforcement layer (ZAP handler, SHM open path,
    /// etc.).  Returns true iff the peer is currently admitted.
    ///
    /// NOT meant to be called from application code; transport
    /// implementations call this when a connection / attachment attempt
    /// occurs.
    [[nodiscard]] virtual bool
    is_peer_allowed(const PeerIdentity &p) const = 0;

    /// True iff this instance's transport currently enforces admission
    /// at the kernel/wire level (CURVE+ZAP wired, SHM secret enforced,
    /// etc.).  False if the instance is a transitional "open" mode.
    /// Used by tests and broker to distinguish "policy declared" from
    /// "policy enforced".
    [[nodiscard]] virtual bool admission_is_enforced() const noexcept = 0;
};

} // namespace pylabhub::utils::security
```

### 4.2 `PeerAdmission` is a separate interface — Queue family unchanged

**Architectural correction (2026-06-02).** An earlier draft had
`QueueReader`/`QueueWriter` inherit `PeerAdmission`. That's wrong on two
counts:

1. **Diamond inheritance.** `ZmqQueue` and `ShmQueue` both inherit from
   `QueueReader` AND `QueueWriter` (`hub_queue.hpp:171, 335` →
   `hub_zmq_queue.hpp:117`, `hub_shm_queue.hpp:50`). If both bases also
   inherit `PeerAdmission`, every `ZmqQueue` instance has TWO
   `PeerAdmission` subobjects. Virtual inheritance fixes the duplication
   but adds vptr indirection and surprises downstream maintainers.
2. **Conceptual conflation.** The Queue family is about *what data
   flows through this transport* (read/write semantics, schema,
   checksum, lifecycle). `PeerAdmission` is about *who is allowed*. The
   two are orthogonal. Coupling them forces every future Queue subclass
   to think about admission even when its transport has no admission
   concept (e.g., a future inproc-only queue), and forces every
   future admission-bearing surface to be a Queue (which excludes the
   broker ROUTER and admin REP — non-queue surfaces that legitimately
   need the gate).

**Correct architecture.** `PeerAdmission` is a **pure abstract interface**
in its own header. Concrete classes that need a gate inherit it directly
alongside (not through) the Queue family:

```cpp
class ZmqQueue : public QueueReader,
                 public QueueWriter,
                 public pylabhub::utils::security::PeerAdmission
{ ... };

class ShmQueue : public QueueReader,
                 public QueueWriter,
                 public pylabhub::utils::security::PeerAdmission
{ ... };

class InboxQueue : public pylabhub::utils::security::PeerAdmission
{ ... };  // not in Queue family at all

class BrokerServiceImpl : public pylabhub::utils::security::PeerAdmission
{ ... };  // not a queue
```

This:
- Avoids the diamond. Each concrete admission-bearing class has exactly
  one `PeerAdmission` subobject.
- Decouples gate concept from queue concept. Future queues that need no
  gate don't pay the cost; future non-queue gates exist naturally.
- Makes the Queue family's existing virtual tables unchanged. No
  behavior or ABI ripple to existing tests.

**No defaults in the interface.** `PeerAdmission` is pure abstract: every
method is `= 0`. Implementers MUST provide all four. There is no
deny-default in the base because there is no concrete base. This is the
right shape for a policy interface: forcing every concrete class to
declare its admission stance explicitly is safer than letting a missing
override silently default to deny (or allow).

### 4.3 Non-queue admission-bearing surfaces

These do NOT live in `hub_queue.hpp` hierarchy but still implement
`PeerAdmission`:

- `InboxQueue` (`hub_inbox_queue.hpp:101`) — network-exposed ROUTER.
  Implements `PeerAdmission` directly (not via QueueReader/QueueWriter).
- `BrokerService` ROUTER — exposes `PeerAdmission` on its broker-side
  socket so the broker's own admission policy (known_roles) is enforced by
  the same mechanism.
- `AdminService` REP — same.
- Federation peer ROUTER — same.

All four live behind a `PeerAdmission*` pointer that the broker (or role)
configures with the right allowlist.

## 5. Design — Layer 2: per-transport implementations

### 5.1 `ZmqQueue` (CURVE + ZAP)

**Construction surface change.** New `Options` struct adopted by both
`push_to` and `pull_from`:

```cpp
struct ZmqQueueAuthOptions
{
    /// This queue's own CURVE keypair (loaded from role vault).
    /// REQUIRED whenever data_transport=zmq and admission is enforced.
    /// Empty → admission_is_enforced() returns false (transitional mode).
    std::string my_pubkey_z85;
    std::string my_seckey_z85;

    /// For PULL (consumer) only: producer's pubkey, used as
    /// curve_serverkey on connect.  Empty → reject construction.
    std::string serverkey_z85;

    /// Unique routing key for the ZAP handler.  The ZAP handler uses
    /// (zap_domain, client_key) to look up which queue's allowlist
    /// to consult.  Auto-derived as <role_uid>:<channel>:<side> if empty.
    std::string zap_domain;

    /// Initial allowlist (PUSH side only — consumers admitted at start).
    /// Updated dynamically via set_peer_allowlist().
    PeerAllowlist initial_allowlist;
};
```

**ZmqQueue::Impl extension:** holds a `std::shared_ptr<PeerAllowlist>`
(atomic-swap on update).

**start() additions** (after `socket = zmq::socket_t(...)`, before bind/connect):

```cpp
if (!auth_opts.my_pubkey_z85.empty())
{
    socket.set(zmq::sockopt::curve_publickey, auth_opts.my_pubkey_z85);
    socket.set(zmq::sockopt::curve_secretkey, auth_opts.my_seckey_z85);

    if (mode == Mode::Write) // PUSH (bind side)
    {
        socket.set(zmq::sockopt::curve_server, 1);
        socket.set(zmq::sockopt::zap_domain, auth_opts.zap_domain);
        // ZAP handler thread (singleton, see §5.2) will be queried on
        // every CURVE handshake using this zap_domain.  The handler
        // looks up the allowlist registered against the domain and
        // ACCEPTs / REJECTs accordingly.
        ZapRouter::instance().register_domain(
            auth_opts.zap_domain, this); // 'this' is a PeerAdmission*
    }
    else // PULL (connect side)
    {
        socket.set(zmq::sockopt::curve_serverkey, auth_opts.serverkey_z85);
        // Client side: no ZAP needed; client trusts server via serverkey.
    }
}
// else: legacy transitional path — no CURVE, admission_is_enforced=false.
```

**ZmqQueue overrides** (uses `PortableAtomicSharedPtr` from
`include/portable_atomic_shared_ptr.hpp` per §12.5 S3 — `std::atomic`
on `shared_ptr` was deprecated in C++20 and removed from libstdc++):

```cpp
bool set_peer_allowlist(PeerAllowlist allowlist) override
{
    if (mode_ != Mode::Write || !bind_socket)
        return false; // only PUSH/bind side enforces
    pImpl->allowlist_.store(
        std::make_shared<const PeerAllowlist>(std::move(allowlist)),
        std::memory_order_release);
    return true;
}

std::optional<PeerAllowlist> peer_allowlist_snapshot() const override
{
    if (mode_ != Mode::Write || !bind_socket) return std::nullopt;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    return snap ? std::optional<PeerAllowlist>{*snap} : std::nullopt;
}

bool is_peer_allowed(const PeerIdentity &p) const override
{
    if (mode_ != Mode::Write || !bind_socket) return false;
    auto snap = pImpl->allowlist_.load(std::memory_order_acquire);
    if (!snap) return false;
    if (snap->unrestricted) return true;
    return snap->peers.find(p) != snap->peers.end();
}

bool admission_is_enforced() const noexcept override
{
    return !auth_opts_.my_pubkey_z85.empty();
}
```

### 5.2 `ZapRouter` — caller-pumped (no internal thread)

**Alignment with the locked HEP.**  HEP-CORE-0036 §7.1 already
prescribes this model: *"Handler runs on the BRC poll thread.
Rationale: (a) cache reads and CHANNEL_AUTH_UPDATE writes happen on
the same thread, no synchronization needed; (b) BRC poll thread
already exists."*  The locked HEP says no dedicated ZAP thread.  My
first cut at this section violated that — both empirically (S13
deadlock) and against the HEP.  This section is rewritten to align.

**Architectural correction (2026-06-02).**  An earlier draft of this
section proposed a dedicated polling thread inside `ZapRouter`,
managed by `ThreadManager`, started lazily on first `register_domain`.
That design has two real problems:

1. **Lifecycle re-entrancy deadlock.**  Spawning a `ThreadManager`
   instance triggers `LifecycleManager::register_dynamic_module`
   internally.  If `ZapRouter` is itself a lifecycle module, that
   registration happens from inside `ZapRouter`'s startup callback —
   re-entrant `LifecycleManager` use is forbidden by HEP-CORE-0001
   (the `RecursionGuard` exists for exactly this reason).  Empirically
   verified: my first cut hung at "trying to lock mutex for
   `ThreadManager:ZapRouter:singleton`" inside the startup chain.
2. **A thread for a thing that's idle 99.9% of the time.**  ZAP
   requests arrive only during CURVE handshakes — i.e., at peer
   connect time.  Dedicating a thread that mostly sleeps on
   `recv_multipart(timeout=100ms)` is overhead per process even
   when CURVE never wires.

The corrected design is **caller-pumped**: `ZapRouter` is a passive
state container with an explicit `pump_one(timeout)` method.  The
process's existing event loops drive it; the library owns no thread.

```cpp
namespace pylabhub::utils::security
{

/// Process-wide ZAP handler.  Owns ONE inproc REP socket at
/// `inproc://zeromq.zap.01` (the libzmq ZAP convention; exactly one
/// such endpoint per zmq::context).  Owns a routing map from
/// `zap_domain` to `PeerAdmission*`.  Does NOT own a thread —
/// callers pump.
///
/// Lifecycle: persistent dynamic LifecycleManager module per
/// HEP-CORE-0001.  Module STARTUP binds the inproc REP socket on the
/// calling thread (zero work).  Module SHUTDOWN closes it.  Stays
/// loaded between ref_count==0 transitions (persistent flag).
class ZapRouter
{
public:
    static ZapRouter &instance();

    /// Register a PeerAdmission* for a `zap_domain`.  Returns an RAII
    /// handle whose destructor calls `unregister_domain_` + decrements
    /// the LifecycleManager ref-count.  Thread-safe; protected by an
    /// internal mutex.
    [[nodiscard]] ZapDomainHandle
    register_domain(std::string domain, PeerAdmission *admission);

    /// **Pump one ZAP request from the inproc REP socket.**  Returns
    /// true iff a request was processed; false on timeout.  The
    /// caller MUST drive this from a thread it owns; ZapRouter does
    /// NOT spawn one.  Called from exactly ONE thread per process at
    /// a time (the inproc REP socket is single-thread-only per ZMQ
    /// rules).
    ///
    /// Per-request behavior:
    ///   - Acquires shared lock on registered_; looks up admission*
    ///     by domain.
    ///   - Calls `admission->is_peer_allowed(PeerIdentity{"curve",
    ///     z85(pubkey)})`.
    ///   - Replies 200 (allow) or 400 (deny) with z85(pubkey) as
    ///     user_id.
    ///
    /// Safe to call between `register_domain` calls from other
    /// threads (separate locks for socket vs registered_ — see §7.2).
    bool pump_one(std::chrono::milliseconds timeout);
};

} // namespace
```

**Who pumps in each binary:**

| Binary | Pumping thread | Where in the code (filled in at Phase D) |
|---|---|---|
| `plh_hub` | Broker main thread | `BrokerServiceImpl::run`'s existing `zmq::poll` adds the ZAP socket to its pollset; calls `pump_one(0)` when it signals readable |
| `plh_role` (producer side) | The role's startup/CTRL thread | `BrokerRequestComm::recv_and_dispatch` loop adds the ZAP socket; calls `pump_one(0)` when it signals readable |
| Tests + demos without an event loop | A `ZapPumpThread` RAII helper provided by `pylabhub::utils::security` | Helper holds an `std::thread` that loops `pump_one(100ms)` until destructor sets a stop flag and joins |

**Failure-mode contract.**  If a binary registers a domain but never
calls `pump_one`, every CURVE handshake to a socket using that domain
hangs.  No watchdog — documented at the API level.  This is the price
of the no-internal-thread architecture; the only enforcement is code
review at integration sites (Phase D for `plh_hub` / `plh_role`;
Phase H for demos).

**Lifecycle module shape:**

```cpp
ModuleDef GetZapRouterModule()
{
    ModuleDef def("ZapRouter");
    def.add_dependency("pylabhub::utils::Logger");
    def.add_dependency("ZMQContext");
    def.set_startup([](const char*, void*){
        ZapRouter::instance().bind_inproc_socket_();
    });
    def.set_shutdown([](const char*, void*){
        ZapRouter::instance().close_inproc_socket_();
    }, /*timeout=*/std::chrono::milliseconds(500));
    def.set_as_persistent(true);
    return def;
}
```

No `ThreadManager`, no `std::thread`, no re-entrancy.  The startup
callback does the minimum: bind a socket.  Shutdown closes it.

**RAII handle (S1 finding still holds):**

```cpp
class ZapDomainHandle
{
public:
    ZapDomainHandle() = default;
    ZapDomainHandle(ZapDomainHandle&&) noexcept;
    ZapDomainHandle& operator=(ZapDomainHandle&&) noexcept;
    ~ZapDomainHandle();  // calls router_->unregister_domain_(domain_)

    ZapDomainHandle(const ZapDomainHandle&)            = delete;
    ZapDomainHandle& operator=(const ZapDomainHandle&) = delete;

    bool is_active() const noexcept;
    const std::string& domain() const noexcept;
};
```

**Test fixture helper (provided alongside `ZapRouter`):**

```cpp
class ZapPumpThread
{
public:
    /// Spawns a thread that loops `ZapRouter::instance().pump_one(tick_ms)`
    /// until the destructor sets stop_ and joins.  Suitable for tests
    /// and demos that don't already have a long-running event loop.
    explicit ZapPumpThread(
        std::chrono::milliseconds tick = std::chrono::milliseconds(100));
    ~ZapPumpThread();

    ZapPumpThread(const ZapPumpThread&)            = delete;
    ZapPumpThread& operator=(const ZapPumpThread&) = delete;
};
```

In production (`plh_hub`, `plh_role`), this helper is NOT used —
those binaries integrate `pump_one` into their existing main loops
(Phase D).

### 5.3 `ShmQueue` (broker-issued secret + (optional) UID guard)

SHM queues are local-only. Two layered admission mechanisms:

1. **Broker-issued ephemeral shm_secret** (replaces today's pre-shared
   config-file secret). Broker generates a 64-bit secret per channel,
   stores it in `ChannelAccessIndex[channel]`, includes it in
   `CONSUMER_REG_ACK` ONLY when the consumer's pubkey passed CTRL-channel
   admission. Consumer constructs ShmQueue with the broker-supplied secret;
   producer rejects attaches that present the wrong secret.

2. **POSIX UID guard (optional, secondary defense).** If
   `auth_opts.allowed_uids` is non-empty, ShmQueue sets the segment's
   parent dir mode to 0700 + (optional) ownership match. Decision §8 P5.

**ShmQueue::Options additions:**

```cpp
struct ShmQueueAuthOptions
{
    /// Broker-issued secret token for this channel.  REQUIRED when
    /// shipping data; legacy 0 = transitional.
    uint64_t broker_issued_secret{0};

    /// (Decision §8 P5) optional UID guard.
    std::set<uint32_t> allowed_uids;
};
```

**ShmQueue PeerIdentity kind:** `"shm"` with data = hex-encoded secret.
`is_peer_allowed` returns true iff `PeerIdentity{kind="shm", data=hex(p)}`
matches the queue's `broker_issued_secret`. set_peer_allowlist replaces
the secret. (Yes, the allowlist is conceptually a one-element set for SHM
— the abstraction holds.)

### 5.4 `InboxQueue` (CURVE + ZAP)

Same mechanism as `ZmqQueue` (it's a ZMQ ROUTER/DEALER pair). Implements
`PeerAdmission` directly. Uses `ZapRouter` with its own `zap_domain`.

### 5.5 `BrokerService` ROUTER (CTRL channel admission)

Implements `PeerAdmission`. Broker maintains a `KnownRoleAllowlist` keyed
on `PeerIdentity{"curve", role_pubkey}`. ZAP handler queries it on every
incoming role CURVE handshake. Replaces the string-based
`RoleIdentityPolicy::Verified` check at REG_REQ time — that check becomes
a redundant secondary check (or is deleted).

### 5.6 `AdminService` REP — DECISION POINT (§8 P-Admin)

Two options; designer picks:

- **Option A:** CURVE-wrap the REP socket with the broker keypair; admin
  client uses broker's pubkey as serverkey + a separate admin client
  keypair stored in a separate vault. ZAP allowlist gates admins.
- **Option B:** Hard-enforce loopback-only at bind time (refuse to bind
  any non-127.0.0.1/::1 endpoint). Token stays in cleartext but the wire
  never leaves the loopback interface.

### 5.7 Federation peer ROUTER + DEALER

ROUTER (inbound): ZAP with `KnownPeerAllowlist` keyed on peer pubkey.
DEALER (outbound): already configures `curve_serverkey` from
`peer_cfg.pubkey_z85` (`broker_service.cpp:580-584`); no ZAP needed on
client side.

## 6. Design — Layer 3: broker glue + wire protocol

### 6.1 `ChannelAccessIndex` (broker-side, central state)

```cpp
namespace pylabhub::broker
{

/// Single source of truth for "which peer is allowed on which channel".
/// Lives in BrokerServiceImpl; updated atomically on REG_REQ /
/// CONSUMER_REG_REQ / DEREG_REQ.
struct ChannelAccessEntry
{
    /// Producer side: which consumer pubkeys may pull.
    std::set<std::string> authorized_consumer_pubkeys;

    /// Broker-issued SHM secret (for shm transport channels only).
    /// Zero for zmq channels.
    uint64_t shm_secret{0};

    /// Producer pubkey (delivered to consumers in CONSUMER_REG_ACK).
    std::string producer_pubkey_z85;

    /// Producer endpoint (zmq channels only).
    std::string producer_endpoint;
};

class ChannelAccessIndex
{
public:
    /// On REG_REQ accept (producer): create entry, generate shm_secret if
    /// transport=shm.  Push initial allowlist (empty) to producer via
    /// CHANNEL_AUTH_UPDATE.
    void on_producer_registered(const std::string &channel,
                                  const PeerIdentity &producer_id,
                                  TransportKind transport);

    /// On CONSUMER_REG_REQ accept: add consumer pubkey to channel entry's
    /// allowlist; push CHANNEL_AUTH_UPDATE(add) to producer.
    /// Returns the entry the broker should include in CONSUMER_REG_ACK
    /// (producer pubkey + endpoint + shm_secret).
    ChannelAccessEntry on_consumer_registered(
        const std::string &channel,
        const PeerIdentity &consumer_id);

    /// On DEREG: push CHANNEL_AUTH_UPDATE(remove).  Existing connections
    /// keep flowing (per HEP-0036 §I5 — revocation gates NEW handshakes).
    void on_consumer_deregistered(const std::string &channel,
                                    const PeerIdentity &consumer_id);

    /// Lookup (broker-internal queries).
    std::optional<ChannelAccessEntry> get(const std::string &channel) const;
};

} // namespace
```

### 6.2 `KnownRoleAllowlist` (broker-side, CTRL admission)

Populated at hub startup from `<hub_dir>/vault/known_roles.json` —
a separate file (NOT embedded in the vault payload).  This was the
resolution of §12.5 S5 (option b): keep the encrypted vault
payload focused on this hub's own keys; operator-managed peer
allowlist is a sibling file with its own ACL (mode 0600, owner =
hub euid, parent dir 0700 — verified at load via
`verify_keyfile_acl(VaultFile)`).  Schema:

```json
{
  "version": 1,
  "roles": [
    {
      "name": "lab.daq.sensor1",
      "uid": "prod.sensor.uid12345678",
      "role": "producer",
      "pubkey_z85": "<40-char Z85>"
    }
  ]
}
```

`KnownRole` struct (`role_identity_policy.hpp:120`) gains
`std::string pubkey_z85`. Allowlist used by:

- Broker ROUTER ZAP (admission to CTRL channel)
- `ChannelAccessIndex::on_consumer_registered` (to authorize the consumer
  for the specific channel)

CLI: `plh_hub --add-known-role <name> <uid> <role> <pubkey_z85>`,
`--revoke-known-role <uid>`, `--list-known-roles`.

### 6.3 Wire protocol: `CHANNEL_AUTH_UPDATE` (broker_proto 5 → 6)

New broker → role message (broker initiates; role does not request).

**Payload (snapshot semantics — decision P-Wire RESOLVED 2026-06-02,
see §8.1):**

```json
{
  "type": "CHANNEL_AUTH_UPDATE",
  "channel_name": "lab.daq.temp.raw",
  "side": "producer",
  "allowlist": [
    {"pubkey_z85": "<40-char Z85>"},
    ...
  ],
  "broker_proto": 6
}
```

The `allowlist` array is the **full current authorized consumer
set** for the channel after the mutation event that triggered this
push.  Empty array (`[]`) is the legal "deny everyone" state.  The
producer REPLACES its local ZAP cache for this channel with the
arriving set — no per-pubkey diff, no merge.  See HEP-CORE-0036
§6.5 (amended 2026-06-02) for the on-wire details and the
skip-disconnected push semantics that surround it.

**Transport:** broker → role uses the existing CTRL channel
(DEALER/ROUTER) in *reverse direction*. The DEALER on the role side
already polls inbound messages; today the broker only replies to requests,
but the DEALER/ROUTER pair is bidirectional.

**Producer-side handling:**
`BrokerRequestComm` recognizes `CHANNEL_AUTH_UPDATE` → `RoleAPIBase` looks
up the matching `tx_queue` → `tx_queue->set_peer_allowlist(snapshot)`.
The queue's atomic-swap completes within microseconds. New CURVE handshakes
see the updated allowlist via the ZapRouter; in-flight ZAP requests
already executing see whatever was current at their lookup time (race
acceptable per §I5).  This maps 1:1 onto the existing
`PeerAdmission::set_peer_allowlist(PeerAllowlist)` interface (Phase A) —
which is a REPLACE interface (`PortableAtomicSharedPtr::store`), so the
snapshot semantics on the wire match the snapshot semantics in code with
no transformation.

### 6.4 Wire protocol: `REG_ACK` + `CONSUMER_REG_ACK` payload extensions

Existing acks gain optional fields:

`REG_ACK` (producer):
- (No new fields — `producer_endpoint` was retracted per §12.5 S7.
  The producer already knows its bound endpoint locally; the broker
  echoing it back was redundant and would have invited a stale-echo
  divergence on producer side.)

`CONSUMER_REG_ACK` (consumer):
- `producers[]` — array of `{endpoint, pubkey_z85}` for each producer of
  the channel. Consumer uses these to populate ZmqQueue PULL's
  `curve_serverkey`. Per HEP-CORE-0036 §4.1 these fields already live
  on `ChannelEntry::producers[i]` per-producer (supports fan-in); see
  also §6.1 / §12.5 M-D1 note.
- `shm_secret` (shm transport only) — broker-issued ephemeral secret.

## 7. Lifecycle + threading

### 7.1 Producer-side allowlist lifecycle

```
plh_role startup
  → RoleVault::open → keys loaded
  → BrokerRequestComm::connect (CTRL channel, CURVE)
  → REG_REQ → broker validates pubkey via KnownRoleAllowlist (ZAP)
  → broker creates ChannelAccessEntry, REG_ACK returns endpoint
  → RoleAPIBase::build_tx_queue(opts) — opts now carries my_pubkey/seckey
  → ZmqQueue::push_to(endpoint, ..., auth_opts={my_pubkey, my_seckey, zap_domain})
  → ZmqQueue::start()
       → socket created
       → curve_server=1, curve_*key set
       → ZapRouter::register_domain(zap_domain, this)
       → socket.bind(endpoint)
       → recv_thread / send_thread spawned (existing flow)
  → role is now serving data; current_allowlist_ is empty (deny-all)
       — until first consumer joins

(later, consumer joins:)
broker → producer: CHANNEL_AUTH_UPDATE(allowlist=<full new set including K>)
  → BrokerRequestComm receives on DEALER
  → dispatched to RoleAPIBase
  → tx_queue->set_peer_allowlist(snapshot)
  → atomic swap; next ZAP query sees K

(consumer disconnects:)
broker → producer: CHANNEL_AUTH_UPDATE(allowlist=<full new set without K>)
  → tx_queue->set_peer_allowlist(snapshot)
  → in-flight session unaffected (§I5); new K can't reconnect
```

### 7.2 Threading

| Thread | Touches | Notes |
|---|---|---|
| Role main / Broker main | BrokerRequestComm CTRL ops, build_tx_queue / build_rx_queue, set_peer_allowlist, ChannelAccessIndex, KnownRoleAllowlist | The same thread that runs the binary's event loop ALSO calls `ZapRouter::pump_one(0)` when its `zmq::poll` reports the ZAP inproc socket readable.  No dedicated ZAP thread |
| Queue send/recv | Queue socket only | Per ZMQ thread-per-socket rule (`hub_zmq_queue.cpp:166-200`).  Never touches admin/admission state directly |
| Test `ZapPumpThread` (test/demo only) | ZAP inproc socket (via `pump_one`) | Spawned by the test fixture's RAII helper; joined on test teardown.  Production binaries use their main loop, NOT this thread |
| CLI ops (`--add-known-role` etc.) | KnownRolesStore file + in-memory state | Run before `LoadModule("ZapRouter")` — no concurrency with the inproc socket |

**The "single pumping thread" invariant.**  Per ZMQ, the inproc REP
socket at `inproc://zeromq.zap.01` is single-thread-only.  At any
instant exactly one thread is allowed to call `pump_one()`:

- **`plh_hub` runtime:** broker's main poll thread (`BrokerServiceImpl::run`).
- **`plh_role` runtime (producer side):** the BRC dispatch thread
  (`BrokerRequestComm::recv_and_dispatch`).
- **Tests + bootstrap demos:** the `ZapPumpThread` RAII helper.

Wiring a SECOND pumping caller into the same process is a regression;
the second `pump_one` will fail with `EAGAIN`/`ETERM` once the first
has the socket (libzmq enforces).  Phase D + Phase H integration sites
are the ONLY places that call `pump_one`; documentation pins this.

**Locking summary:**
- `ZapRouter::registered_` — `std::shared_mutex` (writers = register/unregister;
  reader = pumping thread once per request)
- `ZapRouter::sock_` — touched ONLY by the pumping thread; no lock needed
  given the single-thread invariant above
- `ZmqQueue::current_allowlist_` — `PortableAtomicSharedPtr<const PeerAllowlist>`
  (lock-free reads from `is_peer_allowed`; writers swap whole snapshot)
- `ChannelAccessIndex` — broker single-threaded ⇒ no internal lock
- `KnownRoleAllowlist` — broker single-threaded; CLI writes happen
  pre-startup

### 7.3 Lifecycle of dynamic membership

- Producer's allowlist starts empty (default-deny — `is_peer_allowed`
  returns false until the broker pushes a `CHANNEL_AUTH_UPDATE`).
- First `CHANNEL_AUTH_UPDATE` snapshot populates it.
- Every subsequent push is a full snapshot (P-Wire RESOLVED 2026-06-02;
  see §8.1 + HEP-CORE-0036 §6.5 amendment).
- Producer's `tx_queue` stop → `ZapDomainHandle` destructor →
  `unregister_domain_` + `UnloadModule("ZapRouter")` (decrements
  LifecycleManager ref-count; persistent flag keeps the inproc socket
  bound until LifecycleGuard finalize).
- LifecycleGuard finalize → reverse-topo unload → `ZapRouter` shutdown
  callback closes the inproc socket → `ZMQContext` shutdown.

### 7.4 Failure mode — "registered but unpumped"

If a binary calls `register_domain` but no one calls `pump_one`, every
CURVE handshake on any socket using that `zap_domain` will hang at the
libzmq handshake stage (the producer-side data socket's bind returns
fine, but the consumer's connect never completes a handshake).  There
is NO watchdog timer that detects this.  Diagnosis is by
`netstat`/`ss` showing half-open ZMQ connections + LOGGER_INFO from
ZapRouter showing the inproc bind but no per-request log lines.

Prevention is contractual:
- Phase D integration tests (`plh_hub` + `plh_role`) confirm pumping
  is wired before data flows.
- Phase H demos that use `ZapPumpThread` get this for free via the
  RAII pattern.
- L2 tests use `ZapPumpThread`; the absence of it is the same as the
  production "forgot to wire" bug, surfaced immediately as a test
  failure rather than a production hang.

## 8. Decision points for designer (you)

Two tiers below: **resolved** (decision made and shipped in Phases
A/B/C) and **pending** (block Phase D).

### 8.1 Resolved decisions

| # | Decision | Chosen | Shipped in |
|---|---|---|---|
| **P-API** (was S2) | ZmqQueue auth shape | **(a)** Additive `*_with_auth` overloads — no breaking change to legacy `pull_from` / `push_to` callers.  Factory-level validation rejects misconfig before construction. | Phase C commit `7b7944e8` + close-out `cf4b934e` |
| **P-Wire** (was C-D1) | `CHANNEL_AUTH_UPDATE` semantics | **Snapshot.**  Every push is the full current authorized set for the channel; receiver REPLACES its cache (no merge, no per-pubkey diff).  Rationale: hub is the source of truth, producer is a follower; eliminates the silent-drift failure mode of a lost delta-remove on a security gate; matches the existing `PeerAdmission::set_peer_allowlist(...)` REPLACE interface in code with no transformation. | HEP-CORE-0036 §6.5 amended 2026-06-02; design draft §6.3 + §7.1 updated same day; code uses the snapshot interface natively (no change needed) |
| **P-Vault** (was S5) | Where do known roles live? | **(b)** Separate file `<hub_dir>/vault/known_roles.json` with mode 0600, owner = hub euid, parent 0700 — verified via `verify_keyfile_acl(VaultFile)` on every load. | Phase B commit `a6b44ff8` |
| **P-Threading** (was S13) | ZapRouter threading model | Caller-pumped, no internal thread; pumps from BRC poll thread per HEP-CORE-0036 §7.1.  No `ThreadManager`, no LifecycleManager re-entrancy. | Phase C commits `28a06046` (doc) + `827474f0` (code) |
| **P-S3** | `current_allowlist_` atomic primitive | **PortableAtomicSharedPtr** (project utility) instead of `std::atomic_load_explicit(&shared_ptr)` (deprecated in C++20). | Phase C commit `7b7944e8` |

### 8.2 Pending decisions (Phase D blockers)

| # | Decision | Options | Status |
|---|---|---|---|
| ~~P-Wire~~ | ~~Held earlier — RESOLVED 2026-06-02, see §8.1.~~  Decision: **snapshot**.  HEP-CORE-0036 §6.5 amended to match. | | |
| **P-Schema** | `ChannelAccessEntry` field shape | (a) Four fields (authorized_consumer_pubkeys, shm_secret, producer_pubkey_z85, producer_endpoint) per current design §6.1.  (b) Two fields per HEP-CORE-0036 §4.1 (locked) — producer pubkey + endpoint already live on `ChannelEntry::producers[i]` per-producer, supporting fan-in.  Option (a) duplicates state. | **OPEN** — fresh-eye review M-D1: option (b) preserves fan-in semantics.  Designer to confirm + drop the duplicates from §6.1. |

### 8.3 Original open points (others still applicable)

| # | Decision | Options | Recommendation |
|---|---|---|---|
| **P-InboxQueue** | Where does InboxQueue's admission policy live? | (a) Make InboxQueue inherit from QueueReader/QueueWriter (semantic mismatch). (b) InboxQueue implements PeerAdmission directly, no queue inheritance. (c) New `NetworkEndpoint` interface that both implement. | **(b)** — preserves InboxQueue's REQ/REP nature; gate concept still uniform via `PeerAdmission` |
| **P-Default** | Default behavior when no allowlist is set | (a) Deny all (secure-by-default). (b) Allow all transitional during migration. (c) Configurable per-queue. | **(a)** in production; **(b)** behind a `--allow-anonymous-data` flag for transitional demos, gated to refuse-bind on non-loopback endpoints |
| **P-SHM-Identity** | What is a PeerIdentity for SHM? | (a) Broker-issued shm_secret (token-based). (b) POSIX uid+gid (kernel-based). (c) Both layered. | **(a)** primary, **(c)** with optional uid guard if operator sets it; broker controls the gate via secret issuance |
| **P-Push** | How does broker push CHANNEL_AUTH_UPDATE to role? | (a) Reuse CTRL channel DEALER/ROUTER (bidirectional). (b) New PUB socket on broker + SUB on role. (c) New REQ/REP per push (broker initiates). | **(a)** — DEALER/ROUTER is already bidirectional; adding a "from broker" tag to the wire frame is cheap. (b) doubles the wire complexity. |
| **P-Admin** | AdminService — CURVE-wrap or loopback-enforce? | (a) CURVE-wrap with admin client vault. (b) Hard loopback-only enforce + reject non-loopback bind. | **(b)** for v1 — simpler, no separate admin vault needed; CURVE-wrap is HEP-CORE-0035 §5 future work |
| **P-HEP** | What to do about existing HEP-0036 / HEP-0017 §3.3 with wrong layering? | (a) Amend HEP-0036 / HEP-0017 in place. (b) New HEP supersedes affected sections. (c) Keep this tech_draft as authoritative; HEPs synced at end of impl. | **(c)** — finalize impl, then sync HEPs from observed reality. Avoids HEP churn during implementation. |
| **P-Demos** | How do existing demos migrate? | (a) Hard cutover (every demo updated atomically with the impl). (b) Transitional `--allow-anonymous-data` flag (P-Default option b). (c) Per-demo opt-in `auth_enforced: false`. | **(b)** + demos updated incrementally; flag refuses non-loopback bind to prevent accidental production leak |
| **P-Migration** | Operators with pre-auth vaults | They already have CURVE keypairs in vaults. KnownRole needs pubkey_z85 added. | Auto-derive pubkey from existing vault on first start; warn if known_roles.json absent (admit-none until populated by CLI) |

## 9. Implementation roadmap (phases)

Each phase is one or more commits. Tests added with each phase. **No phase
ships without its tests.**

### Status table (last refreshed 2026-06-02)

| Phase | Status | Commits | Pending |
|---|---|---|---|
| A — Abstraction | ✅ shipped | `d5a90f29` | — |
| B — KnownRole + CLI | ✅ shipped | `a6b44ff8` | — |
| C — ZapRouter + ZmqQueue CURVE | ✅ shipped (close-out done) | `28a06046` (doc), `827474f0` (ZapRouter), `7b7944e8` (ZmqQueue), close-out `62bda863..47aa0374` (5 fix-bundle commits) | — |
| D — Broker glue (gate closes) | ⏳ **blocked on designer decisions** | — | C-D1 (snapshot vs delta wire frame) + M-D1 (`ChannelAccessEntry` shape vs HEP-0036 §4.1) |
| E — Admin loopback enforcement | ⏸ planned | — | unblocked once D ships |
| F — Federation parity | ⏸ planned | — | depends on E + task #105 federation design |
| G — SHM auth migration | ⏸ planned | — | independent of D/E/F; can interleave |
| H — Demo migration | ⏸ planned | — | last; needs D shipped end-to-end |

### Phase A — Abstraction (no behavior change yet)

A1. New header `src/include/utils/security/peer_admission.hpp` —
   `PeerIdentity`, `PeerAllowlist`, `PeerAdmission` interface.
A2. `QueueReader` + `QueueWriter` inherit `PeerAdmission` with default-deny
   stubs.
A3. L2 unit tests: identity equality, allowlist set semantics, default
   deny-all on base.

### Phase B — Identity (KnownRole adds pubkey)

B1. `KnownRole` gains `pubkey_z85`. Vault payload schema bump
   (vault_crypto: payload version field; backward-compat read for
   version=1 → assume empty pubkey, warn).
B2. CLI: `--add-known-role`, `--revoke-known-role`, `--list-known-roles`
   on plh_hub.
B3. L2 tests: vault round-trip with KnownRole.pubkey_z85; CLI round-trip.

### Phase C — ZapRouter + ZmqQueue CURVE  ✅ SHIPPED

C1. ✅ `ZapRouter` as a persistent dynamic LifecycleManager module
   in `src/utils/security/zap_router.{hpp,cpp}` — caller-pumped
   (no internal thread; pumps from BRC poll thread per
   HEP-CORE-0036 §7.1).  Exposes `allowed_count()` /
   `denied_count()` observability counters added in close-out
   commit 1/5 (`62bda863`) for path-level test pinning.
C2. ✅ `ZmqAuthOptions` struct (`hub_zmq_queue.hpp:126-156`) +
   `pull_from_with_auth` / `push_to_with_auth` factory overloads
   returning `unique_ptr<ZmqQueue>` so callers can drive the
   `PeerAdmission` interface (Phase D broker glue path).  Factory-
   level validation (`validate_auth_options`, close-out 2/5)
   rejects misconfigured keys at the call site instead of inside
   `start()` against a stale `errno`.  Decision §8 P-API recorded:
   option (a) — additive overloads, not breaking change.
C3. ✅ `ZmqQueue::start()` wires CURVE sockopts and registers with
   `ZapRouter` BEFORE `bind()` so the first peer's handshake
   always lands on a registered domain.
C4. ✅ `ZmqQueue` overrides `PeerAdmission` virtuals.
   `admission_is_enforced()` returns true iff CURVE keys AND
   `running_` — per the interface contract (close-out 2/5
   `cf4b934e`).
C5. ✅ L2 tests: ZAP handshake accept/deny, mid-flight allowlist
   swap (both directions: new peer admitted AND old peer denied),
   raw `pump_one` frame-level tests for malformed/bad-version/non-
   CURVE mechanism, RAII handle move-semantics.  Path-level
   pinning via `denied_count` / `allowed_count` instead of
   timeout-as-proxy.  All security-grade assertions added in
   close-out commits 1/5 + 2/5.

### Phase D — Broker glue (the gate closes)

D1. `ChannelAccessIndex` in BrokerServiceImpl.
D2. Broker ROUTER ZAP handler installation using KnownRoleAllowlist.
D3. `CHANNEL_AUTH_UPDATE` wire frame; broker_proto bump 5 → 6.
D4. BrokerRequestComm + RoleAPIBase dispatch the broker-initiated
   message to the right tx_queue.
D5. CONSUMER_REG_ACK extends with `producers[]` (endpoint + pubkey).
D6. L3 tests: broker push of allowlist; producer applies; consumer with
   wrong pubkey rejected.
D7. L4 test: full dual-hub data flow with auth gates closed.

### Phase E — Admin loopback enforcement

E1. AdminService refuses non-loopback bind.
E2. Documentation update for ops.

### Phase F — Federation parity

F1. Federation peer ROUTER ZAP handler with known-peers allowlist.
F2. L4 federation auth test.

### Phase G — SHM auth migration

G1. ShmQueue gains broker-issued secret (replaces config-pre-shared).
G2. Consumer-side: broker_issued_secret from CONSUMER_REG_ACK.
G3. L3 + L4 SHM auth tests.

### Phase H — Demo migration

H1. Demos updated to either use the auth path or the `--allow-anonymous-data`
   transitional flag.
H2. Migration doc.

### Parallel: Phase X — Runtime key hardening (existing #102)

mlock + zeroing for in-memory keys. Independent of A–H; lands any time.

## 10. Test strategy

| Level | What gets tested | Example |
|---|---|---|
| **L2** | PeerAdmission semantics + each transport's enforcement in isolation | `ZmqQueue` with real CURVE handshake driven through real `ZapRouter` (no mocks — see project rule, MEMORY `feedback_no_mocks_via_observability.md`); ZAP allow/deny path pinned via `ZapRouter::allowed_count()` / `denied_count()` |
| **L3** | Broker → role allowlist push via real BrokerService + BrokerRequestComm | Drive REG_REQ + CONSUMER_REG_REQ; observe set_peer_allowlist called on tx_queue |
| **L4** | Full binary spin-up with auth on; bad pubkey rejected | Two plh_role binaries on different keypairs; one in allowlist, one not; verify only the allowed one receives data |
| **Mutation sweep** | For every assertion in L4 — invert one bit of the allowlist or the keypair — confirm the test fails | Ensures L4 isn't passing by accident |
| **Threading** | Concurrent set_peer_allowlist + is_peer_allowed from a stress harness | Race detection on the atomic swap |

## 11. Migration story for operators

1. Upgrade plh_hub + plh_role binaries (no auth enforced yet — default
   compat is `--allow-anonymous-data` until operator opts in).
2. Run `plh_hub --add-known-role` for each role's pubkey (extracted via
   `plh_role --print-pubkey` — new flag, prints from vault).
3. Restart hub + roles without `--allow-anonymous-data`. Auth is now
   enforced. Any uninvited connection → refused at handshake.

## 12. Open questions / known limitations

- **Compromised broker** — out of scope (HEP-0036 §I8).
- **Forward secrecy of admin token** under loopback-only (P-Admin b) — token
  is still secret material on disk + in process memory; mlock from #102
  covers in-memory exposure.
- **Federation key rotation** — not addressed; out of scope.
- **Script-issued ad-hoc allowlist updates** — not in this phase; needs
  HEP-CORE-0038 vault keystore.
- **Cross-language clients (Python, Lua)** — they go through the same
  RoleAPIBase admission path, so the gate applies uniformly.

## 12.5 Self-critique — problems found during re-investigation (2026-06-02)

After drafting §§1–12 and re-reading against the actual code (not just
HEPs), I found the following design problems / unclarities. **None
require throwing out the architecture**, but each needs a decision before
implementation starts.

### S1 — `ZapRouter` lifetime vs queue lifetime (memory-safety bug)

**Problem.** §5.1 has `ZapRouter::register_domain(zap_domain, this)` where
`this` is a `PeerAdmission*` (the queue). The queue is owned by
`RoleAPIBase` via `unique_ptr`. If the queue is destroyed without
`unregister_domain` being called first, the router has a dangling
pointer that gets dereferenced by the next ZAP request.

**Fix.** Replace raw `PeerAdmission*` with a **RAII registration handle**:

```cpp
class ZapDomainHandle
{
public:
    ZapDomainHandle(ZapRouter &router, std::string domain,
                    PeerAdmission *admission);
    ~ZapDomainHandle(); // calls router.unregister_domain(domain_)
    // non-copyable, non-moveable for safety; queue owns it as a member.
};
```

The queue holds `std::optional<ZapDomainHandle>` (so it can be
constructed-on-start, destroyed-on-stop). Router's internal map stores
`PeerAdmission*` plus a destruction-callback so the handle's destructor
clears the map entry under the router's lock.

### S2 — ZmqQueue public API break (breaks every existing call site)

**Problem.** §5.1 changes `ZmqQueue::push_to` / `pull_from` to take
an `Auth_Options` struct. That's a signature change to a public API
called by `RoleAPIBase`, every demo, every L3 test.

**Options.**
- **(a) Add new overloads** `push_to(endpoint, schema, ..., ZmqQueueAuthOptions)`
  alongside the existing signatures. Old code path still works (admission
  unenforced). New code path goes through the overload.
- **(b) Replace signatures, update every call site atomically.**
- **(c) Builder pattern** `ZmqQueueBuilder{...}.with_auth(...).build_push_to()`.

**Recommendation:** (a) for v1. Old signatures explicitly marked
`[[deprecated]]` once the new path is wired through `RoleAPIBase`.
Deprecation warning steers future code without breaking the build.

### S3 — `std::atomic_load_explicit` for shared_ptr is deprecated in C++20

**Problem.** §7.2 + §5.1 code snippets use `std::atomic_load_explicit(&shared_ptr_var, ...)` — that overload was deprecated in C++20 and removed in C++26 path. The codebase compiles at C++17+ but the **portable replacement exists already**: `src/utils/portable_atomic_shared_ptr.hpp:PortableAtomicSharedPtr<T>` (used by `lifecycle_impl.hpp:308`).

**Fix.** §5.1 uses `pylabhub::utils::detail::PortableAtomicSharedPtr<const PeerAllowlist>` for the queue's allowlist member. The helper delegates to `std::atomic<std::shared_ptr<T>>` when available, falls back to mutex otherwise. Zero new code; reuse the existing pattern.

### S4 — ShmQueue admission is vestigial in the abstraction

**Problem.** §5.3 says ShmQueue's `is_peer_allowed` returns true iff the
candidate identity matches the broker-issued secret. But nothing in
ShmQueue *calls* `is_peer_allowed` — the secret is checked inside
`DataBlockConsumer::attach()`, not at the queue layer. The abstraction
contract is partly fictional for SHM: it's a vehicle for `set_peer_allowlist`
delivery, not for runtime admission.

**Fix.** Either:
- **(a) Wire SHM admission through the abstraction**: `ShmQueue::attach`
  consults `is_peer_allowed(PeerIdentity{"shm", hex(presented_secret)})`
  before opening the segment. Uniform but adds a per-attach atomic load.
- **(b) Document the asymmetry**: SHM's gate is at issuance (broker side),
  not at attach. The abstraction holds for ZAP-mediated transports only.
  `ShmQueue::is_peer_allowed` always returns true (the policy was already
  enforced upstream).

**Recommendation:** (a) — uniformity is worth the atomic load. Locks in the
"gate at the abstraction" invariant; SHM gains a defense-in-depth layer.

### S5 — KnownRole vault location is underspecified

**Problem.** §6.2 says: "Populated at hub startup from the vault payload
(or a file under `<hub_dir>/vault/known_roles.json`)." Both. Pick one.

**Options.**
- **(a) Inside hub vault payload** — same encryption + ACL as the broker
  keypair. Single password unlocks everything. CLI ops require unlocking
  to add/remove a role.
- **(b) Separate file** `<hub_dir>/vault/known_roles.json` (or
  `.known_roles.cbor`), ACL 0600 + parent 0700 (enforced by #101
  utility). No password required for add/remove. Plaintext pubkeys.

**Recommendation:** (b). Pubkeys aren't secret; mode 0600 protects against
casual tampering; mode 0700 parent + #101 startup verify makes the file's
integrity floor identical to the vault file's. Operator UX wins: no
password prompt for `--add-known-role`. Compromised-host scenario doesn't
change either way (encryption-at-rest of pubkeys provides little since
they're broadcast over the wire anyway).

### S6 — Bootstrap scenario (empty known_roles + new deployment)

**Problem.** With deny-by-default, a freshly initialized hub admits zero
roles until the operator runs `--add-known-role` for each. Bootstrapping
the first role requires extracting its pubkey first (`plh_role --print-pubkey`,
new flag). What if the operator wants the demos to "just work"?

**Options.**
- **(a) Hub refuses to start if `known_roles.json` is missing.** Operator
  must explicitly create + populate. Secure-default but harsh.
- **(b) Hub warns + starts with empty allowlist if file missing.** First
  REG_REQ fails informatively pointing to `--add-known-role`. Demo
  experience: same as (a) but with friendlier first error.
- **(c) `--allow-anonymous-roles` opt-in flag** (analogous to
  `--allow-anonymous-data` from P-Default). Demo / dev path uses this;
  refuses non-loopback bind.

**Recommendation:** (b) + (c). Default behavior is informative-fail-fast;
explicit opt-in enables demos without disabling auth for production.

### S7 — `producer_endpoint` in REG_ACK is redundant (editorial)

**Problem.** §6.4 lists `producer_endpoint` as a new REG_ACK field. But
the producer just bound the socket; it already knows its own endpoint.
What the broker needs to relay is the *resolved* endpoint (port-0 case),
which the producer learns post-bind via `socket.get(last_endpoint)` —
not via broker.

**Fix.** Drop `producer_endpoint` from REG_ACK. Keep `producers[]` in
CONSUMER_REG_ACK (consumer DOES need to know producer's resolved endpoint).
That field already exists per HEP-0036 §6.4 intent; just confirming.

### S8 — LifecycleGuard order for `ZapRouter`

**Problem.** `ZapRouter` owns an `inproc://zeromq.zap.01` socket on the
shared ZMQ context. If the context is torn down before the router stops,
its socket destructor explodes.

**Fix.** Register `ZapRouter::module()` as a LifecycleGuard module with
explicit dependency on `GetZMQContextModule()` ordering — router stops
**before** context teardown. Pattern matches existing modules (e.g.,
`Logger`, `FileLock`, `JsonConfig`).

### S9 — InboxQueue doesn't inherit from QueueReader/QueueWriter, yet still needs the gate

**Problem.** Confirmed by code: `hub_inbox_queue.hpp:101, 218` —
`InboxQueue` and `InboxClient` are standalone. Adding `PeerAdmission` as
a separate base class on each is fine, but it means future consumers of
the gate (HEP-0036 implementer, federation) must remember "queues OR
queue-likes". Risk of forgetting one.

**Fix.** No code fix needed — just document explicitly that `PeerAdmission`
is a transport-side contract, not a queue-class contract. §4.3 already
lists every implementer; keep that list canonical and refresh on every
new transport.

### S10 — How does the role know which `tx_queue` a `CHANNEL_AUTH_UPDATE` is for?

**Problem.** A role process can host multiple producers (multi-channel).
When `CHANNEL_AUTH_UPDATE(channel=X)` arrives, `RoleAPIBase` needs to
look up tx_queue by channel name. Verify this lookup exists.

**Investigation note.** `RoleAPIBase::Impl` holds a map of channels to
queues (it must, for multi-channel hosts). Not verified yet — will check
in Phase A scout before coding.

### S11 — Atomic snapshot semantics under add/remove  ✅ ADOPTED 2026-06-02

**Problem.** §6.3 originally listed `op = "set" | "add" | "remove"`. But the
queue's `set_peer_allowlist` takes a full snapshot (`PeerAllowlist`),
not deltas. So either:
- Broker sends full snapshots (simpler; idempotent; bigger over-the-wire
  for huge allowlists).
- Broker sends deltas, role reconstructs full allowlist before passing
  to queue (extra state on the role side; gets out of sync if
  any delta is missed).

**Decision (adopted).** Full snapshots every time.  See §8.1 P-Wire and
HEP-CORE-0036 §6.5 amendment 2026-06-02 for the formal record.
Producer-side caches are followers; the hub owns the truth.

**Wire frame (now canonical in §6.3 of this doc + HEP-0036 §6.5):**

```json
{
  "type": "CHANNEL_AUTH_UPDATE",
  "channel_name": "lab.daq.temp.raw",
  "side": "producer",
  "allowlist": [
    {"pubkey_z85": "<40-char Z85>"},
    ...
  ],
  "broker_proto": 6
}
```

Empty `allowlist` array = revoke all (deny-all state).

### S12 — Re-entry safety on broker restart

**Problem.** Hub restarts. Roles' DEALER connections to broker survive
(ZMQ auto-reconnect), but the broker's `ChannelAccessIndex` is empty
again. Role's tx_queue has a stale allowlist from before the restart.
After re-REG, broker sends fresh `CHANNEL_AUTH_UPDATE` populating the
correct state — but there's a window where the stale allowlist admits a
consumer the new broker hasn't authorized.

**Mitigation options.**
- **(a) On reconnect, role clears its allowlist** (deny-all) until
  receiving a fresh CHANNEL_AUTH_UPDATE.
- **(b) Persistent ChannelAccessIndex in broker** so restart restores
  prior state.
- **(c) Accept the gap** as a known limitation; document.

**Recommendation:** (a). Tie to BrokerRequestComm's reconnect callback.
Brief window where data flow halts to consumers is acceptable; risk of
admitting an unauthorized consumer during the gap is not.

### S13 — `ZapRouter` thread + `ThreadManager` deadlock (2026-06-02, empirical)

**Problem.** First implementation cut had `ZapRouter` spin up a
`ThreadManager` instance from inside its lifecycle module startup
callback.  Confirmed at runtime: the test hung at
`registerDynamicModule: trying to lock mutex for
'ThreadManager:ZapRouter:singleton'` inside the LifecycleManager's
load-module path that was running the startup callback.

**Root cause.** `ThreadManager`'s constructor calls
`LifecycleManager::register_dynamic_module` (`thread_manager.cpp:331`)
to register its own dynamic lifecycle entry.  But the call site is
already inside another `LifecycleManager` lock acquired by the
`LoadModule("ZapRouter")` that triggered our startup callback.
HEP-CORE-0001 explicitly forbids this re-entrancy pattern (the
`RecursionGuard` documented in §"Design philosophy / Constraints").

**Architectural fix (NOT just "avoid `ThreadManager`").**  The
deeper question: does ZAP need a thread at all?  Re-examining the
ZAP semantics — it's a request-reply RPC handler that's idle 99.9%
of the time.  A dedicated thread is heavyweight for that load.
Caller-pumped design (every process already has a main event loop;
add the ZAP socket to its pollset) is structurally simpler AND
removes the re-entrancy problem by construction (no internal
`ThreadManager` to register).

Doc text in §5.2 was rewritten end-to-end to reflect the
caller-pumped model.  §7.2 threading table redrawn.  §7.4 documents
the "registered but unpumped" failure mode that the caller-pumped
design introduces (no internal watchdog).

**Lessons captured for future phases.**
1. Any new lifecycle module's startup callback must not call into
   `LifecycleManager` (directly or transitively via constructors
   that register).  Phase G's ShmQueue broker-issued-secret path
   needs to respect the same rule.
2. "It uses `ThreadManager` everywhere else in the codebase" is not
   a sufficient reason to use it here.  `ThreadManager`'s value is
   bounded-drain semantics for threads doing user-defined work.
   For passive RPC handlers, plain integration into an existing
   poll loop is simpler.

---

**Summary of self-critique.** 13 issues found in this draft via
re-investigation. Categorization:
- S1, S3, S8, **S13** — concrete bugs in proposed code; fix in §5/§7.
- S2, S5, S6, S11, S12 — design decisions deferred to designer (added
  to §8 as P-API, P-KnownRolesLocation, P-Bootstrap, P-WireFormat,
  P-Reconnect).
- S4, S9, S10 — clarifications added inline.
- S7 — editorial fix.

12 of 13 surfaced BEFORE writing a line of code.  S13 surfaced
during implementation — the empirical-evidence reminder that the
"plan from documentation" stage is not a substitute for actually
running the code.

## 13. Glossary

- **PeerIdentity** — opaque (kind, data) tuple; per-transport meaning.
- **PeerAllowlist** — set of admitted identities + an `unrestricted` escape.
- **PeerAdmission** — interface; every gated transport implements it.
- **ZapRouter** — singleton ZAP handler on the shared ZMQ context.
- **ChannelAccessIndex** — broker-side single source of truth for
  per-channel allowlists.
- **KnownRoleAllowlist** — broker-side allowlist for CTRL-channel
  admission, keyed on role pubkey.
- **zap_domain** — per-socket routing key the ZAP handler uses to find
  the right PeerAdmission.

---

**Designer review checklist:**

- [ ] Read §2 threat model. Does it cover what you want defended?
- [ ] Read §3 evidence. Any gap I missed?
- [ ] Read §4 abstraction. Does the layering match your mental model?
- [ ] Read §5 per-transport. Any transport's mechanism wrong?
- [ ] Read §6 broker glue. Wire protocol shape OK?
- [ ] Read §8 decision points. Each one needs your call before impl.
- [ ] Read §9 roadmap. Phase ordering correct?
- [ ] Read §11 migration. Realistic for your operators?

When ready, give me decisions for each P-* in §8 and I'll update the doc
+ retask + start Phase A.
