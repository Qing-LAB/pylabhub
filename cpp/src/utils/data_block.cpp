#include "plh_service.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "utils/data_block_mutex.hpp"
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace pylabhub::hub
{

// ============================================================================
// Constants
// ============================================================================
namespace
{
static constexpr uint64_t DATABLOCK_MAGIC_NUMBER = 0xBADF00DFEEDFACEL;
static constexpr uint32_t DATABLOCK_VERSION = 2; // Bumped for new header layout
} // namespace

// ============================================================================
// DataBlockIteratorImpl
// ============================================================================
struct DataBlockIteratorImpl
{
    void *segment_base = nullptr;
    size_t segment_size = 0;
    SharedMemoryHeader *header = nullptr;
    void *flexible_zone_base = nullptr;
    size_t flexible_zone_size = 0;
    size_t block_index = 0;
    bool is_end = false;
};

// ============================================================================
// DataBlock - Internal helper
// ============================================================================
class DataBlock
{
  public:
    DataBlock(const std::string &name, const DataBlockConfig &config)
        : m_name(name), m_is_creator(true)
    {
        m_size = sizeof(SharedMemoryHeader) + config.flexible_zone_size +
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

        m_header = new (m_mapped_address) SharedMemoryHeader();
        m_header->init_state.store(0, std::memory_order_release);

#if defined(PYLABHUB_PLATFORM_WIN64)
        m_management_mutex = std::make_unique<DataBlockMutex>(m_name, nullptr, 0, m_is_creator);
#else
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, m_mapped_address, offsetof(SharedMemoryHeader, management_mutex_storage),
            m_is_creator);
#endif

        m_header->init_state.store(1, std::memory_order_release);

        // Initialize header fields
        m_header->shared_secret = config.shared_secret;
        m_header->version = DATABLOCK_VERSION;
        m_header->header_size = sizeof(SharedMemoryHeader);
        m_header->active_consumer_count.store(0, std::memory_order_release);
        m_header->write_index.store(0, std::memory_order_release);
        m_header->commit_index.store(0, std::memory_order_release);
        m_header->read_index.store(0, std::memory_order_release);
        m_header->current_slot_id.store(0, std::memory_order_release);

        // Chain: single block is both head and tail
        m_header->prev_block_offset = 0;
        m_header->next_block_offset = 0;
        m_header->chain_flags = CHAIN_HEAD | CHAIN_TAIL;
        m_header->chain_index = 0;
        m_header->total_spinlock_count.store(SharedMemoryHeader::MAX_SHARED_SPINLOCKS,
                                            std::memory_order_release);
        m_header->total_counter_count.store(SharedMemoryHeader::NUM_COUNTERS_64,
                                            std::memory_order_release);
        m_header->flexible_zone_format = config.flexible_zone_format;
        m_header->flexible_zone_size = static_cast<uint32_t>(config.flexible_zone_size);

        for (size_t i = 0; i < SharedMemoryHeader::MAX_SHARED_SPINLOCKS; ++i)
        {
            m_header->spinlock_allocated[i].clear(std::memory_order_release);
        }
        for (size_t i = 0; i < SharedMemoryHeader::NUM_COUNTERS_64; ++i)
        {
            m_header->counters_64[i].store(0, std::memory_order_release);
        }

        m_flexible_data_zone = reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader);
        m_structured_data_buffer = m_flexible_data_zone + config.flexible_zone_size;

        std::atomic_thread_fence(std::memory_order_release);
        m_header->magic_number = DATABLOCK_MAGIC_NUMBER;
        m_header->init_state.store(2, std::memory_order_release);

        LOGGER_INFO("DataBlock '{}' created with total size {} bytes.", m_name, m_size);
    }

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
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(m_mapped_address, &mbi, sizeof(mbi));
        m_size = mbi.RegionSize;
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

        m_header = reinterpret_cast<SharedMemoryHeader *>(m_mapped_address);

        const int max_wait_ms = 5000;
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
            throw std::runtime_error("DataBlock '" + m_name +
                                    "' initialization timeout - producer may have crashed.");
        }

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
            throw std::runtime_error("DataBlock '" + m_name + "' invalid magic number.");
        }

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

#if defined(PYLABHUB_PLATFORM_WIN64)
        m_management_mutex = std::make_unique<DataBlockMutex>(m_name, nullptr, 0, m_is_creator);
#else
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, m_mapped_address, offsetof(SharedMemoryHeader, management_mutex_storage),
            m_is_creator);
#endif

        m_flexible_data_zone = reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader);
        m_structured_data_buffer = nullptr;

        LOGGER_INFO("DataBlock '{}' opened by consumer.", m_name);
    }

    ~DataBlock()
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        if (m_mapped_address)
        {
            UnmapViewOfFile(m_mapped_address);
        }
        if (m_shm_handle)
        {
            CloseHandle(m_shm_handle);
        }
#else
        if (m_is_creator && m_management_mutex)
        {
            m_management_mutex.reset();
        }
        if (m_mapped_address)
        {
            munmap(m_mapped_address, m_size);
        }
        if (m_shm_fd != -1)
        {
            close(m_shm_fd);
        }
        if (m_is_creator)
        {
            shm_unlink(m_name.c_str());
            LOGGER_INFO("DataBlock '{}' shared memory removed.", m_name);
        }
#endif
    }

    SharedMemoryHeader *header() const { return m_header; }
    char *flexible_data_zone() const { return m_flexible_data_zone; }
    char *structured_data_buffer() const { return m_structured_data_buffer; }
    void *segment() const { return m_mapped_address; }
    size_t size() const { return m_size; }

    size_t acquire_shared_spinlock(const std::string &debug_name)
    {
        DataBlockLockGuard lock(*m_management_mutex);
        for (size_t i = 0; i < SharedMemoryHeader::MAX_SHARED_SPINLOCKS; ++i)
        {
            if (!m_header->spinlock_allocated[i].test_and_set(std::memory_order_acq_rel))
            {
                m_header->shared_spinlocks[i].owner_pid.store(0, std::memory_order_release);
                m_header->shared_spinlocks[i].generation.store(0, std::memory_order_release);
                m_header->shared_spinlocks[i].recursion_count.store(0, std::memory_order_release);
                m_header->shared_spinlocks[i].owner_thread_id = 0;
                LOGGER_INFO("DataBlock '{}': Acquired spinlock slot {} for '{}'.", m_name, i,
                            debug_name);
                return i;
            }
        }
        throw std::runtime_error("DataBlock '" + m_name + "': No free spinlock slots.");
    }

    void release_shared_spinlock(size_t index)
    {
        if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
        {
            throw std::out_of_range("Spinlock index out of range.");
        }
        DataBlockLockGuard lock(*m_management_mutex);
        if (m_header->shared_spinlocks[index].owner_pid.load(std::memory_order_acquire) != 0)
        {
            LOGGER_WARN("DataBlock '{}': Releasing spinlock {} still held. Force releasing.",
                        m_name, index);
        }
        m_header->spinlock_allocated[index].clear(std::memory_order_release);
        m_header->shared_spinlocks[index].owner_pid.store(0, std::memory_order_release);
        m_header->shared_spinlocks[index].generation.store(0, std::memory_order_release);
        m_header->shared_spinlocks[index].recursion_count.store(0, std::memory_order_release);
        m_header->shared_spinlocks[index].owner_thread_id = 0;
        LOGGER_INFO("DataBlock '{}': Released spinlock slot {}.", m_name, index);
    }

    SharedSpinLockState *get_shared_spinlock_state(size_t index)
    {
        if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
        {
            throw std::out_of_range("Spinlock index out of range.");
        }
        return &m_header->shared_spinlocks[index];
    }

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

    SharedMemoryHeader *m_header = nullptr;
    char *m_flexible_data_zone = nullptr;
    char *m_structured_data_buffer = nullptr;
    std::unique_ptr<DataBlockMutex> m_management_mutex;
};

// ============================================================================
// DataBlockIterator
// ============================================================================
DataBlockIterator::DataBlockIterator() : pImpl(std::make_unique<DataBlockIteratorImpl>())
{
}

DataBlockIterator::DataBlockIterator(std::unique_ptr<DataBlockIteratorImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockIterator::~DataBlockIterator() = default;

DataBlockIterator::DataBlockIterator(DataBlockIterator &&other) noexcept = default;

DataBlockIterator &DataBlockIterator::operator=(DataBlockIterator &&other) noexcept = default;

DataBlockIterator::NextResult DataBlockIterator::try_next()
{
    NextResult r;
    r.ok = false;
    r.error_code = 0;
    if (!pImpl || pImpl->is_end)
    {
        r.error_code = 1; // EINVAL or "already at end"
        return r;
    }
    if (pImpl->header->next_block_offset == 0)
    {
        r.error_code = 2; // "next block not available"
        return r;
    }
    // For single-block implementation, next_block_offset is 0 (tail). Multi-block expansion
    // would resolve the next block here. For now we return not available.
    r.error_code = 2;
    return r;
}

DataBlockIterator DataBlockIterator::next()
{
    auto res = try_next();
    if (!res.ok)
    {
        throw std::runtime_error("DataBlockIterator::next: next block not available (error " +
                                 std::to_string(res.error_code) + ")");
    }
    return std::move(res.next);
}

void *DataBlockIterator::block_base() const
{
    return pImpl ? pImpl->header : nullptr;
}

size_t DataBlockIterator::block_index() const
{
    return pImpl ? pImpl->block_index : 0;
}

void *DataBlockIterator::flexible_zone_base() const
{
    return pImpl ? pImpl->flexible_zone_base : nullptr;
}

size_t DataBlockIterator::flexible_zone_size() const
{
    return pImpl ? pImpl->flexible_zone_size : 0;
}

FlexibleZoneFormat DataBlockIterator::flexible_zone_format() const
{
    return pImpl && pImpl->header ? pImpl->header->flexible_zone_format
                                  : FlexibleZoneFormat::Raw;
}

bool DataBlockIterator::is_head() const
{
    return pImpl && pImpl->header && (pImpl->header->chain_flags & CHAIN_HEAD);
}

bool DataBlockIterator::is_tail() const
{
    return pImpl && pImpl->header && (pImpl->header->chain_flags & CHAIN_TAIL);
}

bool DataBlockIterator::is_valid() const
{
    return pImpl && pImpl->header && !pImpl->is_end;
}

// ============================================================================
// DataBlockProducerImpl
// ============================================================================
struct DataBlockProducerImpl
{
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;

    static size_t to_local_index(size_t global_index)
    {
        return global_index % SharedMemoryHeader::MAX_SHARED_SPINLOCKS;
    }
    static size_t to_block_index(size_t global_index)
    {
        return global_index / SharedMemoryHeader::MAX_SHARED_SPINLOCKS;
    }
};

// ============================================================================
// DataBlockProducer
// ============================================================================
DataBlockProducer::DataBlockProducer() : pImpl(nullptr) {}

DataBlockProducer::DataBlockProducer(std::unique_ptr<DataBlockProducerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockProducer::~DataBlockProducer() = default;

DataBlockProducer::DataBlockProducer(DataBlockProducer &&other) noexcept = default;

DataBlockProducer &DataBlockProducer::operator=(DataBlockProducer &&other) noexcept = default;

std::unique_ptr<SharedSpinLockGuardOwning> DataBlockProducer::acquire_spinlock(size_t index,
                                                                               const std::string &debug_name)
{
    if (!pImpl)
    {
        throw std::runtime_error("DataBlockProducer: invalid state");
    }
    size_t local = DataBlockProducerImpl::to_local_index(index);
    if (index >= pImpl->dataBlock->header()->total_spinlock_count.load(std::memory_order_acquire))
    {
        throw std::out_of_range("Spinlock index out of range");
    }
    // For single block, block index 0. Multi-block would resolve header from chain.
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(local);
    std::string name = debug_name.empty() ? pImpl->name + "_spinlock_" + std::to_string(index)
                                         : debug_name;
    return std::make_unique<SharedSpinLockGuardOwning>(state, name);
}

void DataBlockProducer::release_spinlock(size_t index)
{
    if (!pImpl)
    {
        throw std::runtime_error("DataBlockProducer: invalid state");
    }
    size_t local = DataBlockProducerImpl::to_local_index(index);
    pImpl->dataBlock->release_shared_spinlock(local);
}

SharedSpinLock DataBlockProducer::get_spinlock(size_t index)
{
    if (!pImpl)
    {
        throw std::runtime_error("DataBlockProducer: invalid state");
    }
    size_t local = DataBlockProducerImpl::to_local_index(index);
    if (index >= pImpl->dataBlock->header()->total_spinlock_count.load(std::memory_order_acquire))
    {
        throw std::out_of_range("Spinlock index out of range");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(local);
    return SharedSpinLock(state, pImpl->name + "_spinlock_" + std::to_string(index));
}

uint32_t DataBlockProducer::spinlock_count() const
{
    return pImpl && pImpl->dataBlock && pImpl->dataBlock->header()
               ? pImpl->dataBlock->header()->total_spinlock_count.load(std::memory_order_acquire)
               : 0;
}

std::unique_ptr<SharedSpinLockGuardOwning>
DataBlockProducer::acquire_user_spinlock(const std::string &debug_name)
{
    if (!pImpl)
    {
        throw std::runtime_error("DataBlockProducer: invalid state");
    }
    size_t index = pImpl->dataBlock->acquire_shared_spinlock(debug_name);
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(index);
    return std::make_unique<SharedSpinLockGuardOwning>(state, debug_name);
}

void DataBlockProducer::release_user_spinlock(size_t index)
{
    release_spinlock(index);
}

void DataBlockProducer::set_counter_64(size_t index, uint64_t value)
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        throw std::runtime_error("DataBlockProducer: invalid state");
    }
    if (index >= SharedMemoryHeader::NUM_COUNTERS_64)
    {
        throw std::out_of_range("Counter 64 index out of range");
    }
    pImpl->dataBlock->header()->counters_64[index].store(value, std::memory_order_release);
}

uint64_t DataBlockProducer::get_counter_64(size_t index) const
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        throw std::runtime_error("DataBlockProducer: invalid state");
    }
    if (index >= SharedMemoryHeader::NUM_COUNTERS_64)
    {
        throw std::out_of_range("Counter 64 index out of range");
    }
    return pImpl->dataBlock->header()->counters_64[index].load(std::memory_order_acquire);
}

uint32_t DataBlockProducer::counter_count() const
{
    return pImpl && pImpl->dataBlock && pImpl->dataBlock->header()
               ? pImpl->dataBlock->header()->total_counter_count.load(std::memory_order_acquire)
               : 0;
}

DataBlockIterator DataBlockProducer::begin()
{
    if (!pImpl || !pImpl->dataBlock)
    {
        return DataBlockIterator();
    }
    auto impl = std::make_unique<DataBlockIteratorImpl>();
    impl->segment_base = pImpl->dataBlock->segment();
    impl->segment_size = pImpl->dataBlock->size();
    impl->header = pImpl->dataBlock->header();
    impl->flexible_zone_base = pImpl->dataBlock->flexible_data_zone();
    impl->flexible_zone_size = pImpl->dataBlock->header()->flexible_zone_size;
    impl->block_index = 0;
    impl->is_end = false;
    return DataBlockIterator(std::move(impl));
}

DataBlockIterator DataBlockProducer::end()
{
    auto impl = std::make_unique<DataBlockIteratorImpl>();
    impl->is_end = true;
    return DataBlockIterator(std::move(impl));
}

// ============================================================================
// DataBlockConsumerImpl
// ============================================================================
struct DataBlockConsumerImpl
{
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;

    ~DataBlockConsumerImpl()
    {
        if (dataBlock && dataBlock->header())
        {
            dataBlock->header()->active_consumer_count.fetch_sub(1, std::memory_order_acq_rel);
            LOGGER_INFO("DataBlockConsumerImpl: Shutting down for '{}'. Active consumers: {}.",
                        name, dataBlock->header()->active_consumer_count.load());
        }
    }
};

// ============================================================================
// DataBlockConsumer
// ============================================================================
DataBlockConsumer::DataBlockConsumer() : pImpl(nullptr) {}

DataBlockConsumer::DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockConsumer::~DataBlockConsumer() = default;

DataBlockConsumer::DataBlockConsumer(DataBlockConsumer &&other) noexcept = default;

DataBlockConsumer &DataBlockConsumer::operator=(DataBlockConsumer &&other) noexcept = default;

SharedSpinLock DataBlockConsumer::get_spinlock(size_t index)
{
    if (!pImpl)
    {
        throw std::runtime_error("DataBlockConsumer: invalid state");
    }
    size_t local = index % SharedMemoryHeader::MAX_SHARED_SPINLOCKS;
    if (index >= pImpl->dataBlock->header()->total_spinlock_count.load(std::memory_order_acquire))
    {
        throw std::out_of_range("Spinlock index out of range");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(local);
    return SharedSpinLock(state, pImpl->name + "_spinlock_" + std::to_string(index));
}

uint32_t DataBlockConsumer::spinlock_count() const
{
    return pImpl && pImpl->dataBlock && pImpl->dataBlock->header()
               ? pImpl->dataBlock->header()->total_spinlock_count.load(std::memory_order_acquire)
               : 0;
}

SharedSpinLock DataBlockConsumer::get_user_spinlock(size_t index)
{
    return get_spinlock(index);
}

uint64_t DataBlockConsumer::get_counter_64(size_t index) const
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        throw std::runtime_error("DataBlockConsumer: invalid state");
    }
    if (index >= SharedMemoryHeader::NUM_COUNTERS_64)
    {
        throw std::out_of_range("Counter 64 index out of range");
    }
    return pImpl->dataBlock->header()->counters_64[index].load(std::memory_order_acquire);
}

void DataBlockConsumer::set_counter_64(size_t index, uint64_t value)
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        throw std::runtime_error("DataBlockConsumer: invalid state");
    }
    if (index >= SharedMemoryHeader::NUM_COUNTERS_64)
    {
        throw std::out_of_range("Counter 64 index out of range");
    }
    pImpl->dataBlock->header()->counters_64[index].store(value, std::memory_order_release);
}

uint32_t DataBlockConsumer::counter_count() const
{
    return pImpl && pImpl->dataBlock && pImpl->dataBlock->header()
               ? pImpl->dataBlock->header()->total_counter_count.load(std::memory_order_acquire)
               : 0;
}

DataBlockIterator DataBlockConsumer::begin()
{
    if (!pImpl || !pImpl->dataBlock)
    {
        return DataBlockIterator();
    }
    auto impl = std::make_unique<DataBlockIteratorImpl>();
    impl->segment_base = pImpl->dataBlock->segment();
    impl->segment_size = pImpl->dataBlock->size();
    impl->header = pImpl->dataBlock->header();
    impl->flexible_zone_base = pImpl->dataBlock->flexible_data_zone();
    impl->flexible_zone_size = pImpl->dataBlock->header()->flexible_zone_size;
    impl->block_index = 0;
    impl->is_end = false;
    return DataBlockIterator(std::move(impl));
}

DataBlockIterator DataBlockConsumer::end()
{
    auto impl = std::make_unique<DataBlockIteratorImpl>();
    impl->is_end = true;
    return DataBlockIterator(std::move(impl));
}

// ============================================================================
// Factory Functions
// ============================================================================
std::unique_ptr<DataBlockProducer> create_datablock_producer(MessageHub &hub,
                                                             const std::string &name,
                                                             DataBlockPolicy policy,
                                                             const DataBlockConfig &config)
{
    (void)hub;
    (void)policy;
    try
    {
        auto impl = std::make_unique<DataBlockProducerImpl>();
        impl->name = name;
        impl->dataBlock = std::make_unique<DataBlock>(name, config);
        return std::make_unique<DataBlockProducer>(std::move(impl));
    }
    catch (const std::runtime_error &ex)
    {
        LOGGER_ERROR("create_datablock_producer: Failed for '{}': {}", name, ex.what());
        return nullptr;
    }
    catch (const std::bad_alloc &ex)
    {
        LOGGER_ERROR("create_datablock_producer: Memory failed for '{}': {}", name, ex.what());
        return nullptr;
    }
}

std::unique_ptr<DataBlockConsumer> find_datablock_consumer(MessageHub &hub,
                                                            const std::string &name,
                                                            uint64_t shared_secret)
{
    (void)hub;
    try
    {
        auto impl = std::make_unique<DataBlockConsumerImpl>();
        impl->name = name;
        impl->dataBlock = std::make_unique<DataBlock>(name);
        if (impl->dataBlock->header()->magic_number != DATABLOCK_MAGIC_NUMBER)
        {
            throw std::runtime_error("Invalid magic number");
        }
        if (impl->dataBlock->header()->shared_secret != shared_secret)
        {
            throw std::runtime_error("Invalid shared secret");
        }
        impl->dataBlock->header()->active_consumer_count.fetch_add(1, std::memory_order_acq_rel);
        return std::make_unique<DataBlockConsumer>(std::move(impl));
    }
    catch (const std::runtime_error &ex)
    {
        LOGGER_ERROR("find_datablock_consumer: Failed for '{}': {}", name, ex.what());
        return nullptr;
    }
    catch (const std::bad_alloc &ex)
    {
        LOGGER_ERROR("find_datablock_consumer: Memory failed for '{}': {}", name, ex.what());
        return nullptr;
    }
}

} // namespace pylabhub::hub
