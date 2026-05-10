#pragma once
/**
 * @file hub_broker_config.hpp
 * @brief HubBrokerConfig — heartbeat-multiplier role-liveness timeouts.
 *
 * Parsed from the top-level `"broker"` JSON sub-object (HEP-CORE-0033 §6.2).
 * Owns the four heartbeat-multiplier fields that drive the broker's
 * Ready→Pending→deregistered state machine, plus three optional explicit
 * timeout overrides.  Field-for-field parity with `BrokerService::Config`'s
 * heartbeat block (HEP-CORE-0023 §2.5) — Phase 9 wiring is a literal copy,
 * no translation layer.
 *
 * **Auth/access fields deliberately omitted** (`default_channel_policy`,
 * `known_roles`, `channel_policies`).  See HEP-CORE-0035 for the design
 * that must land before they return.
 *
 * Strict key whitelist per HEP-CORE-0033 §6.3.
 */

#include "utils/json_fwd.hpp"
#include "utils/timeout_constants.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct HubBrokerConfig
{
    // ── Heartbeat-multiplier timeouts (HEP-CORE-0023 §2.5) ─────────────
    /// Expected client heartbeat cadence (broker-wide).  Acts as the
    /// **maximum tolerated silence** before the timeout countdown
    /// progresses; roles may heartbeat faster than this (HEP-0023 §2.5
    /// "Role-side preferred cadence vs. hub authority").
    int32_t heartbeat_interval_ms{::pylabhub::kDefaultHeartbeatIntervalMs};

    /// Ready → Pending demotion after this many consecutive missed heartbeats.
    uint32_t ready_miss_heartbeats{::pylabhub::kDefaultReadyMissHeartbeats};

    /// Pending → deregistered atomic teardown (HEP-CORE-0023 §2.1) after
    /// this many additional missed heartbeats, counted from entry into Pending.
    uint32_t pending_miss_heartbeats{::pylabhub::kDefaultPendingMissHeartbeats};

    // ── Optional explicit overrides (null = derive from interval × miss) ─
    /// When set, used verbatim for the Ready→Pending wall-clock timeout
    /// (instead of `heartbeat_interval_ms × ready_miss_heartbeats`).
    std::optional<int32_t> ready_timeout_ms;
    /// Same shape, for Pending→deregistered.
    std::optional<int32_t> pending_timeout_ms;

    /// Checksum-repair policy for slot integrity errors reported via
    /// CHECKSUM_ERROR_REPORT (HEP-CORE-0007 §12.4).
    /// Values:
    ///   "none"        — log the report and ignore (default).
    ///   "notify_only" — log + forward to all channel parties via
    ///                   CHANNEL_EVENT_NOTIFY (HEP-CORE-0030 §5.1).
    /// Maps to `BrokerService::Config::checksum_repair_policy`.
    std::string checksum_repair_policy{"none"};
};

inline HubBrokerConfig parse_hub_broker_config(const nlohmann::json &j)
{
    HubBrokerConfig bc;
    if (!j.contains("broker"))
        return bc;
    if (!j["broker"].is_object())
        throw std::runtime_error("hub: 'broker' must be an object");

    const auto &sect = j["broker"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "heartbeat_interval_ms" &&
            k != "ready_miss_heartbeats" &&
            k != "pending_miss_heartbeats" &&
            k != "ready_timeout_ms" &&
            k != "pending_timeout_ms" &&
            k != "checksum_repair_policy")
            throw std::runtime_error("hub: unknown config key 'broker." + k + "'");
    }

    bc.heartbeat_interval_ms = sect.value("heartbeat_interval_ms",
                                          bc.heartbeat_interval_ms);
    bc.ready_miss_heartbeats   = sect.value("ready_miss_heartbeats",
                                            bc.ready_miss_heartbeats);
    bc.pending_miss_heartbeats = sect.value("pending_miss_heartbeats",
                                            bc.pending_miss_heartbeats);

    auto load_optional = [&](const char *k, std::optional<int32_t> &out)
    {
        if (sect.contains(k) && !sect[k].is_null())
            out = sect[k].get<int32_t>();
    };
    load_optional("ready_timeout_ms",   bc.ready_timeout_ms);
    load_optional("pending_timeout_ms", bc.pending_timeout_ms);

    if (bc.heartbeat_interval_ms <= 0)
        throw std::runtime_error(
            "hub: 'broker.heartbeat_interval_ms' must be > 0 (got " +
            std::to_string(bc.heartbeat_interval_ms) + ")");
    if (bc.ready_miss_heartbeats == 0)
        throw std::runtime_error(
            "hub: 'broker.ready_miss_heartbeats' must be >= 1");
    if (bc.pending_miss_heartbeats == 0)
        throw std::runtime_error(
            "hub: 'broker.pending_miss_heartbeats' must be >= 1");

    auto check_optional_nonneg = [](const char *name,
                                     const std::optional<int32_t> &v)
    {
        if (v && *v < 0)
            throw std::runtime_error(
                std::string("hub: 'broker.") + name + "' must be >= 0 (got " +
                std::to_string(*v) + ")");
    };
    check_optional_nonneg("ready_timeout_ms",   bc.ready_timeout_ms);
    check_optional_nonneg("pending_timeout_ms", bc.pending_timeout_ms);

    bc.checksum_repair_policy = sect.value("checksum_repair_policy",
                                            bc.checksum_repair_policy);
    if (bc.checksum_repair_policy != "none" &&
        bc.checksum_repair_policy != "notify_only")
        throw std::runtime_error(
            "hub: 'broker.checksum_repair_policy' must be \"none\" or "
            "\"notify_only\" (got \"" + bc.checksum_repair_policy + "\")");

    return bc;
}

} // namespace pylabhub::config
