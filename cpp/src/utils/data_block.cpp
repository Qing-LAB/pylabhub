#include "plh_service.hpp"
#include "plh_platform.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "utils/data_block_mutex.hpp"
#include <sodium.h>
#include <cstddef>
#include <cstring>
#include <limits>
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
static constexpr uint64_t DATABLOCK_MAGIC_NUMBER = 0xBADF00DFEEDFACEULL;
static constexpr uint32_t DATABLOCK_VERSION = 4; // Bumped for variable slot checksum region
// HEP-core-0002 §8.3: Version range for consumer compatibility
static constexpr uint32_t DATABLOCK_VERSION_MIN_SUPPORTED = 4;
static constexpr uint32_t DATABLOCK_VERSION_SUPPORTED = 4;
static constexpr uint64_t INVALID_SLOT_ID = std::numeric_limits<uint64_t>::max();

// BLAKE2b checksum via libsodium (crypto_generichash)
inline bool compute_blake2b(uint8_t *out, const void *data, size_t len)
{
    if (sodium_init() < 0)
        return false;
    return crypto_generichash(out, SharedMemoryHeader::CHECKSUM_BYTES,
                             static_cast<const unsigned char *>(data), len, nullptr, 0) == 0;
}

inline bool verify_blake2b(const uint8_t *stored, const void *data, size_t len)
{
    uint8_t computed[SharedMemoryHeader::CHECKSUM_BYTES];
    if (!compute_blake2b(computed, data, len))
        return false;
    return std::memcmp(stored, computed, SharedMemoryHeader::CHECKSUM_BYTES) == 0;
}

// Defined after DataBlock class (uses its methods)
bool update_checksum_flexible_zone_impl(DataBlock *block);
bool update_checksum_slot_impl(DataBlock *block, size_t slot_index);
bool verify_checksum_flexible_zone_impl(const DataBlock *block);
bool verify_checksum_slot_impl(const DataBlock *block, size_t slot_index);

inline bool enter_thread_guard(std::atomic<uint64_t> &owner_tid,
                               std::atomic<uint32_t> &depth,
                               uint64_t tid)
{
    uint64_t current = owner_tid.load(std::memory_order_acquire);
    if (current == tid)
    {
        depth.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t expected = 0;
    if (owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel,
                                          std::memory_order_acquire))
    {
        depth.store(1, std::memory_order_release);
        return true;
    }
    return false;
}

inline bool exit_thread_guard(std::atomic<uint64_t> &owner_tid,
                              std::atomic<uint32_t> &depth,
                              uint64_t tid)
{
    if (owner_tid.load(std::memory_order_acquire) != tid)
        return false;
    uint32_t prev = depth.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1)
    {
        owner_tid.store(0, std::memory_order_release);
    }
    return true;
}
} // namespace

// ============================================================================
// DataBlock - Internal helper
// ============================================================================
class DataBlock
{
  public:
    DataBlock(const std::string &name, const DataBlockConfig &config)
        : m_name(name), m_is_creator(true)
    {
        size_t slot_count = (config.ring_buffer_capacity > 0)
                                ? static_cast<size_t>(config.ring_buffer_capacity)
                                : 1u;
        size_t struct_size = slot_count * to_bytes(config.unit_block_size);
        size_t slot_checksum_size =
            config.enable_checksum ? slot_count * SharedMemoryHeader::SLOT_CHECKSUM_ENTRY_SIZE : 0;
        m_size = sizeof(SharedMemoryHeader) + slot_checksum_size + config.flexible_zone_size +
                 struct_size;

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
        m_header->commit_index.store(INVALID_SLOT_ID, std::memory_order_release);
        m_header->read_index.store(0, std::memory_order_release);
        m_header->current_slot_id.store(INVALID_SLOT_ID, std::memory_order_release);

        m_header->flexible_zone_format = config.flexible_zone_format;
        m_header->flexible_zone_size = static_cast<uint32_t>(config.flexible_zone_size);
        m_header->ring_buffer_capacity =
            (config.ring_buffer_capacity > 0) ? static_cast<uint32_t>(config.ring_buffer_capacity) : 1u;
        m_header->structured_buffer_size = static_cast<uint32_t>(struct_size);
        m_header->unit_block_size = static_cast<uint32_t>(config.unit_block_size);
        m_header->checksum_enabled = config.enable_checksum ? 1u : 0u;

        std::memset(m_header->flexible_zone_checksum, 0, SharedMemoryHeader::CHECKSUM_BYTES);
        m_header->flexible_zone_checksum_valid.store(0, std::memory_order_release);
        if (slot_checksum_size > 0)
        {
            char *slot_checksum_base =
                reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader);
            std::memset(slot_checksum_base, 0, slot_checksum_size);
        }

        for (size_t i = 0; i < SharedMemoryHeader::MAX_SHARED_SPINLOCKS; ++i)
        {
            m_header->spinlock_allocated[i].clear(std::memory_order_release);
        }
        for (size_t i = 0; i < SharedMemoryHeader::NUM_COUNTERS_64; ++i)
        {
            m_header->counters_64[i].store(0, std::memory_order_release);
        }

        m_flexible_data_zone =
            reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader) + slot_checksum_size;
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

        if (m_header->version > DATABLOCK_VERSION_SUPPORTED)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            UnmapViewOfFile(m_mapped_address);
            CloseHandle(m_shm_handle);
#else
            munmap(m_mapped_address, m_size);
            close(m_shm_fd);
#endif
            throw std::runtime_error("DataBlock '" + m_name +
                                     "' version too new (got " + std::to_string(m_header->version) +
                                     "), upgrade client to support up to " +
                                     std::to_string(DATABLOCK_VERSION_SUPPORTED));
        }
        if (m_header->version < DATABLOCK_VERSION_MIN_SUPPORTED)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            UnmapViewOfFile(m_mapped_address);
            CloseHandle(m_shm_handle);
#else
            munmap(m_mapped_address, m_size);
            close(m_shm_fd);
#endif
            throw std::runtime_error("DataBlock '" + m_name +
                                     "' version too old (got " + std::to_string(m_header->version) +
                                     "), upgrade producer to at least " +
                                     std::to_string(DATABLOCK_VERSION_MIN_SUPPORTED));
        }

        size_t expected_size = sizeof(SharedMemoryHeader) + m_header->slot_checksum_region_size() +
                              m_header->flexible_zone_size + m_header->structured_buffer_size;
#if defined(PYLABHUB_PLATFORM_WIN64)
        // Windows: VirtualQuery RegionSize can exceed allocation due to granularity rounding.
        if (m_size < expected_size)
#else
        if (m_size != expected_size)
#endif
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            UnmapViewOfFile(m_mapped_address);
            CloseHandle(m_shm_handle);
#else
            munmap(m_mapped_address, m_size);
            close(m_shm_fd);
#endif
            throw std::runtime_error("DataBlock '" + m_name +
                                    "' size mismatch. Expected " + std::to_string(expected_size) +
                                    ", got " + std::to_string(m_size));
        }

#if defined(PYLABHUB_PLATFORM_WIN64)
        m_management_mutex = std::make_unique<DataBlockMutex>(m_name, nullptr, 0, m_is_creator);
#else
        m_management_mutex = std::make_unique<DataBlockMutex>(
            m_name, m_mapped_address, offsetof(SharedMemoryHeader, management_mutex_storage),
            m_is_creator);
#endif

        size_t slot_checksum_sz = m_header->slot_checksum_region_size();
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_header) + sizeof(SharedMemoryHeader) + slot_checksum_sz;
        m_structured_data_buffer = m_flexible_data_zone + m_header->flexible_zone_size;

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
                m_header->shared_spinlocks[i].owner_thread_id.store(0, std::memory_order_release);
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
        m_header->shared_spinlocks[index].owner_thread_id.store(0, std::memory_order_release);
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

namespace
{
inline bool update_checksum_flexible_zone_impl(DataBlock *block)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->checksum_enabled)
        return false;
    char *flex = block->flexible_data_zone();
    size_t len = h->flexible_zone_size;
    if (!flex || len == 0)
        return false;
    if (!compute_blake2b(h->flexible_zone_checksum, flex, len))
        return false;
    h->flexible_zone_checksum_valid.store(1, std::memory_order_release);
    return true;
}

inline bool update_checksum_slot_impl(DataBlock *block, size_t slot_index)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->checksum_enabled)
        return false;
    uint32_t slot_count = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
    if (slot_index >= slot_count)
        return false;
    size_t slot_size = h->unit_block_size;
    if (slot_size == 0)
        return false;
    char *buf = block->structured_data_buffer();
    if (!buf)
        return false;
    char *slot_checksum_base =
        reinterpret_cast<char *>(h) + sizeof(SharedMemoryHeader);
    uint8_t *slot_checksum =
        reinterpret_cast<uint8_t *>(slot_checksum_base +
                                    slot_index * SharedMemoryHeader::SLOT_CHECKSUM_ENTRY_SIZE);
    std::atomic<uint8_t> *slot_valid =
        reinterpret_cast<std::atomic<uint8_t> *>(slot_checksum + SharedMemoryHeader::CHECKSUM_BYTES);
    const void *slot_data = buf + slot_index * slot_size;
    if (!compute_blake2b(slot_checksum, slot_data, slot_size))
        return false;
    slot_valid->store(1, std::memory_order_release);
    return true;
}

inline bool verify_checksum_flexible_zone_impl(const DataBlock *block)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->checksum_enabled)
        return false;
    if (h->flexible_zone_checksum_valid.load(std::memory_order_acquire) != 1)
        return false;
    const char *flex = block->flexible_data_zone();
    size_t len = h->flexible_zone_size;
    if (!flex || len == 0)
        return false;
    return verify_blake2b(h->flexible_zone_checksum, flex, len);
}

inline bool verify_checksum_slot_impl(const DataBlock *block, size_t slot_index)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->checksum_enabled)
        return false;
    uint32_t slot_count = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
    if (slot_index >= slot_count)
        return false;
    const char *slot_checksum_base =
        reinterpret_cast<const char *>(h) + sizeof(SharedMemoryHeader);
    const uint8_t *slot_checksum =
        reinterpret_cast<const uint8_t *>(slot_checksum_base +
                                          slot_index * SharedMemoryHeader::SLOT_CHECKSUM_ENTRY_SIZE);
    const std::atomic<uint8_t> *slot_valid =
        reinterpret_cast<const std::atomic<uint8_t> *>(slot_checksum +
                                                       SharedMemoryHeader::CHECKSUM_BYTES);
    if (slot_valid->load(std::memory_order_acquire) != 1)
        return false;
    size_t slot_size = h->unit_block_size;
    if (slot_size == 0)
        return false;
    const char *buf = block->structured_data_buffer();
    if (!buf)
        return false;
    const void *slot_data = buf + slot_index * slot_size;
    return verify_blake2b(slot_checksum, slot_data, slot_size);
}
} // namespace

// ============================================================================
// Slot Handles (Primitive Data Transfer API)
// ============================================================================
struct DataBlockProducerImpl;
struct DataBlockConsumerImpl;

struct SlotWriteHandleImpl
{
    DataBlockProducerImpl *owner = nullptr;
    DataBlock *dataBlock = nullptr;
    SharedMemoryHeader *header = nullptr;
    size_t slot_index = 0;
    uint64_t slot_id = 0;
    char *buffer_ptr = nullptr;
    size_t buffer_size = 0;
    char *flexible_ptr = nullptr;
    size_t flexible_size = 0;
    size_t bytes_written = 0;
    bool committed = false;
    bool released = false;
};

struct SlotConsumeHandleImpl
{
    DataBlockConsumerImpl *owner = nullptr;
    DataBlock *dataBlock = nullptr;
    SharedMemoryHeader *header = nullptr;
    size_t slot_index = 0;
    uint64_t slot_id = 0;
    const char *buffer_ptr = nullptr;
    size_t buffer_size = 0;
    const char *flexible_ptr = nullptr;
    size_t flexible_size = 0;
    bool released = false;
};

// ============================================================================
// DataBlockProducerImpl
// ============================================================================
struct DataBlockProducerImpl
{
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::EnforceOnRelease;
    std::atomic<uint64_t> writer_thread_id{0};
    std::atomic<uint32_t> writer_depth{0};

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
    if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
    {
        throw std::out_of_range("Spinlock index out of range");
    }
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
    if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
    {
        throw std::out_of_range("Spinlock index out of range");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(local);
    return SharedSpinLock(state, pImpl->name + "_spinlock_" + std::to_string(index));
}

uint32_t DataBlockProducer::spinlock_count() const
{
    return pImpl && pImpl->dataBlock ? static_cast<uint32_t>(SharedMemoryHeader::MAX_SHARED_SPINLOCKS) : 0;
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

bool DataBlockProducer::update_checksum_flexible_zone()
{
    return pImpl && pImpl->dataBlock ? update_checksum_flexible_zone_impl(pImpl->dataBlock.get())
                                     : false;
}

bool DataBlockProducer::update_checksum_slot(size_t slot_index)
{
    return pImpl && pImpl->dataBlock ? update_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
                                     : false;
}

std::unique_ptr<SlotWriteHandle> DataBlockProducer::acquire_write_slot(int timeout_ms)
{
    (void)timeout_ms; // No blocking policy implemented yet
    if (!pImpl || !pImpl->dataBlock)
        return nullptr;

    uint64_t tid = pylabhub::platform::get_native_thread_id();
    if (!enter_thread_guard(pImpl->writer_thread_id, pImpl->writer_depth, tid))
    {
        throw std::runtime_error("DataBlockProducer: acquire_write_slot recursion or cross-thread use.");
    }

    auto *h = pImpl->dataBlock->header();
    if (!h)
    {
        exit_thread_guard(pImpl->writer_thread_id, pImpl->writer_depth, tid);
        return nullptr;
    }

    uint32_t slot_count = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
    uint64_t slot_id = h->write_index.fetch_add(1, std::memory_order_acq_rel);
    size_t slot_index = static_cast<size_t>(slot_id % slot_count);

    size_t slot_size = h->unit_block_size;
    char *buf = pImpl->dataBlock->structured_data_buffer();
    if (!buf || slot_size == 0)
    {
        exit_thread_guard(pImpl->writer_thread_id, pImpl->writer_depth, tid);
        return nullptr;
    }

    auto impl = std::make_unique<SlotWriteHandleImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->header = h;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = buf + slot_index * slot_size;
    impl->buffer_size = slot_size;
    impl->flexible_ptr = pImpl->dataBlock->flexible_data_zone();
    impl->flexible_size = h->flexible_zone_size;
    return std::make_unique<SlotWriteHandle>(std::move(impl));
}

bool DataBlockProducer::release_write_slot(SlotWriteHandle &handle)
{
    if (!handle.pImpl)
        return false;
    return release_write_handle(*handle.pImpl);
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
    return pImpl && pImpl->dataBlock ? static_cast<uint32_t>(SharedMemoryHeader::NUM_COUNTERS_64) : 0;
}

// ============================================================================
// DataBlockConsumerImpl
// ============================================================================
struct DataBlockConsumerImpl
{
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::EnforceOnRelease;
    std::atomic<uint64_t> reader_thread_id{0};
    std::atomic<uint32_t> reader_depth{0};
    uint64_t last_consumed_slot_id = INVALID_SLOT_ID;

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

namespace
{
std::unique_ptr<SlotConsumeHandleImpl> build_consume_handle_impl(DataBlockConsumerImpl *owner,
                                                                 uint64_t slot_id);
bool release_write_handle(SlotWriteHandleImpl &impl);
bool release_consume_handle(SlotConsumeHandleImpl &impl);
} // namespace

// ============================================================================
// DataBlockSlotIterator (ring-buffer)
// ============================================================================
struct DataBlockSlotIteratorImpl
{
    DataBlockConsumerImpl *owner = nullptr;
    DataBlock *dataBlock = nullptr;
    uint64_t last_seen_slot_id = INVALID_SLOT_ID;
};

DataBlockSlotIterator::DataBlockSlotIterator() : pImpl(nullptr) {}

DataBlockSlotIterator::DataBlockSlotIterator(std::unique_ptr<DataBlockSlotIteratorImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockSlotIterator::~DataBlockSlotIterator() = default;

DataBlockSlotIterator::DataBlockSlotIterator(DataBlockSlotIterator &&other) noexcept = default;

DataBlockSlotIterator &DataBlockSlotIterator::operator=(DataBlockSlotIterator &&other) noexcept =
    default;

DataBlockSlotIterator::NextResult DataBlockSlotIterator::try_next(int timeout_ms)
{
    NextResult r;
    r.ok = false;
    r.error_code = 0;
    if (!pImpl || !pImpl->dataBlock)
    {
        r.error_code = 1;
        return r;
    }
    auto *h = pImpl->dataBlock->header();
    if (!h)
    {
        r.error_code = 1;
        return r;
    }

    uint64_t tid = pylabhub::platform::get_native_thread_id();
    if (!enter_thread_guard(pImpl->owner->reader_thread_id, pImpl->owner->reader_depth, tid))
    {
        r.error_code = 4; // recursion or cross-thread use
        return r;
    }

    const int poll_interval_ms = 2;
    int waited_ms = 0;
    uint64_t slot_id = h->commit_index.load(std::memory_order_acquire);
    while (slot_id == INVALID_SLOT_ID || slot_id == pImpl->last_seen_slot_id)
    {
        if (timeout_ms > 0 && waited_ms >= timeout_ms)
        {
            (void)exit_thread_guard(pImpl->owner->reader_thread_id, pImpl->owner->reader_depth, tid);
            r.error_code = 2; // timeout
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
        if (timeout_ms > 0)
            waited_ms += poll_interval_ms;
        slot_id = h->commit_index.load(std::memory_order_acquire);
    }

    auto impl = build_consume_handle_impl(pImpl->owner, slot_id);
    if (!impl)
    {
        (void)exit_thread_guard(pImpl->owner->reader_thread_id, pImpl->owner->reader_depth, tid);
        r.error_code = 3;
        return r;
    }

    r.next = SlotConsumeHandle(std::move(impl));
    r.ok = true;
    pImpl->last_seen_slot_id = slot_id;
    return r;
}

SlotConsumeHandle DataBlockSlotIterator::next(int timeout_ms)
{
    auto res = try_next(timeout_ms);
    if (!res.ok)
    {
        throw std::runtime_error("DataBlockSlotIterator::next: slot not available (error " +
                                 std::to_string(res.error_code) + ")");
    }
    return std::move(res.next);
}

void DataBlockSlotIterator::seek_latest()
{
    if (!pImpl || !pImpl->dataBlock)
        return;
    auto *h = pImpl->dataBlock->header();
    if (!h)
        return;
    pImpl->last_seen_slot_id = h->commit_index.load(std::memory_order_acquire);
}

void DataBlockSlotIterator::seek_to(uint64_t slot_id)
{
    if (!pImpl)
        return;
    pImpl->last_seen_slot_id = slot_id;
}

uint64_t DataBlockSlotIterator::last_slot_id() const
{
    return pImpl ? pImpl->last_seen_slot_id : INVALID_SLOT_ID;
}

bool DataBlockSlotIterator::is_valid() const
{
    return pImpl && pImpl->dataBlock;
}

// ============================================================================
// Slot Handles (Primitive Data Transfer API) - Implementations
// ============================================================================
namespace
{
std::unique_ptr<SlotConsumeHandleImpl> build_consume_handle_impl(DataBlockConsumerImpl *owner,
                                                                 uint64_t slot_id)
{
    if (!owner || !owner->dataBlock)
        return nullptr;
    auto *h = owner->dataBlock->header();
    if (!h)
        return nullptr;
    uint32_t slot_count = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
    size_t slot_index = static_cast<size_t>(slot_id % slot_count);
    size_t slot_size = h->unit_block_size;
    const char *buf = owner->dataBlock->structured_data_buffer();
    if (!buf || slot_size == 0)
        return nullptr;

    auto impl = std::make_unique<SlotConsumeHandleImpl>();
    impl->owner = owner;
    impl->dataBlock = owner->dataBlock.get();
    impl->header = h;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = buf + slot_index * slot_size;
    impl->buffer_size = slot_size;
    impl->flexible_ptr = owner->dataBlock->flexible_data_zone();
    impl->flexible_size = h->flexible_zone_size;
    return impl;
}

bool release_write_handle(SlotWriteHandleImpl &impl)
{
    if (impl.released)
        return true;
    bool ok = true;
    if (impl.owner && impl.owner->checksum_policy == ChecksumPolicy::EnforceOnRelease &&
        impl.header && impl.header->checksum_enabled && impl.committed)
    {
        ok &= update_checksum_slot_impl(impl.dataBlock, impl.slot_index);
        ok &= update_checksum_flexible_zone_impl(impl.dataBlock);
    }

    if (impl.owner && impl.owner->checksum_policy == ChecksumPolicy::EnforceOnRelease &&
        impl.header && impl.committed)
    {
        impl.header->current_slot_id.store(impl.slot_id, std::memory_order_release);
        impl.header->commit_index.store(impl.slot_id, std::memory_order_release);
    }

    if (impl.owner)
    {
        uint64_t tid = pylabhub::platform::get_native_thread_id();
        ok &= exit_thread_guard(impl.owner->writer_thread_id, impl.owner->writer_depth, tid);
    }
    impl.released = true;
    return ok;
}

bool release_consume_handle(SlotConsumeHandleImpl &impl)
{
    if (impl.released)
        return true;
    bool ok = true;
    if (impl.owner && impl.owner->checksum_policy == ChecksumPolicy::EnforceOnRelease &&
        impl.header && impl.header->checksum_enabled)
    {
        ok &= verify_checksum_slot_impl(impl.dataBlock, impl.slot_index);
        ok &= verify_checksum_flexible_zone_impl(impl.dataBlock);
    }

    if (impl.owner)
    {
        uint64_t tid = pylabhub::platform::get_native_thread_id();
        ok &= exit_thread_guard(impl.owner->reader_thread_id, impl.owner->reader_depth, tid);
    }
    impl.released = true;
    return ok;
}
} // namespace

SlotWriteHandle::SlotWriteHandle() : pImpl(nullptr) {}

SlotWriteHandle::SlotWriteHandle(std::unique_ptr<SlotWriteHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

SlotWriteHandle::~SlotWriteHandle()
{
    if (pImpl)
    {
        (void)release_write_handle(*pImpl);
    }
}

SlotWriteHandle::SlotWriteHandle(SlotWriteHandle &&other) noexcept = default;

SlotWriteHandle &SlotWriteHandle::operator=(SlotWriteHandle &&other) noexcept = default;

size_t SlotWriteHandle::slot_index() const
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotWriteHandle::slot_id() const
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<std::byte> SlotWriteHandle::buffer_span()
{
    if (!pImpl || !pImpl->buffer_ptr || pImpl->buffer_size == 0)
        return {};
    return {reinterpret_cast<std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<std::byte> SlotWriteHandle::flexible_zone_span()
{
    if (!pImpl || !pImpl->flexible_ptr || pImpl->flexible_size == 0)
        return {};
    return {reinterpret_cast<std::byte *>(pImpl->flexible_ptr), pImpl->flexible_size};
}

bool SlotWriteHandle::write(const void *src, size_t len, size_t offset)
{
    if (!pImpl || !pImpl->buffer_ptr || len == 0)
        return false;
    if (offset + len > pImpl->buffer_size)
        return false;
    std::memcpy(pImpl->buffer_ptr + offset, src, len);
    return true;
}

bool SlotWriteHandle::commit(size_t bytes_written)
{
    if (!pImpl || !pImpl->header)
        return false;
    if (bytes_written > pImpl->buffer_size)
        return false;
    pImpl->bytes_written = bytes_written;
    pImpl->committed = true;

    if (pImpl->owner && pImpl->owner->checksum_policy == ChecksumPolicy::Explicit)
    {
        pImpl->header->current_slot_id.store(pImpl->slot_id, std::memory_order_release);
        pImpl->header->commit_index.store(pImpl->slot_id, std::memory_order_release);
    }
    return true;
}

bool SlotWriteHandle::update_checksum_slot()
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return update_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotWriteHandle::update_checksum_flexible_zone()
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return update_checksum_flexible_zone_impl(pImpl->dataBlock);
}

SlotConsumeHandle::SlotConsumeHandle() : pImpl(nullptr) {}

SlotConsumeHandle::SlotConsumeHandle(std::unique_ptr<SlotConsumeHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

SlotConsumeHandle::~SlotConsumeHandle()
{
    if (pImpl)
    {
        (void)release_consume_handle(*pImpl);
    }
}

SlotConsumeHandle::SlotConsumeHandle(SlotConsumeHandle &&other) noexcept = default;

SlotConsumeHandle &SlotConsumeHandle::operator=(SlotConsumeHandle &&other) noexcept = default;

size_t SlotConsumeHandle::slot_index() const
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotConsumeHandle::slot_id() const
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<const std::byte> SlotConsumeHandle::buffer_span() const
{
    if (!pImpl || !pImpl->buffer_ptr || pImpl->buffer_size == 0)
        return {};
    return {reinterpret_cast<const std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<const std::byte> SlotConsumeHandle::flexible_zone_span() const
{
    if (!pImpl || !pImpl->flexible_ptr || pImpl->flexible_size == 0)
        return {};
    return {reinterpret_cast<const std::byte *>(pImpl->flexible_ptr), pImpl->flexible_size};
}

bool SlotConsumeHandle::read(void *dst, size_t len, size_t offset) const
{
    if (!pImpl || !pImpl->buffer_ptr || len == 0)
        return false;
    if (offset + len > pImpl->buffer_size)
        return false;
    std::memcpy(dst, pImpl->buffer_ptr + offset, len);
    return true;
}

bool SlotConsumeHandle::verify_checksum_slot() const
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return verify_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotConsumeHandle::verify_checksum_flexible_zone() const
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return verify_checksum_flexible_zone_impl(pImpl->dataBlock);
}

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
    if (index >= SharedMemoryHeader::MAX_SHARED_SPINLOCKS)
    {
        throw std::out_of_range("Spinlock index out of range");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(local);
    return SharedSpinLock(state, pImpl->name + "_spinlock_" + std::to_string(index));
}

uint32_t DataBlockConsumer::spinlock_count() const
{
    return pImpl && pImpl->dataBlock ? static_cast<uint32_t>(SharedMemoryHeader::MAX_SHARED_SPINLOCKS) : 0;
}

SharedSpinLock DataBlockConsumer::get_user_spinlock(size_t index)
{
    return get_spinlock(index);
}

bool DataBlockConsumer::verify_checksum_flexible_zone() const
{
    return pImpl && pImpl->dataBlock ? verify_checksum_flexible_zone_impl(pImpl->dataBlock.get())
                                     : false;
}

bool DataBlockConsumer::verify_checksum_slot(size_t slot_index) const
{
    return pImpl && pImpl->dataBlock ? verify_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
                                     : false;
}

std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(int timeout_ms)
{
    if (!pImpl || !pImpl->dataBlock)
        return nullptr;

    uint64_t tid = pylabhub::platform::get_native_thread_id();
    if (!enter_thread_guard(pImpl->reader_thread_id, pImpl->reader_depth, tid))
    {
        throw std::runtime_error("DataBlockConsumer: acquire_consume_slot recursion or cross-thread use.");
    }

    auto *h = pImpl->dataBlock->header();
    if (!h)
    {
        exit_thread_guard(pImpl->reader_thread_id, pImpl->reader_depth, tid);
        return nullptr;
    }

    const int poll_interval_ms = 2;
    int waited_ms = 0;
    uint64_t slot_id = h->commit_index.load(std::memory_order_acquire);
    while (slot_id == INVALID_SLOT_ID || slot_id == pImpl->last_consumed_slot_id)
    {
        if (timeout_ms > 0 && waited_ms >= timeout_ms)
        {
            exit_thread_guard(pImpl->reader_thread_id, pImpl->reader_depth, tid);
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
        if (timeout_ms > 0)
            waited_ms += poll_interval_ms;
        slot_id = h->commit_index.load(std::memory_order_acquire);
    }

    auto impl = build_consume_handle_impl(pImpl.get(), slot_id);
    if (!impl)
    {
        exit_thread_guard(pImpl->reader_thread_id, pImpl->reader_depth, tid);
        return nullptr;
    }

    pImpl->last_consumed_slot_id = slot_id;
    return std::make_unique<SlotConsumeHandle>(std::move(impl));
}

bool DataBlockConsumer::release_consume_slot(SlotConsumeHandle &handle)
{
    if (!handle.pImpl)
        return false;
    return release_consume_handle(*handle.pImpl);
}

DataBlockSlotIterator DataBlockConsumer::slot_iterator()
{
    if (!pImpl || !pImpl->dataBlock)
    {
        return DataBlockSlotIterator();
    }
    auto impl = std::make_unique<DataBlockSlotIteratorImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->last_seen_slot_id = INVALID_SLOT_ID;
    return DataBlockSlotIterator(std::move(impl));
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
    return pImpl && pImpl->dataBlock ? static_cast<uint32_t>(SharedMemoryHeader::NUM_COUNTERS_64) : 0;
}

// ============================================================================
// Factory Functions
// ============================================================================
std::unique_ptr<DataBlockProducer> create_datablock_producer(MessageHub &hub,
                                                             const std::string &name,
                                                             DataBlockPolicy policy,
                                                             const DataBlockConfig &config)
{
    // HEP-core-0002 §4.4: hub reserved for broker coordination. Stub until §14 Future Work.
    (void)hub;
    try
    {
        DataBlockConfig config_copy = config;
        if (config_copy.ring_buffer_capacity <= 0)
        {
            config_copy.ring_buffer_capacity =
                (policy == DataBlockPolicy::Single) ? 1
                : (policy == DataBlockPolicy::DoubleBuffer)
                    ? 2
                    : 8; // RingBuffer default
        }
        auto impl = std::make_unique<DataBlockProducerImpl>();
        impl->name = name;
        impl->checksum_policy = config_copy.checksum_policy;
        impl->dataBlock = std::make_unique<DataBlock>(name, config_copy);
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

namespace
{
bool validate_config_against_header(const SharedMemoryHeader *h, const DataBlockConfig &expected)
{
    if (h->flexible_zone_size != expected.flexible_zone_size)
        return false;
    if (static_cast<DataBlockUnitSize>(h->unit_block_size) != expected.unit_block_size)
        return false;
    size_t exp_slots = (expected.ring_buffer_capacity > 0)
                           ? static_cast<size_t>(expected.ring_buffer_capacity)
                           : 1u;
    if (h->ring_buffer_capacity != exp_slots)
        return false;
    if (h->structured_buffer_size != expected.structured_buffer_size())
        return false;
    if ((h->checksum_enabled != 0) != expected.enable_checksum)
        return false;
    return true;
}
} // namespace

std::unique_ptr<DataBlockConsumer> find_datablock_consumer(MessageHub &hub,
                                                            const std::string &name,
                                                            uint64_t shared_secret)
{
    // HEP-core-0002 §4.4: hub reserved for broker discovery. Stub until §14 Future Work.
    (void)hub;
    try
    {
        auto impl = std::make_unique<DataBlockConsumerImpl>();
        impl->name = name;
        impl->checksum_policy = ChecksumPolicy::EnforceOnRelease;
        impl->dataBlock = std::make_unique<DataBlock>(name);
        auto *h = impl->dataBlock->header();
        if (h->magic_number != DATABLOCK_MAGIC_NUMBER)
        {
            throw std::runtime_error("Invalid magic number");
        }
        if (h->shared_secret != shared_secret)
        {
            throw std::runtime_error("Invalid shared secret");
        }
        h->active_consumer_count.fetch_add(1, std::memory_order_acq_rel);
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

std::unique_ptr<DataBlockConsumer> find_datablock_consumer(MessageHub &hub,
                                                            const std::string &name,
                                                            uint64_t shared_secret,
                                                            const DataBlockConfig &expected_config)
{
    // HEP-core-0002 §4.4: hub reserved for broker discovery. Stub until §14 Future Work.
    (void)hub;
    try
    {
        auto impl = std::make_unique<DataBlockConsumerImpl>();
        impl->name = name;
        impl->checksum_policy = expected_config.checksum_policy;
        impl->dataBlock = std::make_unique<DataBlock>(name);
        auto *h = impl->dataBlock->header();
        if (h->magic_number != DATABLOCK_MAGIC_NUMBER)
        {
            throw std::runtime_error("Invalid magic number");
        }
        if (h->shared_secret != shared_secret)
        {
            throw std::runtime_error("Invalid shared secret");
        }
        if (!validate_config_against_header(h, expected_config))
        {
            throw std::runtime_error(
                "DataBlock config mismatch: unit_block_size, flexible_zone_size, "
                "ring_buffer_capacity, or checksum_enabled inconsistent with expected");
        }
        h->active_consumer_count.fetch_add(1, std::memory_order_acq_rel);
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
;
        return nullptr;
    }
}

} // namespace pylabhub::hub
