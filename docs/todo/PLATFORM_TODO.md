# Platform TODO

**Purpose:** Track cross-platform consistency issues, MSVC/Windows build fixes, and
platform-specific behaviour differences.

**Master TODO:** `docs/TODO_MASTER.md`
**Code Review Source:** `docs/code_review/REVIEW_utils_2026-02-15.md`

---

## Current Focus

### Codex Review: Platform support claims vs CI validation (2026-03-15)

- [ ] **CI is Linux-only** but `pyproject.toml` classifiers advertise macOS and Windows,
  and `README_testing.md` says all tests must run on Windows/Linux/macOS/FreeBSD.
  `README_utils.md` still says DataBlock-on-Windows is incomplete.
  Either add macOS/Windows CI jobs or narrow the documented platform support to match
  what is actually validated.

---

## Backlog

### Clang-Tidy Pass

- [ ] **Full clang-tidy quality pass** — Reconfigure with `CC=clang CXX=clang++` and
  `-DPYLABHUB_ENABLE_CLANG_TIDY=ON` for a complete static-analysis sweep. GCC build is
  already clean; clang-tidy provides additional checks (cppcoreguidelines, modernize, etc.).
  Infrastructure is already wired (`cmake/PlatformAndCompiler.cmake`). Run periodically or
  before releases.
  ```bash
  CC=clang CXX=clang++ cmake -S . -B build-clang -DPYLABHUB_ENABLE_CLANG_TIDY=ON
  cmake --build build-clang 2>&1 | grep -E "warning:|error:" | grep -v third_party
  ```

### Code Review REVIEW_FullStack_2026-03-17 — Platform items (all completed)

- [x] L0-01: Windows `shm_create()` >4GB — split size into high/low DWORD ✅ 2026-03-17
- [x] L0-02: POSIX `shm_create()` defensive unlink guard + HEP §2.2 ownership principle ✅ 2026-03-17
- [x] L2-01: DataBlockMutex `long` → `int64_t` for timeout computation ✅ 2026-03-17
- [x] L2-04: `try_lock_for(-1)` fixed on BOTH POSIX and Windows + 2 tests ✅ 2026-03-17
- [x] L0-05: Windows `GetModuleFileNameW` truncation off-by-one ✅ 2026-03-17

### Windows (MSVC) — Known Gaps

- [ ] **`/Zc:preprocessor` PUBLIC propagation audit** — Confirm that all consumers of
  `pylabhub::utils` that use `PLH_DEBUG` / `LOGGER_*` macros receive the flag via CMake
  interface propagation. Run a Windows CI build targeting at least one consumer executable
  and verify no C3878/C2760 VA_OPT errors.
  **File:** `src/utils/CMakeLists.txt`

- [ ] **MSVC warnings-as-errors gate** — Add `/W4 /WX` to MSVC CI to catch future
  C4251/C4324/C4996 regressions early. Currently only runs on Linux with `-Werror`.

---

## Recent Completions

### 2026-02-17 (docs audit — stale item verified and closed)

- ✅ **[A-6] Monotonic clock consistency** — Verified fixed in code: `logger.cpp:87` and
  `format_tools.cpp:100` both use `pylabhub::platform::monotonic_time_ns()`. Item was already
  marked ✅ in `API_TODO.md` but left open here by mistake.

### 2026-02-17 (Windows compatibility pass)

- ✅ **`/Zc:preprocessor` PUBLIC** — Added to `pylabhub-utils` target so `__VA_OPT__`
  works in all consumers of the public headers (`logger.hpp`, `debug_info.hpp`)
  **File:** `src/utils/CMakeLists.txt`

- ✅ **Missing standard headers** — Explicitly added `<filesystem>`, `<chrono>`, `<mutex>`,
  `<optional>`, `<span>`, `<thread>` to all `.cpp` files that relied on transitive includes
  (safe on GCC, broken on MSVC): `message_hub.cpp`, `lifecycle.cpp`, `logger.cpp`,
  `data_block.cpp`, `file_lock.cpp`, `format_tools.cpp`, `json_config.cpp`, `platform.cpp`,
  `crypto_utils.cpp`, `rotating_file_sink.cpp`, `base_file_sink.cpp`

- ✅ **`SharedSpinLock` ABI** — `std::string m_name` replaced with `char m_name[256]`
  in exported class — eliminates MSVC C4251
  **File:** `src/include/utils/shared_memory_spinlock.hpp`

- ✅ **MSVC C4251 — `MessageHub::pImpl`** — `#pragma warning(suppress:4251)` before
  `std::unique_ptr<MessageHubImpl>` member
  **File:** `src/include/utils/message_hub.hpp`

- ✅ **MSVC C4324 — `SlotRWState` alignment padding** — `#pragma warning(push/disable:4324/pop)`
  around `alignas(64) SlotRWState`
  **File:** `src/include/utils/data_block.hpp`

- ✅ **MSVC C4996 — `strncpy` in test type** — `#pragma warning(suppress:4996)` at call site
  **File:** `tests/test_framework/test_datahub_types.h`

- ✅ **`TxAPITestFlexZone` trivially-copyable fix** — `std::atomic<uint32_t/bool>` members
  replaced with plain `uint32_t`; all `.store()/.load()` call sites updated to plain
  read/write. `std::atomic<T>` is not trivially copyable on MSVC.
  **File:** `tests/test_layer3_datahub/workers/datahub_transaction_api_workers.cpp`

---

## Notes

### MSVC vs GCC Differences to Remember

| Behaviour | GCC/Linux | MSVC/Windows |
|---|---|---|
| `std::atomic<T>` trivially copyable | ✅ (for lock-free T) | ❌ (always not trivially copyable) |
| `__VA_OPT__` available | ✅ default | ❌ requires `/Zc:preprocessor` |
| Transitive standard headers | Many come through fmt/zmq | Must be explicit |
| `#pragma warning` | Silently ignored | Enforced |
| `strncpy` | No warning | C4996 by default |

### Pattern: FlexZone/DataBlock POD Requirement

`FlexZoneT` and `DataBlockT` must be **trivially copyable** on ALL platforms.
`std::atomic<T>` members are not allowed. Use plain POD + `std::atomic_ref<T>` at call sites.
See `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` §9 for the full rationale and pattern.
