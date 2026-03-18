# HEP-CORE-0026: Centralized Version Registry

| Field | Value |
|-------|-------|
| **HEP** | CORE-0026 |
| **Title** | Centralized Version Registry |
| **Status** | Implementing |
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

    // Shared memory header layout (bump when SharedMemoryHeader struct changes)
    uint8_t shm_major;
    uint8_t shm_minor;

    // Control-plane wire protocol (REG_REQ, DISC_ACK, etc.)
    uint8_t wire_major;
    uint8_t wire_minor;

    // Python/Lua API surface version
    uint8_t script_api_major;
    uint8_t script_api_minor;

    // Messaging facade sizeof — ABI canary (not major/minor; caught at compile time)
    uint16_t facade_producer_size;
    uint16_t facade_consumer_size;
};

} // namespace pylabhub::version
```

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
| Wire protocol | 1 | 0 | Current REG_REQ/DISC_ACK field set is v1.0 |
| Script API | 1 | 0 | Current Python/Lua API surface is v1.0 |
| Facade sizes | sizeof values | — | No major/minor; compile-time `static_assert` catches drift |

Library version comes from CMake (`project(pylabhub VERSION X.Y)`).

### §2.4 Query API

```cpp
namespace pylabhub::version
{

/// Returns the compile-time component version constants.
PYLABHUB_UTILS_EXPORT ComponentVersions current() noexcept;

/// Human-readable one-liner for logging.
/// Example: "pylabhub 0.1.42 (shm=1.0, wire=1.0, script=1.0, facade=64/48)"
PYLABHUB_UTILS_EXPORT std::string version_info_string();

/// JSON object for script consumption.
/// {"library":"0.1.42", "shm":"1.0", "wire":"1.0", "script_api":"1.0",
///  "facade_producer":64, "facade_consumer":48}
PYLABHUB_UTILS_EXPORT std::string version_info_json();

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
| Broker handshake | Exchange `wire_major.wire_minor` in REG_REQ | Future (HEP-0023) |

### §2.7 Bump Policy

**Who bumps what:**

| When you change... | Bump... |
|-------------------|---------|
| `SharedMemoryHeader` struct fields or slot layout | `shm_major` (breaking) or `shm_minor` (additive) |
| REG_REQ / DISC_ACK / any control message fields | `wire_major` or `wire_minor` |
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
