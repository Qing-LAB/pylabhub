#pragma once
/**
 * @file pattern4_helpers.h
 * @brief Test-framework helpers for Pattern 4 (multi-process wire protocol).
 *
 * See `docs/README/README_testing.md` § "Pattern 4 — Multi-process wire
 * protocol test" for the design pattern these helpers support.
 *
 * Three primitives:
 *
 *   1. `pick_unused_port` — find-unused-TCP-port helper with retry.
 *      Used by the parent test to pre-allocate a port for the broker
 *      subprocess.  Small race window between port allocation and
 *      broker bind is covered belt-and-suspenders by the broker
 *      subprocess's own retry-on-EADDRINUSE loop.
 *
 *   2. `Pattern4Setup` + JSON read/write — the parent writes the
 *      chosen port + CURVE keys to a JSON file under a per-test temp
 *      directory; each subprocess reads it on startup.  Mirrors the
 *      production "operator configures both processes through
 *      hub.json" shape.
 *
 *   3. `wait_for_log` / `expect_log` — live-poll a subprocess's
 *      captured stderr for an expected substring with a timeout.
 *      The parent assertion layer for Pattern 4.  Timeouts MUST come
 *      from canonical library constants
 *      (`pylabhub::kShortTimeoutMs` etc.) — arbitrary numbers are
 *      forbidden per the Pattern 4 doc.
 *
 * Scope note: this header is intentionally test-only — no production
 * code includes it.  Per HEP-CORE-0036 §7.1 + §7.4, the library
 * forbids broker + role co-host; Pattern 4 is how tests respect that
 * invariant.
 */

#include "curve_test_setup.h"
#include "test_process_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pylabhub::tests::pattern4
{

// ─── Port allocation ────────────────────────────────────────────────────────

/// Picks an unused TCP port on `127.0.0.1` with retry.  Implementation:
/// open a SOCK_STREAM, `bind` to `127.0.0.1:0`, capture the OS-assigned
/// port number, close.  Retries up to `max_attempts` on transient
/// errors.  Returns the chosen port, or throws on persistent failure.
///
/// Small race window: between this function's `close` and the broker
/// subprocess's `bind` on the same port, another process can grab the
/// port.  Covered by the broker subprocess's retry-on-EADDRINUSE loop
/// (Pattern 4 doc § "Bind robustness").
[[nodiscard]] int pick_unused_port(int max_attempts = 5);

// ─── Per-test temp directory ────────────────────────────────────────────────

/// Returns a fresh temp directory under `fs::temp_directory_path()`.
/// Names include `getpid()` + a process-local counter so concurrent
/// tests under `-j 2` don't collide.  Caller owns cleanup
/// (recommended: `fs::remove_all` in the test fixture's `TearDown`).
[[nodiscard]] std::filesystem::path
make_temp_dir(std::string_view test_label);

// ─── Pattern4Setup ──────────────────────────────────────────────────────────

/// Setup info the parent test passes to its subprocesses.  Both the
/// broker and each role subprocess receive the same `Pattern4Setup`
/// (deserialised from a JSON file in the temp dir) and configure
/// themselves to bind / connect against `broker_endpoint`.
struct Pattern4Setup
{
    /// "tcp://127.0.0.1:PORT" — the broker binds here, roles connect here.
    std::string broker_endpoint;

    /// CURVE bundle: hub keypair + per-role keypair.  Each subprocess
    /// reads its half from this bundle and seeds its own
    /// `seed_curve_identities()` at startup.
    pylabhub::tests::CurveSetup curve;

    /// Shared log file.  Subprocesses redirect their Logger sink here
    /// via `set_shared_log(shared_log_path)` immediately after the
    /// LifecycleGuard brings up Logger; the parent reads the merged
    /// stream via `expect_log_sequence` to pin cross-process ordering
    /// without timestamp parsing (file-position IS time-order under
    /// `O_APPEND`).  Empty string disables shared-log mode (smoke test
    /// stays on the per-subprocess stderr path).
    std::string shared_log_path;
};

/// Build a `Pattern4Setup` for a list of role uids.  Picks an unused
/// port via `pick_unused_port` and generates a fresh CURVE bundle via
/// `make_curve_setup`.
[[nodiscard]] Pattern4Setup
make_pattern4_setup(const std::vector<std::string> &role_uids);

/// JSON serialisation.  Layout:
///   {
///     "broker_endpoint": "tcp://127.0.0.1:NNNN",
///     "hub": {"public_z85": "...", "secret_z85": "..."},
///     "role_keys": [
///       {"uid": "role.x", "public_z85": "...", "secret_z85": "..."},
///       ...
///     ]
///   }
void write_pattern4_setup(const Pattern4Setup &setup,
                          const std::filesystem::path &path);

[[nodiscard]] Pattern4Setup
read_pattern4_setup(const std::filesystem::path &path);

// ─── Live-poll a subprocess's captured stderr ───────────────────────────────

/// **Wait for `substring` to appear in `proc`'s captured stderr.**
/// Polls the captured stderr file every 25 ms until either the
/// substring is found or `timeout` elapses.
///
/// Pattern 4 contract: subprocesses use `fflush(stderr)` (production
/// logger does this automatically) so log lines appear in the captured
/// file in near-real-time.
///
/// @return true if `substring` was found before `timeout`, false on
///         timeout.  The caller's typical pattern is:
///             EXPECT_TRUE(wait_for_log(proc, "...", kShortTimeoutMs));
///         or use `expect_log` (below) for a cleaner failure message.
[[nodiscard]] bool wait_for_log(
    const pylabhub::tests::helper::WorkerProcess &proc,
    std::string_view substring,
    std::chrono::milliseconds timeout);

/// Wrapper that fails the current gtest scope with a diagnostic
/// message (substring + timeout + tail of captured stderr) if the
/// substring is not found within `timeout`.  Equivalent to
/// `EXPECT_TRUE(wait_for_log(...))` with a richer failure message.
///
/// `timeout` MUST come from canonical lib constants
/// (`pylabhub::kShortTimeoutMs` / `kMidTimeoutMs` / `kLongTimeoutMs`)
/// or HEP-defined intervals — never arbitrary literals.  See
/// Pattern 4 doc § "Verification — log-driven sequence assertion".
void expect_log(const pylabhub::tests::helper::WorkerProcess &proc,
                std::string_view substring,
                std::chrono::milliseconds timeout);

// ─── Shared-log verification (rung 2+) ──────────────────────────────────────

/// **Subprocess-side** — redirect the Logger sink to a file shared
/// across all Pattern-4 subprocesses for this test.  Uses
/// `O_APPEND | O_CREAT` (kernel-atomic appends ≤ `PIPE_BUF`) plus
/// advisory `flock` for belt-and-suspenders serialisation.  Must be
/// called inside the worker lambda AFTER the LifecycleGuard brings up
/// the Logger module (i.e. inside the lambda passed to
/// `run_gtest_worker`) and BEFORE any `LOGGER_*` call that the parent
/// expects to find via `expect_log_sequence`.
///
/// `[WORKER_BEGIN]` / `[WORKER_END_OK]` / `[WORKER_FINALIZED]` markers
/// continue to land on per-subprocess stderr (they go via
/// `fmt::print(stderr, ...)`, not through the Logger sink) so
/// `ExpectWorkerOk` keeps working unchanged.
///
/// Throws on failure (caller may want `ASSERT_TRUE` semantics).
void set_shared_log(const std::filesystem::path &shared_log_path);

/// **Parent-side** — pin a sequence of log markers in `shared_log` IN
/// ORDER.  Each step is searched starting at the byte offset where the
/// previous step's match ended; the file's natural append-order under
/// `O_APPEND` *is* time-order, so this enforces cross-process ordering
/// without parsing timestamps.
///
/// For each step:
///   - Polls the file every 25 ms until the marker is found at or
///     past the current search offset, or `per_step_timeout` elapses.
///   - On match: advances the search offset to the end of the match
///     and proceeds to the next step.
///   - On timeout: fails the current gtest scope with a diagnostic
///     identifying the failed step + how many earlier steps matched
///     + tail of the shared log.
///
/// `per_step_timeout` MUST come from canonical lib constants
/// (`pylabhub::kShortTimeoutMs` etc.) — arbitrary literals forbidden.
///
/// See `docs/README/README_testing.md` § "Pattern 4 — ... — Production
/// INFO marker contract" for the marker format conventions.
void expect_log_sequence(
    const std::filesystem::path &shared_log,
    std::initializer_list<std::string_view> markers,
    std::chrono::milliseconds per_step_timeout);

} // namespace pylabhub::tests::pattern4
