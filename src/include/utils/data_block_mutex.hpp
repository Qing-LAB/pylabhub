#pragma once
/**
 * @file data_block_mutex.hpp
 * @brief Cross-process mutex for DataBlock management structures.
 */
#include "pylabhub_utils_export.h"
#include "plh_platform.hpp"

#include <string>
#include <stdexcept>
#include <mutex>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "data_block.hpp"

namespace pylabhub::hub
{

/**
 * @class DataBlockMutex
 * @brief Cross-process mutex that protects the DataBlock control zone (SharedMemoryHeader).
 *
 * This mutex coordinates access to critical metadata in the SharedMemoryHeader: chain links,
 * spinlock allocation state, counters, indices, etc. It is the internal management mutex
 * for DataBlock operations—not a general-purpose mutex.
 *
 * On POSIX: pthread_mutex_t with PTHREAD_PROCESS_SHARED, stored either inside the DataBlock's
 * shared memory (SharedMemoryHeader::management_mutex_storage) or in a dedicated shm segment
 * when base_shared_memory_address is null (e.g. unit tests).
 * On Windows: named kernel mutex; base_shared_memory_address is ignored.
 */
class PYLABHUB_UTILS_EXPORT DataBlockMutex
{
  public:
    /**
     * @brief Constructs a DataBlockMutex.
     * @param name The unique name of the DataBlock, used to derive the mutex name on Windows.
     * @param base_shared_memory_address Base of the shared memory containing the mutex storage.
     *        For a DataBlock, this is the mapped SharedMemoryHeader; offset_to_mutex_storage
     *        points to management_mutex_storage. May be null: Windows ignores it; on POSIX,
     *        a dedicated shm segment is created (e.g. for unit tests).
     * @param offset_to_mutex_storage Offset from base to the mutex storage.
     * @param is_creator True if this process is creating the mutex.
     * @throws std::runtime_error on mutex creation/opening failure.
     */
    DataBlockMutex(std::string name, void *base_shared_memory_address,
                   size_t offset_to_mutex_storage, bool is_creator);

    /**
     * @brief Destroys the DataBlockMutex, releasing OS resources.
     * On POSIX, only the creator destroys the mutex.
     */
    ~DataBlockMutex();

    // Delete copy/move operations
    DataBlockMutex(const DataBlockMutex &) = delete;
    DataBlockMutex &operator=(const DataBlockMutex &) = delete;
    DataBlockMutex(DataBlockMutex &&) noexcept = delete;
    DataBlockMutex &operator=(DataBlockMutex &&) noexcept = delete;

    /**
     * @brief Acquires the mutex, blocking if necessary.
     */
    void lock();

    /**
     * @brief Tries to acquire the mutex within a timeout.
     * @param timeout_ms Maximum wait in milliseconds (0 = try once, no wait).
     * @return true if the mutex was acquired (including after EOWNERDEAD/WAIT_ABANDONED recovery),
     *         false if the timeout expired without acquiring.
     * @throws std::runtime_error on other errors (invalid state, etc.).
     */
    bool try_lock_for(int timeout_ms);

    /**
     * @brief Releases the mutex.
     */
    void unlock();

  private:
    std::string m_name;
    bool m_is_creator;

#if defined(PYLABHUB_PLATFORM_WIN64)
    HANDLE m_mutex_handle{NULL};
#else
    void *m_base_shared_memory_address{nullptr};
    size_t m_offset_to_mutex_storage{0};
    pylabhub::platform::ShmHandle m_dedicated_shm{}; // When base is null: dedicated shm for mutex
    pthread_mutex_t *get_pthread_mutex() const
    {
        return reinterpret_cast<pthread_mutex_t *>(
            static_cast<char *>(m_base_shared_memory_address) + m_offset_to_mutex_storage);
    }
#endif
};

/**
 * @class DataBlockLockGuard
 * @brief RAII guard for DataBlockMutex.
 *
 * Automatically locks the mutex on construction and unlocks it on destruction.
 */
class PYLABHUB_UTILS_EXPORT DataBlockLockGuard
{
  public:
    explicit DataBlockLockGuard(DataBlockMutex &mutex);
    ~DataBlockLockGuard();

    DataBlockLockGuard(const DataBlockLockGuard &) = delete;
    DataBlockLockGuard &operator=(const DataBlockLockGuard &) = delete;
    DataBlockLockGuard(DataBlockLockGuard &&) noexcept = delete;
    DataBlockLockGuard &operator=(DataBlockLockGuard &&) noexcept = delete;

  private:
    DataBlockMutex &m_mutex;
};

} // namespace pylabhub::hub
