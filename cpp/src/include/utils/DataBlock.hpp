#pragma once

#include "pylabhub_utils_export.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "shared_spin_lock.hpp" // Include for SharedSpinLock and SharedSpinLockGuard

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
    uint64_t magic_number;  // Magic constant to validate memory
    uint64_t shared_secret; // Key to prevent unauthorized access
    uint32_t version;       // Version of the header layout
    uint32_t header_size;   // sizeof(SharedMemoryHeader)

    // Consumer Management
    std::atomic<uint32_t> active_consumer_count;

    // Policy-Specific State and control primitives
    std::atomic<uint64_t> write_index;
    std::atomic<uint64_t> commit_index;
    std::atomic<uint64_t> read_index;
    std::atomic<uint64_t> current_slot_id; // For unique identification of data slots

    // Internal management mutex (OS-specific, protects the allocation map)
#if defined(PYLABHUB_PLATFORM_WIN64)
    // No direct in-memory storage for Windows named kernel mutex. Its name is derived.
#else
    char management_mutex_storage[64]; // Storage for pthread_mutex_t for internal management
#endif

    // Array of atomic-based spin-locks for user-facing data coordination
    // Each entry represents a mutex unit available for users.
    static constexpr size_t MAX_SHARED_SPINLOCKS = 8; // Example: up to 8 spinlocks available
    struct SharedSpinLockState
    {
        std::atomic<uint64_t> owner_pid{0};       // 0 means unlocked
        std::atomic<uint64_t> generation{0};      // Incremented on release, to mitigate PID reuse
        std::atomic<uint32_t> recursion_count{0}; // For recursive locking by same thread
        uint64_t owner_thread_id{0}; // Thread ID of lock holder (only valid if owner_pid != 0)
    } shared_spinlocks[MAX_SHARED_SPINLOCKS];

    // Map to track the allocation status of the shared spinlocks
    std::atomic_flag spinlock_allocated[MAX_SHARED_SPINLOCKS]; // True if allocated, false if free
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

    /**
     * @brief Acquires a user-facing SharedSpinLock instance from the DataBlock.
     * @param debug_name A name for logging/debugging purposes.
     * @return A unique_ptr to a SharedSpinLockGuard for the acquired lock.
     * @throws std::runtime_error if no free spinlocks are available.
     */
    virtual std::unique_ptr<SharedSpinLockGuard>
    acquire_user_spinlock(const std::string &debug_name) = 0;

    /**
     * @brief Releases a user-facing SharedSpinLock instance by its index.
     * @param index The index of the spinlock to release.
     */
    virtual void release_user_spinlock(size_t index) = 0;
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

    /**
     * @brief Gets a SharedSpinLock instance by its index from the DataBlock for direct use.
     * @param index The index of the spinlock to retrieve.
     * @return A SharedSpinLock object.
     * @throws std::out_of_range if the index is invalid.
     */
    virtual SharedSpinLock get_user_spinlock(size_t index) = 0;
};
