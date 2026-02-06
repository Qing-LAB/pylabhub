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
 * @brief Provides cross-process, cross-platform mutex synchronization for DataBlock's *internal
 * management structures*.
 *
 * This mutex is intended to protect operations on the `SharedMemoryHeader` itself,
 * such as allocating/freeing `SharedSpinLockState` units. It relies on OS-specific,
 * robust primitives.
 * On POSIX, this uses `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED` stored directly
 * in the shared memory segment.
 * On Windows, this uses a named kernel mutex, with the name derived from the DataBlock's unique
 * name.
 */
class PYLABHUB_UTILS_EXPORT DataBlockMutex
{
  public:
    /**
     * @brief Constructs a DataBlockMutex.
     * @param name The unique name of the DataBlock, used to derive the mutex name on Windows.
     * @param base_shared_memory_address A pointer to the base address of the shared memory segment.
     * @param offset_to_mutex_storage The offset from `base_shared_memory_address` where
     * `management_mutex_storage` is located.
     * @param is_creator True if this process is creating the DataBlock (and thus the mutex).
     * @throws std::runtime_error on mutex creation/opening failure.
     */
    DataBlockMutex(const std::string &name, void *base_shared_memory_address,
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
