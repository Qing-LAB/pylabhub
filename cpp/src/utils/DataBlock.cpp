
#include "plh_service.hpp"
#include "utils/DataBlock.hpp"
#include "utils/MessageHub.hpp"

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/creation_parameters.hpp>

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
 * shared memory, providing access to the SharedMemoryHeader and data regions.
 */
class DataBlock
{
  public:
    // For producer: create and initialize shared memory
    DataBlock(const std::string &name, const DataBlockConfig &config) : m_name(name)
    {
        // Calculate total size needed for shared memory
        // Header + Flexible Data Zone + Structured Data Buffer
        size_t total_shm_size =
            sizeof(SharedMemoryHeader) + config.flexible_zone_size + config.structured_buffer_size;

        try
        {
            // Remove any previous shared memory with the same name
            boost::interprocess::shared_memory_object::remove(m_name.c_str());

            // Create managed shared memory
            m_segment = std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::create_only, m_name.c_str(), total_shm_size);

            // Allocate and construct the SharedMemoryHeader
            m_header = m_segment->construct<SharedMemoryHeader>("SharedMemoryHeader")();

            // Initialize header fields
            m_header->magic_number = DATABLOCK_MAGIC_NUMBER;
            m_header->shared_secret = config.shared_secret;
            m_header->version = DATABLOCK_VERSION;
            m_header->header_size = sizeof(SharedMemoryHeader);
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
            pthread_mutex_init(reinterpret_cast<pthread_mutex_t *>(m_header->mutex_storage),
                               &mattr);
            pthread_mutexattr_destroy(&mattr);
#endif
            // Get pointers to the flexible data zone and structured buffer
            m_flexible_data_zone = reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader);
            m_structured_data_buffer = m_flexible_data_zone + config.flexible_zone_size;

            LOGGER_INFO("DataBlock '{}' created with total size {} bytes.", m_name, total_shm_size);
        }
        catch (const boost::interprocess::interprocess_exception &ex)
        {
            LOGGER_ERROR("DataBlock: Failed to create shared memory '{}': {}", m_name, ex.what());
            // Re-throw to indicate failure in constructor
            throw;
        }
    }

    // For consumer: open existing shared memory
    DataBlock(const std::string &name) : m_name(name)
    {
        try
        {
            m_segment = std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::open_only, m_name.c_str());

            // Find the header
            auto res = m_segment->find<SharedMemoryHeader>("SharedMemoryHeader");
            if (!res.first)
            {
                LOGGER_ERROR("DataBlock: Shared memory '{}' exists but header not found.", m_name);
                throw boost::interprocess::interprocess_exception("Header not found");
            }
            m_header = res.first;

            // Get pointers to the flexible data zone and structured buffer
            // For consumer, we need to read buffer sizes from the header or config obtained via
            // message hub For now, we assume the producer config would be known via message hub
            // discovery. Placeholder for getting actual sizes from a discovery mechanism. For a
            // consumer, these will be set after successful discovery of producer's config.
            m_flexible_data_zone = reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader);
            m_structured_data_buffer = nullptr; // Will be set after discovery

            LOGGER_INFO("DataBlock '{}' opened.", m_name);
        }
        catch (const boost::interprocess::interprocess_exception &ex)
        {
            LOGGER_ERROR("DataBlock: Failed to open shared memory '{}': {}", m_name, ex.what());
            // Re-throw to indicate failure in constructor
            throw;
        }
    }

    ~DataBlock()
    {
        if (m_segment->get_segment_manager()->get_destroy_on_deletion())
        {
            // Only remove if this instance was the creator (producer)
            boost::interprocess::shared_memory_object::remove(m_name.c_str());
            LOGGER_INFO("DataBlock '{}' shared memory removed.", m_name);
        }
    }

    // Accessors
    SharedMemoryHeader *header() const { return m_header; }
    char *flexible_data_zone() const { return m_flexible_data_zone; }
    char *structured_data_buffer() const { return m_structured_data_buffer; }
    boost::interprocess::managed_shared_memory *segment() const { return m_segment.get(); }

  private:
    std::string m_name;
    std::unique_ptr<boost::interprocess::managed_shared_memory> m_segment;
    SharedMemoryHeader *m_header{nullptr};
    char *m_flexible_data_zone{nullptr};
    char *m_structured_data_buffer{nullptr};
};

class DataBlockProducerImpl : public IDataBlockProducer
{
  public:
    DataBlockProducerImpl(const std::string &name, const DataBlockConfig &config)
    try : m_dataBlock(std::make_unique<DataBlock>(name, config))
    {
        m_name = name;
        LOGGER_INFO("DataBlockProducerImpl: Initialized for '{}'.", m_name);
    }
    catch (const boost::interprocess::interprocess_exception &ex)
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
            throw boost::interprocess::interprocess_exception("Invalid magic number");
        }
        if (m_dataBlock->header()->shared_secret != shared_secret)
        {
            LOGGER_ERROR("DataBlockConsumerImpl: Invalid shared secret for '{}'.", name);
            throw boost::interprocess::interprocess_exception("Invalid shared secret");
        }

        m_dataBlock->header()->active_consumer_count.fetch_add(1, std::memory_order_acq_rel);
        LOGGER_INFO("DataBlockConsumerImpl: Initialized for '{}'. Active consumers: {}.", m_name,
                    m_dataBlock->header()->active_consumer_count.load());
    }
    catch (const boost::interprocess::interprocess_exception &ex)
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

std::unique_ptr<IDataBlockProducer> create_datablock_producer(MessageHub &hub,
                                                              const std::string &name,
                                                              DataBlockPolicy policy,
                                                              const DataBlockConfig &config)
{
    (void)hub;    // MessageHub will be used for registration in future steps
    (void)policy; // Policy will influence DataBlock's internal management

    try
    {
        return std::make_unique<DataBlockProducerImpl>(name, config);
    }
    catch (const boost::interprocess::interprocess_exception &ex)
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
    catch (const boost::interprocess::interprocess_exception &ex)
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
