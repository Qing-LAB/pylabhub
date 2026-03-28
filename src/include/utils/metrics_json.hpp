#pragma once
/**
 * @file metrics_json.hpp
 * @brief Serialize metrics structs to nlohmann::json via X-macro field lists.
 *
 * Provides one function per metrics group. Each populates a JSON object
 * with all fields — the caller assigns it to the appropriate hierarchical key
 * ("queue", "in_queue", "loop", "inbox", etc.).
 *
 * ZMQ-only fields are 0 for SHM transport; included for schema consistency.
 */
#include "utils/hub_queue.hpp"
#include "utils/role_host_core.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/json_fwd.hpp"

namespace pylabhub::hub
{

/// Populate a JSON object with all QueueMetrics fields.
inline void queue_metrics_to_json(nlohmann::json &j, const QueueMetrics &m)
{
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(field) j[#field] = m.field;
    PYLABHUB_QUEUE_METRICS_FIELDS(X)
#undef X
}

/// Populate a JSON object with all loop metrics fields.
inline void loop_metrics_to_json(nlohmann::json &j,
                                 const scripting::RoleHostCore::LoopMetricsSnapshot &m)
{
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(field) j[#field] = m.field;
    PYLABHUB_LOOP_METRICS_FIELDS(X)
#undef X
}

/// Populate a JSON object with all inbox metrics fields.
inline void inbox_metrics_to_json(nlohmann::json &j,
                                  const InboxQueue::InboxMetricsSnapshot &m)
{
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(field) j[#field] = m.field;
    PYLABHUB_INBOX_METRICS_FIELDS(X)
#undef X
}

} // namespace pylabhub::hub
