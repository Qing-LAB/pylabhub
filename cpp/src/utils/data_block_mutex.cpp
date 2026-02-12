#include "utils/data_block_mutex.hpp"
#include "plh_service.hpp" // For LOGGER_ERROR and LOGGER_INFO

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <windows.h>
#include <string> // For std::to_string
#else
#include <cerrno>  // For ETIMEDOUT, EOWNERDEAD
#include <pthread.h>
#include <string>  // For std::to_string
#include <time.h>  // For clock_gettime, CLOCK_REALTIME
#endif

namespace pylabhub::hub
{

// ============================================================================
// DataBlockMutex Implementation
// ============================================================================
//
// INTENTION: The implementation is split into separate Windows and POSIX blocks
// because the entire logic differs by platform:
//
//   - Windows: Uses a named kernel mutex (CreateMutexA/OpenMutexA). No shared
//     memory for the mutex itself; the mutex is a kernel object.
//
//   - POSIX: Uses pthread_mutex_t in shared memory. Either embedded in an
//     existing block (base != null) or in a dedicated shm segment (base == null).
//     The dedicated-segment path uses platform shm_create/shm_attach/shm_close.
//
// ============================================================================

#if defined(PYLABHUB_PLATFORM_WIN64)
// --- Windows: named kernel mutex ---
DataBlockMutex::DataBlockMutex(const std::string &name, void *base_shared_memory_address,
                               size_t offset_to_mutex_storage, bool is_creator)
    : m_name(name), m_is_creator(is_creator)
{
    // For Windows, base_shared_memory_address and offset_to_mutex_storage are ignored.
    // The name is derived from the shared memory's name to ensure uniqueness and association.
    // Use "Global\" prefix for session 0 compatibility and cross-session visibility.
    std::string mutex_name = "Global\\" + m_name + "_DataBlockManagementMutex";

    if (m_is_creator)
    {
        // Attempt to create the mutex. If it already exists, this opens an existing handle.
        m_mutex_handle = CreateMutexA(NULL,  // default security attributes
                                      FALSE, // initially not owned
                                      mutex_name.c_str());

        if (m_mutex_handle == NULL)
        {
            throw std::runtime_error("Windows DataBlockMutex: Failed to create mutex for '" +
                                     m_name + "'. Error: " + std::to_string(GetLastError()));
        }

        // If CreateMutex returns a valid handle and GetLastError is ERROR_ALREADY_EXISTS,
        // it means another process created it first. We still have a valid handle.
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            LOGGER_INFO(
                "Windows DataBlockMutex: Mutex '{}' already existed, opened existing handle.",
                mutex_name);
        }
        else
        {
            LOGGER_INFO("Windows DataBlockMutex: Mutex '{}' created.", mutex_name);
        }
    }
    else
    {
        // For non-creators, try to open an existing mutex.
        // We allow all access to be able to lock/unlock.
        m_mutex_handle = OpenMutexA(MUTEX_ALL_ACCESS,
                                    FALSE, // handle is not inheritable
                                    mutex_name.c_str());

        if (m_mutex_handle == NULL)
        {
            throw std::runtime_error("Windows DataBlockMutex: Failed to open mutex for '" + m_name +
                                     "'. Error: " + std::to_string(GetLastError()));
        }
        LOGGER_INFO("Windows DataBlockMutex: Mutex '{}' opened.", mutex_name);
    }
}

DataBlockMutex::~DataBlockMutex()
{
    if (m_mutex_handle != NULL)
    {
        CloseHandle(m_mutex_handle);
        m_mutex_handle = NULL;
        LOGGER_INFO("Windows DataBlockMutex: Mutex handle for '{}' closed.", m_name);
    }
}

void DataBlockMutex::lock()
{
    if (m_mutex_handle == NULL)
    {
        throw std::runtime_error(
            "Windows DataBlockMutex: Attempt to lock an invalid mutex handle for '" + m_name +
            "'.");
    }
    DWORD wait_result = WaitForSingleObject(m_mutex_handle, INFINITE);
    if (wait_result != WAIT_OBJECT_0)
    {
        // For robustness, check if the mutex was abandoned
        if (wait_result == WAIT_ABANDONED)
        {
            LOGGER_WARN("Windows DataBlockMutex: Mutex for '{}' was abandoned. Attempting to "
                        "acquire ownership.",
                        m_name);
            // On Windows, WAIT_ABANDONED means we still acquired the mutex.
            return;
        }
        throw std::runtime_error("Windows DataBlockMutex: Failed to lock mutex for '" + m_name +
                                 "'. Error: " + std::to_string(GetLastError()));
    }
}

bool DataBlockMutex::try_lock_for(int timeout_ms)
{
    if (m_mutex_handle == NULL)
    {
        throw std::runtime_error(
            "Windows DataBlockMutex: Attempt to lock an invalid mutex handle for '" + m_name +
            "'.");
    }
    DWORD ms = (timeout_ms <= 0) ? 0 : static_cast<DWORD>(timeout_ms);
    DWORD wait_result = WaitForSingleObject(m_mutex_handle, ms);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED)
    {
        if (wait_result == WAIT_ABANDONED)
            LOGGER_WARN("Windows DataBlockMutex: Mutex for '{}' was abandoned. Acquired.", m_name);
        return true;
    }
    if (wait_result == WAIT_TIMEOUT)
        return false;
    throw std::runtime_error("Windows DataBlockMutex: Wait failed for '" + m_name +
                             "'. Error: " + std::to_string(GetLastError()));
}

void DataBlockMutex::unlock()
{
    if (m_mutex_handle == NULL)
    {
        throw std::runtime_error(
            "Windows DataBlockMutex: Attempt to unlock an invalid mutex handle for '" + m_name +
            "'.");
    }
    if (!ReleaseMutex(m_mutex_handle))
    {
        throw std::runtime_error("Windows DataBlockMutex: Failed to unlock mutex for '" + m_name +
                                 "'. Error: " + std::to_string(GetLastError()));
    }
}

#else
// --- POSIX: pthread_mutex in shared memory (embedded or dedicated shm segment) ---
DataBlockMutex::DataBlockMutex(const std::string &name, void *base_shared_memory_address,
                               size_t offset_to_mutex_storage, bool is_creator)
    : m_name(name), m_is_creator(is_creator),
      m_base_shared_memory_address(base_shared_memory_address),
      m_offset_to_mutex_storage(offset_to_mutex_storage)
{
    // When base is null: create/attach to a dedicated shm segment for mutex storage.
    // Used by unit tests; Windows always ignores base and uses a named kernel mutex.
    // Uses platform shm_* API for consistency with DataBlock.
    if (!m_base_shared_memory_address)
    {
        std::string shm_name = m_name + "_DataBlockManagementMutex";
        const size_t mutex_size = sizeof(pthread_mutex_t);

        if (m_is_creator)
        {
            m_dedicated_shm = pylabhub::platform::shm_create(shm_name.c_str(), mutex_size,
                                                            pylabhub::platform::SHM_CREATE_EXCLUSIVE);
            if (m_dedicated_shm.base == nullptr)
            {
                m_dedicated_shm = pylabhub::platform::shm_create(
                    shm_name.c_str(), mutex_size, pylabhub::platform::SHM_CREATE_UNLINK_FIRST);
            }
            if (m_dedicated_shm.base == nullptr)
            {
                throw std::runtime_error("POSIX DataBlockMutex: shm_create failed for '" + m_name +
                                         "'.");
            }
        }
        else
        {
            m_dedicated_shm = pylabhub::platform::shm_attach(shm_name.c_str());
            if (m_dedicated_shm.base == nullptr)
            {
                throw std::runtime_error("POSIX DataBlockMutex: shm_attach failed for '" + m_name +
                                         "'.");
            }
        }
        m_base_shared_memory_address = m_dedicated_shm.base;
        m_offset_to_mutex_storage = 0;
    }

    // m_pthread_mutex is now calculated dynamically via get_pthread_mutex()
    pthread_mutex_t *mutex_ptr = get_pthread_mutex();

    if (m_is_creator)
    {
        pthread_mutexattr_t mattr;
        int res = pthread_mutexattr_init(&mattr);
        if (res != 0)
        {
            throw std::runtime_error("POSIX DataBlockMutex: pthread_mutexattr_init failed for '" +
                                     m_name + "'. Error: " + std::to_string(res));
        }
        res = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        if (res != 0)
        {
            pthread_mutexattr_destroy(&mattr);
            throw std::runtime_error(
                "POSIX DataBlockMutex: pthread_mutexattr_setpshared failed for '" + m_name +
                "'. Error: " + std::to_string(res));
        }

        // Use PTHREAD_MUTEX_ROBUST to handle mutexes left locked by dead processes
        // PTHREAD_MUTEX_NORMAL or PTHREAD_MUTEX_RECURSIVE
        res = pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_NORMAL);
        if (res == 0)
        {
            res = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
        }
        if (res == 0)
        {
            // Workaround for Linux kernel robust-futex bug (lkml.org/lkml/2013/9/27/338):
            // Without PTHREAD_PRIO_INHERIT, pthread_mutex_lock can block indefinitely instead
            // of returning EOWNERDEAD when the owner dies. PI uses exit_pi_state_list() which
            // handles wake-ups more reliably. Ignore ENOTSUP if the platform lacks PI support.
#if defined(PTHREAD_PRIO_INHERIT)
            (void)pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
#endif
        }

        if (res != 0)
        {
            pthread_mutexattr_destroy(&mattr);
            throw std::runtime_error(
                "POSIX DataBlockMutex: Failed to set robust mutex attributes for '" + m_name +
                "'. Error: " + std::to_string(res));
        }

        res = pthread_mutex_init(mutex_ptr, &mattr);
        if (res != 0)
        {
            pthread_mutexattr_destroy(&mattr);
            throw std::runtime_error("POSIX DataBlockMutex: pthread_mutex_init failed for '" +
                                     m_name + "'. Error: " + std::to_string(res));
        }
        pthread_mutexattr_destroy(&mattr);
        LOGGER_INFO(
            "POSIX DataBlockMutex: Mutex for '{}' created and initialized in shared memory.",
            m_name);
    }
    else
    {
        // Non-creators just get a pointer to the already initialized mutex in shared memory.
        // No explicit action needed here.
        LOGGER_INFO("POSIX DataBlockMutex: Mutex for '{}' attached from shared memory.", m_name);
    }
}

DataBlockMutex::~DataBlockMutex()
{
    // We intentionally do NOT call pthread_mutex_destroy. Reasons:
    // (1) The mutex lives in shared memory. When shm_close/shm_unlink releases the segment,
    //     the memory (and the mutex object) is reclaimed by the kernel. No leak.
    // (2) It is unpredictable which process is "last" to exit. Calling destroy while another
    //     process holds or waits on the mutex is undefined behavior. Skipping destroy avoids
    //     EBUSY/EOWNERDEAD races and timing issues entirely.

    // Dedicated-shm cleanup: when mutex used its own segment (base was null)
    if (m_dedicated_shm.base != nullptr)
    {
        pylabhub::platform::shm_close(&m_dedicated_shm);
        if (m_is_creator)
        {
            pylabhub::platform::shm_unlink((m_name + "_DataBlockManagementMutex").c_str());
        }
    }
}

void DataBlockMutex::lock()
{
    if (!m_base_shared_memory_address) // Check base address now
    {
        throw std::runtime_error(
            "POSIX DataBlockMutex: Attempt to lock an uninitialized mutex for '" + m_name + "'.");
    }
    pthread_mutex_t *mutex_ptr = get_pthread_mutex(); // Get dynamically calculated pointer
    int res = pthread_mutex_lock(mutex_ptr);
    if (res != 0)
    {
        // Handle EOWNERDEAD for robust mutexes
        if (res == EOWNERDEAD)
        {
            LOGGER_INFO("POSIX DataBlockMutex: Mutex for '{}' was abandoned by a dead owner. "
                        "Successfully acquired and marked consistent.",
                        m_name);
            // The mutex has been acquired, but its state is inconsistent.
            // We need to make it consistent.
            pthread_mutex_consistent(mutex_ptr);
            return; // Lock was acquired
        }
        throw std::runtime_error("POSIX DataBlockMutex: pthread_mutex_lock failed for '" + m_name +
                                 "'. Error: " + std::to_string(res));
    }
}

bool DataBlockMutex::try_lock_for(int timeout_ms)
{
    if (!m_base_shared_memory_address)
    {
        throw std::runtime_error(
            "POSIX DataBlockMutex: Attempt to lock an uninitialized mutex for '" + m_name + "'.");
    }
    pthread_mutex_t *mutex_ptr = get_pthread_mutex();
    if (timeout_ms <= 0)
    {
        int res = pthread_mutex_trylock(mutex_ptr);
        if (res == 0)
            return true;
        if (res == EOWNERDEAD)
        {
            LOGGER_INFO("POSIX DataBlockMutex: Mutex for '{}' was abandoned. Marked consistent.",
                        m_name);
            pthread_mutex_consistent(mutex_ptr);
            return true;
        }
        if (res == EBUSY)
            return false;
        throw std::runtime_error("POSIX DataBlockMutex: pthread_mutex_trylock failed for '" +
                                 m_name + "'. Error: " + std::to_string(res));
    }
    struct timespec abstime;
    if (clock_gettime(CLOCK_REALTIME, &abstime) != 0)
        throw std::runtime_error("POSIX DataBlockMutex: clock_gettime failed for '" + m_name + "'.");
    abstime.tv_sec += timeout_ms / 1000;
    abstime.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1'000'000;
    if (abstime.tv_nsec >= 1'000'000'000L)
    {
        abstime.tv_sec += 1;
        abstime.tv_nsec -= 1'000'000'000L;
    }
    int res = pthread_mutex_timedlock(mutex_ptr, &abstime);
    if (res == 0)
        return true;
    if (res == EOWNERDEAD)
    {
        LOGGER_INFO("POSIX DataBlockMutex: Mutex for '{}' was abandoned. Marked consistent.",
                    m_name);
        pthread_mutex_consistent(mutex_ptr);
        return true;
    }
    if (res == ETIMEDOUT)
        return false;
    throw std::runtime_error("POSIX DataBlockMutex: pthread_mutex_timedlock failed for '" + m_name +
                             "'. Error: " + std::to_string(res));
}

void DataBlockMutex::unlock()
{
    if (!m_base_shared_memory_address) // Check base address now
    {
        throw std::runtime_error(
            "POSIX DataBlockMutex: Attempt to unlock an uninitialized mutex for '" + m_name + "'.");
    }
    pthread_mutex_t *mutex_ptr = get_pthread_mutex(); // Get dynamically calculated pointer
    int res = pthread_mutex_unlock(mutex_ptr);
    if (res != 0)
    {
        throw std::runtime_error("POSIX DataBlockMutex: pthread_mutex_unlock failed for '" +
                                 m_name + "'. Error: " + std::to_string(res));
    }
}
#endif

// ============================================================================
// DataBlockLockGuard Implementation
// ============================================================================

DataBlockLockGuard::DataBlockLockGuard(DataBlockMutex &mutex) : m_mutex(mutex)
{
    m_mutex.lock();
}

DataBlockLockGuard::~DataBlockLockGuard()
{
    m_mutex.unlock();
}

} // namespace pylabhub::hub