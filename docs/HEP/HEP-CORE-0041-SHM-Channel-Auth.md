# HEP-CORE-0041: SHM Channel Auth (Cross-Platform)

| Property | Value |
|---|---|
| **HEP** | `HEP-CORE-0041` |
| **Title** | SHM Channel Auth — Capability Transport + Pre-Confirm Admission |
| **Status** | 🟢 **DESIGN FINAL; PHASE 1 IN FLIGHT** — promoted from `docs/tech_draft/` 2026-06-16; substeps 1a-1g shipped; 1h-1k pending.  Live status table at §10.1. |
| **Created** | 2026-06-16 |
| **Last revised** | 2026-06-18 — **§5 + §6 + §7 + §10 + §11 + §12 + §13 macOS resynced against shipped code**: bearer-token model in §5 replaced by the actual `crypto_box` challenge-response + `CONSUMER_ATTACH_REQ` pre-confirm; §6 strawman code replaced by L1/L2 split as shipped (with verbatim interfaces from headers); §7 compatibility list rewritten to match the substep chain; §10 gained substep-level §10.1 status table; §11 + §12 status updated for what's shipped vs pending; §13 macOS corrected (`SHM_ANON` is FreeBSD-only, not macOS — backend uses `shm_open`+immediate-`shm_unlink` trick).  Prior revision 2026-06-17 — §9 D4 attach sequence amended for crypto_box (substep 1c).  Prior revision 2026-06-16 — promoted from tech_draft. |
| **Tracker** | task **#244** (umbrella); per-phase tasks under §10 |
| **Sibling docs** | HEP-CORE-0036 (ZMQ Auth — symmetrizes to pre-confirm via task #246); HEP-CORE-0002 (DataBlock — consumes capability abstraction); HEP-CORE-0040 (Locked Key Memory — backing for any role-level encryption); HEP-CORE-0038 (script vault — sibling for script audience); HEP-CORE-0011 (script-engine parity — applies to #247 follow-up) |
| **Filed by** | discussion 2026-06-16 after AUTH-4 (#164) gap analysis surfaced the structural weakness of "secret as discriminator" + POSIX `0666` default |
| **Cross-platform constraint** | MUST work on Linux + FreeBSD + macOS + Windows |
| **Closes** | **#164** (AUTH-4 — superseded; `shm_secret` retires under D1+D7), **#79** (SHM seed in `--init` — superseded with #164) |
| **Does NOT block** | task **#245** (POSIX 0666 → 0600 interim hardening — can ship independently if needed before Phase 1, becomes moot once capability transport ships) |

> The structure mirrors HEP-CORE-0036 deliberately so a reader can
> compare the SHM model side-by-side with the ZMQ model.  All §9
> design decisions were locked during the 2026-06-16 discussion; the
> `[DECISION:]` markers that previously punctuated the body are now
> answered in the §9 table.

---

## 1. The gap (why this exists)

### 1.1 What works today on ZMQ transport (HEP-CORE-0036)

- **Layer 1 (control plane)** — BRC CURVE+ZAP authenticates the role to the broker.  Outsider on the wire sees ciphertext; outsider not in `known_roles[]` is rejected at handshake.
- **Layer 2 (channel scope)** — broker validates REG_REQ against channel ACL.  An authenticated role is only admitted to channels it's permitted on.
- **Layer 3 (data plane)** — role's data ROUTER CURVE-server enforces the per-peer allowlist locally.  Even if an outsider somehow learns the data endpoint, the per-peer CURVE handshake against the role's ZAP cache rejects them.

Three layers, each enforced by a kernel-level mechanism that doesn't rely on operator umask or shell hygiene.

### 1.2 What today's SHM does

- **Layers 1+2**: same as ZMQ (broker-side admission).  ✅
- **Layer 3 — DOESN'T EXIST FOR SHM.**

The role's SHM channel data lives at `/dev/shm/<name>` (POSIX) or in a kernel named-object (Windows).  The `shm_secret` was designed as a Layer-3 gate, but it is **not actually one**:

- The secret is stored **plaintext in the first 8 bytes of the SHM header** (`data_block.cpp:445-449`).
- POSIX `kShmModeRw = 0666` defaults the file to `0644` after typical umask → **world-readable on the host**.
- Any non-root user with shell access can `ls /dev/shm/`, `shm_open(name, O_RDONLY)`, `mmap`, and read both the secret AND the channel data.
- The secret is therefore a **discriminator** (helps detect cross-channel name collisions and stale connections), not an access control.

Windows is slightly better by default (DACL inherited from the creator's token, no world-readable equivalent), but cross-user same-host on Windows is still effectively unguarded unless we set an explicit DACL.

### 1.3 Production-readiness gap

> A consumer should be able to read SHM channel data IF AND ONLY IF the
> broker has authorized it for that channel.

Today's code does not enforce this on any platform.  Operator umask is the *only* line of defense on POSIX; default umask makes the data world-readable.  This is unacceptable for a scientific data acquisition framework intended to handle potentially sensitive experiments.

---

## 2. Threat model

Explicitly enumerated so the design choices below can be argued against it.

| # | Threat | In scope to defeat? | Mechanism |
|---|---|---|---|
| T1 | Network attacker (off-host) | YES | SHM is local; process isolation. Already defeated. |
| T2 | On-host attacker, different EUID, shell access, no privilege escalation | **YES** | The core target of this HEP. |
| T3 | On-host attacker, **same EUID** as the role process | NO | Same-EUID = same trust domain on POSIX. Process isolation can't help; out of scope. |
| T4 | Root / Administrator on the host | NO | Always wins. Out of scope. |
| T5 | Compromised script inside the role process | PARTIAL | Script-side observability rules (HEP-0036 §I11 — scripts never see the secret) apply.  Encryption/capability can't help if the script controls the role; rely on script sandboxing. |
| T6 | Cold-storage attacker who acquires `/dev/shm` contents post-mortem | OPTIONAL | Defeated by Option C (encrypt-at-rest).  Otherwise out of scope. |
| T7 | Kernel-memory snoop | NO | Out of scope; requires kernel-level mitigations. |
| T8 | Side-channel (cache timing, memory contention) | NO | Out of scope. |

**Primary target: T2** — different-EUID outsider on the same host.  This is the threat that the current design fails to defeat.

---

## 3. Mechanism options

Four options, with trade-offs.  **[DECISION needed: which one ships as default?]**

### 3.1 Option A — Capability via anonymous mapping + handle transfer

The producer creates a **nameless** mappable region.  No filename, no kernel-object-name, nothing to enumerate or probe.  The producer then sends the **OS-level handle** to the mapping to authorized consumers via an OS-specific IPC mechanism.  Consumer maps the received handle directly.

- **Outsider cannot enumerate** the region — there's no name.
- **Outsider cannot open** it — without the handle, no access.
- **The handle IS the capability.**  Whoever has it can access; whoever doesn't can't.
- Broker mediates the handle transfer (or authorizes a direct role-to-role handoff).

| OS | Anonymous source | Handle transfer |
|---|---|---|
| Linux | `memfd_create()` | Unix socket + `SCM_RIGHTS` ancillary data |
| FreeBSD (≥13) | `memfd_create()` | Unix socket + `SCM_RIGHTS` |
| FreeBSD (<13) | `shm_open(O_CREAT)` + `shm_unlink` immediately | Unix socket + `SCM_RIGHTS` (the unlinked fd survives) |
| macOS/Darwin | `shm_open(SHM_ANON, ...)` — yes, macOS has `SHM_ANON`! | Unix socket + `SCM_RIGHTS` |
| Windows | `CreateFileMapping(INVALID_HANDLE_VALUE, ..., NULL)` — name=NULL → anonymous | Named pipe + `DuplicateHandle` (target process must be reachable for `OpenProcess`) |

**Pros:**
- Cleanest security: capability model, no name to leak, no secret to manage.
- Defeats T2 by construction across all four platforms.

**Cons:**
- Per-consumer handle transfer adds a Unix socket / named pipe step at consumer-register time.
- Broker becomes a handle-passing relay (or authorizes a direct producer-to-consumer Unix socket).
- Windows path is significantly different (needs `OpenProcess` rights between producer and consumer); deployments where producer and consumer are in different Windows desktops or service contexts may need special handling.

### 3.2 Option B — Named SHM + 0600 + per-consumer ACL grant

Producer creates `/dev/shm/<random_name>` with mode 0600.  Broker tells producer the consumer's EUID at REG_REQ-accept time.  Producer adds an ACL grant for that EUID.

| OS | Name | Per-consumer grant |
|---|---|---|
| Linux | `shm_open(..., 0600)` | `setfacl -m u:<consumer_uid>:rw` on `/dev/shm/<name>` |
| FreeBSD | `shm_open(..., 0600)` | NFSv4 ACLs (less standardized than Linux POSIX ACLs) |
| macOS/Darwin | `shm_open(..., 0600)` | `chmod` only; no per-user ACL beyond owner/group/other |
| Windows | `CreateFileMapping(...)` with explicit DACL | DACL augmented via `SetSecurityInfo` |

**Pros:**
- Simpler than Option A on Linux + Windows (no fd-passing).
- Named API — familiar.

**Cons:**
- Name is still listed in `/dev/shm/` — outsider can enumerate even if they can't open.  Defeats T2 from *attaching* but not from *probing existence/size*.
- macOS lacks POSIX ACLs in the Linux sense — can only do owner/group/other.  Multi-consumer same-uid-only.
- ACL race: brief window between create + first ACL grant where the file is owner-only-still-readable.
- Dynamic membership: consumer joins/leaves → ACL update → race with consumer's `shm_open` retry.

### 3.3 Option C — Encrypt-at-rest in SHM slots

Data slots are AES-GCM-encrypted with a key derived from a broker-distributed secret.  Producer encrypts before commit; consumer decrypts after acquire.

- **Outsider with `mmap` access sees ciphertext** — defeats T2 AND T6 (cold storage).
- Channel name + size still observable, but data confidentiality holds.

**Pros:**
- True confidentiality.  Survives even if mode bits / DACL fail.
- Defeats T6 (cold-storage attacker) — a unique strength.

**Cons:**
- Per-slot crypto cost.  AES-GCM in software is ~1-3 GB/s on modern x86 with AES-NI.  For DAQ pipelines pushing 100s of MB/s of float32 samples, this is the gating cost.
- Defeats the *zero-copy* premise of SHM (consumer must touch every byte).
- Key management — see §4.

### 3.4 Option D — Hybrid: A as transport, C opt-in per-channel

- Default: Option A.  Capability transport handles access control.  No "secret" to manage, no rotation, just fd ownership.
- Per-channel `confidentiality = "encrypted"` config field: layers Option C on top.  Operator opts in for high-sensitivity channels where T6 matters or where defense-in-depth against same-EUID compromise is wanted.

**Pros:**
- Strong default (A defeats T2 cleanly).
- Opt-in C for high-sensitivity channels — accepts the CPU cost where it matters.
- No "one size fits all" — operators can rate each channel.

**Cons:**
- More moving parts in the codebase.
- Config story is more complex (`shm_secret` retires; new `confidentiality` field).

**[DECISION:]** Default to Option A or Option D?  My read: A for MVP, D as the documented enhancement path.  D's encryption work can be a follow-up HEP without changing A's interface.

---

## 4. Key / secret lifecycle — possibly always real-time

The user explicitly raised this: should the secret be generated and rotated in real time, or is a startup-time key sufficient?

Generation timing options, ranked by strength:

| Strategy | Producer generates… | Distributed via | Rotation | Strength |
|---|---|---|---|---|
| 0. Static-in-config | Once at config-write time | Disk (operator-managed) | Never | Today's broken model |
| 1. Per-process-startup | At producer process start | REG_REQ → REG_ACK over CURVE+ZAP BRC | On producer restart | Better |
| 2. Per-channel-open | At each channel-open | REG_ACK + CONSUMER_REG_ACK | On channel close+reopen | Better; minimizes blast radius of leak |
| 3. Per-consumer-session ephemeral | At each consumer attach | CONSUMER_REG_ACK; one unique key per consumer | Per consumer | Strong — leaked key only affects that one consumer |
| 4. Continuous rotation | Background rotation every N seconds; broker pushes via `CHANNEL_KEY_ROTATION_NOTIFY` | CURVE+ZAP BRC | Continuous | Strongest |

For **Option A (capability)** the "key" is the FD itself — there's no secret to generate.  Rotation = close + reopen the mapping.  The lifecycle questions are answered by FD lifetime semantics.

For **Option C (encryption)** the key matters and lifecycle is a real design choice:

### 4.1 Recommended scheme for Option C (encryption channels)

**[DECISION:]** Pick a level on this lifecycle ladder:

1. **MVP**: Strategy 2 (per-channel-open).  Producer mints a 256-bit AES key at channel-open via libsodium `randombytes_buf`.  Broker stores it in the channel access entry (`ChannelAccessEntry::shm_data_key`, replaces today's `shm_secret`).  Consumers receive it via `CONSUMER_REG_ACK.shm_data_key`.  Rotation only on channel reopen.
   - **Strength**: leaked key from one channel doesn't compromise another channel.
   - **Weakness**: a long-running channel uses one key for its lifetime.

2. **Phase 2**: Add Strategy 4 (continuous rotation) on top.  Background rotation every N seconds; broker pushes new key over `CHANNEL_KEY_ROTATION_NOTIFY` (mirror of the existing `CHANNEL_AUTH_CHANGED_NOTIFY` pattern).  Producer and all consumers switch atomically at the next slot boundary.  Two-key window during transition (slot encrypted with old key OR new key; consumer accepts either).
   - **Strength**: leaked key only compromises N seconds of past data.
   - **Cost**: complex synchronization at slot boundaries.  Worth it for sensitive channels.

3. **High-security mode**: Strategy 3 (per-consumer-session).  Each consumer's `CONSUMER_REG_ACK` carries a UNIQUE wrapped key.  Producer encrypts each slot with a "channel master key" but distributes it wrapped under each consumer's session key.
   - **Cost**: per-consumer key wrapping overhead at consumer-attach time.  Storage cost per consumer.  Probably not warranted for typical DAQ.

**My read**: ship MVP (Strategy 2).  File Phase 2 as a follow-up task.  Strategy 3 only if a specific deployment ever demands it.

### 4.2 Key derivation

Use HKDF (libsodium `crypto_kdf_*`) over the broker-distributed seed to derive:
- Slot-encryption key (AES-GCM)
- Slot-nonce derivation key (deterministic per (slot_index, generation))

**[DECISION:]** AES-GCM or ChaCha20-Poly1305?  AES-GCM is hardware-accelerated on modern x86 (AES-NI) — faster.  ChaCha20-Poly1305 is faster on platforms without AES-NI (some ARM) and constant-time by construction.  pyLabHub deployments are primarily x86 + ARM.  My read: ChaCha20-Poly1305 default; performance is comparable on modern x86 too.

### 4.3 Where the key lives

- **On broker**: `ChannelAccessEntry::shm_data_key` — `LockedKey` (HEP-CORE-0040), mlocked.
- **On producer**: in-memory derived key for the lifetime of the channel.  Should be `LockedKey`.
- **On consumer**: same shape as producer.
- **On disk**: NEVER.  No persistence.  All keys ephemeral.

This is a meaningful escalation from today's plaintext `uint64_t` everywhere.  Worth doing if encryption is in scope; pointless overhead if it isn't.

---

## 5. Wire protocol delta

> **Rewritten 2026-06-18 against shipped code.**  The original §5 described
> a bearer-token model (`consumer_authorization_token` signed by broker)
> that D5 retired under D4's pre-confirm pattern.  Substeps 1c (#250) +
> 1d (#251) shipped the actual wire shape; the description below matches
> what's in the code.  Substep 1g (#254) lands the doc + REG_ACK shape
> cleanup in lockstep with deleting the legacy `shm_secret` field.

If Option A (capability) ships as the default:

### 5.1 REG_REQ — what producer says

`shm_secret` field retires entirely.  Producer adds:

```json
{
  ...,
  "data_transport": "shm",
  "shm_capability_endpoint": "unix:///var/run/pylabhub/role-acquisition.daq01.prod/shm-cap.sock"
}
```

Producer is responsible for binding a Unix socket at `shm_capability_endpoint`
(via `IShmCapabilityProducer::bind_endpoint`).  Broker validates the
endpoint string and stores it in `ChannelAccessEntry`.

### 5.2 REG_ACK — what broker tells producer

No new fields.  REG_ACK confirms admission; broker does NOT issue any
authorization material at this point (per D5: no bearer tokens).
Authorization happens later, per-attach, via `CONSUMER_ATTACH_REQ`
(§5.4 below).

### 5.3 CONSUMER_REG_ACK — what consumer receives

```json
{
  ...,
  "data_transport": "shm",
  "shm_capability_endpoint": "unix:///var/run/pylabhub/role-acquisition.daq01.prod/shm-cap.sock",
  "producer_pubkey_z85": "<40 Z85 chars>"
}
```

The consumer receives the producer's endpoint + the producer's pubkey
(needed for the consumer-side `crypto_box_easy` encryption in §5.5
frame 2).  No bearer token; the broker is consulted live by the
producer on each attach attempt — see §5.4.

### 5.4 CONSUMER_ATTACH_REQ — producer pre-confirms (HEP-0041 §9 D4)

Per D4's pre-confirm pattern, the producer queries the broker on every
attach attempt before sending the SHM capability fd.  Shipped under
substep 1d (#251); handler at `broker_service.cpp::handle_consumer_attach_req`.

Request from producer to broker:
```json
{
  "channel_name":      "lab.raw",
  "consumer_pubkey":   "<40 Z85 chars>",
  "consumer_role_uid": "consumer.daq01.uid0042",
  "role_uid":          "<producer's own role_uid>",
  "correlation_id":    "<optional>"
}
```

Reply (auth decision; `CONSUMER_ATTACH_ACK` envelope):
```json
{ "status": "success",  "channel_name": "...", "consumer_pubkey": "..." }
{ "status": "denied",   "channel_name": "...", "consumer_pubkey": "...",
  "denial_reason": "consumer_pubkey not in channel allowlist" }
```

Reply (protocol-level errors, `ERROR` envelope):
- `INVALID_REQUEST` — missing field.
- `CHANNEL_NOT_FOUND` — channel doesn't exist on the broker.
- `PRODUCER_NOT_AUTHORIZED` — caller `role_uid` is not a registered
  producer of the channel (defence in depth — never disclose another
  channel's auth state to a non-producer).
- `INTERNAL_ERROR` — HubState invariant broken (broker bug).

**"denied" is distinct from "error"**: producer-side cache-divergence
WARN logic (substep 1e, see §9 D4 cached-allowlist semantics) needs
to distinguish a clean broker "no" from a wire-level transport
failure.  Dispatcher special-case maps `(status=success | denied)` →
`CONSUMER_ATTACH_ACK`, others → `ERROR`.

### 5.5 Attach-time L2 handshake — `crypto_box` challenge-response

Per substep 1c (#250), the producer's Unix socket listener runs a
two-frame challenge-response with the connecting consumer to prove
the consumer holds the seckey for its claimed pubkey.

Frame 1 (producer → consumer, length-prefixed JSON, 4 KiB DoS cap):
```json
{ "protocol_version": "hep-0041-1",
  "nonce_b64":        "<24-byte crypto_box nonce, base64>",
  "challenge_b64":    "<16 random bytes, base64>" }
```

Frame 2 (consumer → producer, same framing):
```json
{ "protocol_version":         "hep-0041-1",
  "role_uid":                 "consumer.daq01.uid0042",
  "pubkey_z85":               "<40 Z85 chars>",
  "challenge_response_b64":   "<crypto_box_easy(challenge, nonce,
                                                 producer_pk,
                                                 consumer_sk), base64>" }
```

Producer verifies via `crypto_box_open_easy(cipher, nonce, consumer_pk,
producer_sk)`; MAC verification + recovered plaintext equality with
the issued challenge → cryptographic proof of `consumer_sk`
possession.  Same security property as ZMQ's CURVE handshake, using
the same Curve25519 keypairs (no separate Ed25519 signing key
needed).

The `crypto_box` model replaced an earlier "sign with CURVE seckey"
strawman that was cryptographically broken (CURVE seckeys are
Curve25519 ECDH keys, not signing keys; libsodium has no such
primitive).  See substep 1c implementation + §9 D4 amendment block.

**Known limitation.**  This handshake is one-way (consumer proves to
producer).  The symmetric direction — producer proves to consumer —
is tracked under task #262 (mutual-auth follow-up) before Phase 1
production-readiness.

---

## 6. Cross-platform abstraction

> **Rewritten 2026-06-18 against shipped code.**  The original §6 mixed
> transport (anon shm + fd handoff) with auth (token validation) in a
> single `serve_one(SignedAuthToken)` method.  During substep 1a (#248)
> the design was refactored into an explicit **L1 / L2 split** —
> backends only know transport primitives; the auth orchestration is
> a separate layer that consumes L1 from above.  This eliminates
> per-backend duplication of auth logic when macOS / Windows ship.

### 6.1 Layering

```
L3  (broker side)        BrokerService::handle_consumer_attach_req
                         (substep 1d, #251 — read-only against
                         ChannelAccessIndex.authorized_consumer_pubkeys)

L2c (orchestration)      ShmAttachOrchestrator (substep 1e, #252)
                           ├─ injected CacheLookup
                           ├─ injected BrokerQuery (-> CONSUMER_ATTACH_REQ)
                           └─ divergence-WARN per §9 D4 table

L2b (auth handshake)     AttachProtocolAcceptor (substep 1c, #250)
                           + initiate_consumer_handshake
                           ├─ SO_PEERCRED uid sanity (HEP-0036 §I8)
                           ├─ crypto_box challenge-response (§5.5)
                           └─ returns AuthenticatedConsumer

L1  (transport)          IShmCapabilityProducer / IShmCapabilityConsumer
                         (substeps 1a + 1b, #248 + #249)
                           ├─ bind_endpoint, accept_one, send_capability
                           └─ per-platform backend (Linux only today;
                              FreeBSD/macOS/Windows #error pending
                              tasks #259 / #260 / #261)
```

### 6.2 Code structure (as shipped)

```
src/include/utils/security/
    shm_capability_channel.hpp       // L1 abstract interface
    attach_protocol.hpp              // L2b handshake (substep 1c)
    shm_attach_orchestrator.hpp      // L2c orchestrator (substep 1e)

src/utils/security/
    shm_capability_channel.cpp       // L1: per-platform backends
                                     //   (Linux working; others #error)
    attach_protocol.cpp              // L2b impl (Linux only)
    shm_attach_orchestrator.cpp      // L2c impl (Linux only)
```

D6 decision (`utils/security/`) ratified by the as-shipped placement
above; the proposed `shm_capability/` subdirectory was not needed —
single-file backends are clearer at current size.

### 6.3 L1 interface (verbatim from shipped header)

```cpp
class IShmCapabilityProducer {
public:
    struct AcceptedPeer {
#if defined(PYLABHUB_IS_WINDOWS)
        void          *peer_pipe_handle{nullptr};  // HANDLE (opaque)
        unsigned long  peer_pid{0};
#else
        int   peer_socket_fd{-1};
        pid_t pid{};
        uid_t uid{};
        gid_t gid{};
#endif
    };

    virtual bool bind_endpoint(const std::string& endpoint)        = 0;
    virtual std::optional<AcceptedPeer>
        accept_one(std::chrono::milliseconds timeout)              = 0;

#if defined(PYLABHUB_IS_WINDOWS)
    virtual bool send_capability(void* peer_pipe_handle,
                                 unsigned long peer_pid)           = 0;
#else
    virtual bool send_capability(int peer_socket_fd)               = 0;
#endif

    virtual std::span<std::byte> data()                            = 0;
    virtual size_t                size() const noexcept             = 0;
};

class IShmCapabilityConsumer {
public:
    virtual std::span<std::byte> data()                            = 0;
    virtual size_t                size() const noexcept             = 0;
};
```

Per-platform `AcceptedPeer` variants: POSIX exposes the kernel-cred
triple; Windows exposes the pipe HANDLE (as `void*` to keep the public
header free of `<windows.h>` — matches `pylabhub::platform::ShmHandle`
in `plh_platform.hpp:152`).  `send_capability` similarly per-platform.

### 6.4 L2 interface (verbatim from shipped headers)

```cpp
struct ConsumerAuthMaterial {
    std::string    role_uid;
    std::string    pubkey_z85;
    SeckeyAccessor seckey_accessor;   // use-not-export per HEP-0040 §5.2
};

struct AuthenticatedConsumer {
    IShmCapabilityProducer::AcceptedPeer raw_peer;
    std::string                          consumer_role_uid;
    std::string                          consumer_pubkey_z85;
};

class AttachProtocolAcceptor {
public:
    AttachProtocolAcceptor(IShmCapabilityProducer& transport,
                           uid_t expected_uid,
                           SeckeyAccessor producer_seckey_accessor);
    std::optional<AuthenticatedConsumer>
        accept_one(std::chrono::milliseconds timeout);
};

std::optional<int>
initiate_consumer_handshake(const std::string& endpoint,
                            const ConsumerAuthMaterial& self,
                            const std::string& producer_pubkey_z85,
                            std::chrono::milliseconds timeout);

class ShmAttachOrchestrator {
public:
    using CacheLookup = std::function<bool(const std::string&)>;
    using BrokerQuery = std::function<std::optional<nlohmann::json>(
        const std::string& consumer_pubkey,
        const std::string& consumer_role_uid)>;
    enum class Outcome { Sent, DeniedByBroker, DeniedTransportFail,
                         HandshakeFailed, Timeout };

    Outcome accept_and_serve_one(std::chrono::milliseconds timeout);
};
```

### 6.5 Per-platform backend status

| Platform | L1 backend | L2b / L2c | Task |
|---|---|---|---|
| Linux     | ✅ Shipped (`memfd_create` + `SO_PEERCRED` + `SCM_RIGHTS`) | ✅ Shipped | #249, #250, #252 |
| FreeBSD   | ⛔ `#error` + design plan in .cpp                          | ⛔ `#error` | #259 |
| macOS     | ⛔ `#error` + design plan in .cpp                          | ⛔ `#error` | #260 |
| Windows   | ⛔ `#error` + design plan in .cpp                          | ⛔ `#error` | #261 |

---

## 7. Compatibility / migration

> **Updated 2026-06-18 against shipped code.**  The original §7 listed
> "renames" that didn't match what ended up being built (e.g.
> `ChannelAccessEntry::shm_secret` did not rename to
> `shm_capability_endpoint`).  The list below reflects what's actually
> happening across the substep chain.

**Substep 1d (#251, shipped):** broker handler for `CONSUMER_ATTACH_REQ`
added.  No fields removed yet — purely additive.

**Substep 1f (#253, in progress):** DataBlock gains an fd-based
producer/consumer ctor variant alongside the existing name-based path.
Both paths coexist briefly so production tests can migrate
incrementally.

**Substep 1g (#254, pending):** wire-shape clean break.
- Remove `shm_secret` from `CONSUMER_REG_ACK` (and from the wire-shape
  conformance helper).
- Finalize `CONSUMER_ATTACH_REQ` / `_ACK` shape in HEP-CORE-0007.
- Add `shm_capability_endpoint` + `producer_pubkey_z85` to
  `CONSUMER_REG_ACK` for SHM channels.

**Substep 1h (#255, pending):** config schema rejection of
`in_shm_secret` / `out_shm_secret`.  Producer init template stops
emitting the field.

**Substep 1i (#256, pending):** code cleanup — deletes:
- `ChannelAccessEntry::shm_secret` field.
- `ShmQueue::set_shm_secret` API + `apply_master_approval` SHM-secret
  branch.
- `broker_service.cpp:1956` hardcoded-zero secret mint.
- `data_block.cpp:445-449` plaintext secret stamp in the header.
- `data_block.cpp:2768-2777` memcmp "auth" check.
- The named-shm `shm_open` code path in `data_block.cpp`.
- `L2 test_hub_state.cpp` assertions pinning the old contract
  (lines 3593-3709).
- The `shared_secret` field in `SharedMemoryHeader`.

**Substep 1k (#258, pending):** L4 end-to-end test on the new path.

**Backward compat: NONE planned** (per D7 clean-break decision).
Pre-this-HEP SHM channels are insecure by today's definition.
Pre-1.0 framework; clean breaks are accepted.  Coexistence of two
SHM mechanisms is a permanent maintenance liability the design
explicitly rejects.

---

## 8. What stays in HEP-CORE-0036

HEP-CORE-0036 stays as-is.  Cross-references added at:
- §3.5 (AUTH-gate principle) — note that the principle applies to SHM transport via HEP-CORE-0041; layering is symmetric.
- §5.6 (SHM in current diagrams) — replace with cross-reference to HEP-CORE-0041 §5.
- §6.4 (CONSUMER_REG_ACK shape) — SHM-side fields moved to HEP-CORE-0041 §5.3.
- §I6 (T1 resolution) — note SHM transport's HEP-0041 replaces the role of CURVE keypairs for SHM.

---

## 9. Designer decisions

| # | Question | Decision (locked 2026-06-16) |
|---|---|---|
| **D1** | Which mechanism ships at framework level? | ✅ **Option A** (capability via anonymous mapping + handle transfer).  Encryption (Option C) is a ROLE-level concern; framework exposes uniform crypto primitives (key gen, mlocked key storage, AEAD encrypt/decrypt of memory block, HKDF) so any role that wants per-slot encryption can layer it on top of the capability transport.  Framework never manages encryption keys for channel data. |
| **D2** | Lifecycle strategy for encryption keys? | ⏭️ **Out of scope** — see D1.  Role decides its own key lifecycle using framework primitives.  Recommended patterns documented separately (e.g., per-channel-open key, optional continuous rotation) but the framework does not enforce. |
| **D3** | AES-GCM vs ChaCha20-Poly1305? | ⏭️ **Out of scope** — see D1.  Framework's AEAD primitive defaults to ChaCha20-Poly1305 (libsodium `crypto_aead_chacha20poly1305_*`) since it's constant-time without hardware support; a single helper, roles choose to use it or not. |
| **D4** | How does the producer admit a consumer? | ✅ **Pre-attach broker confirmation on every attempt.  No cached fast-path.  Cached allowlist becomes observability only.** Producer ALWAYS queries broker via BRC before sending the fd; broker checks `authorized_consumer_pubkeys` against current state and replies success/denied.  Cache + broker answer COMPARED — divergence → WARN log, broker's answer always wins.  Drift window is eliminated; revocation of established connections retains HEP-0036 §3 "no force-close" semantic (entry gate is the tight control point).  **ZMQ symmetrizes via task #246** (HEP-CORE-0036 amendment). |
| **D5** | Bearer-token format? | ⏭️ **Moot under D4** — pre-confirm pattern replaces bearer tokens.  Producer queries broker by `consumer_pubkey` directly; broker is the live authority.  No token signing, no expiration logic needed. |
| **D6** | Module home for new code? | ✅ **`utils/security/shm_capability/`** — sits alongside `key_store`, `zap_router`, `peer_admission`, `curve_keypair`, etc.  Communicates the security-mechanism role; physical-layer SHM (DataBlock, slot ops) stays in `utils/shm/` and consumes the capability abstraction. |
| **D7** | Backward compat with existing `shm_secret` configs? | ✅ **Clean break.**  Single unified mechanism, no parallel paths.  `in_shm_secret` / `out_shm_secret` config fields retire (schema validation rejects them as unknown).  `ChannelAccessEntry::shm_secret` field removed; replaced by capability endpoint registration.  Demo configs under `share/` updated.  No deprecation period.  Framework is pre-1.0; clean breaks are accepted; coexistence of two SHM mechanisms is a permanent maintenance liability we explicitly reject. |
| **D8** | Public-API exposure of framework crypto primitives (AEAD / KDF / random / LockedKey)? | ✅ **Yes for native (C++) consumers via `PYLABHUB_UTILS_EXPORT`.**  New AEAD + KDF wrappers added to `crypto_utils.hpp` next to existing `generate_random_*` exports.  Costs nothing extra over internal-use scope; benefits native plugin developers and external tooling that links `pylabhub-utils`.  Script-accessible variant (Python / Lua / Native plugin engines via `api.crypto.*`) deferred to a sibling task — see §13. |

### D4 attach sequence (detail)

> **Amended 2026-06-17 (substep 1c implementation).** Steps 2 and 7's "signed_nonce" mechanism was cryptographically broken — CURVE keypairs are Curve25519 (for ECDH), not Ed25519 (for signing), so "signature over a nonce with a CURVE seckey" is not a standard libsodium operation.  Replaced with a two-frame `crypto_box`-based challenge-response using the same CURVE keys (Curve25519 + xsalsa20poly1305 — `crypto_box_easy` / `crypto_box_open_easy`).  The MAC of `crypto_box_easy(challenge, nonce, producer_pk, consumer_sk)` is keyed by `ECDH(consumer_sk, producer_pk)`; successful `crypto_box_open_easy` under `(consumer_pk_from_hello, producer_sk)` proves the cipher was produced by the holder of `consumer_sk` corresponding to `consumer_pk`.  Same security property as ZMQ's CURVE handshake.  See `src/utils/security/attach_protocol.cpp` + `tests/test_layer2_service/test_attach_protocol.cpp::RejectsConsumerWithWrongSeckey`.

1. Consumer connects to producer's Unix socket / named pipe.
2. Producer sends frame 1 (length-prefixed JSON): `{protocol_version, nonce_b64, challenge_b64}` — 24-byte `crypto_box` nonce + 16 random challenge bytes.
3. Consumer encrypts the challenge under its seckey targeting the producer's pubkey:
   `cipher = crypto_box_easy(challenge, nonce, producer_pk, consumer_sk)`.
   Sends frame 2 (length-prefixed JSON): `{protocol_version, role_uid, pubkey_z85, challenge_response_b64}`.
4. Producer reads `SO_PEERCRED` (POSIX) / `GetNamedPipeClientProcessId` (Windows) as a defence-in-depth sanity check (peer must be in the expected trust domain).  Validates the hello shape, decodes the consumer's claimed pubkey from Z85, decrypts:
   `plaintext = crypto_box_open_easy(cipher, nonce, consumer_pk_from_hello, producer_sk)`.
   MAC verification + `plaintext == challenge` → cryptographic proof complete.
5. **Producer sends `CONSUMER_ATTACH_REQ {channel_name, consumer_pubkey, consumer_role_uid}` to broker over its BRC.**
6. Broker checks `authorized_consumer_pubkeys` for the channel.  Replies `CONSUMER_ATTACH_ACK {status: success | denied}`.
7. Producer compares broker's answer against its local cached allowlist:
   - **Agree (success+in-cache, OR denied+not-in-cache)**: silent.  Normal path.
   - **Diverge (success+not-in-cache OR denied+in-cache)**: log `WARN` — broker comm health signal — broker's answer wins.
8. Success → producer sends fd via `SCM_RIGHTS` (POSIX) or `DuplicateHandle` (Windows).  Denied → producer drops the connection.

**Mutual-auth gap.**  The current flow proves consumer→producer identity but does NOT prove producer→consumer (a same-UID process could bind the published endpoint and impersonate the producer to a connecting consumer).  Task #262 tracks adding producer-side proof-of-possession (likely as a 3rd frame: producer echoes a consumer-supplied challenge encrypted under producer_sk) before declaring Phase 1 production-ready.

### D4 cached-allowlist semantics

The producer's cache is maintained by the same `CHANNEL_AUTH_CHANGED_NOTIFY` doorbell + `GET_CHANNEL_AUTH_REQ`/`_ACK` pull pattern as ZMQ today, but its role flips from **load-bearing** to **observability**:

| Cache vs broker | Meaning | Action |
|---|---|---|
| Both say allowed | Healthy | No log |
| Both say denied | Healthy | No log |
| Broker allowed, cache says no | Producer missed a NOTIFY add | WARN; admit; investigate NOTIFY pipeline if frequent |
| Broker denied, cache says yes | Producer missed a NOTIFY remove | WARN; deny; investigate NOTIFY pipeline if frequent |

Sustained divergence rate = broker-NOTIFY-pipeline health metric.  Operators monitor this one signal.

---

## 10. Implementation phasing (locked at D1 + D4)

1. **Phase 1 — Abstract interface + Linux `memfd_create` backend + broker `CONSUMER_ATTACH_REQ` handler.** Strongest platform first.  Production roles on Linux can use capability transport with pre-confirm.  Tests pin the divergence-WARN behavior.
2. **Phase 2 — macOS backend** (anon shm via `shm_open` + immediate `shm_unlink` trick + `SCM_RIGHTS`; see §13).
3. **Phase 3 — Windows backend** (`CreateFileMapping(NULL) + DuplicateHandle` via named pipe).
4. **Phase 4 — Framework AEAD/KDF/key-storage primitives.** Cross-platform via libsodium.  No SHM-specific work — these are general crypto primitives that roles can use for any purpose (channel data encryption being one such use case).
5. **Phase 5 — HEP-CORE-0036 amendment (task #246):** retrofit ZMQ to the same pre-confirm pattern.  Lands AFTER Phase 1 establishes the pattern.

Each phase is its own task and commit cluster.  Phase 1 is the production-readiness gate.

### 10.1 Phase 1 substep chain (live tracker in `docs/todo/AUTH_TODO.md`)

Phase 1 ships as 11 substeps + cross-platform structural + follow-ups:

| Substep | Task | Status | Brief |
|---|---|---|---|
| 1a | #248 | ✅ | Abstract `IShmCapabilityProducer`/`Consumer` skeleton |
| 1b | #249 | ✅ | Linux `memfd_create` + `SCM_RIGHTS` backend |
| 1c | #250 | ✅ | `AttachProtocol` `crypto_box` challenge-response |
| 1d | #251 | ✅ | Broker `CONSUMER_ATTACH_REQ` / `_ACK` handler |
| 1e | #252 | ✅ | `ShmAttachOrchestrator` + divergence-WARN |
| 1f | #253 | ✅ | DataBlock fd-source ctors + `create_datablock_producer_from_fd_impl` / `find_datablock_consumer_from_fd_impl` factories + `datablock_layout_total_size` public sizing accessor; producer/consumer `IShmCapability*::borrow_fd()`; L2 test pin (in-process round-trip + ShmCapability end-to-end + under-sized fd throw); post-ship review fixed an fd-leak (try/catch around `init_producer_state_` in fd-source Create ctor), the `dup → 0` edge case (`F_DUPFD_CLOEXEC` minimum-fd-1), and a stale L3 log-pin |
| 1g | #254 | ✅ | Wire-shape additive (no removal — `shm_secret` was never on the wire): `default_shm_capability_endpoint(channel)` helper (Linux XDG_RUNTIME_DIR → `/tmp` fallback); `ProducerEntry::shm_capability_endpoint` field + `set_producer_shm_capability_endpoint` setter; `ProducerRegInputs::shm_capability_endpoint` field; `build_producer_reg_payload` emits `data_transport="shm"` + `shm_capability_endpoint` for SHM channels; producer + processor role hosts populate the field; broker REG_REQ handler extracts and stores it; broker `CONSUMER_REG_ACK` builder echoes `shm_capability_endpoint` + `producer_pubkey_z85` (sourced from `ProducerEntry::zmq_pubkey`) for SHM channels; consumer-side `apply_consumer_reg_ack` logs `ShmCapabilityFieldsReceived` (plumb-only — actual dial happens in 1i when the legacy named-shm path retires); `hub_state_json.cpp` includes the new field in the admin dump; cross-factory mixing-hazard docstring on the fd-source factories |
| 1h | #255 | ⏸ | Config schema rejection of `in_shm_secret`/`out_shm_secret` |
| 1i | #256 | ⏸ | Code cleanup — delete obsolete `shm_secret` machinery + named-shm path |
| 1j | #257 | ⏸ | L3 broker tests (success / denied / divergence-WARN) |
| 1k | #258 | ⏸ | L4 end-to-end SHM auth-gated data flow |
| Cross-platform structural | #259/#260/#261 | ✅ | Per-platform `AcceptedPeer` variant + `#error` blocks for FreeBSD/macOS/Windows backends |
| Mutual-auth follow-up | #262 | ⏸ | Producer→consumer proof-of-possession (closes one-way auth gap from 1c) |

Substeps are reviewed and committed individually; ✅ items are
production-ready as of their commit (sub-tree green; full L2/L3
sweeps green where applicable).

---

## 11. Next steps

All §9 decisions locked.  Original promotion + early-task items
shipped 2026-06-16 / 2026-06-17; the remaining live work is in §10.1's
substep chain.  Phase-level remaining work:

1. Close out Phase 1 by completing substeps 1f → 1k (status table in §10.1).
2. Close out #262 (mutual auth) before declaring Phase 1 production-ready.
3. Phase 2/3 implementation under #260 / #261 (per-platform L1 + L2 ports).
4. Phase 4 — public `PYLABHUB_UTILS_EXPORT` AEAD + KDF wrappers (D8) under #247.
5. Per task #246, retrofit ZMQ to pre-confirm pattern after Phase 1 lands.

---

## 12. Related tasks + cross-references

| Task | Purpose | Status relative to this HEP |
|---|---|---|
| **#244** | Tracker for this HEP | This HEP IS #244's deliverable; promoted 2026-06-16 |
| **#164 AUTH-4** | Original broker-mint-shm_secret design | ✅ Closed as **SUPERSEDED** by D1+D7 — `shm_secret` retires entirely |
| **#79** | `plh_role --init` non-zero `shm_secret` seed | ✅ Closed as **SUPERSEDED** with #164 |
| **#245** | Interim POSIX `kShmModeRw 0666 → 0600` tightening | ✅ Closed as **KILLED** — named-shm path deleted under 1i; interim hardening would have been throwaway work |
| **#246** | HEP-CORE-0036 ZMQ retrofit to pre-confirm pattern | ⏸ Downstream — lands after Phase 1 establishes the pattern |
| **#248-#258** | Phase 1 substeps 1a-1k | See §10.1 status table |
| **#259-#261** | Cross-platform backends (FreeBSD/macOS/Windows) | ⏸ Structural shipped (#error placeholders); implementations pending |
| **#262** | Mutual auth (producer→consumer proof-of-possession) | ⏸ Phase 1 production-readiness gate |
| **#106 (HEP-CORE-0038)** | Script-accessible vault keystore | ↔ Sibling — script audience for secret-at-rest side of crypto |
| **#247** | Script-accessible crypto primitives | ↔ Sibling to #106; consumer of D8 |

## 13. Appendix: per-platform primitive references

For implementers / reviewers.  Sources to cite when this becomes a real HEP.

### Linux

- `memfd_create(2)` — Linux 3.17+.  Anonymous file in tmpfs, no name in `/dev/shm`, no listing.
- `SCM_RIGHTS` — `man 7 unix` — passes file descriptors over Unix sockets.

### FreeBSD

- `memfd_create(2)` — FreeBSD 13+.
- Earlier: `shm_open(SHM_ANON, ...)` — FreeBSD-specific anon SHM.

### macOS / Darwin

> **Corrected 2026-06-18.**  Earlier text claimed `shm_open(SHM_ANON, ...)` works on
> macOS; that is **wrong** — `SHM_ANON` is a FreeBSD-specific extension.
> macOS does NOT have it.  The macOS backend uses the
> `shm_open`+immediate-`shm_unlink` trick instead (create a named SHM,
> unlink the name immediately while keeping the fd, mmap the now-anonymous
> fd).  See task #260 plan block in `shm_capability_channel.cpp`.

- `shm_open(name, O_CREAT|O_EXCL|O_RDWR, 0600)` + `shm_unlink(name)` immediately
  — emulates an anonymous SHM via the unlink-once-open trick.  Standard portable
  pattern; macOS has no native anonymous-SHM primitive.
- `SCM_RIGHTS` over Unix socket — supported (BSD-derived).
- No `accept4()` — use `accept()` + `fcntl(F_SETFD, FD_CLOEXEC)`.
- `getpeereid(fd, *uid, *gid)` — replaces Linux `SO_PEERCRED`; carries no PID.

### Windows

- `CreateFileMapping(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, hi, lo, NULL)` — `NULL` name = unnamed mapping object.  Handle is local to the creating process.
- `DuplicateHandle(GetCurrentProcess(), src, target_process_handle, &dup, ..., FALSE, DUPLICATE_SAME_ACCESS)` — replicates handle into the target process's handle table.  Requires `OpenProcess(PROCESS_DUP_HANDLE, ...)` on the target — which requires same-user-session OR explicit security context.
- Named pipes for the "Unix socket equivalent" rendezvous between producer and consumer for the duplicate handshake.

---

*End of draft.  Comments welcome.  Promote to permanent HEP after §9 decisions land.*
