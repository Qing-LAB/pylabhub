#pragma once

#include "pylabhub_utils_export.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pylabhub::hub
{

// Forward declaration for the factory
class MessageHub;

/**
 * @enum DataBlockPolicy
 * @brief Defines the buffer management strategy for a DataBlock.
 */
enum class DataBlockPolicy
{
    Single,
    DoubleBuffer,
    RingBuffer
};

/**
 * @struct DataBlockConfig
 * @brief Configuration for creating a new DataBlock.
 */
struct DataBlockConfig
{
    uint64_t shared_secret;
    size_t structured_buffer_size;
    size_t flexible_zone_size;
    int ring_buffer_capacity; // Only used for RingBuffer policy
};

/**
 * @struct SharedMemoryHeader
 * @brief The header structure for every DataBlock shared memory segment.
 *
 * This structure contains metadata, security features, and synchronization
 * primitives for coordinating access between a producer and consumers.
 */
struct SharedMemoryHeader
{
    // Safety & Identification
    uint64_t magic_number;    // Magic constant to validate memory
    uint64_t shared_secret;   // Key to prevent unauthorized access
    uint32_t version;         // Version of the header layout
    uint32_t header_size;     // sizeof(SharedMemoryHeader)

    // Consumer Management
    std::atomic<uint32_t> active_consumer_count;

    // Policy-Specific State and control primitives will be added here
    // e.g., for a ring buffer:
    // std::atomic<uint64_t> write_index;
    // std::atomic<uint64_t> commit_index;

    // Process-Shared Mutex storage
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows doesn't store mutex handles in shared memory. Named objects are used.
#else
    char mutex_storage[64];   // Storage for pthread_mutex_t
#endif
};

/**
 * @class IDataBlockProducer
 * @brief An interface for a DataBlock producer.
 */
class IDataBlockProducer
{
  public:
    virtual ~IDataBlockProducer() = default;
    // Pure virtual methods for producing data will be defined here.
};

/**
 * @class IDataBlockConsumer
 * @brief An interface for a DataBlock consumer.
 */
class IDataBlockConsumer
{
  public:
    virtual ~IDataBlockConsumer() = default;
    // Pure virtual methods for consuming data will be defined here.
};

/**
 * @brief Factory function to create a DataBlock producer.
 * @param hub A connected MessageHub instance for broker communication.
 * @param name The unique name for the DataBlock channel.
 * @param policy The buffer management policy to use.
 * @param config The configuration for the DataBlock.
 * @return A unique_ptr to the producer, or nullptr on failure.
 */
PYLABHUB_UTILS_EXPORT std::unique_ptr<IDataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                          const DataBlockConfig &config);

/**
 * @brief Factory function to find and connect to a DataBlock as a consumer.
 * @param hub A connected MessageHub instance for broker communication.
 * @param name The name of the DataBlock channel to find.
 * @param shared_secret The secret required to access the channel.
 * @return A unique_ptr to the consumer, or nullptr on failure.
 */
PYLABHUB_UTILS_EXPORT std::unique_ptr<IDataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret);

} // namespace pylabhub::hub
