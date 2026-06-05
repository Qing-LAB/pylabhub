# TESTING_TODO completions archive — 2026-06-05

This file preserves verbatim prose for TESTING_TODO entries that were
verified shipped in code as of 2026-06-05.  Moved here so the active
TESTING_TODO can focus on open items.  No content summarized.

Source file: `docs/todo/TESTING_TODO.md`.

---

## Wave-B M9 (#72 + #100) — RoleHostFrame role-host unification

Closed 2026-05-26.  `RoleHostFrame` plain class with shared
`setup_infrastructure_` / `teardown_infrastructure_` /
`wire_api_for_presences_` / `build_presences_`.  Legacy
`RoleHostCore::*_fz_spec_` storage retired; `FlexzoneInfoCache` on
`RoleAPIBase` carries both logical + physical sizes per side.
`presences_[i]` is the single canonical home for per-channel
schemas (single resolve via `build_presences_()`).

**Q1+Q2+Q3 quality concerns from the 2026-05-22 fresh-eye review
all resolved:**

- **Q1** (zmq_buffer_depth placement): unified `make_rx_opts` free
  function in `role_config_translation.cpp` sets the field only
  inside the `if (zmq)` branch on both sides.
- **Q2** (non-empty SchemaSpec field propagation): 3 new tests in
  `test_layer2_setup_infrastructure_translation.cpp` pass a
  2-field SchemaSpec and verify field-by-field propagation.
- **Q3** (`flexzone_checksum = config.flexzone && has_fz` AND-gate):
  same 3 new tests verify `has_*_fz=false` forces
  `opts.flexzone_checksum=false` even when `config.flexzone=true`.

New test binary: `test_layer2_engine_module_params` (9 tests)
covers `validate_fz_info_cache()` cache↔params and cache-internal
invariants.

Design doc: `docs/archive/transient-2026-06-02/role_host_template_design.md`.

---

## N1 (#83) — config→opts translation L2 round-trip test

2026-05-22.  Closed the systemic gap that B5 + B11 came from.
Translation originally extracted into testable static methods
(`ProducerRoleHost::make_tx_opts` etc.); M9 collapsed those into
shared free functions in `scripting::make_{tx,rx}_opts` while
preserving the per-role wrappers for test stability.

L2 test
`tests/test_layer2_service/test_setup_infrastructure_translation.cpp`
exercises the full production round-trip:
`RoleDirectory::init_directory` → on-disk JSON → user edit →
`RoleConfig::load_from_directory` → `make_*_opts` → assert all
fields.  Mutation-sweep verified.  6 tests originally; 9 after M9's
Q2+Q3 additions.

---

## Demo framework (closes harness task #44) — moved to test infrastructure inventory

The demo framework prose is intentionally retained in the active
TESTING_TODO under "Test infrastructure inventory" because it's the
canonical pointer to the L4 reference for new pipeline scenarios.
This archive note records only that task #44 itself ("L4 processor +
consumer test infrastructure (Wave-D)") is closed; the inventory
section in the active file continues to describe what's available.
