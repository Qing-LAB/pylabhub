#include "plh_service.hpp"
#include "utils/DataBlock.hpp"
#include "utils/MessageHub.hpp"
#include <stdexcept>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

        // construct the pylabhub::hub::SharedMemoryHeader
        m_header = new (m_mapped_address) pylabhub::hub::SharedMemoryHeader();

        // Initialize header fields
        m_header->magic_number = DATABLOCK_MAGIC_NUMBER;
        m_header->shared_secret = config.shared_secret;
        m_header->version = DATABLOCK_VERSION;
        m_header->header_size = sizeof(pylabhub::hub::SharedMemoryHeader);
        m_header->active_consumer_count.store(0, std::memory_order_release);
        m_header->write_index.store(0, std::memory_order_release);
        m_header->commit_index.store(0, std::memory_order_release);
        m_header->read_index.store(0, std::memory_order_release);
        m_header->current_slot_id.store(0, std::memory_order_release);

#if !defined(PYLABHUB_PLATFORM_WIN64)
        // Initialize pthread mutex attributes for process sharing
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        // Initialize the mutex in the shared memory
        pthread_mutex_init(reinterpret_cast<pthread_mutex_t *>(m_header->mutex_storage), &mattr);
        pthread_mutexattr_destroy(&mattr);
#else
        // CRITICAL: On Windows, process-shared mutexes are typically named kernel objects.
        // m_header->mutex_storage (a byte array) cannot directly hold a Windows kernel mutex object.
        // It is left uninitialized here.
        // TODO: Implement proper cross-process synchronization for Windows, possibly using a named
        // kernel mutex referenced externally by name, rather than attempting to embed a mutex object
        // directly in shared memory. For now, ensure no code attempts to use this storage as a mutex.
        // As a temporary measure to ensure the memory is initialized, zero out the storage.
        std::memset(m_header->mutex_storage, 0, sizeof(m_header->mutex_storage));
#endif
        // Get pointers to the flexible data zone and structured buffer
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_header) + sizeof(pylabhub::hub::SharedMemoryHeader);
        m_structured_data_buffer = m_flexible_data_zone + config.flexible_zone_size;

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

        // Get pointers to the flexible data zone and structured buffer
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_header) + sizeof(pylabhub::hub::SharedMemoryHeader);
        m_structured_data_buffer = nullptr; // Will be set after discovery

        LOGGER_INFO("DataBlock '{}' opened.", m_name);
    }

    ~DataBlock()
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        UnmapViewOfFile(m_mapped_address);
        CloseHandle(m_shm_handle);
#else
        munmap(m_mapped_address, m_size);
        close(m_shm_fd);
        if (m_is_creator)
        {
            shm_unlink(m_name.c_str());
            LOGGER_INFO("DataBlock '{}' shared memory removed.", m_name);
        }
#endif
    }

    // Accessors
    pylabhub::hub::SharedMemoryHeader *header() const { return m_header; }
    char *flexible_data_zone() const { return m_flexible_data_zone; }
    char *structured_data_buffer() const { return m_structured_data_buffer; }
    void *segment() const { return m_mapped_address; }

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
};

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

    // Producer specific methods will be added here
    // For example: acquire_write_slot, release_write_slot, get_flexible_zone, etc.

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

    // Consumer specific methods will be added here
    // For example: begin_consume, end_consume, get_flexible_zone, etc.

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