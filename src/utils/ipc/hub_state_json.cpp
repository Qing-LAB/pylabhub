/**
 * @file hub_state_json.cpp
 * @brief Implementations of HubState → JSON serializers.
 *
 * Promoted from a private anonymous namespace inside
 * `broker_service.cpp` so AdminService (HEP-CORE-0033 §11.2 query
 * RPCs) and any future consumer can produce the same on-the-wire
 * shape that the metrics-API has emitted historically.
 *
 * Field-by-field formatting is preserved verbatim from the previous
 * broker_service.cpp implementation; this commit is a pure move +
 * symbol-export, no semantic change.  See `hub_state_json.hpp` for
 * the wire-stability contract.
 */

#include "utils/hub_state_json.hpp"

#include "utils/format_tools.hpp"

#include <chrono>
#include <vector>

namespace pylabhub::hub
{

namespace
{

/// Format a system-clock time point into the RFC3339-like string the
/// rest of the hub uses, returning empty string for zero-valued time
/// points (treat as "never set").
inline std::string fmt_time(std::chrono::system_clock::time_point tp)
{
    if (tp.time_since_epoch().count() == 0)
        return {};
    return pylabhub::format_tools::formatted_time(tp);
}

} // namespace

nlohmann::json channel_to_json(const ChannelEntry &c)
{
    nlohmann::json j;
    j["name"]                = c.name;
    j["status"]              = to_string(c.status);
    j["shm_name"]            = c.shm_name;
    j["schema_hash"]         = c.schema_hash;
    j["schema_version"]      = c.schema_version;
    j["schema_id"]           = c.schema_id;
    j["schema_owner"]        = c.schema_owner;
    j["producer_pid"]        = c.producer_pid;
    j["producer_role_uid"]   = c.producer_role_uid;
    j["producer_role_name"]  = c.producer_role_name;
    j["has_shared_memory"]   = c.has_shared_memory;
    j["data_transport"]      = c.data_transport;
    j["zmq_node_endpoint"]   = c.zmq_node_endpoint;
    j["_collected_at"]       = fmt_time(c.created_at);

    nlohmann::json consumers = nlohmann::json::array();
    for (const auto &cons : c.consumers)
    {
        nlohmann::json cj;
        cj["consumer_pid"]      = cons.consumer_pid;
        cj["role_uid"]          = cons.role_uid;
        cj["role_name"]         = cons.role_name;
        cj["inbox_endpoint"]    = cons.inbox_endpoint;
        cj["_collected_at"]     = fmt_time(cons.connected_at);
        consumers.push_back(std::move(cj));
    }
    j["consumers"] = std::move(consumers);
    return j;
}

nlohmann::json role_to_json(const RoleEntry &r)
{
    nlohmann::json j;
    j["uid"]            = r.uid;
    j["name"]           = r.name;
    j["role_tag"]       = r.role_tag;
    j["state"]          = to_string(r.state);
    j["channels"]       = r.channels;
    j["pubkey_z85"]     = r.pubkey_z85;
    j["latest_metrics"] = r.latest_metrics;
    j["_collected_at"]  = fmt_time(r.metrics_collected_at);
    j["first_seen"]     = fmt_time(r.first_seen);
    return j;
}

nlohmann::json band_to_json(const BandEntry &b)
{
    nlohmann::json j;
    j["name"] = b.name;
    nlohmann::json members = nlohmann::json::array();
    for (const auto &m : b.members)
    {
        nlohmann::json mj;
        mj["role_uid"]  = m.role_uid;
        mj["role_name"] = m.role_name;
        // joined_at is steady_clock — emit relative milliseconds since
        // join instead of guessing wall-clock.
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m.joined_at);
        mj["joined_ms_ago"] = since.count();
        members.push_back(std::move(mj));
    }
    j["members"] = std::move(members);
    return j;
}

nlohmann::json peer_to_json(const PeerEntry &p)
{
    nlohmann::json j;
    j["uid"]             = p.uid;
    j["endpoint"]        = p.endpoint;
    j["state"]           = to_string(p.state);
    j["pubkey_z85"]      = p.pubkey_z85;
    j["relay_channels"]  = p.relay_channels;
    return j;
}

nlohmann::json broker_counters_to_json(const BrokerCounters &c)
{
    nlohmann::json j;
    j["ready_to_pending_total"]        = c.ready_to_pending_total;
    j["pending_to_deregistered_total"] = c.pending_to_deregistered_total;
    j["pending_to_ready_total"]        = c.pending_to_ready_total;
    j["bytes_in_total"]                = c.bytes_in_total;
    j["bytes_out_total"]               = c.bytes_out_total;
    j["msg_type_counts"]               = c.msg_type_counts;
    j["msg_type_errors"]               = c.msg_type_errors;
    j["schema_registered_total"]       = c.schema_registered_total;
    j["schema_evicted_total"]          = c.schema_evicted_total;
    j["schema_citation_rejected_total"] = c.schema_citation_rejected_total;
    return j;
}

} // namespace pylabhub::hub
