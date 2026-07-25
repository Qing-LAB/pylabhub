# HEP-CORE-0026: Centralized Version Registry

| Field | Value |
|-------|-------|
| **HEP** | CORE-0026 |
| **Title** | Centralized Version Registry |
| **Status** | Implemented |
| **Created** | 2026-03-17 |
| **Motivation** | Version information is scattered across the codebase. No unified query mechanism exists for scripts, operators, or connecting peers to discover what component versions a build supports. |

---

## §1 Problem Statement

pyLabHub has multiple independently-versioned components:

| Component | Current location | Queryable? |
|-----------|-----------------|------------|
| Library (pylabhub-utils) | `plh_platform.hpp` — CMake-injected `PYLABHUB_VERSION_{MAJOR,MINOR,ROLLING}` | C++ only |
| SHM layout | `data_block.hpp` — `HEADER_VERSION_MAJOR/MINOR` (hardcoded) | At attach time only |
| Wire protocol | Not versioned | No |
| Messaging facade ABI | `static_assert(sizeof)` — compile-time canary | No runtime query |
| Script API surface | Not versioned | No |

There is no single place to ask "what versions does this build support?" and no way for a
script to check compatibility at runtime. Validation at connection boundaries (SHM attach,
broker handshake) is ad hoc.

---

## §2 Design

### §2.1 ComponentVersions struct

A plain aggregate of compile-time constants, living in a new Layer 0 header
`plh_version_registry.hpp`:

```cpp
namespace pylabhub::version
{

struct ComponentVersions
{
    // Library identity (from CMake project() VERSION + git rev-list --count)
    uint16_t library_major;
    uint16_t library_minor;
    uint16_t library_rolling;

    // Shared-memory header layout (mirrors data_block.hpp HEADER_VERSION_*)
    uint8_t shm_major;
    uint8_t shm_minor;

    // Broker control-plane protocol (renamed from "wire" 2026-04-22;
    // the BrokerRequestComm ↔ BrokerService message set, HEP-CORE-0007)
    uint8_t broker_proto_major;
    uint8_t broker_proto_minor;

    // ZMQ data-plane envelope (ZmqQueue + InboxQueue shared msgpack
    // 5-tuple [magic, schema_tag, seq, payload, checksum])
    uint8_t zmq_frame_major;
    uint8_t zmq_frame_minor;

    // Python/Lua script API surface
    uint8_t script_api_major;
    uint8_t script_api_minor;

    // ScriptEngine C++ virtual-interface version (Lua/Python/Native)
    uint8_t script_engine_major;
    uint8_t script_engine_minor;

    // JSON config file schema (allowed-keys sets across the config parsers)
    uint8_t config_major;
    uint8_t config_minor;
};

} // namespace pylabhub::version
```

Seven axes, each independently versioned.  The pre-implementation
`facade_producer_size` / `facade_consumer_size` canaries were NOT
adopted — facade-size drift is caught by compile-time static_asserts,
not by wire-carried fields.

### §2.2 Version Semantics

Each component follows **major.minor** semantic versioning:

| Change type | Version bump | Compatibility |
|-------------|-------------|---------------|
| Breaking (removed/renamed field, changed semantics, incompatible layout) | **Major** | Old peer **cannot** interoperate |
| Additive (new method, new optional field, new enum value) | **Minor** | Old peer can still interoperate — just doesn't know about the new feature |

**Validation rule at boundaries:**
- `peer.major != local.major` → **reject** (incompatible)
- `peer.minor != local.minor` → **log warning**, proceed (feature gap)

### §2.3 Initial Version Values

| Component | Major | Minor | Rationale |
|-----------|-------|-------|-----------|
| SHM layout | 1 | 0 | Matches existing `HEADER_VERSION_MAJOR/MINOR` in `data_block.hpp` |
| Broker protocol (then named "wire") | 1 | 0 | REG_REQ/DISC_ACK field set at adoption |
| Script API | 1 | 0 | Current Python/Lua API surface is v1.0 |
| Facade sizes | — | — | NOT adopted as registry axes; compile-time `static_assert` catches drift |

Library version comes from CMake (`project(pylabhub VERSION X.Y)`).

### §2.4 Query API

Two families ship in `plh_version_registry.hpp`: identity queries and
the runtime ABI-check surface (the latter is what HEP-CORE-0032 §8
builds on).

```cpp
namespace pylabhub::version
{

// ── Identity queries ─────────────────────────────────────────────────

/// Compile-time component version constants (the 7-axis struct above).
PYLABHUB_UTILS_EXPORT ComponentVersions current() noexcept;

/// Git build id (nullptr when not compiled in; present iff
/// PYLABHUB_HAVE_BUILD_ID).
PYLABHUB_UTILS_EXPORT const char *build_id() noexcept;

/// PEP 440 release string.
PYLABHUB_UTILS_EXPORT const char *release_version() noexcept;

/// Embedded Python runtime version (display).
PYLABHUB_UTILS_EXPORT const char *python_runtime_version() noexcept;

/// Human-readable one-liner for logging.
PYLABHUB_UTILS_EXPORT std::string version_info_string();

/// Display-oriented JSON (release + dotted library + axis pairs).
/// NOTE: this is NOT the wire shape — the wire uses to_json_object()
/// below (HEP-CORE-0032 §8.2).
PYLABHUB_UTILS_EXPORT std::string version_info_json();

// ── Runtime ABI-check surface (HEP-CORE-0032 §8) ─────────────────────

/// Compare an expected snapshot against this process (drift detection).
PYLABHUB_UTILS_EXPORT AbiCheckResult
check_abi(const ComponentVersions &expected,
          const char *expected_build_id = nullptr) noexcept;

/// Compare a PEER's advertised versions against this process.
PYLABHUB_UTILS_EXPORT AbiCheckResult
verify_peer_versions(const ComponentVersions &peer_versions,
                     const char *peer_build_id = nullptr) noexcept;

/// Fold an AbiCheckResult into the §8.5/§8.6 operator verdict
/// (Ok / BuildOnly / MinorMismatch / MajorMismatch + axis lists).
PYLABHUB_UTILS_EXPORT AbiPeerVerdict
classify_peer_verdict(const AbiCheckResult &verdict);

/// The WIRE codec for `abi_fingerprint` (HEP-CORE-0032 §8.2):
/// per-axis integer fields; build_id travels as a sibling field.
PYLABHUB_UTILS_EXPORT nlohmann::json to_json_object(const ComponentVersions &v);
PYLABHUB_UTILS_EXPORT ComponentVersions from_json_object(const nlohmann::json &j);

} // namespace pylabhub::version
```

### §2.5 Script Exposure

- **Python** (`pylabhub_module.cpp`): `pylabhub.version_info()` → returns the JSON string
- **Lua** (via `RoleHostCore` or direct): `api.version_info()` → returns the JSON string

Both engines get the same output from the same C++ function.

### §2.6 Validation Integration Points

| Boundary | Check | Action on mismatch |
|----------|-------|-------------------|
| SHM attach (DataBlock constructor) | `header.version_major != current().shm_major` | Throw (existing behavior) |
| SHM attach | `header.version_minor > current().shm_minor` | LOGGER_WARN (new) |
| Startup | Log `version_info_string()` at INFO level | Informational |
| Broker handshake | Peer exchanges the full 7-axis `abi_fingerprint` on REG_REQ / CONSUMER_REG_REQ, verified via `verify_peer_versions` + `classify_peer_verdict` | Shipped (HEP-CORE-0032 §8) |

### §2.7 Bump Policy

**Who bumps what:**

| When you change... | Bump... |
|-------------------|---------|
| `SharedMemoryHeader` struct fields or slot layout | `shm_major` (breaking) or `shm_minor` (additive) |
| REG_REQ / DISC_ACK / any control message fields | `broker_proto_major` or `broker_proto_minor` |
| pybind11 or Lua `api.*` methods (remove/rename) | `script_api_major` |
| pybind11 or Lua `api.*` methods (add new) | `script_api_minor` |
| `ProducerMessagingFacade` / `ConsumerMessagingFacade` layout | `static_assert` will fail — update size constant |

**Process**: Update the constants in `plh_version_registry.hpp` as part of the PR that
makes the breaking/additive change. The PR review checklist should include "version bump?".

---

## §3 File Layout

| File | Purpose |
|------|---------|
| `src/include/plh_version_registry.hpp` | Public header — struct + API declarations |
| `src/utils/core/version_registry.cpp` | Implementation — `current()`, `version_info_string()`, `version_info_json()` |

The header is Layer 0 (no dependencies beyond `plh_platform.hpp`). The `.cpp` links
against `pylabhub-utils` and reads the CMake-injected version macros.

---

## §4 Relationship to Other HEPs

- **HEP-0002** (DataHub): SHM version constants are the authoritative source; this HEP
  re-exports them in a unified struct.
- **HEP-0007** (Control Plane Protocol): Wire protocol version defined here; future
  handshake extension in HEP-0023 will use `wire_major/minor`.
- **HEP-0011** (ScriptHost): Script API version tracks the pybind11/Lua API surface.
- **HEP-0017** (Pipeline Architecture): Facade sizes tracked as ABI canaries.
