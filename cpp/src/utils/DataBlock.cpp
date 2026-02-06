#include "plh_service.hpp"
#include "utils/DataBlock.hpp"
#include "utils/MessageHub.hpp"
#include "utils/shared_memory_mutex.hpp" // Include the new DataBlockMutex header
#include <cstddef>                       // For offsetof
#include <stdexcept>
#include <thread> // For std::this_thread::sleep_for
#include <chrono> // For std::chrono::milliseconds

namespace pylabhub::hub
{

// ============================================================================
// Constants
// ============================================================================
namespace
{
static constexpr uint64_t DATABLOCK_MAGIC_NUMBER = 0xBADF00DFEEDFACEL;
static constexpr uint32_t DATABLOCK_VERSION = 1;
} // namespace

// ============================================================================
// Internal DataBlock Helper Class
// ============================================================================

/**
 * @class DataBlock
 * @brief Internal helper class to manage the shared memory segment for a DataBlock.
 *
 * This class encapsulates the creation, mapping, and destruction of the underlying
 * shared memory, providing access to the pylabhub::hub::SharedMemoryHeader and data regions.
 */
class DataBlock
{
  public:
    // For producer: create and initialize shared memory
    DataBlock(const std::string &name, const pylabhub::hub::DataBlockConfig &config)
        : m_name(name), m_is_creator(true)
    {
        // Calculate total size needed for shared memory
        // Header + Flexible Data Zone + Structured Data Buffer
        m_size = sizeof(pylabhub::hub::SharedMemoryHeader) + config.flexible_zone_size +
                 config.structured_buffer_size;

#if defined(PYLABHUB_PLATFORM_WIN64)
        m_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(m_size), m_name.c_str());
        if (m_shm_handle == NULL)
        {
            throw std::runtime_error("Failed to create file mapping.");
        }

        m_mapped_address = MapViewOfFile(m_shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, m_size);
        if (m_mapped_address == NULL)
        {
            CloseHandle(m_shm_handle);
            throw std::runtime_error("Failed to map view of file.");
        }
#else
        // Remove any previous shared memory with the same name
        shm_unlink(m_name.c_str());

        m_shm_fd = shm_open(m_name.c_str(), O_CREAT | O_RDWR, 0666);
        if (m_shm_fd == -1)
        {
            throw std::runtime_error("shm_open failed.");
        }

        if (ftruncate(m_shm_fd, m_size) == -1)
        {
            close(m_shm_fd);
            shm_unlink(m_name.c_str());
            throw std::runtime_error("ftruncate failed.");
        }

        m_mapped_address = mmap(NULL, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
        if (m_mapped_address == MAP_FAILED)
        {
            close(m_shm_fd);
            shm_unlink(m_name.c_str());
            throw std::runtime_error("mmap failed.");
        }
#endif

        // Construct the pylabhub::hub::SharedMemoryHeader in-place
        m_header = new (m_mapped_address) pylabhub::hub::SharedMemoryHeader();

        // CRITICAL INITIALIZATION ORDER:
        // 1. Zero-initialize all fields (done by placement new above)
        // 2. Initialize management mutex FIRST
        // 3. Initialize other fields while mutex exists
        // 4. Set magic_number LAST as the "ready" flag

        // Step 1: Mark as uninitialized
        m_header->init_state.store(0, std::memory_order_release);

        // Step 2: Initialize the management mutex BEFORE anything else
#if defined(PYLABHUB_PLATFORM_WIN64)
        PLH_DEBUG("DataBlock '{}': Initializing Windows management mutex.", m_name);
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, nullptr, 0, m_is_creator); // Offset is ignored for Windows
#else
        PLH_DEBUG("DataBlock '{}': Initializing POSIX management mutex at {}.", m_name,
                  (void *)((char *)m_mapped_address +
                           offsetof(SharedMemoryHeader, management_mutex_storage)));
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, m_mapped_address, offsetof(SharedMemoryHeader, management_mutex_storage),
            m_is_creator);
#endif
        PLH_DEBUG("DataBlock '{}': Management mutex initialized.", m_name);

        // Mark mutex as ready
        m_header->init_state.store(1, std::memory_order_release);

        // Step 3: Initialize other header fields (mutex protects this now)
        m_header->shared_secret = config.shared_secret;
        m_header->version = DATABLOCK_VERSION;
        m_header->header_size = sizeof(pylabhub::hub::SharedMemoryHeader);
        m_header->active_consumer_count.store(0, std::memory_order_release);
        m_header->write_index.store(0, std::memory_order_release);
        m_header->commit_index.store(0, std::memory_order_release);
        m_header->read_index.store(0, std::memory_order_release);
        m_header->current_slot_id.store(0, std::memory_order_release);

        // Initialize atomic flags for spinlock allocation
        for (size_t i = 0; i < SharedMemoryHeader::MAX_SHARED_SPINLOCKS; ++i)
        {
            m_header->spinlock_allocated[i].clear(std::memory_order_release);
        }

        // Get pointers to the flexible data zone and structured buffer
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_header) + sizeof(pylabhub::hub::SharedMemoryHeader);
        m_structured_data_buffer = m_flexible_data_zone + config.flexible_zone_size;

        // Step 4: Set magic_number LAST - this signals the DataBlock is fully initialized
        // Use memory_order_release to ensure all prior writes are visible
        std::atomic_thread_fence(std::memory_order_release);
        m_header->magic_number = DATABLOCK_MAGIC_NUMBER;
        m_header->init_state.store(2, std::memory_order_release);

        LOGGER_INFO("DataBlock '{}' created with total size {} bytes.", m_name, m_size);
    }

    // For consumer: open existing shared memory
    DataBlock(const std::string &name) : m_name(name), m_is_creator(false)
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        m_shm_handle = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, m_name.c_str());
        if (m_shm_handle == NULL)
        {
            throw std::runtime_error("Failed to open file mapping.");
        }

        m_mapped_address = MapViewOfFile(m_shm_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        if (m_mapped_address == NULL)
        {
            CloseHandle(m_shm_handle);
            throw std::runtime_error("Failed to map view of file for consumer.");
        }
#else
        m_shm_fd = shm_open(m_name.c_str(), O_RDWR, 0666);
        if (m_shm_fd == -1)
        {
            throw std::runtime_error("shm_open failed for consumer.");
        }

        struct stat shm_stat;
        if (fstat(m_shm_fd, &shm_stat) == -1)
        {
            close(m_shm_fd);
            throw std::runtime_error("fstat failed.");
        }
        m_size = shm_stat.st_size;

        m_mapped_address = mmap(NULL, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
        if (m_mapped_address == MAP_FAILED)
        {
            close(m_shm_fd);
            throw std::runtime_error("mmap failed for consumer.");
        }
#endif

        m_header = reinterpret_cast<pylabhub::hub::SharedMemoryHeader *>(m_mapped_address);

        // CRITICAL: Wait for producer to finish initialization before proceeding
        // Check init_state with acquire semantics to ensure we see all producer's writes
        const int max_wait_ms = 5000; // 5 second timeout
        const int poll_interval_ms = 10;
        int total_wait_ms = 0;

        uint32_t init_state = m_header->init_state.load(std::memory_order_acquire);
        while (init_state < 2 && total_wait_ms < max_wait_ms)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
            total_wait_ms += poll_interval_ms;
            init_state = m_header->init_state.load(std::memory_order_acquire);
        }

        if (init_state < 2)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            UnmapViewOfFile(m_mapped_address);
            CloseHandle(m_shm_handle);
#else
            munmap(m_mapped_address, m_size);
            close(m_shm_fd);
#endif
            throw std::runtime_error(
                "DataBlock '" + m_name +
                "' initialization timeout - producer may have crashed during setup.");
        }

        // Validate magic number with acquire semantics
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_header->magic_number != DATABLOCK_MAGIC_NUMBER)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            UnmapViewOfFile(m_mapped_address);
            CloseHandle(m_shm_handle);
#else
            munmap(m_mapped_address, m_size);
            close(m_shm_fd);
#endif
            throw std::runtime_error(
                "DataBlock '" + m_name +
                "' has invalid magic number - not a valid DataBlock or corrupted.");
        }

        // Validate version
        if (m_header->version != DATABLOCK_VERSION)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            UnmapViewOfFile(m_mapped_address);
            CloseHandle(m_shm_handle);
#else
            munmap(m_mapped_address, m_size);
            close(m_shm_fd);
#endif
            throw std::runtime_error("DataBlock '" + m_name + "' version mismatch. Expected " +
                                     std::to_string(DATABLOCK_VERSION) + ", got " +
                                     std::to_string(m_header->version));
        }

        // Now it's safe to attach to the management mutex
#if defined(PYLABHUB_PLATFORM_WIN64)
        PLH_DEBUG("DataBlock '{}': Attaching to Windows management mutex.", m_name);
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, nullptr, 0, m_is_creator); // Offset is ignored for Windows
#else
        PLH_DEBUG("DataBlock '{}': Attaching to POSIX management mutex at {}.", m_name,
                  (void *)((char *)m_mapped_address +
                           offsetof(SharedMemoryHeader, management_mutex_storage)));
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, m_mapped_address, offsetof(SharedMemoryHeader, management_mutex_storage),
            m_is_creator);
#endif
        PLH_DEBUG("DataBlock '{}': Management mutex attached.", m_name);

        // Get pointers to the flexible data zone and structured buffer
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_header) + sizeof(pylabhub::hub::SharedMemoryHeader);
        m_structured_data_buffer = nullptr; // Will be set after discovery

        LOGGER_INFO("DataBlock '{}' opened by consumer.", m_name);
    }

    ~DataBlock();

    // Accessors declarations
    pylabhub::hub::SharedMemoryHeader *header() const;
    char *flexible_data_zone() const;
    char *structured_data_buffer() const;
    void *segment() const;

    // Methods to manage shared spinlocks declarations
    size_t acquire_shared_spinlock(const std::string &debug_name);
    void release_shared_spinlock(size_t index);
    SharedSpinLockState *get_shared_spinlock_state(size_t index);

  private:
    std::string m_name;
    bool m_is_creator;
    size_t m_size = 0;

#if defined(PYLABHUB_PLATFORM_WIN64)
    HANDLE m_shm_handle = NULL;
    LPVOID m_mapped_address = NULL;
#else
    int m_shm_fd = -1;
    void *m_mapped_address = nullptr;
#endif

    pylabhub::hub::SharedMemoryHeader *m_header{nullptr};
    char *m_flexible_data_zone{nullptr};
    char *m_structured_data_buffer{nullptr};
    std::unique_ptr<DataBlockMutex> m_management_mutex; // Internal OS-specific mutex for management
}; // End of class DataBlock

// ============================================================================
// DataBlock Method Implementations (outside class definition)
// ============================================================================

// Constructors (already defined)

// Destructor (already defined)
DataBlock::~DataBlock()
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    UnmapViewOfFile(m_mapped_address);
    CloseHandle(m_shm_handle);
#else
    // For creator, explicitly destroy the management mutex while memory is still mapped.
    // This calls ~DataBlockMutex() which attempts pthread_mutex_destroy.
    if (m_is_creator && m_management_mutex)
    {
        m_management_mutex.reset(); // Calls ~DataBlockMutex()
    }

    munmap(m_mapped_address, m_size);
    close(m_shm_fd);
    if (m_is_creator)
    {
        shm_unlink(m_name.c_str());
        LOGGER_INFO("DataBlock '{}' shared memory removed.", m_name);
    }
#endif
}

// Accessors implementations
pylabhub::hub::SharedMemoryHeader *DataBlock::header() const
{
    return m_header;
}
char *DataBlock::flexible_data_zone() const
{
    return m_flexible_data_zone;
}
char *DataBlock::structured_data_buffer() const
{
    return m_structured_data_buffer;
}
void *DataBlock::segment() const
{
    return m_mapped_address;
}

// Methods to manage shared spinlocks implementations
SharedSpinLockState *DataBlock::get_shared_spinlock_state(size_t index)
{
    if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
    {
        LOGGER_ERROR(
            "DataBlock '{}': Attempted to access shared spinlock at invalid index {}. Max is {}.",
            m_name, index, SharedMemoryHeader::MAX_SHARED_SPINLOCKS - 1);
        throw std::out_of_range("Shared spinlock index out of range.");
    }
    return &m_header->shared_spinlocks[index];
}

size_t DataBlock::acquire_shared_spinlock(const std::string &debug_name)
{
    DataBlockLockGuard lock(*m_management_mutex); // Protect allocation map

    for (size_t i = 0; i < SharedMemoryHeader::MAX_SHARED_SPINLOCKS; ++i)
    {
        // test_and_set returns true if the flag was ALREADY set, false if it was clear and is now
        // set. We want to acquire a slot that is currently CLEAR (false).
        if (!m_header->spinlock_allocated[i].test_and_set(std::memory_order_acq_rel))
        {
            // Found a free slot, marked it as allocated.
            // Initialize the spinlock state for this slot.
            m_header->shared_spinlocks[i].owner_pid.store(0, std::memory_order_release);
            m_header->shared_spinlocks[i].generation.store(0, std::memory_order_release);
            m_header->shared_spinlocks[i].recursion_count.store(0, std::memory_order_release);
            m_header->shared_spinlocks[i].owner_thread_id =
                0; // Not atomic, written by owner process

            LOGGER_INFO("DataBlock '{}': Acquired shared spinlock slot {} for '{}'.", m_name, i,
                        debug_name);
            return i;
        }
    }
    throw std::runtime_error("DataBlock '" + m_name +
                             "': No free shared spinlock slots available.");
}

void DataBlock::release_shared_spinlock(size_t index)
{
    if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
    {
        LOGGER_ERROR(
            "DataBlock '{}': Attempted to release shared spinlock at invalid index {}. Max is {}.",
            m_name, index, SharedMemoryHeader::MAX_SHARED_SPINLOCKS - 1);
        throw std::out_of_range("Shared spinlock index out of range.");
    }

    DataBlockLockGuard lock(*m_management_mutex); // Protect allocation map

    // Ensure the spinlock is not currently held by any process before marking it free.
    // A robust check would also involve checking generation counts, but for simple release,
    // confirming owner_pid == 0 is typically sufficient as a safety measure.
    if (m_header->shared_spinlocks[index].owner_pid.load(std::memory_order_acquire) != 0)
    {
        LOGGER_WARN("DataBlock '{}': Releasing allocated shared spinlock slot {} which is still "
                    "held by PID {}. Force releasing.",
                    m_name, index, m_header->shared_spinlocks[index].owner_pid.load());
        // Force clear for robustness, but log warning.
    }

    // Clear the allocated flag. This makes the slot available for re-acquisition.
    m_header->spinlock_allocated[index].clear(std::memory_order_release);

    // Reset state to ensure clean start for next acquirer (though acquire_shared_spinlock also does
    // this).
    m_header->shared_spinlocks[index].owner_pid.store(0, std::memory_order_release);
    m_header->shared_spinlocks[index].generation.store(0, std::memory_order_release);
    m_header->shared_spinlocks[index].recursion_count.store(0, std::memory_order_release);
    m_header->shared_spinlocks[index].owner_thread_id = 0;

    LOGGER_INFO("DataBlock '{}': Released shared spinlock slot {}.", m_name, index);
}

class DataBlockProducerImpl : public IDataBlockProducer
{
  public:
    DataBlockProducerImpl(const std::string &name, const pylabhub::hub::DataBlockConfig &config)
    try : m_dataBlock(std::make_unique<DataBlock>(name, config))
    {
        m_name = name;
        LOGGER_INFO("DataBlockProducerImpl: Initialized for '{}'.", m_name);
    }
    catch (const std::runtime_error &ex)
    {
        LOGGER_ERROR("DataBlockProducerImpl: Failed to create DataBlock for '{}': {}", name,
                     ex.what());
        throw; // Re-throw to indicate construction failure
    }

    ~DataBlockProducerImpl() override
    {
        // Producer destructor, cleanup as needed
        LOGGER_INFO("DataBlockProducerImpl: Shutting down for '{}'.", m_name);
    }

    // Producer specific methods
    std::unique_ptr<SharedSpinLockGuard>
    acquire_user_spinlock(const std::string &debug_name) override
    {
        size_t index = m_dataBlock->acquire_shared_spinlock(debug_name);
        
        // WARNING: Design Flaw - Potential for dangling reference!
        // SharedSpinLockGuard currently takes a SharedSpinLock&.
        // If the SharedSpinLock is created as a temporary here, it will be destroyed
        // at the end of this function, leading to a dangling reference in the returned guard.
        // This needs to be addressed by modifying SharedSpinLockGuard to own the SharedSpinLock,
        // or by rethinking the return type of this interface.
        // See docs/tech_draft/shared_spinlock_guard_design_review.md for details.
        
        // For compilation purposes, we create a temporary SharedSpinLock.
        // This is UNSAFE for actual runtime use if the guard outlives this function.
        SharedSpinLock temp_lock(m_dataBlock->get_shared_spinlock_state(index), debug_name);
        return std::make_unique<SharedSpinLockGuard>(temp_lock);
    }

    void release_user_spinlock(size_t index) override
    {
        m_dataBlock->release_shared_spinlock(index);
    }


  private:
    std::string m_name;
    std::unique_ptr<DataBlock> m_dataBlock;
};

// ============================================================================
// DataBlockConsumer Implementation
// ============================================================================

class DataBlockConsumerImpl : public IDataBlockConsumer
{
  public:
    DataBlockConsumerImpl(const std::string &name, uint64_t shared_secret)
    try : m_dataBlock(std::make_unique<DataBlock>(name))
    {
        m_name = name;

        // Verify magic number and shared secret
        if (m_dataBlock->header()->magic_number != DATABLOCK_MAGIC_NUMBER)
        {
            LOGGER_ERROR("DataBlockConsumerImpl: Invalid magic number for '{}'.", name);
            throw std::runtime_error("Invalid magic number");
        }
        if (m_dataBlock->header()->shared_secret != shared_secret)
        {
            LOGGER_ERROR("DataBlockConsumerImpl: Invalid shared secret for '{}'.", name);
            throw std::runtime_error("Invalid shared secret");
        }

        m_dataBlock->header()->active_consumer_count.fetch_add(1, std::memory_order_acq_rel);
        LOGGER_INFO("DataBlockConsumerImpl: Initialized for '{}'. Active consumers: {}.", m_name,
                    m_dataBlock->header()->active_consumer_count.load());
    }
    catch (const std::runtime_error &ex)
    {
        LOGGER_ERROR("DataBlockConsumerImpl: Failed to open DataBlock for '{}': {}", name,
                     ex.what());
        throw; // Re-throw to indicate construction failure
    }

    ~DataBlockConsumerImpl() override
    {
        if (m_dataBlock && m_dataBlock->header())
        {
            m_dataBlock->header()->active_consumer_count.fetch_sub(1, std::memory_order_acq_rel);
            LOGGER_INFO("DataBlockConsumerImpl: Shutting down for '{}'. Active consumers: {}.",
                        m_name, m_dataBlock->header()->active_consumer_count.load());
        }
        else
        {
            LOGGER_WARN(
                "DataBlockConsumerImpl: Shutting down for '{}' but DataBlock or header was null.",
                m_name);
        }
    }

    // Consumer specific methods
    SharedSpinLock get_user_spinlock(size_t index) override
    {
        return SharedSpinLock(m_dataBlock->get_shared_spinlock_state(index),
                              m_name + "_spinlock_" + std::to_string(index));
    }

  private:
    std::string m_name;
    std::unique_ptr<DataBlock> m_dataBlock;
};

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<IDataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                          const pylabhub::hub::DataBlockConfig &config)
{
    (void)hub;    // MessageHub will be used for registration in future steps
    (void)policy; // Policy will influence DataBlock's internal management

    try
    {
        return std::make_unique<DataBlockProducerImpl>(name, config);
    }
    catch (const std::runtime_error &ex)
    {
        LOGGER_ERROR("create_datablock_producer: Failed to create producer for '{}': {}", name,
                     ex.what());
        return nullptr;
    }
    catch (const std::bad_alloc &ex)
    {
        LOGGER_ERROR("create_datablock_producer: Memory allocation failed for '{}': {}", name,
                     ex.what());
        return nullptr;
    }
}

std::unique_ptr<IDataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret)
{
    (void)hub; // MessageHub will be used for discovery in future steps

    try
    {
        return std::make_unique<DataBlockConsumerImpl>(name, shared_secret);
    }
    catch (const std::runtime_error &ex)
    {
        LOGGER_ERROR("find_datablock_consumer: Failed to create consumer for '{}': {}", name,
                     ex.what());
        return nullptr;
    }
    catch (const std::bad_alloc &ex)
    {
        LOGGER_ERROR("find_datablock_consumer: Memory allocation failed for '{}': {}", name,
                     ex.what());
        return nullptr;
    }
}

} // namespace pylabhub::hub