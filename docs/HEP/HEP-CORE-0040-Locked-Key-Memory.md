# HEP-CORE-0040: Locked Key Memory

| Field           | Value |
|-----------------|-------|
| **Status**      | 🚧 **Design — impl in flight.** Promoted from tech_draft 2026-06-05 after four fresh-eye review rounds (#166). Implementation tracked as tasks #169 (SecureMemorySubsystem) + #170 (KeyStore + LockedKey + SecureBuffer) + #171 (HubConfig/RoleConfig migration) + #172 (BrokerService/BRC migration) + #173 (RoleAPIBase accessors) + #174 (HubAPI accessors) + #175 (vault hardening prerequisite). |
| **Type**        | Framework primitive (cross-cutting). |
| **Author**      | Quan Qing. |
| **Date**        | 2026-06-05. |
| **Supersedes**  | HEP-CORE-0035 §4.7 utility-only specification (the SecureKeyBuffer + disable_core_dumps sketch). §4.7 retains the **threat model + consumer requirement**; the framework primitives move here. |
| **Related**     | HEP-CORE-0001 (Hybrid Lifecycle Model) — base contract for registered subsystems. HEP-CORE-0031 (ThreadManager + Thread Shutdown Contract) — dynamic auto-registered module pattern this HEP mirrors. HEP-CORE-0035 §4.6 (key-file storage discipline at rest) + §4.7 (runtime key handling consumer surface). HEP-CORE-0038 (script-accessible vault keystore) — consumer of dynamic KeyStore for runtime-saved scripted secrets. |
| **Tracks**      | Tasks #165 (this draft) → #166 (review) → #167 (promote) → #168 (HEP-0035 cross-ref) → #169..#174 (impl chain). |
| **Charter**     | **Narrow.** "Locked Key Memory" — covers seckey-class secret storage backed by mlock'd / sodium_malloc'd memory and zero-on-destruct. Does NOT define a general "secure memory subsystem"; if future use cases emerge (TLS session keys, signing contexts), they may cite this HEP or motivate a broader successor. |

---

## 1. Motivation

CURVE identity keypairs (HEP-CORE-0035) and script-saved vault secrets (HEP-CORE-0038 / task #106) share a runtime-memory protection requirement: locked pages, no core dump exposure, zero-on-destruction. Today HEP-0035 §4.7 sketches a flat utility layer (`SecureKeyBuffer` RAII + `disable_core_dumps()` free function) but treats those as ad-hoc helpers rather than registered framework subsystems.

That gap matters for three reasons:

1. **Process-init failures need to be visible.** `setrlimit(RLIMIT_CORE, 0)` / `prctl(PR_SET_DUMPABLE, 0)` / Windows `SetErrorMode` are one-shot platform configuration with failure modes the framework should surface ("subsystem X failed to start") rather than swallow inside a config-load routine.
2. **Dynamic registration matters.** Keys do not exist at `main()` entry — vault decrypt happens after CLI parsing and password prompt. Script-saved secrets (HEP-0038) come into existence at arbitrary runtime moments. A flat utility cannot model these lifecycles; a registered, dynamic LifecycleGuard module can.
3. **One owner per secret, period.** Multiple value-copies of the seckey across `Config` / `BrokerService::Config` / `RoleAPIBase::Impl` each require their own mlock region, own zero-on-destruct, own audit. A single owner that the rest of the system holds references into is materially simpler to harden under §4.7 threats.

This HEP lifts the §4.7 utility layer into the framework: a **static** process-init lifecycle module + a **dynamic** per-key store, both registered with LifecycleGuard. HEP-0035 §4.7 becomes a one-line consumer reference; HEP-0038 / #106 cites the same primitive.

---

## 2. Threat model + scope

Inherits HEP-0035 §4.7.1 threats verbatim:

| # | Leak channel | What an attacker gets without locked-memory protection |
|---|---|---|
| 1 | OS pages memory to swap | seckey bytes carved from the swap partition |
| 2 | Process crashes → core dump | seckey bytes in the dump file |
| 3 | Lingering plaintext in heap buffers after key has been handed to libsodium / libzmq | seckey bytes recoverable via heap forensics |

Out-of-scope (also inherited from §4.7.1 / HEP-0036 I8):

- Live-memory attackers (debugger, ptrace from root, malicious in-process script).
- libzmq's internal copy after we hand a key to a CURVE-configured socket (libzmq holds its own copy for the socket's lifetime; we do not have access to it).
- Hardware-backed key isolation (HSM / TEE).

---

## 3. Architecture overview

Two pieces, both registered LifecycleGuard modules:

```
                                 plh_role / plh_hub main()
                                          │
            ┌─────────────────────────────┴─────────────────────────────┐
            │                                                            │
            ▼                                                            │
   ┌─────────────────────────────┐                                       │
   │ SecureMemorySubsystem       │  STATIC, process singleton.           │
   │ (one per process,           │  Created at main() entry, BEFORE      │
   │  named "SecureMemory")      │  vault open / key load.               │
   │                             │                                       │
   │ ctor:                       │  Consumer access via guarded global   │
   │  - setrlimit RLIMIT_CORE    │  accessor in secure_memory_subsystem  │
   │  - prctl PR_SET_DUMPABLE    │  .hpp:                                │
   │  - inspect RLIMIT_MEMLOCK   │                                       │
   │  - Win: SetErrorMode +      │   secure_memory_subsystem()           │
   │    WerAddExcluded, +        │     (throws if not constructed)       │
   │    SeLockMemoryPrivilege    │   secure_memory_subsystem_ready()     │
   │  - register lifecycle       │                                       │
   │    module "SecureMemory"    │  Fail-fast at ctor if dumps cannot be │
   │    dep on "Logger"          │  disabled.                            │
   │                             │                                       │
   │ Holds no key material.      │                                       │
   └─────────────────────────────┘                                       │
            │                                                            │
            │  (must be running before any KeyStore ctor)                │
            ▼                                                            │
   ┌─────────────────────────────┐                                       │
   │ KeyStore                    │  DYNAMIC, process singleton.          │
   │ (one per process,           │  Constructed by HubHost (hub binary)  │
   │  named "KeyStore")          │  or RoleHostFrame (role binary).      │
   │                             │                                       │
   │ • owns N LockedKey by name  │  Consumer access via guarded global   │
   │                             │  accessor in key_store.hpp:           │
   │ ctor:                       │                                       │
   │  - registers lifecycle      │  WRITES (exclusive lock):             │
   │    module "KeyStore" dep    │   key_store().add_identity(name, span)│
   │    on "SecureMemory" +      │   key_store().add_raw(name, span)     │
   │    "Logger"                 │   key_store().remove(name)            │
   │                             │                                       │
   │ Use-not-export API for      │  READS — parallel (shared lock):      │
   │  secrets:                   │   key_store().pubkey(name) →          │
   │  - pubkey returned as       │     std::string_view (non-secret)     │
   │    string_view              │   key_store().with_seckey(name, cb)   │
   │  - seckey via with_seckey   │     // cb invoked with std::string_view│
   │    callback only            │     // valid for callback scope only  │
   │  - lookup_raw for HEP-0038  │     // cb MUST be prompt (no I/O)     │
   │    script secrets           │   key_store().lookup_raw(name) → span │
   │                             │   key_store().has(name)               │
   │  key_store() ref):          │                                       │
   │  add_identity / add_raw     │  Lifecycle handles ORDERING only —    │
   │  lookup / lookup_raw        │  never instance retrieval.            │
   │  remove / has               │  key_store() throws if not yet ctor'd.│
   └─────────────────────────────┘                                       │
            │                                                            │
            │ owns                                                       │
            ▼                                                            │
   ┌─────────────────────────────┐                                       │
   │ LockedKey                   │  RAII (not a Lifecycle Module on      │
   │ (one per stored secret)     │  its own — owned by KeyStore)         │
   │                             │                                       │
   │ ctor:                       │  Allocated via sodium_malloc          │
   │  - sodium_malloc(len)       │  (mlock + guard pages + canary).      │
   │  - copy plaintext in        │  Caller-provided plaintext source     │
   │  - sodium_memzero source    │  buffer zeroed immediately after.     │
   │                             │                                       │
   │ dtor:                       │  Compiler-safe zero (sodium_memzero), │
   │  - sodium_memzero buffer    │  unlock+free via sodium_free.         │
   │  - sodium_free              │                                       │
   │                             │                                       │
   │ accessor:                   │  Non-owning view (CurveKeypair        │
   │  - const CurveKeypair&      │  type for identity keys; future       │
   │    keypair() const          │  HEP-0038 secrets may use a           │
   │                             │  different view shape).               │
   └─────────────────────────────┘
```

---

## 4. SecureMemorySubsystem (static)

### 4.1 Charter

Process-wide platform setup. Exactly one instance per OS process. Created at `main()` entry by the binary's startup code. Registers a dynamic lifecycle module named `"SecureMemory"` in its ctor (mirroring ThreadManager's pattern per HEP-CORE-0031 §3 — ctor registers, dtor deregisters; the lifecycle dependency is on `Logger`, ordering only). Holds no key material.

### 4.2 Startup steps

In order:

1. **Disable core dumps.**
   - POSIX: `setrlimit(RLIMIT_CORE, {0, 0})`. Fatal if it fails.
   - Linux additional: `prctl(PR_SET_DUMPABLE, 0)` (also blocks `ptrace` from non-root).
   - Linux page-granular: per-allocation `madvise(addr, len, MADV_DONTDUMP)` is applied in LockedKey ctor, not here.
   - Windows: `SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS)` + `WerAddExcludedApplication(L"plh_<bin>.exe")`. Fatal if SetErrorMode fails.
2. **Inspect mlock capability.**
   - POSIX: `getrlimit(RLIMIT_MEMLOCK)`. Log WARN if low (<256 KB) — operator-visible advisory; do not fatal because some hardened sandboxes set RLIMIT_MEMLOCK low for non-key purposes.
   - Windows: attempt to enable `SeLockMemoryPrivilege` via `AdjustTokenPrivileges`. Log WARN if not granted (operator must grant via `secpol.msc` Group Policy → Local Policies → User Rights Assignment).
3. **Mark module Running.** LifecycleGuard transitions to Running; any subsequent `KeyStore` construction is permitted.

### 4.3 Failure modes

| Failure | Disposition |
|---|---|
| `setrlimit(RLIMIT_CORE)` fails | FATAL — subsystem fails to start; binary aborts before vault open. Emits "SecureMemorySubsystem: failed to disable core dumps: <errno>" to stderr. |
| `prctl(PR_SET_DUMPABLE)` fails | FATAL on Linux. |
| `SetErrorMode` / `WerAddExcludedApplication` fail on Windows | FATAL. |
| `RLIMIT_MEMLOCK` low | WARN. Continues. |
| `SeLockMemoryPrivilege` not granted on Windows | WARN. KeyStore construction will fail later if mlock denies. |

### 4.4 Shutdown

No teardown work. `disable_core_dumps` is irreversible by design (we do NOT re-enable on shutdown). LifecycleGuard transitions through `Stopping` → `ShutDown` with no side effects; this is recorded for ordering audit only.

### 4.5 Ordering invariant

`SecureMemorySubsystem` must have completed its ctor before any `KeyStore` is constructed. Enforced two ways:

1. **Lifecycle dependency** (declarative): `KeyStore`'s registered module declares a dependency on `"SecureMemory"`. LifecycleGuard refuses to start the dependent if the dependency hasn't completed — same mechanism ThreadManager uses for its Logger dependency.
2. **Runtime check** in `KeyStore`'s ctor: calls `secure_memory_subsystem_ready()` (the global probe; see §4.6) and throws `std::logic_error` if false. Belt and braces — catches the case where someone constructs a `KeyStore` outside the LifecycleGuard-driven path (e.g., a malformed test).

### 4.6 Class skeleton and global access

Public class follows the canonical pImpl convention (`docs/IMPLEMENTATION_GUIDANCE.md §"pImpl Idiom (MANDATORY for Public Classes)"`).

```cpp
// secure_memory_subsystem.hpp
namespace pylabhub::utils::security {

class PYLABHUB_UTILS_EXPORT SecureMemorySubsystem {
public:
    /// Runs the §4.2 startup steps in ctor (disable core dumps,
    /// inspect mlock capability, platform setup). Throws on fatal
    /// failures per §4.3. Registers the dynamic lifecycle module
    /// `"SecureMemory"` with dependency `"pylabhub::utils::Logger"`;
    /// startup/shutdown thunks are no-ops (the actual work runs in
    /// ctor/dtor — LifecycleGuard provides only the ordering
    /// guarantee).
    SecureMemorySubsystem();

    /// Deregisters the lifecycle module. Does NOT re-enable core
    /// dumps (irreversible by design per §4.4).
    /// Defined in .cpp so Impl is complete at the dtor site.
    ~SecureMemorySubsystem();

    SecureMemorySubsystem(const SecureMemorySubsystem&)            = delete;
    SecureMemorySubsystem& operator=(const SecureMemorySubsystem&) = delete;
    SecureMemorySubsystem(SecureMemorySubsystem&&)                 = delete;
    SecureMemorySubsystem& operator=(SecureMemorySubsystem&&)      = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

/// Global access — guarded singleton accessor.  Standard C++ idiom,
/// not a HEP-specific pattern.  Consumer access is the class's OWN
/// concern; LifecycleManager provides no instance-retrieval API
/// (see HEP-CORE-0001).
PYLABHUB_UTILS_EXPORT SecureMemorySubsystem& secure_memory_subsystem();         ///< throws if not constructed
PYLABHUB_UTILS_EXPORT bool                  secure_memory_subsystem_ready() noexcept;

}  // namespace pylabhub::utils::security
```

```cpp
// secure_memory_subsystem.cpp (sketch)
namespace pylabhub::utils::security {

namespace {
std::mutex                g_sms_mu;
SecureMemorySubsystem*    g_sms = nullptr;
}

struct SecureMemorySubsystem::Impl {
    // (state from §4.2 setup — recorded RLIMIT_MEMLOCK value,
    //  Windows privilege handle, etc.)
};

SecureMemorySubsystem::SecureMemorySubsystem()
    : pImpl(std::make_unique<Impl>())
{
    // §4.2 step 1: disable_core_dumps (fatal on failure)
    // §4.2 step 2: inspect mlock capability (WARN on low)
    // §4.2 step 3: register lifecycle module "SecureMemory" with dep
    //              ["pylabhub::utils::Logger"]; thunks no-op.
    std::lock_guard lk(g_sms_mu);
    if (g_sms)
        throw std::logic_error("SecureMemorySubsystem: already constructed");
    g_sms = this;
}

SecureMemorySubsystem::~SecureMemorySubsystem() {
    std::lock_guard lk(g_sms_mu);
    if (g_sms == this) g_sms = nullptr;
    // deregister lifecycle module.
}

SecureMemorySubsystem& secure_memory_subsystem() {
    std::lock_guard lk(g_sms_mu);
    if (!g_sms)
        throw std::runtime_error("SecureMemorySubsystem: not initialized");
    return *g_sms;
}

bool secure_memory_subsystem_ready() noexcept {
    std::lock_guard lk(g_sms_mu);
    return g_sms != nullptr;
}

}  // namespace
```

The **class API** (`secure_memory_subsystem()` accessor) is a separate concern from the **lifecycle contract** (the registered module that LifecycleGuard orders). Lifecycle's job is ordering; the global accessor is the class's own access mechanism. Same separation HEP-CORE-0031 ThreadManager uses (lifecycle module for ordering; consumers reach the instance directly — never through a registry call).

---

## 5. KeyStore (dynamic)

### 5.1 Charter

Owns N `LockedKey` instances. Exactly one instance per OS process — hub processes have HubHost and not RoleHostFrame, role processes vice versa; the scopes are disjoint by binary, so "one per scope" = "one per process". Constructed by HubHost (in hub binaries) or RoleHostFrame (in role binaries) during their own startup, after the `SecureMemory` lifecycle module is running.

Lifecycle contract (mirrors ThreadManager §3): ctor registers a dynamic lifecycle module named `"KeyStore"` with `"SecureMemory"` (and `"pylabhub::utils::Logger"`) as dependencies. Startup thunk: no-op (LockedKey instances populate via `add_identity` / `add_raw` calls during the process's own setup, not during the module's startup callback). Shutdown thunk: no-op (instances tear down in ctor's reverse via the owning HubHost/RoleHostFrame dtor; LifecycleGuard provides the ordering guarantee, not the destruction step). Dtor deregisters the module.

Global access — see §5.6.

### 5.2 API surface (C++ only — not script-bound)

Plain instance methods. Consumers reach the unique instance via the global accessor in §5.6, then call methods directly.

Public class follows the canonical pImpl convention (`docs/IMPLEMENTATION_GUIDANCE.md §"pImpl Idiom (MANDATORY for Public Classes)"`): `struct Impl;` forward-declared inner; `std::unique_ptr<Impl> pImpl;` outer member; dtor declared in header, defined in `.cpp`; all state (mutex, name→LockedKey map, owner_tag/owner_id) lives in Impl; outer class holds nothing besides `pImpl`.

**Design principle — use-not-export for secrets (2026-06-06).**  Public-half keys (`pubkey` — non-secret) are returned as views into LockedKey-owned bytes.  Secret-half keys are NEVER returned as data — they are accessed only via `with_seckey(name, callback)`, where the callback receives a `std::string_view` valid only for its scope.  Bytes never leave the LockedKey region; the security module owns the OPERATION, not the byte handout.  See §11 audit trail for the round-5 refinement that led to this shape.

```cpp
// key_store.hpp
namespace pylabhub::utils::security {

class PYLABHUB_UTILS_EXPORT KeyStore {
public:
    /// Constructs and registers the dynamic lifecycle module
    /// `"KeyStore"` with dependencies on `"SecureMemory"` and
    /// `"pylabhub::utils::Logger"`. Startup/shutdown thunks are no-ops
    /// (the LockedKey instances populate via add_identity/add_raw calls
    /// during process setup, and tear down with KeyStore's dtor —
    /// LifecycleGuard provides only the ordering guarantee).
    KeyStore(std::string scope_tag, std::string owner_id);

    /// Deregisters the lifecycle module and destroys all stored
    /// LockedKey instances (each sodium_memzero + sodium_free).
    /// Defined in .cpp so Impl is complete at the dtor site.
    ~KeyStore();

    KeyStore(const KeyStore&)            = delete;
    KeyStore& operator=(const KeyStore&) = delete;
    KeyStore(KeyStore&&)                 = delete;
    KeyStore& operator=(KeyStore&&)      = delete;

    // ── Writes (exclusive lock) ──────────────────────────────────

    /// Insert an identity keypair packed as pub_z85 (40 bytes) +
    /// sec_z85 (40 bytes) — 80 bytes total. Source buffer zeroed
    /// before return. Throws `std::runtime_error` if `name` already
    /// present.
    void add_identity(std::string_view name,
                      std::span<std::byte> packed_pub_sec);

    /// Insert raw secret bytes (HEP-0038 script vault_save).
    /// `plaintext` zeroed before return. Caller-side name validation
    /// enforces reserved prefixes (framework names like
    /// `"hub_identity"` / `"role_identity"` must not be reachable
    /// from HEP-0038 scripts).
    void add_raw(std::string_view name,
                 std::span<std::byte> plaintext);

    /// Remove a stored secret. No-op if absent.  Blocks until any
    /// in-flight `with_seckey` callback for the same name returns
    /// (correct security semantic — bytes become unreachable for
    /// every caller as soon as remove() returns).
    void remove(std::string_view name);

    // ── Reads (shared lock; parallel across consumers) ──────────

    /// Return the Z85 PUBLIC key (40 chars) for an identity entry.
    /// View points into LockedKey-owned bytes; lifetime is until
    /// remove() or KeyStore dtor. Pubkeys are non-secret — fine to
    /// pass / log / copy.
    /// Throws `std::out_of_range` if `name` is absent or refers to a
    /// raw entry (use `lookup_raw` for HEP-0038 secrets).
    [[nodiscard]] std::string_view pubkey(std::string_view name) const;

    /// Invoke `use` with the Z85 SECRET key (40 chars) for an
    /// identity entry.  View is valid ONLY inside the callback;
    /// storing it past return is undefined.  Bytes live in the
    /// LockedKey region; the security module never materializes a
    /// std::string copy.
    ///
    /// Shared lock is held for the callback's duration — concurrent
    /// `with_seckey` / `pubkey` / `lookup_raw` calls run in parallel,
    /// but a concurrent `remove(name)` waits.  Callback MUST be
    /// prompt (microseconds): no blocking I/O, no syscalls beyond
    /// what's needed to consume the bytes (typically a single
    /// `socket.set(zmq::sockopt::curve_secretkey, sv)` call).
    ///
    /// Throws `std::out_of_range` if `name` is absent or refers to a
    /// raw entry.  Rethrows anything thrown by `use` after releasing
    /// the shared lock.
    void with_seckey(std::string_view name,
                     std::function<void(std::string_view)> use) const;

    /// HEP-0038 raw-secret access.  Span lifetime is until remove()
    /// or KeyStore dtor; script bindings MUST materialize the bytes
    /// into a script-owned buffer before returning to the script,
    /// not pass the span to script code.
    /// Throws `std::out_of_range` if `name` is absent.
    [[nodiscard]] std::span<const std::byte>
                  lookup_raw(std::string_view name) const;

    /// Existence check (tests; production uses pubkey() /
    /// with_seckey() and lets the throw signal).
    [[nodiscard]] bool has(std::string_view name) const noexcept;

    /// Number of stored entries.  Snapshot under shared lock.
    [[nodiscard]] std::size_t size() const noexcept;

    /// Implementation state.  Declared public so the free-function
    /// lifecycle thunks (in `key_store.cpp`) can dispatch against it.
    /// Still opaque — struct definition lives in the `.cpp`.  Same
    /// pattern as `pylabhub::utils::ThreadManager::Impl`.
    struct Impl;

private:
    std::unique_ptr<Impl> pImpl;
};

/// Global access — guarded singleton accessor.  Mirrors the pattern in
/// HEP-CORE-0031 §3 (ThreadManager) where lifecycle handles ordering
/// and the class itself owns its access mechanism.  LifecycleManager
/// does NOT provide instance retrieval (see HEP-CORE-0001).
PYLABHUB_UTILS_EXPORT KeyStore& key_store();             ///< throws if not constructed
PYLABHUB_UTILS_EXPORT bool      key_store_ready() noexcept;

}  // namespace pylabhub::utils::security
```

**What this design eliminates that earlier iterations had:**

- No `lookup(name) → const CurveKeypair&` method — that signature forced KeyStore to cache a second copy of pub_z85 + sec_z85 in heap-allocated `std::string` members (so the returned reference had a stable target).  The cached strings were unlocked, never zeroed on destruction, and persisted for KeyStore's lifetime — exactly the second-copy / unlocked-plaintext anti-pattern this HEP exists to eliminate.  Round-5 refinement (2026-06-06) replaced it with the use-not-export pair `pubkey` / `with_seckey`.

```cpp
// key_store.cpp (sketch)
namespace pylabhub::utils::security {

namespace {
std::mutex   g_key_store_mu;
KeyStore*    g_key_store = nullptr;
}

struct KeyStore::Impl {
    std::string                                       scope_tag;
    std::string                                       owner_id;
    mutable std::shared_mutex                         mu;
    std::unordered_map<std::string, LockedKey>        store;
};

KeyStore::KeyStore(std::string scope_tag, std::string owner_id)
    : pImpl(std::make_unique<Impl>(Impl{std::move(scope_tag),
                                          std::move(owner_id), {}, {}}))
{
    if (!secure_memory_subsystem_ready())
        throw std::logic_error("KeyStore: SecureMemorySubsystem not initialized");
    {
        std::lock_guard lk(g_key_store_mu);
        if (g_key_store)
            throw std::logic_error("KeyStore: already constructed");
        g_key_store = this;
    }
    // register dynamic lifecycle module "KeyStore" with deps
    // ["SecureMemory", "pylabhub::utils::Logger"]; thunks no-op.
}

KeyStore::~KeyStore() {
    // deregister lifecycle module.
    std::lock_guard lk(g_key_store_mu);
    if (g_key_store == this) g_key_store = nullptr;
}

KeyStore& key_store() {
    std::lock_guard lk(g_key_store_mu);
    if (!g_key_store)
        throw std::runtime_error("KeyStore: not initialized");
    return *g_key_store;
}

bool key_store_ready() noexcept {
    std::lock_guard lk(g_key_store_mu);
    return g_key_store != nullptr;
}

}  // namespace
```

Consumers always call `pylabhub::utils::security::key_store().lookup(name)` — never `key_store().lookup(...)` (no static methods on the class) and never anything LifecycleManager-shaped. `CurveKeypair` is the existing strong type at `src/include/utils/security/curve_keypair.hpp`. HEP-0038 secrets are accessed via `lookup_raw` (returns a span); they do NOT go through `CurveKeypair` because they have no required structure.

### 5.3 Naming convention

| Use case | Key name |
|---|---|
| HubHost identity (one per process) | `"hub_identity"` |
| RoleHostFrame identity (one per process) | `"role_identity"` |
| HEP-0038 / #106 script-saved secret | `"vault:<script-chosen-name>"` (prefix-scoped to avoid colliding with framework-owned names) |

Future federation peer pubkeys are NOT stored here — pubkeys don't need locking.

### 5.4 Lifecycle

- **Constructed**: by HubHost / RoleHostFrame ctor (after SecureMemorySubsystem has reached Running).
- **Running**: during process operation; `add` / `get` / `remove` allowed.
- **Stopping**: LifecycleGuard transition. Existing LockedKey instances are NOT destroyed during Stopping (consumers may still hold references); destruction happens in dtor.
- **ShutDown**: dtor runs. All LockedKey instances destruct in reverse-add order. Each sodium_memzero + sodium_free's its buffer. KeyStore deregisters from LifecycleGuard.

### 5.5 Thread safety

Two write paths exist in the full production picture:

1. **Startup** — `add_identity` called once during vault open (single-threaded by construction; load_keypair runs on the main / startup thread before any worker exists).
2. **Runtime** — `add_raw` / `remove` called by HEP-0038 script threads at arbitrary moments while broker / data-plane threads are reading via `pubkey` / `with_seckey` / `lookup_raw`.

The runtime case forces real concurrency control. KeyStore's `Impl` uses `std::shared_mutex` (fairness policy is implementation-defined — the expected workload is read-dominated with rare writes, so policy choice is not load-bearing).

- **Read paths** (`pubkey`, `with_seckey`, `lookup_raw`, `has`, `size`): shared lock. Multiple consumers run in parallel — broker's bind path, BRC's connect path, a script's `vault_load`, and a federation peer's connect can all be inside read methods simultaneously without blocking each other.
- **Write paths** (`add_identity`, `add_raw`, `remove`): exclusive lock. Serialized; block until all in-flight readers release.

LockedKey bytes are immutable after `add_*` until `remove()`.  Once a read method has the shared lock and a pointer into the LockedKey buffer, the bytes can't change underneath it.  The shared lock protects against the MAP entry being removed (which would destroy the LockedKey and zero the bytes — UAF for any in-flight reader).  Thus:

- **`pubkey(name)`** returns `std::string_view` into the LockedKey buffer.  Caller can hold the view past the call **only** if the caller can prove no `remove(name)` will run before they drop it.  For framework identity keys (never removed at runtime), holding indefinitely is safe.  For HEP-0038 secrets, materialize immediately.
- **`with_seckey(name, use)`** holds the shared lock for the duration of `use`.  Concurrent `remove(name)` waits for `use` to return.  Callback contract: prompt completion (microseconds — a `socket.set` call is the typical operation).  Blocking I/O inside `use` starves writers; explicitly prohibited.
- **`lookup_raw(name)`** returns a span; same lifetime rules as `pubkey`.

`remove()` blocks behind any in-flight `with_seckey` for the same name — that's the correct security semantic.  When a script asks to delete a secret, no in-flight caller retains addressable bytes after `remove()` returns.

---

## 6. LockedKey (RAII)

Not a LifecycleGuard module on its own. Owned by KeyStore.

### 6.1 Construction

```cpp
LockedKey::LockedKey(std::span<std::byte> plaintext_src) {
    buf_ = static_cast<std::byte*>(sodium_malloc(plaintext_src.size_bytes()));
    if (!buf_) throw std::runtime_error("sodium_malloc failed (RLIMIT_MEMLOCK?)");
    std::memcpy(buf_, plaintext_src.data(), plaintext_src.size_bytes());
    sodium_memzero(plaintext_src.data(), plaintext_src.size_bytes());
    len_ = plaintext_src.size_bytes();
    // Linux: madvise(buf_, len_, MADV_DONTDUMP) for defence-in-depth
    // (in addition to the process-wide PR_SET_DUMPABLE=0).
}
```

### 6.2 Destruction

```cpp
LockedKey::~LockedKey() noexcept {
    if (buf_) {
        sodium_memzero(buf_, len_);
        sodium_free(buf_);  // also munlocks + free
    }
}
```

### 6.3 Properties

- Non-copyable, non-movable. Pass by reference.
- Source plaintext zeroed in ctor — caller does not need to remember to zero.
- libsodium handles platform abstraction (Linux mlock, Windows VirtualLock).
- libsodium adds guard pages + canary; we get those for free.

### 6.4 `SecureBuffer<N>` — stack-only RAII for short-lived plaintext

Helper for intermediate plaintext buffers (e.g. the pack buffer in `load_keypair` before handoff to KeyStore). Lighter than LockedKey — no sodium_malloc, no mlock, no guard pages — but zeroes on dtor so failed-path exceptions don't leave plaintext on the stack.

```cpp
template <std::size_t N>
class SecureBuffer {
public:
    SecureBuffer() = default;
    ~SecureBuffer() noexcept { sodium_memzero(data_.data(), N); }
    SecureBuffer(const SecureBuffer&)            = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    [[nodiscard]] std::span<std::byte> span() noexcept { return data_; }

private:
    std::array<std::byte, N> data_{};
};
```

Use only for buffers whose lifetime is bounded by a single function call. NOT for long-lived storage (use LockedKey). Pages are unlocked, so swap exposure exists for the (sub-millisecond) lifetime — acceptable for the brief handoff window but not as a primary storage primitive.

---

## 7. Cross-platform mechanism

Inherits HEP-0035 §4.7.3 table verbatim. libsodium wraps all primitives; we do not fall back to platform-native calls.

| Measure | Wrapper used |
|---|---|
| Lock memory pages | `sodium_mlock()` (internal to `sodium_malloc`) |
| Hardened allocator (mlock + guard pages + canary + auto-wipe) | `sodium_malloc()` / `sodium_free()` |
| Disable core dumps (POSIX) | `setrlimit(RLIMIT_CORE, {0,0})` |
| Disable core dumps (Linux defence-in-depth) | `prctl(PR_SET_DUMPABLE, 0)` |
| Exclude key pages from core dump (Linux, page-granular) | `madvise(addr, len, MADV_DONTDUMP)` (in LockedKey ctor) |
| Suppress Windows error reporting | `SetErrorMode` + `WerAddExcludedApplication` |
| Compiler-safe memory zero | `sodium_memzero()` |

---

## 8. Integration — KeyStore is the global API; nothing threads keys through ctors

**Design principle.** A lifecycle module providing a global API to locked-memory secrets means consumers QUERY the module — they do not get bytes passed to them via ctors, struct fields, or config. There are no `CurveKeypair*` views in Config, no `client_pubkey/_seckey` fields anywhere, no ctor signature changes. RoleAPIBase + HubAPI accessors look identical; BrokerService, BRC, ZmqQueue factories all reach into the same global API at the moment they need a key.

### 8.1 Config holds vault paths only — no keypair fields

`HubConfig::AuthConfig` / `RoleConfig::AuthConfig` keep ONLY the vault file path:

```cpp
struct AuthConfig {
    std::string keyfile;   // path; vault password not stored.
};
```

All `client_pubkey` / `client_seckey` fields are DELETED. Config never holds keypair bytes.

**The vault flow is NOT as clean as a single-call sketch.** Today's reality (verified 2026-06-05 against `src/utils/service/vault_crypto.hpp` + `src/utils/config/hub_config.cpp:219-269`):

- `vault_crypto::vault_read(path, password, uid)` returns `std::string plaintext_json` — a heap-allocated string containing JSON with embedded `public_z85` / `secret_z85` fields.
- `HubVault::open` (and `RoleVault::open`) parses that JSON and **caches the keypair as `std::string` members on the vault object** (which then live in the process unlocked, indefinitely).
- Today's `load_keypair` copies vault's strings into `auth.client_pubkey` / `_seckey` at hub_config.cpp:268-269. Net: at least three unlocked copies of the seckey live in the process.

The migration this HEP forces — done as part of #171 — must address all three plaintext landing pads, not just Config:

1. **Add `vault_crypto::vault_read_secure(...)`** that returns the plaintext via a sodium_malloc'd buffer the caller owns, or accepts an output span and writes through it, never materializing the plaintext as a `std::string` whose destructor cannot be trusted to zero.
2. **Refactor HubVault / RoleVault** so they do NOT cache keypair bytes as long-lived `std::string` members. Either: (a) vault objects become short-lived helpers that exist only during `load_keypair`'s scope and are destroyed (with explicit zeroing) immediately after handing the keypair to KeyStore; or (b) vault objects retain only the vault PATH and password-derived KDF state, re-reading and re-zeroing the plaintext per access.
3. **`load_keypair` itself** packs the freshly-decrypted pub+sec into a stack buffer, calls `key_store().add_identity("hub_identity", buf)` (which zeroes the source), then returns void. No keypair member assignment anywhere.

Sketch after these changes:

```cpp
void HubConfig::load_keypair(const std::string& password) {
    pylabhub::utils::security::SecureBuffer<80> pair_buf;
    vault_crypto::vault_read_secure(keyfile_path, password, uid, pair_buf.span());
    key_store().add_identity("hub_identity", pair_buf.span());
    // pair_buf.span() bytes zeroed by key_store().add_identity before return.
    // pair_buf dtor zeroes again on scope exit (defence-in-depth).
}
```

After `load_keypair` returns, NOTHING outside KeyStore's LockedKey holds the plaintext seckey. The `SecureBuffer<N>` helper is a small stack-only RAII wrapper that calls `sodium_memzero` on dtor — defined in HEP-0040 §6 alongside LockedKey for stack-buffer use cases.

The two-line HubVault refactor + the vault_read_secure addition are part of #171's scope. Without them, the LockedKey storage is a fig leaf — there's still an unlocked copy living for the whole process lifetime on the vault object.

### 8.2 API surface — pubkey via accessor, seckey via on-site operation

The original framing ("symmetric `auth_client_pubkey()` / `auth_client_seckey()` accessors on RoleAPIBase + HubAPI") was refined 2026-06-06: there is no legitimate caller that needs the seckey as a string.  Tracing every existing reader of `auth_client_seckey()`:

- `role_handler.cpp` populates `BRC::Config.client_seckey` → BRC connect calls `socket.set(zmq::sockopt::curve_secretkey, ...)`.
- `role_api_base.cpp::build_tx_queue` populates `ZmqAuthOptions.my_seckey_z85` → ZmqQueue factory calls `socket.set(curve_secretkey, ...)`.

In both cases the seckey is plumbed through call chains as `std::string` only to feed a libzmq socket option setter at the end.  The plumbing has no other purpose; once `auth_client_seckey()` returns, the bytes get assigned into another std::string, then another, then handed to libzmq.  Every hop is a copy of unlocked plaintext.

**The use-not-export form**: the security module owns the operation directly.  Consumers call `key_store().with_seckey(name, cb)` at the actual use site; `cb` runs with a `std::string_view` into the LockedKey-owned bytes and applies them to the socket option.  The bytes never leave the LockedKey region.

`auth_client_seckey()` accessors are DELETED from both RoleAPIBase and HubAPI — they have no legitimate role in the new design.

`auth_client_pubkey()` is OPTIONAL on RoleAPIBase / HubAPI.  Pubkeys are non-secret; consumers can call `key_store().pubkey(name)` directly.  If a script binding wants script-callable access to the role's own pubkey (e.g. for logging), the accessor can stay as a thin delegation to `key_store().pubkey("role_identity")` returning `std::string_view`.  Default: delete, add back only if a real caller surfaces.

Use sites — the actual code:

```cpp
// BrokerService binds the CTRL ROUTER (hub_host.cpp area):
auto& ks = pylabhub::utils::security::key_store();
ks.with_seckey("hub_identity", [&](std::string_view sec) {
    ctrl_router_.set(zmq::sockopt::curve_secretkey, sec);
});
ctrl_router_.set(zmq::sockopt::curve_publickey, ks.pubkey("hub_identity"));
ctrl_router_.set(zmq::sockopt::curve_server, 1);

// BRC connects to the broker (broker_request_comm.cpp):
auto& ks = pylabhub::utils::security::key_store();
ks.with_seckey("role_identity", [&](std::string_view sec) {
    dealer_.set(zmq::sockopt::curve_secretkey, sec);
});
dealer_.set(zmq::sockopt::curve_publickey,  ks.pubkey("role_identity"));
dealer_.set(zmq::sockopt::curve_serverkey,  broker_pubkey_z85);

// ZmqQueue producer-side bind (push_to factory):
auto& ks = pylabhub::utils::security::key_store();
ks.with_seckey(identity_key_name, [&](std::string_view sec) {
    push_.set(zmq::sockopt::curve_secretkey, sec);
});
push_.set(zmq::sockopt::curve_publickey, ks.pubkey(identity_key_name));
push_.set(zmq::sockopt::curve_server, 1);
```

Canonical names: `"role_identity"` for any role process, `"hub_identity"` for the hub process — exactly one per process; no collision possible.

No RoleAPIBase / HubAPI ctor signature changes. No backref to host needed. No `if constexpr` in EngineHost. RoleAPIBase + HubAPI remain 3-arg constructors (RoleHostCore&, role_tag, uid).  The previous "symmetric accessor" claim is replaced by "symmetric absence" — neither class exposes the seckey as data.

### 8.3 BrokerService — on-site use, no Config carriage

`BrokerService::Config` DROPS the `server_secret_key` / `server_public_key` fields entirely. At bind, the broker calls `with_seckey` for the secret half and `pubkey` for the public half — see the BrokerService example in §8.2.

Federation peer DEALERs: same `"hub_identity"` for own keys, plus peer pubkey from `peers[].pubkey_z85` (which stays in Config; pubkeys don't need locking).

`BrokerRequestComm::Config` similarly drops `client_pubkey` / `client_seckey`. BRC connect path calls `with_seckey("role_identity", ...)` + `pubkey("role_identity")` directly — see BRC example in §8.2.

### 8.4 ZmqQueue factories — accept a key name, apply on-site

Concrete signatures after C2 + #170:

```cpp
// Producer / server side (bind): identity key name + optional zap_domain.
static ZmqQueue ZmqQueue::push_to(
    std::string_view endpoint,
    std::string_view identity_key_name = "role_identity",  // hub-side overrides to "hub_identity"
    std::string_view zap_domain        = "");

// Consumer / client side (connect): own identity key name + server's pubkey
// (Z85PublicKey strong type from C2; NOT a string).
static ZmqQueue ZmqQueue::pull_from(
    std::string_view endpoint,
    Z85PublicKey     server_pubkey,
    std::string_view identity_key_name = "role_identity");
```

Factory body calls `key_store().with_seckey(identity_key_name, cb)` to apply the secret half to the socket and `key_store().pubkey(identity_key_name)` for the public half — see the producer-side ZmqQueue example in §8.2.  No `CurveKeypair` parameter on the public API. No `ZmqAuthOptions` struct (it dies in C2). Wrong / missing key name → loud `std::out_of_range` from KeyStore inside `with_seckey` / `pubkey`.

### 8.5 Lookup failures are loud — HB-1 closure

Reading a key before `key_store().add_identity(...)` has been called OR before the name is registered throws `std::out_of_range`. There is no path by which `auth_client_pubkey()` returns `""`. There is no silent-fallback hook surviving in any layer. HB-1's "build_tx_queue sees empty keys" mode is structurally impossible.

### 8.6 What this design eliminates

- `client_pubkey / client_seckey` fields in HubConfig::AuthConfig + RoleConfig::AuthConfig
- `server_secret_key / server_public_key` in BrokerService::Config
- `client_pubkey / client_seckey` in BrokerRequestComm::Config
- `auth_client_pubkey_ / auth_client_seckey_` strings in RoleAPIBase::Impl
- `my_pubkey_z85 / my_seckey_z85` in ZmqAuthOptions (and ZmqAuthOptions itself in C2)
- `RoleAPIBase::set_auth()` setter (and its three call sites in producer/consumer/processor role hosts)
- `RoleAPIBase::auth_client_seckey()` and `HubAPI::auth_client_seckey()` accessors — never added under the round-5 use-not-export design; seckey access is only via `key_store().with_seckey(name, cb)` at the libzmq use site
- `RoleAPIBase::auth_client_pubkey()` and `HubAPI::auth_client_pubkey()` accessors — DEFAULT delete; add back only as a thin `std::string_view` delegation to `key_store().pubkey(name)` if a script-binding caller surfaces
- `CurveKeypair::empty()` method on the struct — its only purpose was as a "did we set auth?" probe, which has no meaning in a world where keys exist only inside KeyStore-owned LockedKey storage (zero callers in current codebase, verified 2026-06-05)
- Every `.empty()` check on a pubkey/seckey field that produced silent fallback behavior
- The `if constexpr (requires { config_.auth(); })` workaround in `engine_host.cpp:143` — never needed under this design
- ANY path that materializes the seckey as a `std::string` outside the LockedKey region — round-5 use-not-export design guarantees zero unlocked seckey copies in pylabhub source (libzmq's internal copy after `socket.set` remains out-of-scope per §12)

---

## 9. Sibling HEP integration

### 9.1 HEP-CORE-0035 §4.7

§4.7 keeps:
- §4.7.1 threat model
- §4.7.2 three-measure summary (lock pages, disable dumps, zero plaintext)
- §4.7.5 integration-points table — REWRITTEN to cite HEP-0040 API names (LockedKey, KeyStore)
- §4.7.6 ordering with §4.6
- §4.7.7 limitations

§4.7 drops:
- §4.7.3 cross-platform mechanism table → moved here as §7.
- §4.7.4 `SecureKeyBuffer` utility sketch → split: the RAII allocator becomes `LockedKey` (§6) for KeyStore-owned long-lived secrets, plus a small `SecureBuffer<N>` stack wrapper (§6) for short-lived intermediates (e.g. `load_keypair`'s pack buffer).
- §4.7.4 `disable_core_dumps()` free function → absorbed into `SecureMemorySubsystem` startup (§4.2). Not a free function under HEP-0040; lives inside the registered module's startup so failures are visible as "subsystem failed to start" rather than as a silent return from an early-`main()` helper.

§4.7 closes with: "Implementation is the LockedKey + KeyStore + SecureMemorySubsystem stack defined in HEP-CORE-0040."

### 9.2 HEP-CORE-0038 / task #106

HEP-0038 script-vault keystore consumes the same KeyStore primitive. `api.vault_save(name, bytes)` translates to `keystore_.add("vault:" + name, bytes)`. `api.vault_load(name)` returns a read-only view backed by the LockedKey. No second locked-memory implementation; HEP-0038 cites HEP-0040.

### 9.3 HEP-CORE-0001 + HEP-CORE-0031

SecureMemorySubsystem + KeyStore follow the HEP-0001 LifecycleModule contract (Constructed → Running → Stopping → ShutDown). KeyStore's dynamic auto-registration mirrors the HEP-0031 ThreadManager pattern (`"ThreadManager:<scope>:<uid>"`).

---

## 10. Implementation plan (mirror of task chain)

| Step | Task | Scope |
|---|---|---|
| 1 | #165 (this draft) | Draft HEP in tech_draft. ✅ |
| 2 | #166 | Fresh-eye review. ✅ (Five rounds — caught threaded-ctor slippage, LifecycleManager-instance-retrieval gap, vault plaintext retention, lifecycle/class-API conflation, and the second-copy residue from a byte-export API.  Final design: use-not-export for seckeys + canonical pImpl + guarded global accessor.) |
| 3 | #167 | Promote tech_draft → docs/HEP/. Archive draft per DOC_STRUCTURE §1.8. ✅ |
| 4 | #168 | HEP-0035 §4.7 rewrite — keep threat model + ordering; replace utility-spec body with one-line ref to HEP-0040. ✅ |
| 5 | #169 | SecureMemorySubsystem class (pImpl) — process singleton; ctor registers `"SecureMemory"` (Logger dep); ctor runs §4.2 startup; guarded global accessor.  ✅ shipped. |
| 6 | #170 | KeyStore class (pImpl) + LockedKey RAII + SecureBuffer<N>.  Process singleton; ctor registers `"KeyStore"` (deps: SecureMemory + Logger).  **Round-5 use-not-export API**: `add_identity` / `add_raw` / `remove` (writes), `pubkey` returning `std::string_view`, `with_seckey(name, cb)` callback-scoped seckey access, `lookup_raw` for HEP-0038, `has`, `size`.  No `lookup() → CurveKeypair&`.  Guarded global accessor `key_store()`. |
| 7 | #175 | Vault hardening: add `vault_read_secure(...)` writing through a span; drop HubVault / RoleVault long-lived plaintext `std::string` members. Prerequisite for #171. |
| 8 | #171 | DROP keypair fields from HubConfig::AuthConfig + RoleConfig::AuthConfig entirely; load_keypair uses SecureBuffer + vault_read_secure → `key_store().add_identity(name, span)`. |
| 9 | #172 | DROP keypair fields from BrokerService::Config + BRC::Config entirely; bind/connect call `key_store().with_seckey(name, cb)` + `key_store().pubkey(name)` directly at the libzmq socket-option site. |
| 10 | #173 | RoleAPIBase: DELETE `auth_client_seckey()` and `auth_client_pubkey()` (no legitimate caller).  DELETE `set_auth()` + the three role-host call sites.  DELETE `Impl::auth_client_pubkey_/_seckey_` strings.  DELETE `CurveKeypair::empty()`.  Sites that read these accessors migrate to `key_store().with_seckey` / `pubkey` at the actual use point.  Ctor unchanged. |
| 11 | #174 | HubAPI: do NOT add `auth_client_pubkey()` / `auth_client_seckey()` accessors (round-5 design — neither needed).  No code change; this task closes as "no-op confirmed by design" once #170+#172 land. |
| 12 | #102 close | After 4–11 land, close #102 (utility-only §4.7 plan fully absorbed). |
| 13 | C-chain resume | After #173 lands, resume C1 (#157 strict validator deletes 5 empty-skip conditionals + silent-ignore) → C2 (#158 Z85PublicKey strong type for non-keypair pubkey fields) → C4 (#160 delete legacy factories + ZmqAuthOptions + migrate 103 test sites) → C5 (#161 CURVE-engagement assertions). |
| 14 | HB chain | HB-2 (#162 producer-side ZAP pump on BRC poll thread) → HB-3 (folds into A3 / #103 D4 wiring) → HB-4+5 (#163 Authorized state + data-loop guard) → HB-6 (#164 random shm_secret). |

---

## 11. Open questions (resolve during fresh-eye review #166)

Resolved during the §8 rewrite — kept here as a record so the next reviewer sees what was considered and dropped:

- ~~§8.2 accessor-pattern inconsistency~~ — **resolved.** Both accessors now query `key_store().lookup(name)` directly; no host backref or ctor-ref carried. RoleAPIBase + HubAPI have identical impls.
- ~~§5.2 add signature one-span vs two-span~~ — **resolved.** KeyStore exposes `add_identity(name, packed_pub_sec_span)` for identity keys and `add_raw(name, span)` for HEP-0038 scripted secrets. Different entry points; same LockedKey storage underneath.
- ~~Move-assignment of `AuthConfig`~~ — **moot.** AuthConfig has no keypair fields under §8.1; only `keyfile` (plain string) remains. Move semantics unchanged from today.

Still open (carry to #167 promote):

- **Should KeyStore key names be a strong type** (e.g. `KeyName` wrapping a string with a charset invariant + reserved-prefix check for `"hub_identity"` / `"role_identity"` / `"vault:"`) rather than raw `std::string`? HEP-0038 scripts pass strings; framework callers use literals. Lean: typed wrapper at the framework API (key_store().lookup(KeyName)), with implicit construction from `std::string_view` to keep call sites readable. Reject names starting with reserved prefixes when added via HEP-0038 (script-side check inside `api.vault_save`, not inside KeyStore — keeps KeyStore policy-free).
- **Should `SecureMemorySubsystem::shutdown_()` log "core dumps remain disabled until process exit"** as a one-line audit trail? Lean yes — operators reviewing shutdown logs should see the floor stayed in force.
- **`key_store().add_identity` / `add_raw` failure mode when sodium_malloc fails** — `std::runtime_error` with a clear message is fine; no typed error needed for the broker error taxonomy.

Resolved during the §8 + §5 rewrite — kept here as a record so the next reviewer sees what was considered and dropped:

- ~~§8.2 accessor-pattern inconsistency~~ — **resolved.** Both accessors query `key_store().lookup(name)` directly; no host backref or ctor-ref carried. RoleAPIBase + HubAPI have identical impls.
- ~~§5.2 add signature one-span vs two-span~~ — **resolved.** §5.2 declares `add_identity(name, packed_pub_sec_span)` for identity keys and `add_raw(name, span)` for HEP-0038 scripted secrets. Different entry points; same LockedKey storage underneath.
- ~~Move-assignment of `AuthConfig`~~ — **moot.** AuthConfig has no keypair fields under §8.1; only `keyfile` (plain string) remains. Move semantics unchanged from today.
- ~~Thread safety of `key_store().lookup`~~ — **resolved in §5.5.** Shared mutex inside KeyStore::Impl, reader-preferring. lookup takes shared lock; add_identity/add_raw/remove take exclusive lock. LockedKey storage immutable after add — reference reads need no further locking. Removed bytes can UAF if caller holds reference past `remove`; mitigation is by-convention short-lived references.

Additional findings from review round 3 (2026-06-05 PM, against actual code):

- ~~Consumer access via `LifecycleManager::instance().get_module(...)`~~ — **resolved as design correction (not a question).** Verified against `src/include/utils/lifecycle.hpp:255-312`: that API does not exist. LifecycleManager exposes only `register_module / register_dynamic_module / load_module / unload_module / dynamic_module_state(name) → enum`, no `Module*` retrieval. Initial fix (round 3) used "singleton-by-construction" with a static atomic instance pointer — invented terminology. Round-4 correction (after user push-back + reading HEP-CORE-0031 §3 ThreadManager pattern): consumer access is the class's OWN concern, separate from lifecycle. Final design uses a standard guarded global accessor function in the security namespace (`key_store()` / `secure_memory_subsystem()`) — file-scope `std::mutex` + raw pointer set by ctor / cleared by dtor — and the class itself uses canonical pImpl per `IMPLEMENTATION_GUIDANCE.md §"pImpl Idiom"`. Lifecycle module registration enforces ORDERING ONLY. Module names `"KeyStore"` / `"SecureMemory"` (process singletons).
- ~~vault flow returns a `std::string`, not a span~~ — **resolved as design correction.** Verified against `vault_crypto.hpp:91` (`vault_read` returns `std::string`) + `hub_config.cpp:268-269` (today copies vault's cached `std::string` members into AuthConfig). The locked-memory storage is meaningless if HubVault/RoleVault retain plaintext `std::string` members for the process lifetime. Added new task #175 (vault hardening) — adds `vault_read_secure(...)` that writes through a span, drops vault long-lived plaintext members. #171 blocked on #175.
- ~~`disable_core_dumps()` free function placement~~ — **resolved.** Moved from §9.1's "lives in LockedKey" wording to "absorbs into SecureMemorySubsystem startup" — it's a process-init concern, not a per-allocation concern. §4.2 step 1 is now the canonical location.

Additional findings from review round 5 (2026-06-06, auto-code-review after #170 first build):

- ~~`lookup(name) → const CurveKeypair&` byte-export API~~ — **resolved as design refinement.** The first shipped #170 impl exposed identity keys via `lookup()` returning a `const CurveKeypair&`.  Because `CurveKeypair` holds `std::string` members and `lookup` must return a stable reference, the impl had to cache a `CurveKeypair view` field in `KeyStore::Impl::Entry` containing heap-allocated copies of pub_z85 + sec_z85 — second-copy / unlocked-plaintext for every identity key, persisting for the KeyStore's lifetime.  The auto code-review pass caught this as a CRITICAL design violation; user surfaced the right boundary ("the security module should provide on-site validation or something to avoid passing keys around").  Replaced with use-not-export: `pubkey(name) → std::string_view` (non-secret, view into LockedKey bytes) + `with_seckey(name, cb)` (callback-scoped seckey access; bytes never copied out).  RoleAPIBase + HubAPI `auth_client_seckey()` accessors are deleted entirely under this design — no legitimate caller needs the seckey as data; the only existing use sites (BRC connect, broker bind, ZmqQueue factory) call `with_seckey` directly at the libzmq socket-option point.  Zero unlocked seckey copies in pylabhub source after #170+#172 land (libzmq internal copy remains out-of-scope per §12).
- ~~validator-thunk `userdata` cast discarded~~ — **resolved minor cleanup.** Both `sms_impl_validate` (secure_memory_subsystem.cpp:76) and `ks_impl_validate` (key_store.cpp:145) cast `userdata` to `Impl*` then discarded the result, relying on the file-scope singleton pointer.  ThreadManager mirrors `impl_alive` via the userdata pointer.  Resolved by dropping the cast in both validators (`(void)userdata;`) — functionally equivalent to the singleton-pointer check that follows.
- ~~`std::shared_mutex` fairness vs §5.5 "reader-preferring" wording~~ — **resolved doc softening.** `std::shared_mutex` policy is implementation-defined; the strict "reader-preferring" requirement was not load-bearing for the expected read-dominated workload.  §5.5 updated to "implementation-defined fairness; not load-bearing for this workload."

Still open (carry to #167 promote):

- **Should KeyStore key names be a strong type** (e.g. `KeyName` wrapping a string with a charset invariant + reserved-prefix check for `"hub_identity"` / `"role_identity"` / `"vault:"`) rather than raw `std::string`? HEP-0038 scripts pass strings; framework callers use literals. Lean: typed wrapper at the framework API (key_store().lookup(KeyName)), with implicit construction from `std::string_view` to keep call sites readable. Reject names starting with reserved prefixes when added via HEP-0038 (script-side check inside `api.vault_save`, not inside KeyStore — keeps KeyStore policy-free).
- **Should `SecureMemorySubsystem::shutdown_()` log "core dumps remain disabled until process exit"** as a one-line audit trail? Lean yes — operators reviewing shutdown logs should see the floor stayed in force.
- **`key_store().add_identity` / `add_raw` failure mode when sodium_malloc fails** — `std::runtime_error` with a clear message is fine; no typed error needed for the broker error taxonomy.
- **Test fixture pattern for L2/L3** — fixtures must register a SecureMemorySubsystem + KeyStore in SetUp, populate with canonical test keys, tear down. Two patterns: (a) per-test-file fixture; (b) test-binary global env (gtest `Environment` once per binary). (b) is cheaper but means tests share state — acceptable since tests are single-threaded and tear down per-binary. Lean (b) at L2 with per-test reset of stored keys; (a) at L3 for isolation. Decide in #170 task scope.

---

## 12. Out of scope (explicit)

- **Live-memory attackers.** Same scope statement as HEP-0035 §4.7.7 / HEP-0036 §3 I8.
- **libzmq internal key copy.** After we pass a key to a CURVE-configured socket, libzmq stores it for the socket's lifetime; not under our control.
- **Hardware-backed key isolation.** HSM / TEE are deferred to future HEP if pursued.
- **General "secure memory" allocator** for non-key data. This HEP's charter is narrow — only key-class secrets. Future broader HEP may supersede if scope grows.
- **Federation peer pubkeys**. Public keys do not need locking; stored as plain values in `peers[].pubkey_z85` per HEP-0036.

---

*Draft ends. Review against §11 open questions before promoting.*
