#pragma once
/**
 * @file hub_admin_config.hpp
 * @brief HubAdminConfig — AdminService endpoint configuration.
 *
 * Parsed from the top-level `"admin"` JSON sub-object (HEP-CORE-0033 §6.2 / §11).
 * Controls whether the structured admin RPC endpoint is enabled and where it
 * binds.  The admin plane is CURVE-secured (§11.1) and always token-gated
 * (§11.3) — there is no token-less path.  Loopback is the default bind
 * (defense-in-depth); a network bind is a safe operator opt-in because the
 * transport is encrypted.
 */

#include "utils/json_fwd.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

/// Operator-console pull output-buffer caps (HEP-CORE-0033 §11.0.4).  These are
/// the safety-valve bounds — a healthy poll cadence keeps the buffer near-empty,
/// so they only bite an absent/hung console or a flooding script.  Defaults
/// mirror `ConsoleOutputBuffer::Caps` (kept in sync; the buffer's own defaults
/// apply when no HubHost wires these, e.g. tests).
struct ConsoleOutputBufferConfig
{
    std::size_t max_lines{1000};                 ///< line-count cap
    std::size_t max_bytes{std::size_t{1024} * 1024};      ///< total-byte cap (1 MiB)
    std::size_t max_line_bytes{std::size_t{64} * 1024};   ///< per-line-byte cap (64 KiB)
};

struct HubAdminConfig
{
    bool enabled{true};                           ///< AdminService on/off
    std::string endpoint{"tcp://127.0.0.1:5600"}; ///< ZMQ endpoint (CURVE-server, §11.1)
    ConsoleOutputBufferConfig output_buffer;      ///< §11.0.4 pull-buffer caps

    /// Runtime-only: 64-char hex admin token.  Populated by
    /// `HubConfig::load_keypair()` from the unlocked `HubVault`; NOT
    /// parsed from JSON (the token is a vault secret, never on disk
    /// in plaintext).  Empty until the vault is unlocked.  Mirrors
    /// the `AuthConfig::client_pubkey/seckey` runtime-only pattern.
    std::string admin_token;
};

inline HubAdminConfig parse_hub_admin_config(const nlohmann::json &j)
{
    HubAdminConfig ac;
    if (!j.contains("admin"))
        return ac;
    if (!j["admin"].is_object())
        throw std::runtime_error("hub: 'admin' must be an object");

    const auto &sect = j["admin"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "enabled" && k != "endpoint" && k != "output_buffer")
            throw std::runtime_error("hub: unknown config key 'admin." + k + "'");
    }

    ac.enabled = sect.value("enabled", ac.enabled);
    ac.endpoint = sect.value("endpoint", ac.endpoint);

    if (sect.contains("output_buffer"))
    {
        const auto &ob = sect["output_buffer"];
        if (!ob.is_object())
            throw std::runtime_error("hub: 'admin.output_buffer' must be an object");
        for (auto it = ob.begin(); it != ob.end(); ++it)
        {
            const auto &k = it.key();
            if (k != "max_lines" && k != "max_bytes" && k != "max_line_bytes")
                throw std::runtime_error("hub: unknown config key 'admin.output_buffer." + k + "'");
        }
        ac.output_buffer.max_lines = ob.value("max_lines", ac.output_buffer.max_lines);
        ac.output_buffer.max_bytes = ob.value("max_bytes", ac.output_buffer.max_bytes);
        ac.output_buffer.max_line_bytes =
            ob.value("max_line_bytes", ac.output_buffer.max_line_bytes);

        // Fail fast on a broken config: a zero cap would drop everything, and a
        // per-line cap above the total cap would let one line evict the buffer.
        if (ac.output_buffer.max_lines == 0 || ac.output_buffer.max_bytes == 0 ||
            ac.output_buffer.max_line_bytes == 0)
            throw std::runtime_error("hub: 'admin.output_buffer' caps must all be > 0");
        if (ac.output_buffer.max_line_bytes > ac.output_buffer.max_bytes)
            throw std::runtime_error(
                "hub: 'admin.output_buffer.max_line_bytes' must be <= max_bytes");
    }
    return ac;
}

} // namespace pylabhub::config
