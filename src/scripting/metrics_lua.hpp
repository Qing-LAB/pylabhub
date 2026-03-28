#pragma once
/**
 * @file metrics_lua.hpp
 * @brief Serialize metrics structs to Lua tables via X-macro field lists.
 *
 * Provides one function per metrics group. Each populates a Lua table (at stack top)
 * with all fields — the caller assigns it to the appropriate hierarchical key
 * ("queue", "in_queue", "loop", "inbox", etc.) via lua_setfield.
 *
 * ZMQ-only fields are 0 for SHM transport; included for schema consistency.
 *
 * Usage:
 * @code
 *   lua_newtable(L);                              // top-level metrics table
 *   lua_newtable(L);                              // "queue" sub-table
 *   queue_metrics_to_lua(L, queue_metrics);
 *   lua_setfield(L, -2, "queue");                 // metrics.queue = <sub-table>
 * @endcode
 */
#include "utils/hub_queue.hpp"
#include "utils/role_host_core.hpp"
#include "utils/hub_inbox_queue.hpp"
#include <lua.hpp>

namespace pylabhub::scripting
{

/// Populate the Lua table at stack top with all QueueMetrics fields.
inline void queue_metrics_to_lua(lua_State *L, const hub::QueueMetrics &m)
{
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(field) lua_pushinteger(L, static_cast<lua_Integer>(m.field)); lua_setfield(L, -2, #field);
    PYLABHUB_QUEUE_METRICS_FIELDS(X)
#undef X
}

/// Populate the Lua table at stack top with all loop metrics fields.
inline void loop_metrics_to_lua(lua_State *L,
                                const RoleHostCore::LoopMetricsSnapshot &m)
{
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(field) lua_pushinteger(L, static_cast<lua_Integer>(m.field)); lua_setfield(L, -2, #field);
    PYLABHUB_LOOP_METRICS_FIELDS(X)
#undef X
}

/// Populate the Lua table at stack top with all inbox metrics fields.
inline void inbox_metrics_to_lua(lua_State *L,
                                 const hub::InboxQueue::InboxMetricsSnapshot &m)
{
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(field) lua_pushinteger(L, static_cast<lua_Integer>(m.field)); lua_setfield(L, -2, #field);
    PYLABHUB_INBOX_METRICS_FIELDS(X)
#undef X
}

} // namespace pylabhub::scripting
