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

    /// Pending → deregistered (+ CHANNEL_CLOSING_NOTIFY) after this many
    /// additional missed heartbeats, counted from entry into Pending.
    uint32_t pending_miss_heartbeats{::pylabhub::kDefaultPendingMissHeartbeats};

    /// CHANNEL_CLOSING_NOTIFY → FORCE_SHUTDOWN grace window, in heartbeats.
    uint32_t grace_heartbeats{::pylabhub::kDefaultGraceHeartbeats};

    // ── Optional explicit overrides (null = derive from interval × miss) ─
    /// When set, used verbatim for the Ready→Pending wall-clock timeout
    /// (instead of `heartbeat_interval_ms × ready_miss_heartbeats`).
    std::optional<int32_t> ready_timeout_ms;
    /// Same shape, for Pending→deregistered.
    std::optional<int32_t> pending_timeout_ms;
    /// Same shape, for CLOSING_NOTIFY→FORCE_SHUTDOWN grace.  `0` is meaningful
    /// here ("FORCE_SHUTDOWN immediately on voluntary close").
    std::optional<int32_t> grace_ms;
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
            k != "grace_heartbeats" &&
            k != "ready_timeout_ms" &&
            k != "pending_timeout_ms" &&
            k != "grace_ms")
            throw std::runtime_error("hub: unknown config key 'broker." + k + "'");
    }

    bc.heartbeat_interval_ms = sect.value("heartbeat_interval_ms",
                                          bc.heartbeat_interval_ms);
    bc.ready_miss_heartbeats   = sect.value("ready_miss_heartbeats",
                                            bc.ready_miss_heartbeats);
    bc.pending_miss_heartbeats = sect.value("pending_miss_heartbeats",
                                            bc.pending_miss_heartbeats);
    bc.grace_heartbeats        = sect.value("grace_heartbeats",
                                            bc.grace_heartbeats);

    auto load_optional = [&](const char *k, std::optional<int32_t> &out)
    {
        if (sect.contains(k) && !sect[k].is_null())
            out = sect[k].get<int32_t>();
    };
    load_optional("ready_timeout_ms",   bc.ready_timeout_ms);
    load_optional("pending_timeout_ms", bc.pending_timeout_ms);
    load_optional("grace_ms",           bc.grace_ms);

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
    // grace_heartbeats == 0 is allowed — meaningful as "no grace, FORCE_SHUTDOWN immediately".

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
    check_optional_nonneg("grace_ms",           bc.grace_ms);

    return bc;
}

} // namespace pylabhub::config
