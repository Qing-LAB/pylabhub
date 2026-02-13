# [[nodiscard]] Return Values: Where We Do Not Check (For Discussion)

**Purpose:** List every call site that intentionally does **not** check the return value of a `[[nodiscard]]` function, with a short rationale. Use this for consistency review and to decide whether `[[nodiscard]]` or the code should change.

**Policy:** Our default is to **check and handle** (log, propagate, throw). Any exception is documented here so we can discuss.

---

## 1. Test / example code

### 1.1 Zombie writer test: write/commit without checking

| File | Call | Rationale |
|------|------|------------|
| `tests/test_layer3_datahub/workers/slot_protocol_workers.cpp` | `wh->write(&p, sizeof(p))`, `wh->commit(sizeof(p))` inside `zombie_writer_acquire_then_exit` | Process calls `_exit(0)` immediately after; we never return to assert. The test’s goal is “writer holds lock and exits without release.” Checking return would require a different test shape. **Discussion:** Should we avoid [[nodiscard]] on write/commit for this pattern, or keep (void) with this comment? |

### 1.2 JsonConfig transaction proxy not consumed

| File | Call | Rationale |
|------|------|------------|
| `tests/test_layer2_service/workers/jsonconfig_workers.cpp` | `cfg.transaction()` | Test intentionally does **not** call `.read()` or `.write()` on the proxy so that the proxy destructor runs without “consumption,” triggering the debug-build warning. The return value (the proxy) is deliberately discarded to test that warning. **Discussion:** [[nodiscard]] on `transaction()` is meant to prevent “create proxy and forget to use it.” This test is the one valid case of “create and don’t use.” Keep (void) and this note, or add a test-only escape? |

---

## 2. Production / library code

There are no remaining production call sites that ignore `[[nodiscard]]` returns without handling. Guard destructors/assignment check and log; broker registration checks and logs; `with_typed_write` checks commit and throws; `SlotDiagnostics` ctor checks `refresh()` and logs.

---

## 3. Summary

- **Checked and handled:** Guard release in dtor/assignment (log on failure), broker register_producer/register_consumer (log), with_typed_write commit (throw), SlotDiagnostics ctor refresh (log), and test paths that expect success (EXPECT_TRUE/ASSERT_TRUE).
- **Intentionally not checked (documented):** (1) zombie test write/commit before _exit(0), (2) JsonConfig test that does not consume the transaction proxy.

If you add a new “intentionally ignore” call site, add it here with rationale and mark it for discussion.
