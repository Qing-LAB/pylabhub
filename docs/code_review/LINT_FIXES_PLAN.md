# Clang-Tidy Lint Fixes Plan

Collected from build logs (clang + PYLABHUB_ENABLE_CLANG_TIDY=ON).  
**Rule:** Minor edits (braces, includes, explicit bool, return style, redundant init) are applied in batch.  
**Logic or name changes require your confirmation first.**

---

## Already fixed (this session)

- `src/utils/logging/logger_sinks/rotating_file_sink.cpp`: argument comment `/*synchronous=*/` → `/*sync_flag=*/`.
- `src/utils/shm/data_block.cpp`: `DataBlockProducer::metrics()` and `DataBlockConsumer::metrics()` — avoid ternary returning reference (clang-analyzer null reference); rewritten to `if (pImpl == nullptr) return kEmpty; return pImpl->metrics_;`.

---

## 1. Minor fixes (will apply in batch, no confirmation)

These are style/mechanical only: no logic change, no renames.

| Check | What we do |
|-------|------------|
| `readability-duplicate-include` | Remove duplicate `#include` lines. |
| `readability-braces-around-statements` | Add `{ }` around single-statement if/else bodies. |
| `readability-implicit-bool-conversion` | Use explicit `!= 0` or `!= nullptr` where suggested. |
| `modernize-return-braced-init-list` | `return std::string(...)` → `return std::string{...}` or `return { ... }` where equivalent. |
| `readability-redundant-member-init` | Remove redundant `{}` or `= 0` member initializers. |
| `readability-named-parameter` | Name unused parameters (e.g. `const char * /*unused*/`). |
| `readability-simplify-boolean-expr` | Simplify `return x ? false : true` to `return !x` (or as suggested). |
| `readability-convert-member-functions-to-static` | Add `static` to private methods that don’t use `this`. |

**Files and counts (minor only):**

- `src/utils/core/debug_info.cpp`: duplicate include (2).
- `src/utils/core/platform.cpp`: braces (2).
- `src/utils/core/interactive_signal_handler.cpp`: braces (many), implicit bool (4), named param (1), convert-to-static (5).
- `src/utils/core/uuid_utils.cpp`: braces (many), return braced-init (1).
- `src/utils/service/lifecycle_helpers.cpp`: (only identifier-length here; see “needs confirmation”).
- `src/utils/service/vault_crypto.cpp`: braces (many), return braced-init (1).
- `src/utils/service/hub_vault.cpp`: braces (2).
- `src/utils/service/actor_vault.cpp`: braces (3).
- ~~`src/utils/config/hub_config.cpp`: braces (many), return braced-init (1), redundant-member-init (2).~~ — *Obsolete: legacy file deleted 2026-04-29 with the `pylabhub::HubConfig` singleton. The new file at the same path is the HEP-0033 §6.1 composite (fresh code, no legacy lint debt).*
- `src/utils/logging/logger.cpp`: braces (1), implicit bool (2).
- `src/utils/shm/data_block.cpp`: braces (many), redundant-member-init (4), simplify-boolean (1), return braced-init (1).

---

## 2. Needs your confirmation (logic or names)

Only apply after you approve.

### 2.1 Variable / parameter name length (`readability-identifier-length`)

Clang-tidy wants names of at least 3 characters. Renaming would touch many call sites and might affect clarity (e.g. `it` → `iter` in loops).

| File | Current name | Suggested (example) | Notes |
|------|--------------|---------------------|--------|
| interactive_signal_handler.cpp | `c` | `ch` | 2 uses (char) |
| interactive_signal_handler.cpp | `tt` | `time_val` | time_t |
| interactive_signal_handler.cpp | `ts` | `timestamp` | char buffer |
| interactive_signal_handler.cpp | `lk` | `lock` | 2 uses |
| interactive_signal_handler.cpp | `n` | `num_read` | ssize_t from read() |
| interactive_signal_handler.cpp | `cb` (param) | `callback` | set_status_callback |
| uuid_utils.cpp | `s` (param) | `str` | is_valid_uuid4 |
| uuid_utils.cpp | `v` | `variant_char` | char at index 19 |
| hub_config.cpp | `p`, `f`, `j`, `h`, `a`, `ka`, `cp` | longer names | multiple |
| lifecycle_helpers.cpp | `lk` | `lock` | 2 uses |
| lifecycle_dynamic.cpp | `it` | `iter` | 2 uses |
| hub_vault.cpp | `v`, `j` | `vault`, `jobj` | |
| actor_vault.cpp | `ec`, `v`, `j` | `err_code`, `vault`, `jobj` | |
| logger.cpp | `ec` | `err_code` | |
| data_block.cpp | `ps`, `h`, `m` | `page_size`, `header`, `metrics` | multiple |

**Recommendation:** Either (a) rename as in the table for consistency, or (b) add a single NOLINT for this check in the files that use short names (e.g. `// NOLINT(readability-identifier-length)` on the line or for the block). Which do you prefer?

---

### 2.2 C-style arrays → `std::array` (`modernize-avoid-c-arrays`)

Replacing `T buf[N]` with `std::array<T,N>` can require `.data()` when passing to C APIs (e.g. `read()`, `poll()`, ZMQ). Some arrays are in shared or static storage.

| File | Location | Current | Note |
|------|----------|---------|------|
| interactive_signal_handler.cpp | global | `int g_wake_pipe[2]` | Used with pipe(), read(), poll(); needs .data() or [0]/[1]. |
| interactive_signal_handler.cpp | several | `char buf[16]`, `char ts[32]`, `struct pollfd fds[1]`/`fds[2]` | pollfd passed to poll(). |
| uuid_utils.cpp | | `uint8_t bytes[16]`, `char buf[37]` | UUID layout; std::array is straightforward. |
| vault_crypto.cpp | | `uint8_t salt[...]`, `uint8_t nonce[...]` | Fixed size; libsodium may expect pointer. |
| hub_vault.cpp | | `uint8_t raw[...]`, `char hex[...]` | |
| data_block.cpp | struct members | `consumer_uid_buf[40]`, `consumer_name_buf[32]` | Part of layout; confirm before changing. |

**Recommendation:** We can convert where it’s clearly local buffer + C API (use `.data()`), and leave struct members / globals as-is unless you want a broader refactor. Confirm which you prefer.

---

### 2.3 Magic numbers → named constants (`readability-magic-numbers`)

Introduce `constexpr` or `static const` for literals (e.g. UUID 16, 36, 37; timeouts 500, 7000; buffer sizes 32, 64; etc.). Purely additive naming.

**Recommendation:** Apply in batch (e.g. at file top or in anonymous namespace) with names like `kUuidNumBytes`, `kWakePollMs`, `kTimestampBufSize`. If you prefer to keep some literals (e.g. 0x40U for UUID version) as-is, say which.

---

### 2.4 Enum storage (`performance-enum-size`)

Suggest changing enum base from `int` to `std::uint8_t` where values fit:

- `interactive_signal_handler.cpp`: `enum class PromptResult`
- `hub_config.cpp`: `enum HubConfigState`

**Recommendation:** Only change if you’re comfortable with a different underlying type (could matter for ABI/serialization). Otherwise we can NOLINT these two.

---

### 2.5 Cognitive complexity (`readability-function-cognitive-complexity`)

- `lifecycle_dynamic.cpp`: `computeUnloadClosure` (45), `processOneUnloadInThread` (39).
- `hub_config.cpp`: `apply_json` (116).

Refactor would mean splitting into smaller functions (logic change).

**Recommendation:** Add `// NOLINT(readability-function-cognitive-complexity)` for now, or plan a separate refactor; confirm which.

---

### 2.6 Empty catch (`bugprone-empty-catch`)

- `hub_config.cpp`: two empty catch blocks.

**Recommendation:** Either add a comment (e.g. `// intentionally ignore`) or `std::ignore = ex;` and NOLINT, or implement real handling. Confirm.

---

### 2.7 Easily swappable parameters (`bugprone-easily-swappable-parameters`)

- `vault_crypto.cpp`: `vault_write(json_payload, password)` — two `const std::string&` params.

**Recommendation:** Add NOLINT at call site or function definition, or change API (e.g. use a struct). Confirm.

---

## 3. Applied (batch minor fixes)

The following **§1 minor fixes** have been applied in one pass (no logic or renames):

- **debug_info.cpp**: Removed duplicate `#include <array>` in POSIX block.
- **platform.cpp**: Braces around two `if (created_by_us)` bodies.
- **uuid_utils.cpp**: Braces around all single-statement `if` bodies in `is_valid_uuid4`; `return std::string{buf}`.
- **interactive_signal_handler.cpp**: Braces for all single-statement if/else; explicit `!= 0` for `isatty` and `(revents & POLLIN)`; `static` for `platform_init`, `platform_cleanup`, `wait_for_signal`, `read_stdin_with_wake`, `drain_pipe`; named param `/*unused*/` in `signal_handler_lifecycle_cleanup`; `s_lifecycle_instance != nullptr` and braces.
- **vault_crypto.cpp**: Braces around every `if (!ofs)`, `if (!ifs)`, `if (sodium_init() == -1)`, pwhash check, `vault_bytes.size() < kMinSize`, `crypto_secretbox_open_easy`; `return std::string{plaintext.begin(), plaintext.end()}`.
- **hub_vault.cpp**: Braces for `zmq_curve_keypair` and both `if (!ofs)`.
- **actor_vault.cpp**: Braces for `zmq_curve_keypair`, `if (ec)`, and key-length check.
- **logger.cpp**: `arg == nullptr`, `path != nullptr`, and braces for the rotating-sink lambda.
- **data_block.cpp**: `return total_size == expected_total` (simplify-boolean); `read_id_field` return braced-init; braces for `sysconf`/page-size ifs and for Producer `hub_uid`/`hub_name`/`producer_uid`/`producer_name` null checks.

**Not done in this batch:** hub_config (braces, return braced-init, redundant-member-init), remaining data_block.cpp braces/redundant-member-init, lifecycle_helpers, lifecycle_dynamic (only identifier-length and cognitive-complexity in plan; those need confirmation).

---

## 4. Next step

- Run **one** clang-tidy build to confirm the above and see remaining diagnostics.
- For **§2 (needs confirmation)**, reply with: which of 2.1–2.7 you want applied as suggested, and which should be NOLINT or deferred. Then apply those in a second batch (or add NOLINTs only).
