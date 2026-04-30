#pragma once
/**
 * @file hub_metrics_filter.hpp
 * @brief MetricsFilter — selector struct for the unified hub-state query
 *        engine (HEP-CORE-0033 §10.3).
 *
 * Entry-level granularity (HEP-0033 §10.3 step 1).  The filter selects
 * which categories appear in the response and, within each category,
 * which entries by identity.  No field-level projection — clients
 * post-filter the returned JSON if they want a subset.  See the HEP §10
 * rationale for why the contract sits at category × identity, not deeper.
 */

#include "pylabhub_utils_export.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace pylabhub::hub
{

/// Category tags accepted in MetricsFilter::categories.  An empty set
/// means "all categories"; otherwise only listed categories appear in
/// the response.  Per HEP-0033 §10.3.
namespace metrics_category
{
inline constexpr const char *kChannel = "channel";
inline constexpr const char *kRole    = "role";
inline constexpr const char *kBand    = "band";
inline constexpr const char *kPeer    = "peer";
inline constexpr const char *kBroker  = "broker";
inline constexpr const char *kShm     = "shm";
inline constexpr const char *kSchema  = "schema";
} // namespace metrics_category

/// Selector for `BrokerService::query_metrics`.  All identity selectors
/// are applied with AND semantics within a category — i.e. only entries
/// whose identity appears in the relevant list are included.  Empty list
/// for a given category means "no identity filter; include all entries
/// in that category."  Empty `categories` set means "include all
/// categories."  Combination: a category appears in the response iff
/// (categories empty OR category is in the set).
struct PYLABHUB_UTILS_EXPORT MetricsFilter
{
    /// Category tags to include.  Empty = all.  See `metrics_category`.
    std::unordered_set<std::string> categories;

    /// Channel-name whitelist.  Empty = all channels (when the channel
    /// category is included).  Also restricts the SHM category — only
    /// shm_blocks whose channel is in this list (or all if empty) are
    /// collected.
    std::vector<std::string> channels;

    /// Role-uid whitelist.  Empty = all roles.
    std::vector<std::string> roles;

    /// Band-name whitelist.  Empty = all bands.
    std::vector<std::string> bands;

    /// Peer hub-uid whitelist.  Empty = all peers.
    std::vector<std::string> peers;

    /// Convenience predicate for the response builder.
    [[nodiscard]] bool wants(const char *category) const
    {
        return categories.empty() || categories.count(category) > 0;
    }
};

} // namespace pylabhub::hub
