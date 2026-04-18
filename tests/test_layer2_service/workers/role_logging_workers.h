#pragma once
/**
 * @file role_logging_workers.h
 * @brief Worker functions for role logging tests (Pattern 3).
 *
 * Each worker runs in a freshly spawned subprocess (IsolatedProcessTest::
 * SpawnWorker) and owns its own lifecycle via run_gtest_worker(). The
 * parent creates a unique temp dir, passes it as arg, and cleans it up
 * after the worker exits. No in-process LifecycleGuard in parent tests.
 *
 * Covers two parent files:
 *   - test_role_logging_roundtrip.cpp  (CLI log flags → JSON → LoggingConfig)
 *   - test_configure_logger.cpp        (configure_logger_from_config flow)
 */

#include <cstddef>
#include <string>

namespace pylabhub::tests::worker
{
namespace role_logging
{

// ── test_role_logging_roundtrip.cpp ─────────────────────────────────────────

/** Init dir with no overrides; assert LoggingConfig defaults preserved. */
int roundtrip_defaults_preserved(const std::string &dir, const std::string &role);

/** Init with max_size_mb override; assert cfg.logging().max_size_bytes. */
int roundtrip_max_size(const std::string &dir, const std::string &role,
                       double max_size_mb, std::size_t expected_bytes);

/** Init with backups override; assert cfg.logging().max_backup_files. */
int roundtrip_backups(const std::string &dir, const std::string &role, int backups,
                      std::size_t expected_count);

/** Init with both overrides; assert all three logging fields. */
int roundtrip_both(const std::string &dir, const std::string &role, double max_size_mb,
                   int backups, std::size_t expected_bytes, std::size_t expected_count);

/** Init with backups=0; assert load_from_directory throws runtime_error. */
int roundtrip_error_backups_zero(const std::string &dir, const std::string &role);

/** Init with max_size_mb=0; assert load_from_directory throws runtime_error. */
int roundtrip_error_maxsize_zero(const std::string &dir, const std::string &role);

// ── test_configure_logger.cpp ───────────────────────────────────────────────

/** Auto-composed path: verify <uid>-<ts>.log appears under <dir>/logs/. */
int configure_auto_composed_path(const std::string &dir);

/** Rotation params (max_size=1MB, backups=3) flow from CLI → cfg.logging. */
int configure_rotation_params(const std::string &dir);

/** backups=-1 sentinel → LoggingConfig::kKeepAllBackups. */
int configure_keep_all_sentinel(const std::string &dir);

/** POSIX-only: unwritable role dir → configure_logger returns false + ec set. */
int configure_unwritable_dir(const std::string &dir);

/** Explicit logging.file_path in JSON → used verbatim when timestamped=false. */
int configure_explicit_file_path(const std::string &dir);

} // namespace role_logging
} // namespace pylabhub::tests::worker
