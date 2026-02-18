#pragma once
/**
 * @file channel_registry.hpp
 * @brief In-memory channel registry for the pylabhub broker.
 *
 * Stores channel-to-shared-memory-segment mappings. Single-threaded access only —
 * all methods are called exclusively from the BrokerService run() thread.
 */
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pylabhub::broker
{

struct ChannelEntry
{
    std::string shm_name;
    std::string schema_hash;       ///< Hex-encoded (64 chars), as received from producer
    uint32_t schema_version{0};
    uint64_t producer_pid{0};
    std::string producer_hostname;
    nlohmann::json metadata; ///< ring_buffer_capacity, policy, etc.
};

/**
 * @class ChannelRegistry
 * @brief Thread-unsafe in-memory registry mapping channel names to ChannelEntry.
 *
 * No mutex required: only the BrokerService run() thread accesses this registry.
 */
class ChannelRegistry
{
public:
    /**
     * @brief Register or update a channel.
     * @return true if registration succeeded (new channel, or same schema_hash for re-registration).
     *         false if schema_hash differs from an existing entry (caller should reply SCHEMA_MISMATCH).
     */
    bool register_channel(const std::string& channel_name, ChannelEntry entry);

    /**
     * @brief Look up a channel by name.
     * @return The ChannelEntry, or std::nullopt if not found.
     */
    [[nodiscard]] std::optional<ChannelEntry> find_channel(const std::string& channel_name) const;

    /**
     * @brief Remove a channel from the registry.
     * @return true if the channel was found and the producer_pid matches; false otherwise.
     */
    bool deregister_channel(const std::string& channel_name, uint64_t producer_pid);

    [[nodiscard]] std::vector<std::string> list_channels() const;
    [[nodiscard]] size_t size() const;

private:
    std::unordered_map<std::string, ChannelEntry> m_channels;
};

} // namespace pylabhub::broker
