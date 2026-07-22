/**
 * @file role_e2e_harness.h
 * @brief Shared L4 helpers for `plh_hub`/`plh_role` end-to-end
 *        auth-gated data-flow tests (HEP-CORE-0036 §I11 +
 *        HEP-CORE-0041 §6).
 *
 * Header-only.  Helpers cover the parts of an L4 e2e scenario that
 * are transport-AGNOSTIC: rotating-log file reading, marker waiters,
 * the `plh_role` binary path derivation, role keygen + known_roles
 * registration via `plh_hub` CLI.  Transport-SPECIFIC pieces
 * (producer/consumer config writers, transport-specific event
 * markers like `ShmCapabilityTransportBound` or
 * `ConsumerAttachAuthorized`) stay inline per test file.
 *
 * Extracted 2026-06-30 from `test_plh_hub_role_shm_e2e.cpp`
 * (#154 AUTH-7) so the ZMQ-transport L4 e2e sibling can reuse
 * the orchestration without duplicating it.
 *
 * Naming + log discipline matches the HEP-CORE-0004 async-logger
 * "test-side reading" rule: production INFO/WARN/ERROR lands in
 * `<role_dir>/logs/` `.log` files AFTER the role's Logger sink switch;
 * pre-switch boot output lands on stderr.  Marker waiters check
 * both so the test sees the marker regardless of when it fires.
 */
#pragma once

#include "plh_hub_fixture.h"

#include "utils/role_vault.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <thread>

namespace pylabhub::tests::plh_role_e2e
{

namespace fs = std::filesystem;
using WorkerProcess = pylabhub::tests::helper::WorkerProcess;

// ─── Log readers (rotating-file aware) ──────────────────────────────────────

/// Reads the hub's rotating log file (under `<hub_dir>/logs/`).
/// Returns "" if the directory or files don't exist yet.
inline std::string read_hub_log(const fs::path &hub_dir)
{
    const fs::path logs = hub_dir / "logs";
    std::error_code ec;
    if (!fs::is_directory(logs, ec))
        return {};
    fs::path newest;
    for (const auto &e : fs::directory_iterator(logs, ec))
        if (e.path().extension() == ".log" && e.path() > newest)
            newest = e.path();
    if (newest.empty())
        return {};
    std::ifstream f(newest);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});
}

/// Polls `read_hub_log(dir)` until `marker` appears as a substring or
/// `timeout` elapses.  Polls every 50 ms.
inline bool
wait_for_hub_marker(const fs::path &dir, const std::string &marker,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (read_hub_log(dir).find(marker) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// Reads the role's rotating log file (under `<role_dir>/logs/`).
/// After Logger sink switch (early in `plh_role` main), production
/// INFO/WARN/ERROR markers go HERE — not stderr.
inline std::string read_role_log(const fs::path &role_dir)
{
    const fs::path logs = role_dir / "logs";
    std::error_code ec;
    if (!fs::is_directory(logs, ec))
        return {};
    fs::path newest;
    for (const auto &e : fs::directory_iterator(logs, ec))
        if (e.path().extension() == ".log" && e.path() > newest)
            newest = e.path();
    if (newest.empty())
        return {};
    std::ifstream f(newest);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});
}

/// Polls the role's rotating log file AND its stderr (early-boot
/// messages before sink switch) until `marker` appears as a substring
/// or `timeout` elapses.  Polls every 50 ms.
inline bool
wait_for_role_marker(const fs::path &role_dir, WorkerProcess &p, const std::string &marker,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (read_role_log(role_dir).find(marker) != std::string::npos)
            return true;
        if (p.get_stderr().find(marker) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// Parses `Broker: listening on tcp://...` out of the hub log.
/// Returns "" if not yet bound.
inline std::string extract_bound_endpoint(const std::string &log)
{
    static const std::regex re(R"(Broker: listening on (tcp://[^\s]+))");
    std::smatch m;
    if (std::regex_search(log, m, re))
        return m[1].str();
    return {};
}

// ─── plh_role binary path resolution ────────────────────────────────────────

/// Derives the `plh_role` binary path from the test binary's own path
/// (`g_self_exe_path`, set by gtest main).  Tests link against the
/// same staging dir layout as production: `<stage>/bin/plh_role`,
/// `<stage>/tests/<test_binary>`.
inline std::string plh_role_binary()
{
    return (fs::path(::g_self_exe_path).parent_path() / ".." / "bin" / "plh_role").string();
}

// ─── Vault + known_roles helpers ────────────────────────────────────────────

/// Spawn `plh_role --keygen` for a role and return its z85 public key.
/// Asserts the keygen process exited 0 and that the vault file has
/// the required ACL (`ExpectVaultFileSecured`).
///
/// CONTRACT: caller MUST have set `PYLABHUB_ROLE_PASSWORD` to
/// `role_password` BEFORE invoking — `plh_role --keygen` reads the
/// password from that env var.
inline std::string keygen_role_and_read_pubkey(const fs::path &role_dir,
                                               const std::string &role_kind, const std::string &uid,
                                               const std::string &role_password)
{
    using pylabhub::tests::helper::ExpectVaultFileSecured;
    WorkerProcess kg(
        plh_role_binary(), "--role",
        {role_kind, "--config", (role_dir / (role_kind + ".json")).string(), "--keygen"});
    EXPECT_EQ(kg.wait_for_exit(), 0) << "plh_role --keygen (" << role_kind << ") failed:\n"
                                     << kg.get_stderr();
    const fs::path vault_path = role_dir / "vault" / (uid + ".vault");
    ExpectVaultFileSecured(vault_path);
    auto vault = pylabhub::utils::RoleVault::open(vault_path, uid, role_password);
    return std::string(vault.public_key());
}

/// Append a role to `<hub_dir>/vault/known_roles.json` via
/// `plh_hub --add-known-role`.  Asserts the process exited 0.
inline void add_known_role(const fs::path &hub_dir, const std::string &display_name,
                           const std::string &uid, const std::string &role_kind,
                           const std::string &pubkey_z85)
{
    using pylabhub::tests::plh_hub_l4::plh_hub_binary;
    WorkerProcess add(plh_hub_binary(), "--config",
                      {(hub_dir / "hub.json").string(), "--add-known-role", display_name, uid,
                       role_kind, pubkey_z85});
    ASSERT_EQ(add.wait_for_exit(), 0) << "plh_hub --add-known-role failed:\n" << add.get_stderr();
}

} // namespace pylabhub::tests::plh_role_e2e
