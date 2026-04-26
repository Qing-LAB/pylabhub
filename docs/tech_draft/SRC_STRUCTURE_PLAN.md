# `src/` and `src/include/` Restructure Plan (deferred)

**Status**: 🔵 Design draft — no execution scheduled yet.
**Created**: 2026-04-21
**Triggered by**: HEP-0024 Phase 20 (per-role binary deletion) cleanup discussion.
**Blocks on**: nothing structural; can land any time builds are otherwise quiet.

This document captures the agreed-upon target shape for `src/` and
`src/include/` so that when execution starts we don't re-litigate the
design.  No file moves have happened — current tree is unchanged.

---

## 1. Philosophy

- **`pylabhub-utils` (shared library) is the primary product.** Keep as
  much code there as the library can responsibly hold.
- **Binaries are thin consumers** of the library — they wire CLI parsing
  and lifecycle to library facilities, no domain logic of their own.
- Subdirectory organization happens **inside** `src/utils/` (and
  `src/scripting/`).  The top-level `src/` should be flat, with binary
  `main.cpp` files sitting next to the library subdirs.
- A header is **public** iff it is reachable (transitively) from one of
  the `plh_*.hpp` umbrella headers.  Public headers live under
  `src/include/utils/<subdir>/`; internal headers live next to their
  `.cpp` in `src/utils/<subdir>/` (or, rarely, under
  `src/include/utils/<subdir>/internal/` when shared across translation
  units inside the same subdir but not external API).

---

## 2. Target `src/` layout

```
src/
├── CMakeLists.txt
├── plh_role_main.cpp          # binary main, flat
├── plh_hub_main.cpp           # future — binary main, flat
├── hubshell.cpp               # parked legacy binary (until hub refactor consumes it)
├── hub_python/                # parked legacy hubshell support (likewise)
├── utils/
│   ├── CMakeLists.txt
│   ├── basic/                 # was `core/` — see §4.1 rename
│   ├── ipc/
│   ├── logging/
│   ├── service/
│   ├── shm/
│   ├── hub/                   # broker-side library code
│   ├── roles/                 # role-side library code (role_host, init, fields)
│   ├── config/
│   ├── schema/
│   └── network_comm/
├── scripting/
│   ├── CMakeLists.txt
│   ├── roles/                 # role pybind11 API (producer_api, consumer_api, processor_api)
│   ├── hub/                   # future — hub pybind11 API
│   └── (engine_factory, python_engine, lua_engine, ...)
└── include/
    └── utils/
        └── (mirror of src/utils/ subdir layout — see §3)
```

**No more top-level wrapper dirs**: `src/producer/`, `src/consumer/`,
`src/processor/`, `src/plh_role/`, `src/roles/`, `src/hub/` (other than
the parked-legacy `hub_python/`) all gone.

---

## 3. Target `src/include/utils/` layout

Mirror `src/utils/`'s subdir structure for the public headers:

```
src/include/utils/
├── basic/         # debug_info, format_tools, recursion_guard, scope_guard,
│                  # platform, uuid_utils, version_registry
├── ipc/           # broker_service, broker_request_comm, channel_*,
│                  # heartbeat_manager, hub_config (the broker-side one)
├── logging/       # logger, logger_sinks/*
├── service/       # lifecycle, file_lock, crypto_utils, hub_vault,
│                  # engine_module_params, startup_config, startup_wait
├── shm/           # data_block, data_block_*, integrity_validator,
│                  # shared_memory_spinlock, slot_*, zone_ref
├── hub/           # hub_queue, hub_shm_queue, hub_zmq_queue, hub_inbox_queue
├── config/        # (already exists) auth_config, identity_config, ...
├── schema/        # schema_registry, schema_types, schema_utils
├── network_comm/  # net_address, zmq_context, zmq_poll_loop
└── roles/         # role_api_base, role_cli, role_directory,
                   # role_host_base, role_host_core
```

Each header is reviewed during the move for public-vs-internal:
- **Public** → moves under `src/include/utils/<subdir>/`.
- **Internal** → moves to `src/utils/<subdir>/` (next to its `.cpp`).
- Includes throughout the tree update to use the new paths
  (`<utils/<subdir>/foo.hpp>`).

---

## 4. Concrete moves (sequenced — see §6 for phasing)

### 4.1 Rename `src/utils/core/` → `src/utils/basic/`

`src/utils/core/` currently holds 6 files (debug_info.cpp,
format_tools.cpp, interactive_signal_handler.cpp, platform.cpp,
uuid_utils.cpp, version_registry.cpp).  These are foundational
utilities, not "core" in any architectural sense.  `basic/` reads
better and matches the layered umbrella vocabulary
(`plh_base.hpp` is Layer 1).

- `git mv src/utils/core src/utils/basic`
- Update path references in `src/utils/CMakeLists.txt`.

### 4.2 Move role library code into `src/utils/roles/`

Files to move out of `src/{producer,consumer,processor}/` into
`src/utils/roles/` (flat, prefix-disambiguated):

| from | to | compiled into |
|---|---|---|
| `src/producer/producer_role_host.{cpp,hpp}` | `src/utils/roles/` | `pylabhub-utils` |
| `src/producer/producer_init.{cpp,hpp}` | `src/utils/roles/` | `pylabhub-utils` |
| `src/producer/producer_fields.hpp` | `src/utils/roles/` | header |
| `src/consumer/consumer_*` (5 files) | `src/utils/roles/` | same pattern |
| `src/processor/processor_*` (5 files) | `src/utils/roles/` | same pattern |

Then `rmdir src/{producer,consumer,processor}`.

### 4.3 Move role pybind11 API into `src/scripting/roles/`

| from | to | compiled into |
|---|---|---|
| `src/producer/producer_api.{cpp,hpp}` | `src/scripting/roles/` | `pylabhub-scripting` |
| `src/consumer/consumer_api.{cpp,hpp}` | `src/scripting/roles/` | `pylabhub-scripting` |
| `src/processor/processor_api.{cpp,hpp}` | `src/scripting/roles/` | `pylabhub-scripting` |

(Listed in 4.2 too — order moves carefully so the `_api.*` files end up
in `scripting/roles/`, not `utils/roles/`.)

### 4.4 Flatten `plh_role` binary

- `git mv src/plh_role/plh_role_main.cpp src/plh_role_main.cpp`
- Move the `add_executable(plh_role ...)` block out of
  `src/plh_role/CMakeLists.txt` into `src/CMakeLists.txt`.
- `rmdir src/plh_role`.
- Update include dirs: drop `../{producer,consumer,processor}` → these
  headers are now `<utils/roles/...>` (Phase B include reorg) or stay
  in current paths if Phase A only.

### 4.5 Future hub-binary parallel

When the hub refactor lands:
- `src/plh_hub_main.cpp` flat under `src/` (or `src/plh_hub/main.cpp`,
  mirroring `src/plh_role/`).
- Hub library code under `src/utils/hub/` (already exists; absorbs
  `BrokerService`, `Messenger`, etc. as the abstraction matures).
- Hub pybind11 API under `src/scripting/hub/`.
- Legacy `src/hubshell.cpp` and `src/hub_python/` already deleted in the
  post-G2 cleanup pass — `plh_hub` lands as greenfield.

---

## 5. Umbrella header cleanup criteria

Current umbrellas: `plh_platform.hpp` (L0), `plh_base.hpp` (L1),
`plh_service.hpp` (L2), `plh_datahub_client.hpp` (L3a),
`plh_datahub.hpp` (L3b), `plh_version_registry.hpp`.

Cleanup pass for each umbrella:
1. Each `#include` listed in the umbrella defines what is **public** at
   that layer.
2. If a header in `src/include/utils/` is NOT included by any umbrella,
   either:
   - It is internal — move it to `src/utils/<subdir>/` next to its `.cpp`.
   - It is intentionally public-but-not-umbrellaed (rare; document why
     in a one-line comment in the file).
3. Trim any include in an umbrella that references a header now
   demoted to internal.
4. Verify by building external consumers (tests are the proxy) using
   only umbrella includes — anything that breaks signals a missing
   public header that needs adding back.

---

## 6. Phasing (each phase ends with a green build + green ctest)

- **Phase A — Mechanical moves + rename.** Sections §4.1–§4.4 (and
  later §4.5).  Pure file moves + CMake path updates; no API change.
  Highest churn but lowest risk per change.
- **Phase B — Include reorganization.** Section §3.  Moves headers
  into mirrored subdirs; updates `#include` statements across the
  tree (sources, tests, examples).  Use `clang-tidy --fix` or scripted
  `sed` for the include rewrites.
- **Phase C — Umbrella + public/internal audit.** Section §5.
  Highest cognitive cost; per-header decisions.  Land in small
  passes (one umbrella at a time), each with a green build/test.

Each phase is a separately landed set of commits.  Don't bundle
phases — they're easier to review and revert independently.

---

## 7. Out-of-scope for this plan

- **Hub binary refactor itself** — separate work; this plan only
  prepares the directory shape so the hub refactor lands cleanly.
- **Renaming `pylabhub-utils` or any library target** — keep target
  names stable; only directory shapes change.
- **Doc rewrites referencing old paths** — sweep happens after
  Phase A so the new paths are the ones documented.

---

## 8. Cross-references to update during execution

- HEP-CORE-0024 (role binary unification) — references `src/producer/`,
  `src/consumer/`, `src/processor/`, `src/plh_role/`.
- `README_Deployment.md` — code-layout sections.
- Any HEP that documents source-tree layout.
- `MEMORY.md` and per-topic memory files that mention paths.
