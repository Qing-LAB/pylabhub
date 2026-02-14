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

Guard destructors/assignment check and log; broker registration checks and logs; `with_typed_write` checks commit and throws; `SlotDiagnostics` ctor checks `refresh()` and logs. The following production sites intentionally do not use the return value; the operation is best-effort or the return is irrelevant:

| File | Call | Rationale |
|------|------|------------|
| `src/utils/data_block.cpp` | `(void)release_write_handle(*pImpl)` in `SlotWriteHandle::~SlotWriteHandle` | Release function already logs on failure (LOGGER_WARN). In dtor we perform best-effort release; no need to log again. |
| `src/utils/data_block.cpp` | `(void)release_consume_handle(*pImpl)` in `SlotConsumeHandle::~SlotConsumeHandle` | Same as above. |
| `src/utils/logger.cpp` | `(void)future_err.get()` (multiple sink error paths), `(void)future.get()` (flush, set_error_callback, set_log_sink_messages_enabled) | We enqueue a command and wait for the worker to process it; the boolean return is intentionally unused (sync-only or error path). |
| `src/utils/data_block_mutex.cpp` | `(void)pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT)` | Best-effort set priority-inherit; comment explains ENOTSUP is tolerated if platform lacks PI. |
| `src/utils/debug_info.cpp` | `(void)SymInitialize(process, nullptr, TRUE)` | Best-effort Windows symbol init; comment: “Best-effort initialize; failures tolerated.” |
| `src/include/utils/json_config.hpp` | `(void)wlock.json().dump()` in write transaction validation | Intentional: we call `dump()` to validate JSON (side effect); return value is unused. |

---

## 3. Summary

- **Checked and handled:** Guard release in dtor/assignment (log on failure), broker register_producer/register_consumer (log), with_typed_write commit (throw), SlotDiagnostics ctor refresh (log), and test paths that expect success (EXPECT_TRUE/ASSERT_TRUE).
- **Intentionally not checked (documented):** Tests: (1) zombie test write/commit before _exit(0), (2) JsonConfig test that does not consume the transaction proxy. Production: SlotWriteHandle/SlotConsumeHandle dtor release_* (release logs internally), Logger future.get() (sync only), data_block_mutex setprotocol, debug_info SymInitialize, json_config dump() validation.

If you add a new “intentionally ignore” call site, add it here with rationale and mark it for discussion.
