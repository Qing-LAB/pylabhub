# Security, Identity, and Provenance TODO

**Purpose:** Track the design and implementation of hub/actor identity, secrets management,
connection policy, collision detection, and data provenance for pylabhub.

**Master TODO:** `docs/TODO_MASTER.md`
**Related:** `docs/todo/API_TODO.md` § "Header / Include Layering Refactor"

---

## Overview

This work area covers four tightly coupled concerns:

1. **Hub vault** — encrypted secrets store; stable CurveZMQ keypair; never plaintext on disk
2. **Directory model** — hub, producer, and consumer each have a persistent identity directory
3. **Connection policy** — who is allowed to connect to a hub channel (open → verified)
4. **Provenance chain** — full data lineage from hub through producer to consumer,
   accessible at both the C-API level (SHM header) and the C++ abstraction level

All four require changes across every layer of the stack: C-API (`SharedMemoryHeader`,
`slot_rw_coordinator.h`), C++ abstractions (`HubConfig`, `ProducerOptions`,
`ConsumerOptions`, `ChannelRegistry`), executable logic (`hubshell.cpp`, `actor_main.cpp`),
and on-disk layout (`hub.json`, `hub.vault`, `actor.json`).

---

## Design: Hub Directory Model

Each hub, producer, and consumer has its own **persistent directory**. The directory *is*
the identity — copying it clones the identity; `--init` on a new directory creates a fresh one.

### Hub directory layout

```
<hub_dir>/
  hub.json          ← public config: hub_name, hub_uid, broker_endpoint,
  |                     connection_policy, known_actors, channel_policies
  hub.vault         ← encrypted secrets: broker CurveZMQ keypair, admin token
  hub.pubkey        ← broker public key written at startup (safe to distribute)
  startup.py        ← optional Python startup script
  logs/
  run/
    hub.pid
    admin.sock
```

### Actor directory layout

```
<actor_dir>/
  actor.json        ← actor_name, actor_uid, role, channel, hub_dir,
  |                     slot_schema, flexzone_schema, validation policy
  script.py         ← Python callbacks (name referenced from actor.json)
  logs/
  run/
    actor.pid
```

`hub_dir` in `actor.json` is the only reference an actor needs:
- `<hub_dir>/hub.json` → `broker_endpoint`
- `<hub_dir>/hub.pubkey` → broker CurveZMQ public key for connection

### Config naming changes from current design

| Current | New |
|---|---|
| `config/hub.default.json` | Compiled-in C++ defaults (no file) |
| `config/hub.user.json` | `<hub_dir>/hub.json` |
| actor config (`<any_name>.json`) | `<actor_dir>/actor.json` |
| `config/broker.pubkey` (not yet implemented) | `<hub_dir>/hub.pubkey` (written at startup) |

---

## Design: Hub Vault (Encrypted Secrets)

All sensitive material lives exclusively in `hub.vault`. Nothing secret is written
to `hub.json` or `hub.pubkey`.

### Key derivation

We already have **libsodium** as a dependency. Use its Argon2id KDF — not raw SHA256,
which is too fast to be safe for password-derived keys (brute-forceable at GB/s).

```
key[32] = crypto_pwhash(
    password,
    salt = hub_uid,            ← hub_uid serves as the salt
    OPSLIMIT_INTERACTIVE,      ← tunable time cost (~0.5s on reference hardware)
    MEMLIMIT_INTERACTIVE       ← ~64 MB memory cost; GPU-resistant
)
```

### Encryption / authentication

```
nonce[24]  = randombytes_buf()          ← random per write
ciphertext = crypto_secretbox_easy(plaintext_json, nonce, key)
```

`crypto_secretbox_easy` uses XSalsa20-Poly1305. The Poly1305 MAC is the integrity check —
no separate checksum needed. A wrong password or corrupted file produces an authentication
failure, not garbage.

### Vault file format

```
hub.vault = [nonce(24 bytes)][ciphertext + MAC(16 bytes)]
```

Binary file. The decrypted payload is JSON:

```json
{
  "broker": {
    "curve_secret_key": "<Z85 40-char>",
    "curve_public_key":  "<Z85 40-char>"
  },
  "admin": {
    "token": "<random hex>"
  }
}
```

### Stable keypair benefit

Current design regenerates the broker CurveZMQ keypair on every restart — actors
cannot cache the public key. With the vault, the keypair is **stable across restarts**.
The public key is written to `hub.pubkey` at startup. Actors read it once from their
`hub_dir` and never need to refresh it.

### Password input policy

| Mode | Mechanism |
|---|---|
| Default (interactive) | `getpass()`-style terminal prompt |
| Service / CI | `PYLABHUB_MASTER_PASSWORD` environment variable |
| Key file | `--password-file <path>` (file must have mode 0600) |
| Dev mode | `--dev` flag: ephemeral keypair, no vault, no password |

Dev mode is required for local testing and CI without vault overhead.

---

## Design: Actor Identity

Each actor has a **persistent UUID4** generated at `--init` time, stored in `actor.json`.
The UUID is re-used across restarts of the same actor. A new `--init` on an existing
directory prompts: *re-use identity or generate fresh?*

```json
{
  "actor_name": "lab.physics.daq.sensor1",
  "actor_uid":  "e7a9f3b2-...",
  "role":       "producer",
  "channel":    "lab.daq.sensor1.raw",
  "hub_dir":    "/opt/pylabhub/hubs/daq1",
  ...
}
```

`actor_name` follows the same reverse-domain convention as `hub_name` and channel names.

---

## Design: Connection Policy

Configured in `hub.json`. Applies hub-wide with optional per-channel overrides.

### Four policy levels

| Level | Broker behaviour on REG_REQ / CONSUMER_REG_REQ |
|---|---|
| `open` | No name or UID required. Any client connects. (default for dev) |
| `tracked` | Name + UID accepted and recorded if provided; not required. Full provenance without enforcement. |
| `required` | `actor_name` + `actor_uid` must be present in the request. Rejected if absent. Not cross-checked against a list. |
| `verified` | `actor_name` + `actor_uid` must match a pre-registered entry in `hub.json::known_actors`. Unknown actors rejected. |

### Per-channel policy override

```json
{
  "connection_policy": "tracked",
  "channel_policies": {
    "lab.daq.raw.*":     { "connection_policy": "verified" },
    "lab.monitor.*":     { "connection_policy": "open" }
  },
  "known_actors": [
    { "name": "lab.daq.sensor1", "uid": "e7a9...", "role": "producer" },
    { "name": "lab.analysis.logger", "uid": "c2f1...", "role": "consumer" }
  ]
}
```

Glob patterns on channel names. Hub-wide policy is the floor; per-channel may be
stricter or more open.

`known_actors` is **not** sensitive — it is names and UIDs only, equivalent to a
network allowlist. It stays in `hub.json`, not `hub.vault`.

### Deferred: `signed` policy level

A fifth level where each actor holds a private key and signs its registration request.
The hub verifies the signature against a stored public key. Requires PKI design.
Upgrade path is clear: add `signed` level, add `actor_pubkey` to `known_actors` entries.
Not needed until the threat model requires adversarial-resistant authentication.

---

## Design: UID Collision Detection

UID scope is **per-hub**. Globally unique identity is always `(hub_uid, actor_uid)`.

| Scenario | Problem? |
|---|---|
| Same actor_uid on Hub A and Hub B | ✅ Fine — different hub_uid → different global identity |
| Same actor_uid, same hub, both processes alive | ❌ Collision — reject or warn per policy |
| Same actor_uid, same hub, previous process dead | ✅ Stale — existing liveness check cleans it up |

### Collision resolution per policy

```
Incoming REG_REQ with actor_uid = X for channel C:

  Is there a live entry with actor_uid = X on channel C?
  └─ No  → accept
  └─ Yes → is_process_alive(existing_pid)?
       └─ Dead  → stale; replace (existing liveness mechanism)
       └─ Alive → COLLISION:
            open/tracked  → warn, allow (log duplicate identity)
            required      → configurable: warn or reject
            verified      → reject (known_actors has exactly one entry per uid)
```

`verified` prevents collisions as a structural side effect.

---

## Design: Provenance Chain

Full data lineage accessible at every layer:

```
hub_uid        "b3f2a1c7-..."      which hub instance (persistent)
hub_name       "lab.physics.daq1"  human-readable location
  └─ channel   "lab.daq.sensor1.raw"
     schema_hash                   what data type (existing)
       └─ producer_uid  "e7a9..."  which producer config (new)
          producer_name            human-readable producer identity
          producer_pid             which OS process (already tracked)
            └─ slot_seq            which specific write cycle
               consumer_uid "c2f1..." which consumer config (new)
               consumer_name
               consumer_pid
```

For scientific data this answers: what the data was, which system produced it, which
system consumed it, and from which hub — without any external logging service.

---

## Required C-API and Abstraction Changes

This section identifies every layer that needs modification, ordered from lowest to highest.

### 1. UUID utility (new — no dependencies)

New file: `src/include/utils/hub_identity.hpp` + `src/utils/hub_identity.cpp`

```cpp
namespace pylabhub::utils
{
    /// Generate a UUID4 string ("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx").
    /// Uses libsodium randombytes_buf() as the entropy source.
    std::string generate_uuid4();

    /// True if s is a valid UUID4 string (36 chars, correct format).
    bool is_valid_uuid4(const std::string& s);
}
```

No dependency on JSON or ZMQ. Used by `--init` flows for both hub and actors.

### 2. HubVault (new)

New files: `src/include/utils/hub_vault.hpp` + `src/utils/hub_vault.cpp`

```cpp
namespace pylabhub::utils
{
class HubVault
{
public:
    /// Create a new vault file. Generates broker CurveZMQ keypair + admin token.
    /// Derives key from password + hub_uid (Argon2id), encrypts with secretbox.
    static HubVault create(const fs::path& hub_dir,
                           const std::string& hub_uid,
                           const std::string& password);

    /// Open an existing vault. Fails (throws) if MAC check fails.
    static HubVault open(const fs::path& hub_dir,
                         const std::string& hub_uid,
                         const std::string& password);

    const std::string& broker_curve_secret_key() const noexcept;
    const std::string& broker_curve_public_key()  const noexcept;
    const std::string& admin_token()              const noexcept;

    /// Write broker public key to <hub_dir>/hub.pubkey (called after open()).
    void publish_public_key(const fs::path& hub_dir) const;

private:
    struct Secrets {
        std::string broker_secret_key;
        std::string broker_public_key;
        std::string admin_token;
    } secrets_;
};
}
```

Pimpl not strictly needed (no shared-library ABI concern for internal tooling), but
follow project convention if exposed in `pylabhub-utils`.

### 3. SharedMemoryHeader — full identity block (C-API level)

**This is a core structure change requiring the mandatory review checklist in
`docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol".**

The goal: the SHM segment is **self-describing** — any C consumer, diagnostic tool,
or language FFI can open a DataBlock and read the complete identity chain (who created
it, who writes it, who reads it) without querying the broker, reading JSON configs, or
using the C++ layer.

Three sets of identities, each with different write semantics:

**Channel identity — written once at creation, never changed:**

```cpp
// Carved from reserved_header (208 bytes):
char hub_uid[40];        ///< UUID4 of the hub that created this channel
char hub_name[64];       ///< Human name: "lab.physics.daq1" (truncated, null-term)
char producer_uid[40];   ///< UUID4 of the producer actor
char producer_name[64];  ///< Human name: "lab.daq.sensor1" (truncated, null-term)
```

UIDs and names together so the SHM is readable without any external name lookup.
Written by `create_datablock_producer_impl()` from `DataBlockConfig` fields.
`attach_datablock_as_writer_impl()` does NOT overwrite (identity belongs to the creator).

**Consumer identity — dynamic: written at attach, cleared at detach:**

`ConsumerHeartbeat` expands from 64 bytes (1 cache line) to 128 bytes (2 cache lines)
to carry UID + name alongside the existing liveness fields:

```cpp
struct ConsumerHeartbeat {
    std::atomic<uint64_t> consumer_pid;        //  8 — OS PID (liveness, unchanged)
    std::atomic<uint64_t> last_heartbeat_ns;   //  8 — monotonic (unchanged)
    char                  consumer_uid[40];    // 40 — actor UUID4 (new)
    char                  consumer_name[32];   // 32 — human name, truncated (new)
    uint8_t               padding[40];         // 40 → total 128 bytes (2 cache lines)
} consumer_heartbeats[MAX_CONSUMER_HEARTBEATS];
// 8 slots × 128 bytes = 1024 bytes (was 8 × 64 = 512 bytes)
```

Slot lifecycle (managed by `DataBlockConsumer`):
- Attach: CAS `consumer_pid` 0 → own PID (claims slot); write `consumer_uid` + `consumer_name`
- Heartbeat: update `last_heartbeat_ns` as before
- Detach: zero `consumer_uid` + `consumer_name`; store `consumer_pid = 0` (releases slot)

**`reserved_header` budget after all identity changes:**

| Allocation | Bytes |
|---|---|
| Current `reserved_header` | 2320 |
| − Channel identity (hub + producer uid + name) | −208 |
| − Consumer heartbeat expansion (8 × 64 extra) | −512 |
| Remaining `reserved_header` | **1600** |

4KB static_assert continues to hold. BLDS schema hash must be recomputed (layout changed).

### 4. New C-API types and functions

Two new C structs and two new functions added to `recovery_api.hpp` (extern "C"):

```c
/* Hub + producer identity written at channel creation */
typedef struct {
    char hub_uid[40];
    char hub_name[64];
    char producer_uid[40];
    char producer_name[64];
} plh_channel_identity_t;

/* Per-slot consumer identity (one entry per attached consumer) */
typedef struct {
    char     consumer_uid[40];
    char     consumer_name[32];
    uint64_t consumer_pid;
    uint64_t last_heartbeat_ns;
    int      slot_index;
} plh_consumer_identity_t;

/// Read hub + producer identity written at channel creation.
/// Returns 0 on success, non-zero on error (shm not found, wrong magic, etc.)
PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
int datablock_get_channel_identity(const char             *shm_name,
                                   plh_channel_identity_t *out);

/// Enumerate all currently attached consumers (non-zero consumer_pid slots).
/// entries: caller-allocated array of max_entries elements.
/// count_out: number of valid entries written (≤ max_entries).
/// Returns 0 on success, non-zero on error.
PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
int datablock_list_consumers(const char              *shm_name,
                              plh_consumer_identity_t *entries,
                              int                      max_entries,
                              int                     *count_out);
```

Example usage from C:

```c
plh_channel_identity_t chan;
datablock_get_channel_identity("/pylabhub_lab.daq.sensor1.raw", &chan);
// → hub_name: "lab.physics.daq1"  hub_uid: "b3f2a1c7-..."
//   producer_name: "lab.daq.sensor1"  producer_uid: "e7a9..."

plh_consumer_identity_t consumers[8];
int count = 0;
datablock_list_consumers("/pylabhub_lab.daq.sensor1.raw", consumers, 8, &count);
// → consumers[0]: name="lab.analysis.logger"  uid="c2f1..."  pid=4521
```

A diagnostic tool, third-party C binding, or language FFI reconstructs the full
provenance chain from just the SHM name — zero external dependencies.

### 5. ConsumerHeartbeat::consumer_id rename (part of item 3)

`consumer_id` is currently documented as "PID or UUID" — ambiguous. With the new
layout this is replaced by separate `consumer_pid` (atomic uint64, liveness) and
`consumer_uid[40]` (identity). The rename alone is a core structure change since
it affects field offsets if the existing `consumer_id` is not the last field.

### 6. DataBlockConfig / DataBlockProducer — identity propagation

`DataBlockConfig` gets two new optional fields:

```cpp
struct DataBlockConfig {
    // ... existing ...
    char hub_uid[40]{};      ///< Written into SharedMemoryHeader on create; "" = not set
    char producer_uid[40]{}; ///< Written into SharedMemoryHeader on create; "" = not set
};
```

`create_datablock_producer_impl()` copies these into the header after creation.
`attach_datablock_as_writer_impl()` does NOT overwrite them (write-attach mode must
not change identity of an existing DataBlock).

Typed accessors on `DataBlockProducer` and `DataBlockConsumer`:

```cpp
std::string hub_uid() const;      ///< Read from SharedMemoryHeader
std::string producer_uid() const; ///< Read from SharedMemoryHeader
```

### 7. ProducerOptions / ConsumerOptions — identity fields

```cpp
struct ProducerOptions {
    // ... existing ...
    std::string actor_name;   ///< Actor human name (sent in REG_REQ)
    std::string actor_uid;    ///< Actor UUID4 (sent in REG_REQ; written to SHM header)
};

struct ConsumerOptions {
    // ... existing ...
    std::string actor_name;   ///< Sent in CONSUMER_REG_REQ + HELLO body
    std::string actor_uid;    ///< Sent in CONSUMER_REG_REQ + HELLO body
};
```

`hub::Producer::create()` propagates `actor_uid` into `DataBlockConfig::producer_uid`
before calling the DataBlock factory. It also sends `actor_name` + `actor_uid` in
`REG_REQ` JSON body.

### 8. ProducerInfo / ConsumerInfo — identity in broker protocol

```cpp
struct ProducerInfo {
    // ... existing ...
    std::string actor_name;
    std::string actor_uid;
};

struct ConsumerInfo {
    // ... existing ...
    std::string actor_name;
    std::string actor_uid;
};
```

These flow through `REG_REQ` → `ChannelEntry` → `DISC_ACK`. A connecting consumer
learns the producer's `actor_uid` from the `DISC_ACK` before touching SHM. This
enables policy enforcement before SHM attachment (fail fast on mismatch).

### 9. ChannelRegistry — identity in channel and consumer entries

```cpp
// In ChannelRegistry (src/utils/channel_registry.hpp — internal)
struct ChannelEntry {
    // ... existing ...
    std::string producer_actor_name;
    std::string producer_actor_uid;
    std::string producer_hostname;
};

struct ConsumerEntry {
    // ... existing ...
    std::string actor_name;
    std::string actor_uid;
    std::string hostname;
    std::chrono::system_clock::time_point connected_at;
};
```

### 10. BrokerService — connection policy enforcement

New fields in `BrokerService::Config`:

```cpp
struct Config {
    // ... existing ...
    enum class ConnectionPolicy { Open, Tracked, Required, Verified };
    ConnectionPolicy connection_policy{ConnectionPolicy::Open};

    struct KnownActor {
        std::string name;
        std::string uid;
        std::string role; // "producer" or "consumer" or "any"
    };
    std::vector<KnownActor> known_actors;

    struct ChannelPolicy {
        std::string channel_glob;
        ConnectionPolicy policy;
    };
    std::vector<ChannelPolicy> channel_policies; // per-channel overrides
};
```

Policy enforcement in `handle_reg_req()` and `handle_consumer_reg_req()`:

```
1. Determine effective policy (channel override > hub default)
2. If Required/Verified: reject if actor_name or actor_uid absent in request
3. If Verified: reject if (actor_name, actor_uid) not in known_actors
4. Collision check (see above)
5. If Tracked/Required/Verified and actor_uid present: record in ChannelEntry/ConsumerEntry
```

### 11. HubConfig — directory model + new getters

`HubConfig` changes from reading `config/hub.default.json` + `config/hub.user.json`
to reading a single `hub.json` from the hub directory, with compiled-in defaults.

New fields and getters:

```cpp
class HubConfig {
public:
    // --- New ---
    const std::string& hub_uid()            const noexcept; ///< UUID4 from hub.json
    const std::string& hub_pubkey_path()    const noexcept; ///< <hub_dir>/hub.pubkey
    const fs::path&    hub_dir()            const noexcept; ///< The hub directory
    BrokerService::Config::ConnectionPolicy connection_policy() const noexcept;
    std::vector<BrokerService::Config::KnownActor> known_actors() const;
    std::vector<BrokerService::Config::ChannelPolicy> channel_policies() const;

    // --- Modified: now also exposes broker pubkey path ---
    // broker_endpoint() already exists

    // --- Internal: called from hubshell --init ---
    static void init_hub_directory(const fs::path& hub_dir,
                                   const std::string& hub_name,
                                   const std::string& password);
    void load_(const fs::path& hub_dir); // public but internal (lifecycle only)
};
```

### 12. ActorConfig — directory model

`ActorConfig::from_json_file(path)` is replaced by `ActorConfig::from_directory(dir)`.
The directory is the actor identity. Config file is always `<actor_dir>/actor.json`.
Script path is resolved relative to `<actor_dir>/`.

New fields:

```cpp
struct ActorConfig {
    // ... existing ...
    std::string actor_name;  ///< Human name from actor.json
    std::string actor_uid;   ///< UUID4 from actor.json (empty if not init'd)
    fs::path    hub_dir;     ///< Path to hub directory (resolves endpoint + pubkey)
    fs::path    actor_dir;   ///< The actor's own directory
};
```

`actor_main.cpp` reads `<actor_dir>/actor.json`, then reads
`<hub_dir>/hub.json` for `broker_endpoint` and `<hub_dir>/hub.pubkey` for the key,
and finally calls `Messenger::connect(endpoint, pubkey)` before creating the host.
This is the missing connection step currently blocked by the key distribution problem.

---

## Required CLI Changes

### pylabhub-hubshell

```bash
# First-time setup
pylabhub-hubshell --init <hub_dir>
  → prompt: hub_name (e.g. "lab.physics.daq1")
  → prompt: master password (twice, confirm)
  → generate hub_uid (UUID4)
  → write hub.json with hub_uid, hub_name, defaults
  → generate broker CurveZMQ keypair + admin token
  → encrypt into hub.vault (Argon2id + secretbox)

# Run (subsequent)
pylabhub-hubshell <hub_dir>
  → read hub_uid from hub.json
  → prompt: master password
  → derive key (Argon2id) → decrypt hub.vault → validate MAC
  → write hub.pubkey (safe to share)
  → start broker with stable keypair from vault

pylabhub-hubshell <hub_dir> --dev
  → ephemeral keypair, no vault, no password (local testing)

# If <hub_dir> omitted, default to current directory (git convention)
```

### pylabhub-actor

```bash
# First-time setup
pylabhub-actor --init <actor_dir>
  → prompt: actor_name (e.g. "lab.daq.sensor1")
  → prompt: hub_dir (path to the hub this actor connects to)
  → prompt: role (producer/consumer), channel, schema
  → generate actor_uid (UUID4)
  → write actor.json

# Register with hub (for 'verified' policy)
pylabhub-actor --register-with <hub_dir> <actor_dir>
  → reads actor_name + actor_uid from actor.json
  → appends to hub.json known_actors (requires hub_dir write access)

# Run
pylabhub-actor <actor_dir>
  → reads actor.json → reads hub.json + hub.pubkey → connect → run

pylabhub-actor <actor_dir> --validate
  → validate script + schema layout, do not connect

# Default to current directory if <actor_dir> omitted
```

---

## Implementation Phases

Dependencies flow top-to-bottom within each phase.

### Phase 1 — Foundation (no existing code changes)

- [ ] `hub_identity.hpp/cpp` — `generate_uuid4()`, `is_valid_uuid4()` (libsodium entropy)
- [ ] `hub_vault.hpp/cpp` — `HubVault::create()`, `HubVault::open()`, `publish_public_key()`
- [ ] `pylabhub-hubshell --init` — directory creation, hub.json, vault, hub.pubkey
- [ ] `pylabhub-hubshell <hub_dir>` — directory-based invocation, password prompt, vault open

### Phase 2 — Actor identity + connection wiring

- [ ] `ActorConfig` — directory model, `actor_uid`, `actor_name`, `hub_dir` fields
- [ ] `ActorConfig::from_directory()` — read actor.json, resolve hub endpoint + pubkey
- [ ] `actor_main.cpp` — call `Messenger::connect(endpoint, pubkey)` before host creation
- [ ] `pylabhub-actor --init` — directory creation, actor.json, UUID4 generation
- [ ] `ProducerOptions` / `ConsumerOptions` — add `actor_name`, `actor_uid`
- [ ] `ProducerInfo` / `ConsumerInfo` — add `actor_name`, `actor_uid`
- [ ] Update REG_REQ / CONSUMER_REG_REQ JSON bodies to include identity fields

**Milestone: `pylabhub-actor <actor_dir>` runs end-to-end against a live hub.**

### Phase 3 — Connection policy + collision detection

- [ ] `BrokerService::Config` — `connection_policy`, `known_actors`, `channel_policies`
- [ ] `HubConfig` — `hub_uid()`, `connection_policy()`, `known_actors()` getters
- [ ] `ChannelEntry` / `ConsumerEntry` — `actor_name`, `actor_uid`, `hostname`, `connected_at`
- [ ] `BrokerService` — policy enforcement in `handle_reg_req()` / `handle_consumer_reg_req()`
- [ ] Collision detection logic (liveness check → replace or reject)
- [ ] `pylabhub-actor --register-with` — append to hub.json known_actors

### Phase 4 — C-API provenance (core structure change — review checklist required)

**Goal**: SHM segment is fully self-describing. Any C consumer can read the complete
identity chain (hub → producer → consumers) from the SHM alone.

- [x] Review checklist: `SharedMemoryHeader` changes per `IMPLEMENTATION_GUIDANCE.md`
      (static_assert 4KB still passes; reserved_header budget documented in header comment)
- [x] `SharedMemoryHeader` — add channel identity block:
      `hub_uid[40]`, `hub_name[64]`, `producer_uid[40]`, `producer_name[64]` (208 bytes)
- [x] `SharedMemoryHeader::ConsumerHeartbeat` — expand 64→128 bytes:
      rename `consumer_id` → `consumer_pid`; add `consumer_uid[40]`, `consumer_name[32]`
- [x] `reserved_header` reduced from 2320 → 1600 bytes (budget: +512 heartbeats +208 identity)
- [x] New C types in `slot_rw_coordinator.h`: `plh_channel_identity_t`, `plh_consumer_identity_t`
      (placed here per unified API placement pattern)
- [x] New C functions `slot_rw_get_channel_identity()`, `slot_rw_list_consumers()` in
      `slot_rw_coordinator.h` / `data_block.cpp`
- [x] New C wrappers `datablock_get_channel_identity()`, `datablock_list_consumers()` in
      `recovery_api.hpp` / `data_block_recovery.cpp`
- [x] Init: `ConsumerHeartbeat` loop zeroes `consumer_uid`/`consumer_name`; channel identity
      fields zeroed at header creation (2026-02-20)
- [x] All `consumer_id` → `consumer_pid` renames: `data_block.cpp`, `data_block_recovery.cpp`,
      test workers (policy enforcement, recovery scenario) — 424/424 tests pass
- [ ] `DataBlockConfig` — add `hub_uid[40]`, `hub_name[64]`, `producer_uid[40]`, `producer_name[64]`
- [ ] `create_datablock_producer_impl()` — write all channel identity fields into header
- [ ] `attach_datablock_as_writer_impl()` — explicitly skip identity fields (do NOT overwrite)
- [ ] `DataBlockConsumer` attach — claim heartbeat slot; write `consumer_uid` + `consumer_name`
- [ ] `DataBlockConsumer` detach — zero `consumer_uid` + `consumer_name`; clear `consumer_pid`
- [ ] `DataBlockProducer` / `DataBlockConsumer` — typed accessors `hub_uid()`, `hub_name()`,
      `producer_uid()`, `producer_name()`
- [ ] `hub::Producer::create()` — propagate `actor_uid` + `actor_name` into `DataBlockConfig`
- [ ] `hub::Consumer::connect_from_parts()` — write `actor_uid` + `actor_name` to heartbeat slot
- [ ] Update BLDS schema hash computation (layout hash changes with new fields)
- [ ] Tests: identity round-trip create/attach (C++ and C-API); `datablock_list_consumers()`
      reflects consumer attach/detach correctly

### Phase 5 — HubConfig directory model migration

- [ ] `HubConfig` — switch from `hub.default.json` + `hub.user.json` to `hub.json` + compiled defaults
- [ ] `HubConfig::hub_uid()` — new getter
- [ ] `HubConfig::hub_dir()` — new getter
- [ ] `hubshell.cpp` — pass `hub_dir` to `HubConfig::load_()`; open vault; supply stable keypair
      to `BrokerService::Config`
- [ ] Update `BrokerService` startup to accept keypair from vault instead of generating fresh
- [ ] Remove stale `hub.default.json.in` template from CMake staging if no longer needed

---

## Deferred

| Item | Reason |
|---|---|
| `signed` policy level | Requires PKI design; actor key distribution mechanism not defined |
| Cross-hub identity federation | Multiple-hub topologies; depends on hub naming convention being stable |
| Actor key rotation | Re-encrypting vault with new password; procedure not defined |
| Provenance query API | Query broker for full chain given a slot_seq; separate design phase |
| Consumer UID write ordering | Consumer writes uid/name before atomic pid store; needs `memory_order_release` / `memory_order_seq_cst` analysis to ensure readers see complete data |
| Actor config schema validation | JSON schema for actor.json; nice-to-have for `--validate` |

---

## Non-goals

- Cryptographic actor authentication (`signed` policy) — deferred explicitly above
- Runtime secret injection (vault contents changing without restart)
- Multi-user hub access (one master password per hub)
- Network-accessible key management service

---

## Recent Completions

### 2026-02-20
- Phase 4 (partial) — C-API identity layer for `SharedMemoryHeader`:
  - `ConsumerHeartbeat` expanded 64→128 bytes; `consumer_id` renamed to `consumer_pid`;
    `consumer_uid[40]` and `consumer_name[32]` added
  - Channel identity block added (208 bytes): `hub_uid[40]`, `hub_name[64]`,
    `producer_uid[40]`, `producer_name[64]`
  - `reserved_header` reduced 2320→1600; static_assert 4KB still holds
  - New C types: `plh_channel_identity_t`, `plh_consumer_identity_t` in `slot_rw_coordinator.h`
  - New C functions: `slot_rw_get_channel_identity()`, `slot_rw_list_consumers()` (header-ptr)
    and `datablock_get_channel_identity()`, `datablock_list_consumers()` (shm-name wrappers)
  - All `consumer_id` → `consumer_pid` renames propagated to all call sites in src/ and tests/
  - 424/424 tests pass
