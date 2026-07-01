# Core Structure Change Protocol walk — #275 S5

**Date drafted:** 2026-06-27
**Status:** 🟡 PRE-WALK CHECKLIST (S5 not yet implemented; this doc is
the pre-flight done at Phase 0b time so S5 lands as a single in-and-out
commit when the chain reaches it)
**Tracker:** #275 S5 — rename `SharedMemoryHeader::shared_secret[64]`
→ `reserved_capability_token[64]`
**Sequencing:** Lands AFTER #275 S2 (L3 worker migration off legacy
`find_datablock_consumer_impl(name, secret, ...)` C API) + #275 S3
(role-layer secret machinery deletion: `set_shm_secret`, `shm_shared_secret`
options, `create_writer`/`create_reader` overloads) + #275 S4 (C API
secret parameter deletion).
**Protocol source:** `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure
Change Protocol" (line 1386).

---

## Why this walk exists

The protocol at IMPLEMENTATION_GUIDANCE.md:1386 mandates an impact-matrix
review BEFORE any modification to `SharedMemoryHeader`.  The matrix
specifies 9 review areas; this doc walks each against the actual code
at HEAD (`dfe86a61`) so S5 can ship as a single-pass commit.

S5 is mechanically simple but structurally load-bearing:
- 64-byte array rename (preserves layout)
- Schema-fields macro update (schema hash bumps due to field name)
- Initialization line removal (`data_block.cpp:542`)

Every other site that references the field has already been removed by
S3+S4.  S5 is the final byte-rename pass.

---

## Impact matrix walk

Per IMPLEMENTATION_GUIDANCE.md table at line 1396-1408:

### 1. Size & Alignment (MANDATORY)

**Requirement:** `static_assert` at 4096 bytes.
**Verification:** Field is `char shared_secret[64]`.  Rename to
`char reserved_capability_token[64]` is byte-equivalent — same type,
same size, same alignment.  Header total stays at 4096 bytes; the
`static_assert(sizeof(SharedMemoryHeader) == 4096)` continues to pass.
**Action at S5 ship:** none required beyond the rename itself.  Re-run
the static_assert via a sanity build before commit.

### 2. Schema Macro (MANDATORY)

**Requirement:** Update `PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS`.
**Site to update:** `src/include/utils/hub/shared_memory_header.hpp`
(macro location — verify before edit).
**Action at S5 ship:**
- Locate the schema-fields macro row that names the `shared_secret`
  field.
- Rename the row to `reserved_capability_token`.
- Schema hash WILL bump because field name is part of the hash input.
  This is intentional — it forces incompatible memory layouts to fail
  the schema-mismatch check rather than silently match.

### 3. Constructor/Initialization (MANDATORY)

**Requirement:** `DataBlock::DataBlock()` in `data_block.cpp`.
**Site to update:** `data_block.cpp:542` (line cited in AUTH_TODO
S5 scope; verify at S5 ship).
**Current shape (at HEAD):** the constructor initializes
`shared_secret` from `config.shared_secret`.  Per S4 (lands before S5),
`config.shared_secret` is gone — the constructor either no-longer-
initializes the field (zero-by-default for trivially constructible
`char[64]`), or initializes it to a known sentinel.
**Action at S5 ship:**
- Delete the `data_block.cpp:542` initialization line entirely.
- The field is "reserved" — zero-initialized by the trivial ctor or
  by explicit `std::memset` if the rest of the struct's init pattern
  warrants it.  Verify against the post-S4 codebase before deciding.

### 4. Producer Registration (MANDATORY)

**Requirement:** `register_with_broker()` schema hash logic.
**Site to update:** producer registration site that hashes the schema.
**Current state:** schema hash is computed from
`generate_schema_info<SharedMemoryHeader>()` via the macro at
PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS.  No direct field-name
mention in registration code beyond the schema-info call.
**Action at S5 ship:**
- No direct code change at the registration site.
- The schema hash bumps automatically — verify by comparing the
  schema_info hash output before and after the rename.

### 5. Consumer Discovery (MANDATORY)

**Requirement:** `find_datablock_consumer_impl()` validation.
**Current state (post-S4):** the C API no longer takes a `secret`
parameter (S4 deletes it).  Consumer discovery does NOT validate the
field's content (the capability-fd path does that via the SCM_RIGHTS
handoff + `fstat(fd).st_size` check at `data_block.cpp:1308`).
**Action at S5 ship:**
- Verify no string-comparison of the field name `shared_secret`
  remains in the discovery code path after S3+S4.  Grep for
  `shared_secret` across `src/` post-S4 — should return zero hits in
  production code; only the `SharedMemoryHeader::shared_secret[64]`
  declaration itself plus the data_block.cpp:542 init line.

### 6. Schema Validation (MANDATORY)

**Requirement:** All `generate_schema_info<SharedMemoryHeader>()` calls.
**Sites to verify:** grep `generate_schema_info<SharedMemoryHeader>`
in `src/` + `tests/`.
**Action at S5 ship:**
- No code change.  Schema info uses the field name as part of the
  hash input; the rename propagates through automatically.
- Verify the schema hash changes (record the pre/post hash in the S5
  commit message for future readers).

### 7. Checksum Logic (MANDATORY)

**Requirement:** `update_checksum_flexible_zone()`,
`verify_checksum_flexible_zone()`.
**Current state:** Checksum logic does NOT touch `shared_secret` —
it operates on the flexible-zone region (separate from the header
field-region containing the 64-byte secret).
**Action at S5 ship:** no change.  Verify by grep — `shared_secret`
should not appear in any checksum-update or verify code path.

### 8. Test Coverage (MANDATORY)

**Requirement:** Update `test_datahub_schema_blds.cpp`,
`test_datahub_schema_validation.cpp`.
**Sites:** `tests/test_layer3_datahub/test_datahub_schema_blds.cpp`
+ `tests/test_layer3_datahub/test_datahub_schema_validation.cpp`.
**Action at S5 ship:**
- These tests compute the schema hash and assert against a hardcoded
  expected value.  The expected-value constant WILL change because
  the field name participates in the hash.
- Update the hardcoded expected hash (record both pre/post values in
  the commit message for audit trail).
- BLDS schema diagram in `test_datahub_schema_blds.cpp` may also
  embed the field name — search-and-replace `shared_secret` →
  `reserved_capability_token` in the test fixture / golden data.

### 9. Documentation (MANDATORY)

**Requirement:** Update memory layout diagrams, IMPLEMENTATION_GUIDANCE.
**Sites:**
- `docs/HEP/HEP-CORE-0002-Memory-Layout.md` (if it carries a header
  diagram) — verify at S5 ship.
- `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` §"Wire shape clean
  break" — verify any reference to the old `shared_secret[64]`
  diagram is updated to `reserved_capability_token[64]`.
- `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
  — the matrix above mentions `SharedMemoryHeader::shared_secret`
  field by name; update to reflect the new name.
- `data_block.hpp` file-level comment block (where the header layout
  is narrated) — update.
- Doc comment on the `reserved_capability_token` field itself
  explaining the rename history + why "reserved" (it's a capability-
  path layout-preservation slot; future use is intentionally open).

---

## Sequencing pre-conditions (must verify before starting S5)

| Pre-condition | Verification command | Required state |
|---|---|---|
| S2 shipped | `grep -rn 'find_datablock_consumer_impl' tests/test_layer3_datahub/workers/` | All 16 sites use capability-path (no `secret` arg) |
| S3 shipped | `grep -n 'set_shm_secret\|shm_shared_secret' src/include/utils/` | No matches in headers |
| S4 shipped | `grep -n 'shared_secret' src/include/utils/hub/data_block_c_api.h` (or wherever the C API lives) | No `shared_secret` parameter |
| Stage-debug build clean | `cmake --build build/ninja-debug -j 2` | Exit 0; no S2/S3/S4 residue warnings |
| L2+L3+L4 sweep green | `ctest --test-dir build/ninja-debug -L "layer2|layer3|layer4" -j 2` | All green |

**Do NOT start S5 if any pre-condition is unmet.**  Each prior stage
(S2, S3, S4) MUST land + ship + be verified green before S5 starts —
that's the staged-execution discipline in `docs/todo/AUTH_TODO.md`
§"#275 1i-cleanup staged execution".

---

## S5 ship-step checklist

When S5 starts (after S4 ships green):

- [ ] Run the 5 pre-condition verifications above.
- [ ] Open `src/include/utils/hub/shared_memory_header.hpp` (verify
      filename); rename field `shared_secret` → `reserved_capability_token`.
- [ ] Open `PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS` macro definition;
      rename the row.
- [ ] Open `data_block.cpp:542`; delete the initialization line (or
      whatever line carries the now-redundant init — re-verify line
      number against S4-shipped state).
- [ ] Grep `shared_secret` across `src/` — should return only the
      rename target (the field declaration) and possibly the
      data_block.cpp init line if not yet deleted.  Fix any stragglers.
- [ ] Grep `shared_secret` across `docs/` — update HEP-CORE-0002,
      HEP-CORE-0041, IMPLEMENTATION_GUIDANCE references.
- [ ] Compute pre/post schema hash:
      ```bash
      # Pre-rename build:
      ctest --test-dir build/ninja-debug -R 'SchemaHash' -V 2>&1 | grep 'schema hash'
      # Save value.  Then apply rename, rebuild, re-run, save new value.
      ```
      Record both in the commit message.
- [ ] Update `test_datahub_schema_blds.cpp` + `test_datahub_schema_validation.cpp`
      hardcoded expected-hash constants to the new value.
- [ ] Add doc-comment on `reserved_capability_token` explaining the
      rename history (cite #275 S5 + HEP-CORE-0041 §"Wire shape clean
      break").
- [ ] Verify `static_assert(sizeof(SharedMemoryHeader) == 4096)`
      still passes — sanity build.
- [ ] Run full L2+L3+L4 sweep — all green.
- [ ] Commit message format:
      ```
      #275 S5: rename SharedMemoryHeader::shared_secret → reserved_capability_token

      Core Structure Change Protocol walked via
      docs/code_review/REVIEW_S5_CoreStructure_2026-06-27.md.

      Schema hash: <pre-value> → <post-value> (intentional bump per
      protocol §"Producer Registration").

      Files touched:
      ...
      ```
- [ ] Archive this review doc to
      `docs/archive/transient-2026-MM-DD/code_reviews/` per
      DOC_STRUCTURE.md §2.2 after S5 ships green.

---

## Out of scope for S5

- Any other `SharedMemoryHeader` field changes (S5 is a single-field
  rename; touching other fields multiplies risk).
- Layout changes (S5 preserves bytes exactly).
- Field-purpose redesign — `reserved_capability_token` is a name
  reflecting that the bytes are reserved for the capability path's
  future use; what those bytes mean operationally is outside this
  rename's scope.
- Test infrastructure changes — Pattern 4 reform (#285) is orthogonal
  to S5 (verified 2026-06-27).

---

## References

- `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
  (line 1386) — protocol source
- `docs/todo/AUTH_TODO.md` § "#275 1i-cleanup staged execution" —
  active S2..S5 plan
- `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md`
  § "HEP-0041 Phase 1 substep narratives" — historical context for
  why this rename matters
- `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` — capability transport
  design that retires the secret
- `docs/HEP/HEP-CORE-0002-Memory-Layout.md` — memory layout reference
  (may need diagram update)
