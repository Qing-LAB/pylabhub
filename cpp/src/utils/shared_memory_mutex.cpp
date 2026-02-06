#include "utils/shared_memory_mutex.hpp"
#include "plh_service.hpp" // For LOGGER_ERROR and LOGGER_INFO

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <windows.h>
#include <string> // For std::to_string
#else
#include <cerrno> // For errno
#include <string> // For std::to_string
#endif

namespace pylabhub::hub
{

// ============================================================================
// DataBlockMutex Implementation
// ============================================================================

#if defined(PYLABHUB_PLATFORM_WIN64)
// Windows implementation
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
// POSIX implementation
DataBlockMutex::DataBlockMutex(const std::string &name, void *base_shared_memory_address,
                               size_t offset_to_mutex_storage, bool is_creator)
    : m_name(name), m_is_creator(is_creator),
      m_base_shared_memory_address(base_shared_memory_address),
      m_offset_to_mutex_storage(offset_to_mutex_storage)
{
    if (!m_base_shared_memory_address)
    {
        throw std::runtime_error(
            "POSIX DataBlockMutex: base_shared_memory_address cannot be null.");
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
    // Only the creator should destroy the mutex.
    // This must be done while no other process is holding or waiting on the mutex.
    // If a process dies while holding the mutex, its state becomes abandoned.
    // A robust mutex allows another process to acquire it and then "clean up" the abandoned state.
    // It is generally not safe to destroy a robust mutex if it might be abandoned or held.
    // The safest approach is to only destroy it if this process is the creator AND it's certain no
    // other process is using it. Given the multi-tiered locking, this management mutex should
    // ideally be destroyed only when the *entire* DataBlock is no longer in use by any process. For
    // now, we will destroy it only if we are the creator and can ensure it's not locked. This needs
    // careful thought in the overall DataBlock cleanup strategy.

    // A simpler strategy for now: if this is the creator, try to destroy.
    // Robust mutexes often don't need explicit destruction if the shared memory block is unlinked
    // and destroyed, as their resources are often tied to the memory itself.
    if (m_base_shared_memory_address && m_is_creator)
    {
        pthread_mutex_t *mutex_ptr = get_pthread_mutex();
        // Attempt to lock and unlock it to ensure it's not abandoned and we can destroy it safely.
        // This is a common pattern to ensure the mutex is in a destroyable state.
        int res = pthread_mutex_trylock(mutex_ptr);
        if (res == 0)
        { // Successfully locked
            pthread_mutex_unlock(mutex_ptr);
            res = pthread_mutex_destroy(mutex_ptr);
            if (res != 0)
            {
                LOGGER_ERROR(
                    "POSIX DataBlockMutex: pthread_mutex_destroy failed for '{}'. Error: {} ({})",
                    m_name, std::strerror(res), res);
            }
            else
            {
                LOGGER_INFO("POSIX DataBlockMutex: Mutex for '{}' destroyed.", m_name);
            }
        }
        else if (res == EBUSY)
        {
            LOGGER_WARN(
                "POSIX DataBlockMutex: Mutex for '{}' is busy and cannot be destroyed by creator.",
                m_name);
        }
        else if (res == EOWNERDEAD)
        {
            LOGGER_WARN("POSIX DataBlockMutex: Mutex for '{}' is in an abandoned state. Will "
                        "attempt to destroy.",
                        m_name);
            // If EOWNERDEAD, we should unlock and then destroy.
            pthread_mutex_unlock(mutex_ptr); // Unlock the abandoned mutex
            res = pthread_mutex_destroy(mutex_ptr);
            if (res != 0)
            {
                LOGGER_ERROR("POSIX DataBlockMutex: pthread_mutex_destroy failed for abandoned "
                             "mutex '{}'. Error: {} ({})",
                             m_name, std::strerror(res), res);
            }
            else
            {
                LOGGER_INFO("POSIX DataBlockMutex: Mutex for abandoned '{}' destroyed.", m_name);
            }
        }
        else
        {
            LOGGER_ERROR("POSIX DataBlockMutex: pthread_mutex_trylock failed unexpectedly for "
                         "'{}'. Error: {} ({})",
                         m_name, std::strerror(res), res);
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
            LOGGER_WARN("POSIX DataBlockMutex: Mutex for '{}' was abandoned by a dead owner. "
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