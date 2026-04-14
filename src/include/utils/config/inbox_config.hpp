#pragma once
/**
 * @file inbox_config.hpp
 * @brief InboxConfig — categorical config for the typed inbox facility.
 *
 * Parsed from top-level "inbox_schema", "inbox_endpoint", etc.
 * Single source of truth for inbox validation.
 *
 * ## Endpoint security model
 *
 * The inbox ROUTER socket binds on the configured endpoint. Two bind addresses
 * are supported:
 *
 * | Address       | Meaning                                                |
 * |---------------|--------------------------------------------------------|
 * | 127.0.0.1     | Loopback only — peers must be on the same machine      |
 * | 0.0.0.0       | All interfaces — network-accessible (requires CurveZMQ)|
 *
 * Port 0 means OS auto-assigns an ephemeral port (advertised via broker).
 * A fixed port can be specified for firewall/monitoring purposes.
 *
 * Default: "tcp://127.0.0.1:0" (safe local-only, auto port).
 */

#include "utils/hub_zmq_queue.hpp" // kZmqDefaultBufferDepth
#include "utils/json_fwd.hpp"
#include "utils/net_address.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

/// Default inbox bind endpoint: loopback-only, OS-assigned port.
inline constexpr const char *kDefaultInboxEndpoint = "tcp://127.0.0.1:0";

struct InboxConfig
{
    // ── Parsed from config ──────────────────────────────────────────────
    nlohmann::json schema_json{};            ///< Schema (JSON object or named string ref). Null = no inbox.
    std::string    endpoint{kDefaultInboxEndpoint}; ///< ROUTER bind endpoint. See endpoint security model above.
    size_t         buffer_depth{hub::kZmqDefaultBufferDepth}; ///< ZMQ RCVHWM.
    std::string    overflow_policy{"drop"};  ///< "drop" or "block".

    // ── Resolved from schema (populated at schema resolution time) ──────
    std::string    schema_fields_json{};     ///< JSON-serialized ZmqSchemaField array.
    std::string    packing{"aligned"};       ///< "aligned" or "packed" (from SchemaSpec).
    std::string    checksum{"enforced"};     ///< Checksum policy string (from role config).

    /// True if inbox_schema is configured (non-null, non-empty).
    [[nodiscard]] bool has_inbox() const noexcept
    {
        return !schema_json.is_null() && !schema_json.empty();
    }

    /// True if the endpoint binds on all interfaces (network-accessible).
    [[nodiscard]] bool is_network_exposed() const noexcept
    {
        return endpoint.find("0.0.0.0") != std::string::npos;
    }
};

/// Parse inbox fields from a JSON config object.
/// @param j   Root JSON object.
/// @param tag Context tag for error messages (e.g. "producer", "consumer").
inline InboxConfig parse_inbox_config(const nlohmann::json &j, const char *tag)
{
    InboxConfig ic;

    if (j.contains("inbox_schema") && !j["inbox_schema"].is_null())
        ic.schema_json = j["inbox_schema"];

    // Endpoint: use configured value, or keep default (tcp://127.0.0.1:0).
    if (j.contains("inbox_endpoint") && j["inbox_endpoint"].is_string())
        ic.endpoint = j["inbox_endpoint"].get<std::string>();

    ic.buffer_depth    = j.value("inbox_buffer_depth",    hub::kZmqDefaultBufferDepth);
    ic.overflow_policy = j.value("inbox_overflow_policy", std::string{"drop"});

    if (ic.buffer_depth == 0)
        throw std::runtime_error(
            std::string(tag) + ": 'inbox_buffer_depth' must be > 0");

    if (ic.overflow_policy != "drop" && ic.overflow_policy != "block")
        throw std::runtime_error(
            std::string(tag) + ": invalid 'inbox_overflow_policy': '"
            + ic.overflow_policy + "' (expected 'drop' or 'block')");

    if (ic.has_inbox())
    {
        if (!ic.schema_json.is_string() && !ic.schema_json.is_object())
            throw std::runtime_error(
                std::string(tag) + ": 'inbox_schema' must be a JSON object "
                "(inline schema) or string (named schema reference)");

        // Validate endpoint format when inbox is active.
        auto ep_result = pylabhub::validate_tcp_endpoint(ic.endpoint);
        if (!ep_result.ok())
            throw std::runtime_error(
                std::string(tag) + ": invalid inbox_endpoint: " + ep_result.error);
    }

    return ic;
}

} // namespace pylabhub::config
