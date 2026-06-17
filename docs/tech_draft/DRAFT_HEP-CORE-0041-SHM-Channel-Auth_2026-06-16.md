# DRAFT — HEP-CORE-0041: SHM Channel Auth (Cross-Platform)

| Property | Value |
|---|---|
| **HEP (proposed)** | `HEP-CORE-0041` |
| **Title** | SHM Channel Auth — Capability Transport + Optional Encrypt-at-Rest |
| **Status** | 🟡 **DRAFT — OPEN FOR DESIGNER DECISIONS** |
| **Created** | 2026-06-16 |
| **Tracker** | task **#244** |
| **Sibling docs** | HEP-CORE-0036 (ZMQ Auth — keep, this draft mirrors its shape), HEP-CORE-0002 (DataBlock — gets a transport-policy hook) |
| **Filed by** | discussion 2026-06-16 after the AUTH-4 (#164) gap analysis surfaced the structural weakness of "secret as discriminator" + POSIX `0666` default |
| **Cross-platform constraint** | MUST work on Linux + FreeBSD + macOS + Windows |
| **Blocks** | task **#164** (AUTH-4) and **#79** (SHM seed in `--init`) — both deferred until this lands |
| **Does NOT block** | task **#245** (POSIX 0666 → 0600 interim hardening) — can ship independently |

> **Read first.**  This is a working draft, not the design of record.
> The structure mirrors HEP-CORE-0036 deliberately so a reader can
> compare the SHM model side-by-side with the ZMQ model.  Sections
> marked "**[DECISION:]**" are explicit asks of the designer — they
> need answers before this graduates from tech_draft to HEP.

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

Producer is responsible for binding a Unix socket / named pipe at `shm_capability_endpoint`.  Broker validates the endpoint string and stores it in `ChannelAccessEntry`.

### 5.2 REG_ACK — what broker tells producer

No new fields.  REG_ACK confirms admission; producer's local SHM region is created lazily on first consumer demand (broker forwards consumer's identity and Unix socket connect address — see §5.4).

### 5.3 CONSUMER_REG_ACK — what consumer receives

```json
{
  ...,
  "data_transport": "shm",
  "shm_capability_endpoint": "unix:///var/run/pylabhub/role-acquisition.daq01.prod/shm-cap.sock",
  "consumer_authorization_token": "<random_64_byte_blob_signed_by_broker>"
}
```

Consumer connects to `shm_capability_endpoint` over Unix socket / named pipe.  Sends `consumer_authorization_token`.  Producer validates the token's signature against the broker's pubkey, sends the SHM fd via `SCM_RIGHTS` (POSIX) or duplicated handle (Windows).  Consumer maps and proceeds.

### 5.4 Open wire-protocol questions

**[DECISION:]** Should the broker proxy the fd-pass (broker-in-the-middle) OR authorize a direct producer-to-consumer Unix socket?

- **Proxy**: simpler authorization model (only broker has Unix sockets to both sides).  Higher latency at register time.  Broker holds fd briefly.
- **Direct**: producer and consumer connect to each other over Unix socket.  Lower latency.  More complex authorization (broker must vouch — hence the signed `consumer_authorization_token` above).

My read: direct.  Broker's job is to issue the bearer token, not relay fds.

**[DECISION:]** What format for `consumer_authorization_token`?

Strawman: 64-byte random nonce + Ed25519 signature by broker over `(channel_name, consumer_pubkey, expiration_time)`.  Producer validates signature using broker's pubkey (already known via the BRC handshake).  Expiration prevents replay; 5-minute TTL.

---

## 6. Cross-platform abstraction

Code structure (proposed):

```
src/include/utils/security/
    shm_capability_channel.hpp      // abstract interface

src/utils/security/shm_capability/
    posix_memfd_backend.cpp          // Linux + FreeBSD>=13
    posix_shm_anon_backend.cpp       // macOS + FreeBSD<13
    windows_filemap_backend.cpp      // Windows

include/utils/security/shm_capability/
    *_backend.hpp                    // per-platform headers
```

Abstract interface (rough):

```cpp
class ShmCapabilityProducer {
public:
    static std::unique_ptr<ShmCapabilityProducer> create(size_t bytes);
    // Bind a Unix socket / named pipe; ready to receive consumer connections.
    bool bind_capability_endpoint(const std::string& endpoint);
    // Accept a consumer, validate token, hand off handle.
    bool serve_one(const SignedAuthToken& expected_token,
                   std::chrono::milliseconds timeout);
    // Direct mmap access for the producer's own slot writes.
    std::span<std::byte> data();
};

class ShmCapabilityConsumer {
public:
    static std::unique_ptr<ShmCapabilityConsumer> attach(
        const std::string& capability_endpoint,
        const SignedAuthToken& token);
    std::span<const std::byte> data() const;
};
```

Backends implement the abstract interface using the per-platform primitives in §3.1's table.

**[DECISION:]** Where in the existing module structure does this go?  Strawman: under `utils/security/` next to `key_store` and `zap_router`.

---

## 7. Compatibility / migration

- Current code paths through `ShmQueue::apply_master_approval` reading `shm_secret` from REG_ACK and calling `set_shm_secret` retire.
- `ChannelAccessEntry::shm_secret` field renames to `shm_capability_endpoint` (and, if Option D, `shm_data_key`).
- `producer_init.cpp` `out_shm_secret` field deprecates.
- Existing tests using config-supplied `shm_secret` need updating.
- Backward compat: NONE planned.  Pre-this-HEP SHM channels are insecure by today's definition; we don't want to keep a "fallback" path.

This is acceptable for a pre-1.0 framework.  An L4 test sweep + L3 broker test rewrites are needed.

---

## 8. What stays in HEP-CORE-0036

HEP-CORE-0036 stays as-is.  Cross-references added at:
- §3.5 (AUTH-gate principle) — note that the principle applies to SHM transport via HEP-CORE-0041; layering is symmetric.
- §5.6 (SHM in current diagrams) — replace with cross-reference to HEP-CORE-0041 §5.
- §6.4 (CONSUMER_REG_ACK shape) — SHM-side fields moved to HEP-CORE-0041 §5.3.
- §I6 (T1 resolution) — note SHM transport's HEP-0041 replaces the role of CURVE keypairs for SHM.

---

## 9. Open designer decisions (consolidated)

For convenience — every `**[DECISION:]**` from the body:

| # | Question | My recommendation |
|---|---|---|
| D1 | Which mechanism ships as default? | Option D (A for transport, C opt-in for encryption) |
| D2 | Lifecycle strategy for encryption keys? | MVP = Strategy 2 (per-channel-open); Phase 2 = Strategy 4 (continuous rotation) |
| D3 | AES-GCM vs ChaCha20-Poly1305? | ChaCha20-Poly1305 |
| D4 | Broker proxy fd-pass OR direct producer↔consumer? | Direct, broker issues bearer token |
| D5 | `consumer_authorization_token` format? | Random nonce + Ed25519 signature by broker, 5-min TTL |
| D6 | Module home for new code? | `utils/security/shm_capability/` |
| D7 | Backward compat with existing `shm_secret` configs? | None — clean break, framework is pre-1.0 |

---

## 10. Implementation phasing (conditional on §9 outcomes)

Strawman, depends on §9 D1 + D2 outcomes:

1. **Phase 1 — Abstract interface + Linux/FreeBSD memfd backend.** Strongest platform first.  Production roles on Linux can start using capability transport.  Tests use this.
2. **Phase 2 — macOS backend** (`shm_open + SHM_ANON`).
3. **Phase 3 — Windows backend** (`CreateFileMapping(NULL) + DuplicateHandle via named pipe`).
4. **Phase 4 — Encryption layer** (Option C) if D1=D.  ChaCha20-Poly1305 over the capability transport.
5. **Phase 5 — Continuous rotation** (Strategy 4) if D2 escalates beyond MVP.

Each phase is its own task and its own commit cluster.  Phase 1 is the only one that's a true blocker for "production-ready SHM channels on Linux."

---

## 11. Next steps

1. Designer reads this draft, answers §9.
2. Promote to permanent `docs/HEP/HEP-CORE-0041-...md`.
3. File Phase-1 implementation task + sibling HEP updates per §8.
4. Revive #164 / #79 either as supersede-close (if D1 retires `shm_secret`) or as scope-update (if D1 keeps it as discriminator).

---

## 12. Appendix: per-platform primitive references

For implementers / reviewers.  Sources to cite when this becomes a real HEP.

### Linux

- `memfd_create(2)` — Linux 3.17+.  Anonymous file in tmpfs, no name in `/dev/shm`, no listing.
- `SCM_RIGHTS` — `man 7 unix` — passes file descriptors over Unix sockets.

### FreeBSD

- `memfd_create(2)` — FreeBSD 13+.
- Earlier: `shm_open(SHM_ANON, ...)` — FreeBSD-specific anon SHM.

### macOS / Darwin

- `shm_open(SHM_ANON, ...)` — supported (yes, despite folklore).  Unlink-on-create yields anon fd.
- `SCM_RIGHTS` over Unix socket — supported.

### Windows

- `CreateFileMapping(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, hi, lo, NULL)` — `NULL` name = unnamed mapping object.  Handle is local to the creating process.
- `DuplicateHandle(GetCurrentProcess(), src, target_process_handle, &dup, ..., FALSE, DUPLICATE_SAME_ACCESS)` — replicates handle into the target process's handle table.  Requires `OpenProcess(PROCESS_DUP_HANDLE, ...)` on the target — which requires same-user-session OR explicit security context.
- Named pipes for the "Unix socket equivalent" rendezvous between producer and consumer for the duplicate handshake.

---

*End of draft.  Comments welcome.  Promote to permanent HEP after §9 decisions land.*
