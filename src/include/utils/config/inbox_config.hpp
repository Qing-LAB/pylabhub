#pragma once
/**
 * @file inbox_config.hpp
 * @brief InboxConfig — categorical config for the typed inbox facility.
 *
 * Parsed from top-level "inbox_schema", "inbox_endpoint", etc.
 * Single source of truth for inbox validation.
 */

#include "utils/hub_zmq_queue.hpp" // kZmqDefaultBufferDepth
#include "utils/json_fwd.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct InboxConfig
{
    nlohmann::json schema_json{};            ///< Schema (JSON object or named string ref). Null = no inbox.
    std::string    endpoint;                 ///< ROUTER bind endpoint. Empty = auto-assign (port 0).
    size_t         buffer_depth{hub::kZmqDefaultBufferDepth}; ///< ZMQ RCVHWM.
    std::string    overflow_policy{"drop"};  ///< "drop" or "block".

    [[nodiscard]] bool has_inbox() const noexcept
    {
        return !schema_json.is_null() && !schema_json.empty();
    }
};

/// Parse inbox fields from a JSON config object.
/// @param j   Root JSON object.
/// @param tag Context tag for error messages.
inline InboxConfig parse_inbox_config(const nlohmann::json &j, const char *tag)
{
    InboxConfig ic;

    if (j.contains("inbox_schema") && !j["inbox_schema"].is_null())
        ic.schema_json = j["inbox_schema"];

    ic.endpoint        = j.value("inbox_endpoint",        std::string{});
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
    }

    return ic;
}

} // namespace pylabhub::config
