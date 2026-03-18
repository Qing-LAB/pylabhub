#include "data_block_internal.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pylabhub::hub
{
using namespace internal;

// ============================================================================
// system_page_size — runtime OS page size query
// ============================================================================

DataBlockPageSize system_page_size()
{
    long page_bytes = 4096; // fallback
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page_bytes = static_cast<long>(si.dwPageSize);
#else
    const long sc = sysconf(_SC_PAGESIZE);
    if (sc > 0)
    {
        page_bytes = sc;
    }
#endif
    const auto ps = static_cast<size_t>(page_bytes);
    if (ps <= to_bytes(DataBlockPageSize::Size4K))
    {
        return DataBlockPageSize::Size4K;
    }
    if (ps <= to_bytes(DataBlockPageSize::Size4M))
    {
        return DataBlockPageSize::Size4M;
    }
    return DataBlockPageSize::Size16M;
}

// ============================================================================
// DataBlockLayout – single control surface for memory model
// ============================================================================
// All layout, sizes, and derived access (slot stride, offsets) come from this struct.
// Populated once at init from config (creator) or from header (attacher). All access
// (slot buffer pointer/size, checksum region, flexible zone) uses layout only.
// Validation entry points: validate_header_layout_hash (ABI); validate_attach_layout_and_config
// (layout checksum + optional config match); used by find_datablock_consumer_impl and recovery.
// ============================================================================
// ============================================================================
// DataBlockLayout - Memory Layout Calculator and Validator
// ============================================================================
// CRITICAL: This is the SINGLE SOURCE OF TRUTH for all memory layout calculations.
// All layout offsets, sizes, and validations MUST go through these functions.
// Do NOT duplicate layout logic elsewhere in the codebase.
//
// Design: HEP-CORE-0002 §3 (Memory Layout and Data Structures)
// Memory Structure:
//   [Header 4K] [Control Zone → 4K pad] [Flex Zone N×4K] [Ring-Buffer]
//
struct DataBlockLayout
{
    size_t slot_rw_state_offset = 0;
    size_t slot_rw_state_size = 0;
    size_t slot_checksum_offset = 0;
    size_t slot_checksum_size = 0;
    // Phase 2 memory layout (per DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md §2.3):
    //   - flexible_zone_offset: PAGE_ALIGNMENT-aligned (start of DATA region)
    //   - flexible_zone_size: rounded up to PAGE_ALIGNMENT at creation; 0 if no flex zone
    //   - structured_buffer_offset: PAGE_ALIGNMENT-aligned (= flex_zone_offset + flex_zone_size)
    size_t flexible_zone_offset = 0;
    size_t flexible_zone_size = 0;
    size_t structured_buffer_offset = 0; // Ring-buffer offset (4K-aligned)
    size_t structured_buffer_size = 0;
    size_t total_size = 0;
    /** Slot stride (bytes per slot). Single source for slot buffer pointer arithmetic. */
    size_t slot_stride_bytes_ = 0;
    /** Physical page size (bytes). Allocation granularity. */
    size_t physical_page_size_bytes = 0;
    /** Effective slot count (single source; 0 capacity treated as 1). */
    uint32_t slot_count = 0;
    
    // === Layout Query APIs (Public Interface) ===
    // These are the ONLY way to query layout information
    
    /** Get control zone total size (SlotRWState + SlotChecksum arrays) */
    [[nodiscard]] size_t control_zone_size() const noexcept {
        return slot_rw_state_size + slot_checksum_size;
    }
    
    /** Get control zone end offset (before padding to 4K) */
    [[nodiscard]] size_t control_zone_end() const noexcept {
        return slot_rw_state_offset + control_zone_size();
    }
    
    /** Check if flex zone is configured (size > 0) */
    [[nodiscard]] bool has_flex_zone() const noexcept {
        return flexible_zone_size > 0;
    }
    
    /** Get flex zone pointer from base address */
    [[nodiscard]] char* flex_zone_ptr(void* base) const noexcept {
        return (base != nullptr && has_flex_zone()) 
            ? static_cast<char*>(base) + flexible_zone_offset 
            : nullptr;
    }
    
    /** Get ring-buffer pointer from base address */
    [[nodiscard]] char* ring_buffer_ptr(void* base) const noexcept {
        return (base != nullptr) 
            ? static_cast<char*>(base) + structured_buffer_offset 
            : nullptr;
    }
    
    /** Get slot pointer from base address and slot index */
    [[nodiscard]] char* slot_ptr(void* base, size_t slot_index) const noexcept {
        if (base == nullptr || slot_index >= slot_count) {
            return nullptr;
        }
        return static_cast<char*>(base) + structured_buffer_offset + (slot_index * slot_stride_bytes_);
    }
    
    // === Layout Factory Methods (Creation) ===
    // These are the ONLY way to create valid layouts

    static DataBlockLayout from_config(const DataBlockConfig &config)
    {
        DataBlockLayout layout{};
        layout.slot_count =
            (config.ring_buffer_capacity > 0) ? config.ring_buffer_capacity : 1U;
        layout.slot_rw_state_offset = sizeof(SharedMemoryHeader);
        layout.slot_rw_state_size = layout.slot_count * sizeof(SlotRWState);
        layout.slot_checksum_size =
            (config.checksum_type != ChecksumType::Unset)
                ? (layout.slot_count * detail::SLOT_CHECKSUM_ENTRY_SIZE)
                : 0;
        layout.slot_checksum_offset = layout.slot_rw_state_offset + layout.slot_rw_state_size;
        
        // Memory layout per HEP-CORE-0002 §3 (Memory Layout and Data Structures):
        // 
        // Memory structure:
        //   1. Global Header (4K or 8K)
        //   2. Control Zone (SlotRWState + SlotChecksum arrays, padded to 4K)
        //   3. DATA REGION (4K-aligned):
        //      - Flex zone (N×4K)
        //      - Ring-buffer (M × logical_unit_size)
        
        const size_t control_zone_size = layout.slot_rw_state_size + layout.slot_checksum_size;
        const size_t control_zone_end = layout.slot_rw_state_offset + control_zone_size;
        
        // Align data region start to PAGE_ALIGNMENT boundary (design §2.3)
        const size_t data_region_offset =
            (control_zone_end + detail::PAGE_ALIGNMENT - 1) & ~(detail::PAGE_ALIGNMENT - 1);

        // Single flex zone (Phase 2 refactoring)
        // Round up to the next PAGE_ALIGNMENT boundary (OS mmap granularity).
        // flex_zone_size=sizeof(MyType) is valid input; it will be rounded up transparently.
        // The rounded value is stored in the header; consumers applying the same rounding
        // will always agree on the size.
        layout.flexible_zone_size =
            (config.flex_zone_size + detail::PAGE_ALIGNMENT - 1) & ~(detail::PAGE_ALIGNMENT - 1);
        
        // Layout per design §2.3:
        //   flex_zone_offset = data_region_offset
        //   ring_buffer_offset = data_region_offset + flex_zone_size
        layout.flexible_zone_offset = data_region_offset;
        
        // Ring-buffer parameters
        layout.slot_stride_bytes_ = config.effective_logical_unit_size();
        layout.physical_page_size_bytes = to_bytes(config.physical_page_size);
        layout.structured_buffer_size = config.structured_buffer_size();
        
        // Ring-buffer starts immediately after flex zone
        // Since data_region_offset is PAGE_ALIGNMENT-aligned and flex_zone_size is
        // PAGE_ALIGNMENT-aligned, ring_buffer is guaranteed to be PAGE_ALIGNMENT-aligned (design §2.2)
        layout.structured_buffer_offset = layout.flexible_zone_offset + layout.flexible_zone_size;
        
        // Validate ring-buffer alignment (design requirement)
        if (layout.structured_buffer_offset % detail::PAGE_ALIGNMENT != 0) {
            throw std::logic_error(
                "Internal error: ring-buffer offset is not PAGE_ALIGNMENT ("
                    + std::to_string(detail::PAGE_ALIGNMENT) + " bytes)-aligned. "
                      "This violates the memory layout design.");
        }
        
        // Round total_size up to PAGE_ALIGNMENT: with sub-4K slots the ring buffer
        // size may not be a page multiple (e.g. 100 × 64 B = 6400 B), but the OS
        // always allocates/maps in page-sized chunks.  Making total_size agree with
        // the OS rounding avoids mismatches between producer and consumer.
        layout.total_size =
            (layout.structured_buffer_offset + layout.structured_buffer_size +
             detail::PAGE_ALIGNMENT - 1) &
            ~(detail::PAGE_ALIGNMENT - 1);
        return layout;
    }

    static DataBlockLayout from_header(const SharedMemoryHeader *header)
    {
        DataBlockLayout layout{};
        if (header == nullptr)
        {
            return layout;
        }
        layout.slot_count = detail::get_slot_count(header);
        layout.slot_rw_state_offset = sizeof(SharedMemoryHeader);
        layout.slot_rw_state_size = layout.slot_count * sizeof(SlotRWState);
        layout.slot_checksum_size =
            (detail::get_checksum_type(header) != ChecksumType::Unset)
                ? (layout.slot_count * detail::SLOT_CHECKSUM_ENTRY_SIZE)
                : 0;
        layout.slot_checksum_offset = layout.slot_rw_state_offset + layout.slot_rw_state_size;
        
        // Phase 2 refactoring: Calculate data region offset per design
        const size_t control_zone_size = layout.slot_rw_state_size + layout.slot_checksum_size;
        const size_t control_zone_end = layout.slot_rw_state_offset + control_zone_size;
        
        const size_t data_region_offset =
            (control_zone_end + detail::PAGE_ALIGNMENT - 1) & ~(detail::PAGE_ALIGNMENT - 1);
        
        // Flex zone offset and size from header
        layout.flexible_zone_size = header->flexible_zone_size;
        layout.flexible_zone_offset = data_region_offset;
        
        // Ring-buffer parameters
        layout.slot_stride_bytes_ = static_cast<size_t>(detail::get_slot_stride_bytes(header));
        layout.physical_page_size_bytes = static_cast<size_t>(header->physical_page_size);
        layout.structured_buffer_size = layout.slot_count * layout.slot_stride_bytes_;
        
        // Ring-buffer starts immediately after flex zone
        // Since data_region_offset is PAGE_ALIGNMENT-aligned and flex_zone_size is
        // PAGE_ALIGNMENT-aligned, ring_buffer is guaranteed to be PAGE_ALIGNMENT-aligned (design §2.2)
        layout.structured_buffer_offset = layout.flexible_zone_offset + layout.flexible_zone_size;
        
        // Validate ring-buffer alignment (design requirement)
        if (layout.structured_buffer_offset % detail::PAGE_ALIGNMENT != 0) {
            throw std::logic_error(
                "Internal error: ring-buffer offset is not PAGE_ALIGNMENT ("
                    + std::to_string(detail::PAGE_ALIGNMENT) + " bytes)-aligned. "
                      "This violates the memory layout design.");
        }
        
        // Round up to PAGE_ALIGNMENT (same reason as from_config: sub-4K slots may
        // leave the raw sum non-page-aligned; explicit rounding keeps producer/consumer
        // in agreement without relying on implicit OS rounding of ftruncate/mmap).
        layout.total_size =
            (layout.structured_buffer_offset + layout.structured_buffer_size +
             detail::PAGE_ALIGNMENT - 1) &
            ~(detail::PAGE_ALIGNMENT - 1);
        return layout;
    }

    /** Slot buffer stride (bytes per slot). Use for all slot buffer pointer arithmetic. */
    [[nodiscard]] size_t slot_stride_bytes() const { return slot_stride_bytes_; }
    /** Physical page size (bytes). Allocation granularity. */
    [[nodiscard]] size_t physical_page_size() const { return physical_page_size_bytes; }
    /** Effective slot count. Use this for all slot index bounds; do not read header directly. */
    [[nodiscard]] uint32_t slot_count_value() const { return slot_count; }

    char *slot_checksum_base(char *segment_base) const
    {
        return segment_base + slot_checksum_offset;
    }
    const char *slot_checksum_base(const char *segment_base) const
    {
        return segment_base + slot_checksum_offset;
    }

#if !defined(NDEBUG)
    [[nodiscard]] bool validate() const
    {
        // Validate header is at start
        if (slot_rw_state_offset != sizeof(SharedMemoryHeader))
        {
            return false;
        }
        
        // Validate control zone is contiguous
        if (slot_checksum_offset != slot_rw_state_offset + slot_rw_state_size)
        {
            return false;
        }
        
        // Validate PAGE_ALIGNMENT-aligned data region start
        // flexible_zone_offset must be PAGE_ALIGNMENT-aligned and >= control zone end
        const size_t control_zone_end = slot_checksum_offset + slot_checksum_size;
        if (flexible_zone_offset % detail::PAGE_ALIGNMENT != 0)
        {
            return false;
        }
        if (flexible_zone_offset < control_zone_end)
        {
            return false;
        }
        
        // Validate flex zone size is PAGE_ALIGNMENT-aligned
        if (flexible_zone_size % detail::PAGE_ALIGNMENT != 0)
        {
            return false;
        }
        
        // Validate ring-buffer follows flex zone immediately (no padding)
        if (structured_buffer_offset != flexible_zone_offset + flexible_zone_size)
        {
            return false;
        }
        
        // Validate ring-buffer is PAGE_ALIGNMENT-aligned
        if (structured_buffer_offset % detail::PAGE_ALIGNMENT != 0)
        {
            return false;
        }
        
        // Validate total size: total_size is rounded up to PAGE_ALIGNMENT so sub-4K
        // ring buffers agree with the OS mmap granularity.  Accept the rounded value.
        const size_t expected_total =
            (structured_buffer_offset + structured_buffer_size +
             detail::PAGE_ALIGNMENT - 1) & ~(detail::PAGE_ALIGNMENT - 1);
        return total_size == expected_total;
    }
#endif
};

// ============================================================================
// Phase 2 refactoring: FlexibleZoneInfo and build_flexible_zone_info removed
// Single flex zone design (N×4K), no need for multi-zone mapping
// ============================================================================

// ============================================================================
// Flexible Zone Access Helpers (Single Zone Design)
// ============================================================================
// CRITICAL: These are the ONLY implementations for flex zone access.
// All public APIs must delegate to these helpers.
//
namespace detail {

/** Validate and get flex zone span for producer/write handle.
 *  Uses DataBlockLayout as single source of truth for memory offsets.
 *  All offsets are absolute from segment() (the shared memory base pointer). */
template<typename ImplT>
inline std::span<std::byte> get_flex_zone_span_mutable(ImplT* impl) noexcept {
    if (impl == nullptr || impl->dataBlock == nullptr) {
        return {};
    }
    const auto& layout = impl->dataBlock->layout();
    if (layout.flexible_zone_size == 0) {
        return {};
    }
    char* base = static_cast<char*>(impl->dataBlock->segment());
    if (base == nullptr) {
        return {};
    }
    return {reinterpret_cast<std::byte*>(base + layout.flexible_zone_offset),
            layout.flexible_zone_size};
}

/** Validate and get flex zone span for consumer/read handle (const).
 *  Uses DataBlockLayout as single source of truth for memory offsets.
 *  All offsets are absolute from segment() (the shared memory base pointer). */
template<typename ImplT>
inline std::span<const std::byte> get_flex_zone_span_const(const ImplT* impl) noexcept {
    if (impl == nullptr || impl->dataBlock == nullptr) {
        return {};
    }
    const auto& layout = impl->dataBlock->layout();
    if (layout.flexible_zone_size == 0) {
        return {};
    }
    const char* base = static_cast<const char*>(impl->dataBlock->segment());
    if (base == nullptr) {
        return {};
    }
    return {reinterpret_cast<const std::byte*>(base + layout.flexible_zone_offset),
            layout.flexible_zone_size};
}

} // namespace detail

// ============================================================================
// DataBlock - Internal helper
// ============================================================================
class DataBlock
{
  public:
    // Single point of config validation and memory creation; do not add alternate creation paths without updating this.
    DataBlock(const std::string &name, const DataBlockConfig &config)
        : m_name(name), m_open_mode(DataBlockOpenMode::Create)
    {
        if (config.policy == DataBlockPolicy::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.policy must be set explicitly (Single, DoubleBuffer, or RingBuffer).", name);
            throw std::invalid_argument("DataBlockConfig::policy must be set explicitly");
        }
        if (config.consumer_sync_policy == ConsumerSyncPolicy::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.consumer_sync_policy must be set explicitly (Latest_only, Sequential, or Sequential_sync).", name);
            throw std::invalid_argument("DataBlockConfig::consumer_sync_policy must be set explicitly");
        }
        if (config.physical_page_size == DataBlockPageSize::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.physical_page_size must be set explicitly (Size4K, Size4M, or Size16M).", name);
            throw std::invalid_argument("DataBlockConfig::physical_page_size must be set explicitly");
        }
        if (config.ring_buffer_capacity == 0)
        {
            LOGGER_ERROR("DataBlock '{}': config.ring_buffer_capacity must be set explicitly (>= 1).", name);
            throw std::invalid_argument("DataBlockConfig::ring_buffer_capacity must be set (1 or more)");
        }
        if (config.checksum_type == ChecksumType::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.checksum_type must be set (e.g. BLAKE2b). Checksum is mandatory.", name);
            throw std::invalid_argument("DataBlockConfig::checksum_type must be set");
        }
        // Note: logical_unit_size is intentionally not validated for alignment here.
        // effective_logical_unit_size() rounds up to the nearest 64-byte cache-line boundary
        // (and further to physical_page_size for multi-page slots) transparently.
        m_layout = DataBlockLayout::from_config(config);
        m_size = m_layout.total_size;
#if !defined(NDEBUG)
        assert(m_layout.validate() && "DataBlockLayout invariant violated");
#endif

        m_shm = pylabhub::platform::shm_create(m_name.c_str(), m_size,
                                               pylabhub::platform::SHM_CREATE_UNLINK_FIRST);
        if (m_shm.base == nullptr)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            throw std::runtime_error(
                fmt::format("Failed to create file mapping for '{}'. Error: {}", m_name, GetLastError()));
#else
            throw std::runtime_error(
                fmt::format("shm_create failed for '{}'. Error: {}", m_name, errno));
#endif
        }
        // Placement new for SharedMemoryHeader (value-initialize for deterministic layout; zero padding)
        m_header = new (m_shm.base) SharedMemoryHeader{};

        // 2. Initialize SharedMemoryHeader fields
        m_header->version_major = HEADER_VERSION_MAJOR;
        m_header->version_minor = HEADER_VERSION_MINOR;
        m_header->total_block_size = m_size;

        pylabhub::crypto::generate_random_bytes(
            m_header->shared_secret, sizeof(m_header->shared_secret)); // Generate random secret
        if (config.shared_secret != 0)
        {
            std::memcpy(m_header->shared_secret, &config.shared_secret,
                        sizeof(config.shared_secret)); // Store capability for discovery
        }

        m_header->schema_version = 0; // Will be set by factory function if schema is used
        std::memset(m_header->flexzone_schema_hash, 0, sizeof(m_header->flexzone_schema_hash));
        std::memset(m_header->datablock_schema_hash, 0, sizeof(m_header->datablock_schema_hash));

        m_header->policy = config.policy;
        m_header->consumer_sync_policy = config.consumer_sync_policy;
        m_header->physical_page_size = static_cast<uint32_t>(to_bytes(config.physical_page_size));
        {
            const size_t logical = config.effective_logical_unit_size();
            if (logical > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
            {
                throw std::invalid_argument("logical_unit_size exceeds maximum storable in header");
            }
            // Always store resolved bytes; never 0 (0 at config input means use physical).
            m_header->logical_unit_size = static_cast<uint32_t>(logical);
        }
        m_header->ring_buffer_capacity = config.ring_buffer_capacity;
        m_header->flexible_zone_size = m_layout.flexible_zone_size;
        m_header->checksum_type = static_cast<uint8_t>(config.checksum_type);
        m_header->checksum_policy = config.checksum_policy;

        // Initialize hot path indices
        m_header->write_index.store(0, std::memory_order_release);
        m_header->commit_index.store(INVALID_SLOT_ID, std::memory_order_release);
        m_header->read_index.store(0, std::memory_order_release);
        m_header->active_consumer_count.store(0, std::memory_order_release);

        // All metric fields are zeroed by SharedMemoryHeader{} value-initialization above.
        // Only creation_timestamp_ns requires an explicit non-zero value.
        m_header->creation_timestamp_ns.store(
            pylabhub::platform::monotonic_time_ns(),
            std::memory_order_release);

        // Consumer Heartbeat char fields are zeroed explicitly for clarity
        // (all fields are zero-initialized by SharedMemoryHeader{} above).
        for (auto &consumer_heartbeat : m_header->consumer_heartbeats)
        {
            std::memset(consumer_heartbeat.consumer_uid, 0, sizeof(consumer_heartbeat.consumer_uid));
            std::memset(consumer_heartbeat.consumer_name, 0,
                        sizeof(consumer_heartbeat.consumer_name));
        }

        // Initialize Channel Identity fields (written by producer create; empty until security phase)
        std::memset(m_header->hub_uid, 0, sizeof(m_header->hub_uid));
        std::memset(m_header->hub_name, 0, sizeof(m_header->hub_name));
        std::memset(m_header->producer_uid, 0, sizeof(m_header->producer_uid));
        std::memset(m_header->producer_name, 0, sizeof(m_header->producer_name));

        // Initialize SharedSpinLock states (same factory logic as in-process spinlock)
        for (auto &spinlock_state : m_header->spinlock_states)
        {
            init_spinlock_state(&spinlock_state);
        }

        // 3. Initialize SlotRWState array (using layout)
        m_slot_rw_states_array = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(m_shm.base) + m_layout.slot_rw_state_offset);

        for (uint32_t i = 0; i < m_layout.slot_count_value(); ++i)
        {
            m_slot_rw_states_array[i].write_lock.store(0, std::memory_order_release);
            m_slot_rw_states_array[i].reader_count.store(0, std::memory_order_release);
            m_slot_rw_states_array[i].slot_state.store(SlotRWState::SlotState::FREE,
                                                       std::memory_order_release);
            m_slot_rw_states_array[i].writer_waiting.store(0, std::memory_order_release);
            m_slot_rw_states_array[i].write_generation.store(0, std::memory_order_release);
        }

        // 4. Set pointers from layout
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_shm.base) + m_layout.flexible_zone_offset;
        m_structured_data_buffer =
            reinterpret_cast<char *>(m_shm.base) + m_layout.structured_buffer_offset;

        // Phase 2 refactoring: Single flex zone, no need to populate flexible_zone_info map

        // Sequential_sync: initialize per-consumer read positions in reserved_header
        for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
        {
            consumer_next_read_slot_ptr(m_header, i)->store(0, std::memory_order_release);
        }

        std::atomic_thread_fence(std::memory_order_release);
        m_header->magic_number.store(
            detail::DATABLOCK_MAGIC_NUMBER,
            std::memory_order_release); // Set magic number last for atomicity

        // Store header layout hash for protocol check (consumer validates same ABI)
        pylabhub::schema::SchemaInfo header_schema = get_shared_memory_header_schema_info();
        std::memcpy(m_header->reserved_header + detail::HEADER_LAYOUT_HASH_OFFSET,
                    header_schema.hash.data(), detail::HEADER_LAYOUT_HASH_SIZE);
        // Store layout checksum (segment layout-defining values; validated on attach and integrity)
        store_layout_checksum(m_header);

        // Initialize producer heartbeat (creator is initial producer)
        detail::update_producer_heartbeat_impl(m_header, pylabhub::platform::get_pid());

        LOGGER_INFO("DataBlock '{}' created with total size {} bytes.", m_name, m_size);
    }

    // WriteAttach / ReadAttach constructor: attaches to an existing segment without creating or
    // initializing. Used for source processes (WriteAttach) and diagnostic tools (ReadAttach).
    // The caller is responsible for passing the correct mode.
    DataBlock(std::string name, DataBlockOpenMode mode)
        : m_name(std::move(name)), m_open_mode(mode)
    {
        m_shm = pylabhub::platform::shm_attach(m_name.c_str());
        if (m_shm.base == nullptr)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            throw std::runtime_error(
                fmt::format("Failed to open file mapping for attaching '{}'. Error: {}", m_name, GetLastError()));
#else
            throw std::runtime_error(
                fmt::format("shm_attach failed for attaching '{}'. Error: {}", m_name, errno));
#endif
        }
        m_size = m_shm.size;
        m_header = reinterpret_cast<SharedMemoryHeader *>(m_shm.base);

        // Wait for creator to fully initialize the header.
        if (!wait_for_header_magic_valid(m_header, get_attach_timeout_ms()))
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' initialization timeout - creator may have crashed.", m_name));
        }

        // Validate version compatibility
        if (m_header->version_major != HEADER_VERSION_MAJOR ||
            m_header->version_minor > HEADER_VERSION_MINOR)
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' version mismatch. Creator: {}.{}, Attacher: {}.{}",
                            m_name, m_header->version_major, m_header->version_minor,
                            HEADER_VERSION_MAJOR, HEADER_VERSION_MINOR));
        }

        // Validate total size
        if (m_size != m_header->total_block_size)
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' size mismatch. Expected {}, got {}",
                            m_name, m_header->total_block_size, m_size));
        }

        // Reconstruct layout from header (no config available)
        m_layout = DataBlockLayout::from_header(m_header);
#if !defined(NDEBUG)
        assert(m_layout.validate() && "DataBlockLayout invariant violated");
#endif
        m_slot_rw_states_array = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(m_shm.base) + m_layout.slot_rw_state_offset);
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_shm.base) + m_layout.flexible_zone_offset;
        m_structured_data_buffer =
            reinterpret_cast<char *>(m_shm.base) + m_layout.structured_buffer_offset;

        const char *mode_str = (mode == DataBlockOpenMode::WriteAttach) ? "write-attach" : "read-attach";
        LOGGER_INFO("DataBlock '{}' attached ({}) with total size {} bytes.", m_name, mode_str, m_size);
    }

    DataBlock(std::string name) : m_name(std::move(name)), m_open_mode(DataBlockOpenMode::ReadAttach)
    {
        m_shm = pylabhub::platform::shm_attach(m_name.c_str());
        if (m_shm.base == nullptr)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            throw std::runtime_error(
                fmt::format("Failed to open file mapping for consumer '{}'. Error: {}", m_name, GetLastError()));
#else
            throw std::runtime_error(
                fmt::format("shm_attach failed for consumer '{}'. Error: {}", m_name, errno));
#endif
        }
        m_size = m_shm.size;

        m_header = reinterpret_cast<SharedMemoryHeader *>(m_shm.base);

        // Wait for producer to fully initialize the header.
        if (!wait_for_header_magic_valid(m_header, get_attach_timeout_ms()))
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' initialization timeout - producer may have crashed or not fully initialized.", m_name));
        }

        // Validate version compatibility
        if (m_header->version_major != HEADER_VERSION_MAJOR ||
            m_header->version_minor >
                HEADER_VERSION_MINOR) // Consumer can read older minor versions
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' version mismatch. Producer: {}.{}, Consumer: {}.{}",
                            m_name, m_header->version_major, m_header->version_minor,
                            HEADER_VERSION_MAJOR, HEADER_VERSION_MINOR));
        }

        // Validate total size
        if (m_size != m_header->total_block_size)
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' size mismatch. Expected {}, got {}",
                            m_name, m_header->total_block_size, m_size));
        }

        // Calculate pointers from layout (single source of truth)
        m_layout = DataBlockLayout::from_header(m_header);
#if !defined(NDEBUG)
        assert(m_layout.validate() && "DataBlockLayout invariant violated");
#endif
        m_slot_rw_states_array = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(m_shm.base) + m_layout.slot_rw_state_offset);
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_shm.base) + m_layout.flexible_zone_offset;
        m_structured_data_buffer =
            reinterpret_cast<char *>(m_shm.base) + m_layout.structured_buffer_offset;

        // Populate flexible zone info map (this is tricky for consumer as it doesn't have config)
        // This will be handled in a factory function with expected_config.
        // For now, this constructor doesn't know about individual flexible zones.

        LOGGER_INFO("DataBlock '{}' opened by consumer. Total size {} bytes.", m_name, m_size);
    }

    ~DataBlock()
    {
        pylabhub::platform::shm_close(&m_shm);
        if (m_open_mode == DataBlockOpenMode::Create)
        {
            pylabhub::platform::shm_unlink(m_name.c_str());
            LOGGER_INFO("DataBlock '{}' shared memory removed.", m_name);
        }
    }

    [[nodiscard]] SharedMemoryHeader *header() const { return m_header; }
    [[nodiscard]] char *flexible_data_zone() const { return m_flexible_data_zone; }
    [[nodiscard]] char *structured_data_buffer() const { return m_structured_data_buffer; }
    [[nodiscard]] void *segment() const { return m_shm.base; }
    [[nodiscard]] size_t size() const { return m_size; }

    size_t acquire_shared_spinlock(const std::string &debug_name)
    {
        // Simple allocation: find first spinlock with owner_pid == 0
        for (size_t i = 0; i < MAX_SHARED_SPINLOCKS; ++i)
        {
            uint64_t expected_pid = 0;
            if (m_header->spinlock_states[i].owner_pid.compare_exchange_strong(
                    expected_pid, 1, // Use 1 as "allocated but not locked" marker
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                // Successfully claimed this spinlock slot; reset to free state (same as factory)
                init_spinlock_state(&m_header->spinlock_states[i]);
                LOGGER_INFO("DataBlock '{}': Acquired spinlock slot {} for '{}'.", m_name, i,
                            debug_name);
                return i;
            }
        }
        throw std::runtime_error(
            fmt::format("DataBlock '{}': No free spinlock slots.", m_name));
    }

    void release_shared_spinlock(size_t index)
    {
        if (index >= detail::MAX_SHARED_SPINLOCKS)
        {
            throw std::out_of_range("Spinlock index out of range.");
        }
        if (m_header->spinlock_states[index].owner_pid.load(std::memory_order_acquire) != 0)
        {
            LOGGER_WARN("DataBlock '{}': Releasing spinlock {} still held. Force releasing.",
                        m_name, index);
        }
        init_spinlock_state(&m_header->spinlock_states[index]);
        LOGGER_INFO("DataBlock '{}': Released spinlock slot {}.", m_name, index);
    }

    SharedSpinLockState *get_shared_spinlock_state(size_t index)
    {
        if (index >= detail::MAX_SHARED_SPINLOCKS)
        {
            return nullptr; // Programming error; caller checks nullptr
        }
        return &m_header->spinlock_states[index];
    }

    // Returns nullptr on out-of-range (programming error) rather than throwing, so that
    // noexcept callers (acquire_write_slot, acquire_consume_slot, try_next) can handle it
    // via their existing null checks without risking std::terminate through noexcept.
    [[nodiscard]] SlotRWState *slot_rw_state(size_t index) const noexcept
    {
        if (m_header == nullptr || index >= m_layout.slot_count_value())
        {
            return nullptr;
        }
        return &m_slot_rw_states_array[index];
    }
    
    // Phase 2 refactoring: flexible_zone_info() and set_flexible_zone_info_for_attach() removed
    // Single flex zone no longer needs named zone mapping
    
    [[nodiscard]] const DataBlockLayout &layout() const { return m_layout; }

  private:
    std::string m_name;
    DataBlockOpenMode m_open_mode;
    pylabhub::platform::ShmHandle m_shm{};
    size_t m_size = 0; // Cached from m_shm.size for convenience
    DataBlockLayout m_layout{};

    SharedMemoryHeader *m_header = nullptr;
    SlotRWState *m_slot_rw_states_array = nullptr; // New member
    char *m_flexible_data_zone = nullptr;
    char *m_structured_data_buffer = nullptr;
    // Removed m_management_mutex as it's no longer managed by DataBlock directly
    // Phase 2 refactoring: m_flexible_zone_info removed (single flex zone, no named mapping)
};

// ============================================================================
// DataBlockDiagnosticHandle (opaque attach for recovery/tooling)
// ============================================================================
struct DataBlockDiagnosticHandleImpl
{
    SharedMemoryHeader *header_ptr = nullptr;
    SlotRWState *slot_rw_states = nullptr;
    uint32_t ring_buffer_capacity = 0;
    pylabhub::platform::ShmHandle m_shm{};
};

namespace
{
/// Definition (DataBlock is complete here).
inline std::pair<SharedMemoryHeader *, uint32_t> get_header_and_slot_count(DataBlock *dataBlock)
{
    if (dataBlock == nullptr)
    {
        return {nullptr, 0};
    }
    SharedMemoryHeader *hdr = dataBlock->header();
    if (hdr == nullptr)
    {
        return {nullptr, 0};
    }
    return {hdr, dataBlock->layout().slot_count_value()};
}

inline bool update_checksum_flexible_zone_impl(DataBlock *block)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    auto *hdr = block->header();

    // Phase 2: Single flex zone (always at index 0)
    constexpr size_t flex_zone_idx = 0;
    
    const auto &layout = block->layout();
    if (layout.flexible_zone_size == 0)
    {
        return false; // No flex zone configured
    }

    char *flex = block->flexible_data_zone();
    size_t len = layout.flexible_zone_size;
    char *zone_ptr = flex; // Single zone starts at offset 0

    if (zone_ptr == nullptr || len == 0)
    {
        return false;
    }

    // Checksum data stored in SharedMemoryHeader::flexible_zone_checksums[0]
    if (!pylabhub::crypto::compute_blake2b(
            hdr->flexible_zone_checksums[flex_zone_idx].checksum_bytes, zone_ptr, len))
    {
        return false;
    }

    hdr->flexible_zone_checksums[flex_zone_idx].valid.store(
        1, std::memory_order_release);
    return true;
}

inline bool update_checksum_slot_impl(DataBlock *block, size_t slot_index)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    if (slot_index >= block->layout().slot_count_value())
    {
        return false;
    }
    // Slot data pointer: step size = logical (slot_stride_bytes), so ring iteration uses logical size.
    const size_t slot_size = block->layout().slot_stride_bytes();
    if (slot_size == 0)
    {
        return false;
    }
    char *buf = block->structured_data_buffer();
    if (buf == nullptr)
    {
        return false;
    }
    char *base = reinterpret_cast<char *>(block->segment());
    char *slot_checksum_base_ptr = block->layout().slot_checksum_base(base);
    auto *slot_checksum = reinterpret_cast<uint8_t *>(
        slot_checksum_base_ptr + slot_index * detail::SLOT_CHECKSUM_ENTRY_SIZE);
    auto *slot_valid =
        reinterpret_cast<std::atomic<uint8_t> *>(slot_checksum + detail::CHECKSUM_BYTES);
    const void *slot_data = buf + slot_index * slot_size;
    if (!pylabhub::crypto::compute_blake2b(slot_checksum, slot_data, slot_size))
    {
        return false;
    }
    slot_valid->store(1, std::memory_order_release);
    return true;
}

inline bool verify_checksum_flexible_zone_impl(const DataBlock *block)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    auto *hdr = block->header();

    // Phase 2: Single flex zone (always at index 0)
    constexpr size_t flex_zone_idx = 0;
    
    const auto &layout = block->layout();
    if (layout.flexible_zone_size == 0)
    {
        return false; // No flex zone configured
    }

    if (hdr->flexible_zone_checksums[flex_zone_idx].valid.load(std::memory_order_acquire) != 1)
    {
        return false;
    }

    const char *flex = block->flexible_data_zone();
    size_t len = layout.flexible_zone_size;
    const char *zone_ptr = flex; // Single zone starts at offset 0

    if (zone_ptr == nullptr || len == 0)
    {
        return false;
    }

    // Checksum data stored in SharedMemoryHeader::flexible_zone_checksums[0]
    return pylabhub::crypto::verify_blake2b(
        hdr->flexible_zone_checksums[flex_zone_idx].checksum_bytes, zone_ptr, len);
}

inline bool verify_checksum_slot_impl(const DataBlock *block, size_t slot_index)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    if (slot_index >= block->layout().slot_count_value())
    {
        return false;
    }
    const char *base = reinterpret_cast<const char *>(block->segment());
    const char *slot_checksum_base_ptr = block->layout().slot_checksum_base(base);
    const auto *slot_checksum = reinterpret_cast<const uint8_t *>(
        slot_checksum_base_ptr + slot_index * detail::SLOT_CHECKSUM_ENTRY_SIZE);
    const auto *slot_valid =
        reinterpret_cast<const std::atomic<uint8_t> *>(slot_checksum + detail::CHECKSUM_BYTES);
    if (slot_valid->load(std::memory_order_acquire) != 1)
    {
        return false;
    }
    // Step size for slot data = logical (slot_stride_bytes).
    const size_t slot_size = block->layout().slot_stride_bytes();
    if (slot_size == 0)
    {
        return false;
    }
    const char *buf = block->structured_data_buffer();
    if (buf == nullptr)
    {
        return false;
    }
    const void *slot_data = buf + slot_index * slot_size;
    return pylabhub::crypto::verify_blake2b(slot_checksum, slot_data, slot_size);
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
    size_t bytes_written = 0;
    bool committed = false;
    bool released = false;
    SlotRWState *rw_state = nullptr; // Pointer to the SlotRWState for this slot
    /// Per-handle acquire timestamp for last_slot_work_us.
    /// Stored here (not in owner) so the RAII multi-iteration path records the
    /// correct work time even after the owner's t_iter_start_ is overwritten by the
    /// next acquire_write_slot() call (HEP-CORE-0008 §4.2).
    ContextMetrics::Clock::time_point t_slot_acquired_{};
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
    bool released = false;
    bool mark_as_consumed = false;    // Set by release_consume_slot(); guards last_consumed_slot_id update
    SlotRWState *rw_state = nullptr;  // Pointer to the SlotRWState for this slot
    uint64_t captured_generation = 0; // Captured generation for validation
    int consumer_heartbeat_slot = -1; // For Sequential_sync: which consumer slot to update on release
    /// Per-handle acquire timestamp for last_slot_work_us (symmetric with SlotWriteHandleImpl).
    ContextMetrics::Clock::time_point t_slot_acquired_{};
};

namespace
{
bool release_write_handle(SlotWriteHandleImpl &impl);
bool release_consume_handle(SlotConsumeHandleImpl &impl);

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- slot_id/slot_index and captured_generation/consumer_heartbeat_slot are semantically distinct; internal helper with 2 call sites
std::unique_ptr<SlotConsumeHandleImpl> make_slot_consume_handle_impl(
    DataBlockConsumerImpl *owner,
    DataBlock *dataBlock,
    SharedMemoryHeader *header,
    uint64_t slot_id,     // NOLINT(bugprone-easily-swappable-parameters)
    size_t slot_index,
    const char *buf,
    size_t slot_stride_bytes,
    SlotRWState *rw_state,
    uint64_t captured_generation,  // NOLINT(bugprone-easily-swappable-parameters)
    int consumer_heartbeat_slot)
{
    auto impl = std::make_unique<SlotConsumeHandleImpl>();
    impl->owner = owner;
    impl->dataBlock = dataBlock;
    impl->header = header;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = slot_buffer_ptr(buf, slot_index, slot_stride_bytes);
    impl->buffer_size = slot_stride_bytes;
    impl->rw_state = rw_state;
    impl->captured_generation = captured_generation;
    impl->consumer_heartbeat_slot = consumer_heartbeat_slot;
    return impl;
}
} // namespace

// ============================================================================
// DataBlockProducerImpl
// ============================================================================
struct DataBlockProducerImpl
{
    std::mutex mutex; // Protects slot acquire/release and context; makes Producer thread-safe
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;
    // Single flexible zone (Phase 2 refactoring)
    size_t flex_zone_offset = 0;
    size_t flex_zone_size = 0;
    /// Display name (with optional suffix). Set once via call_once; not hot path.
    mutable std::once_flag name_fallback_once;
    mutable std::string name_fallback;

    // ── LoopPolicy / ContextMetrics (HEP-CORE-0008) ──────────────────────────
    LoopPolicy loop_policy{LoopPolicy::MaxRate};
    uint64_t   period_ms{0};
    ContextMetrics metrics_;
    /// Timestamp when the previous slot acquire completed (start of the previous iteration).
    /// Zero-initialized; set on first successful acquire.
    ContextMetrics::Clock::time_point t_iter_start_{};
};

// ============================================================================
// DataBlockProducer
// ============================================================================
DataBlockProducer::DataBlockProducer() : pImpl(nullptr) {}

DataBlockProducer::DataBlockProducer(std::unique_ptr<DataBlockProducerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockProducer::~DataBlockProducer() noexcept = default;

DataBlockProducer::DataBlockProducer(DataBlockProducer &&other) noexcept = default;

DataBlockProducer &DataBlockProducer::operator=(DataBlockProducer &&other) noexcept = default;

// ============================================================================
// DataBlockConsumerImpl
// ============================================================================
struct DataBlockConsumerImpl
{
    std::recursive_mutex mutex; // Protects slot acquire/release, iterator, heartbeat; recursive (register_heartbeat called from acquire_consume_slot)
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;
    uint64_t last_consumed_slot_id = INVALID_SLOT_ID;  // This field is still needed
    // Single flexible zone (Phase 2 refactoring)
    size_t flex_zone_offset = 0;
    size_t flex_zone_size = 0;
    int heartbeat_slot = -1; // For Sequential_sync: index into consumer_heartbeats / read positions
    // Consumer identity written to the heartbeat slot during register_heartbeat().
    // Must be populated BEFORE register_heartbeat() is called (set in find_datablock_consumer_impl).
    char consumer_uid_buf[40]{};   // Null-terminated hex string; empty = not set
    char consumer_name_buf[32]{};  // Null-terminated name; empty = not set
    /// Display name (with optional suffix). Set once via call_once; not hot path.
    mutable std::once_flag name_fallback_once;
    mutable std::string name_fallback;

    // ── LoopPolicy / ContextMetrics (HEP-CORE-0008) ──────────────────────────
    LoopPolicy loop_policy{LoopPolicy::MaxRate};
    uint64_t   period_ms{0};
    ContextMetrics metrics_;
    /// Timestamp when the previous slot acquire completed (start of the previous iteration).
    /// Zero-initialized; set on first successful acquire.
    ContextMetrics::Clock::time_point t_iter_start_{};

    ~DataBlockConsumerImpl()
    {
        // Auto-unregister heartbeat to release the consumer heartbeat slot back to the pool.
        // This is the symmetric cleanup for the auto-register done in find_datablock_consumer_impl.
        if (heartbeat_slot >= 0 && dataBlock != nullptr)
        {
            SharedMemoryHeader *header = dataBlock->header();
            if (header != nullptr &&
                heartbeat_slot < static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
            {
                uint64_t pid = pylabhub::platform::get_pid();
                uint64_t expected = pid;
                auto hb_slot = static_cast<size_t>(heartbeat_slot);
                if (header->consumer_heartbeats[hb_slot].consumer_pid.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel))
                {
                    header->active_consumer_count.fetch_sub(1, std::memory_order_relaxed);
                    // Zero uid/name after releasing pid (slot no longer claimed)
                    std::memset(header->consumer_heartbeats[hb_slot].consumer_uid, 0, 40);
                    std::memset(header->consumer_heartbeats[hb_slot].consumer_name, 0, 32);
                }
                heartbeat_slot = -1;
            }
        }

        std::string label;
        if (!name_fallback.empty())
        {
            label = name_fallback;
        }
        else if (!name.empty())
        {
            label = name;
        }
        else
        {
            label = "(unnamed)";
        }
        LOGGER_INFO("DataBlockConsumerImpl: Shutting down for '{}'.", label);
    }
};

namespace
{
/// Returned by name() when pImpl is null (default-constructed or moved-from). Neutral state, not an error.
const std::string kNullProducerOrConsumerName("(null)");

/// Prefix of the runtime suffix appended to names for context. See docs/NAME_CONVENTIONS.md.
constexpr std::string_view kNameSuffixPrefix(" | pid:");
/// Single counter for both named (suffix) and unnamed (full id) so each instance has a unique index.
std::atomic<uint64_t> g_name_instance_counter{0};

/// Not hot path: called at most once per instance via call_once; result stored in name_fallback.
void ensure_producer_display_name(DataBlockProducerImpl *impl)
{
    if (impl == nullptr)
    {
        return;
    }
    uint64_t pid = pylabhub::platform::get_pid();
    uint64_t idx = g_name_instance_counter.fetch_add(1, std::memory_order_relaxed);
    if (impl->name.empty())
    {
        impl->name_fallback = fmt::format("producer-{}-{}", pid, idx);
    }
    else
    {
        impl->name_fallback = fmt::format("{}{}{}-{}", impl->name, kNameSuffixPrefix, pid, idx);
    }
}

/// Not hot path: called at most once per instance via call_once; result stored in name_fallback.
void ensure_consumer_display_name(DataBlockConsumerImpl *impl)
{
    if (impl == nullptr)
    {
        return;
    }
    uint64_t pid = pylabhub::platform::get_pid();
    uint64_t idx = g_name_instance_counter.fetch_add(1, std::memory_order_relaxed);
    if (impl->name.empty())
    {
        impl->name_fallback = fmt::format("consumer-{}-{}", pid, idx);
    }
    else
    {
        impl->name_fallback = fmt::format("{}{}{}-{}", impl->name, kNameSuffixPrefix, pid, idx);
    }
}
}

const std::string &DataBlockProducer::name() const noexcept
{
    if (pImpl == nullptr)
    {
        return kNullProducerOrConsumerName;
    }
    std::call_once(pImpl->name_fallback_once, [this] { ensure_producer_display_name(pImpl.get()); });
    return pImpl->name_fallback;
}

const std::string &DataBlockConsumer::name() const noexcept
{
    if (pImpl == nullptr)
    {
        return kNullProducerOrConsumerName;
    }
    std::call_once(pImpl->name_fallback_once, [this] { ensure_consumer_display_name(pImpl.get()); });
    return pImpl->name_fallback;
}

ChecksumPolicy DataBlockProducer::checksum_policy() const noexcept
{
    return (pImpl != nullptr) ? pImpl->checksum_policy : ChecksumPolicy::None;
}

// ============================================================================
// DataBlockDiagnosticHandle implementation
// ============================================================================
DataBlockDiagnosticHandle::DataBlockDiagnosticHandle(
    std::unique_ptr<DataBlockDiagnosticHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockDiagnosticHandle::~DataBlockDiagnosticHandle() noexcept
{
    if (pImpl != nullptr)
    {
        pylabhub::platform::shm_close(&pImpl->m_shm);
    }
}

DataBlockDiagnosticHandle::DataBlockDiagnosticHandle(DataBlockDiagnosticHandle &&) noexcept =
    default;

DataBlockDiagnosticHandle &
DataBlockDiagnosticHandle::operator=(DataBlockDiagnosticHandle &&) noexcept = default;

SharedMemoryHeader *DataBlockDiagnosticHandle::header() const
{
    return pImpl ? pImpl->header_ptr : nullptr;
}

SlotRWState *DataBlockDiagnosticHandle::slot_rw_state(uint32_t index) const
{
    if (pImpl == nullptr || pImpl->slot_rw_states == nullptr || index >= pImpl->ring_buffer_capacity)
    {
        return nullptr;
    }
    return &pImpl->slot_rw_states[index];
}

std::unique_ptr<DataBlockDiagnosticHandle> open_datablock_for_diagnostic(const std::string &name)
{
    auto impl = std::make_unique<DataBlockDiagnosticHandleImpl>();
    try
    {
        impl->m_shm = pylabhub::platform::shm_attach(name.c_str());
        if (impl->m_shm.base == nullptr)
        {
            return nullptr;
        }
        impl->header_ptr = reinterpret_cast<SharedMemoryHeader *>(impl->m_shm.base);
        if (!detail::is_header_magic_valid(&impl->header_ptr->magic_number,
                                            detail::DATABLOCK_MAGIC_NUMBER))
        {
            return nullptr;
        }
        DataBlockLayout layout = DataBlockLayout::from_header(impl->header_ptr);
        impl->ring_buffer_capacity = layout.slot_count_value();
        impl->slot_rw_states = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(impl->m_shm.base) + layout.slot_rw_state_offset);
        return std::unique_ptr<DataBlockDiagnosticHandle>(
            new DataBlockDiagnosticHandle(std::move(impl)));
    }
    catch (...)
    {
        return nullptr;
    }
}

uint64_t DataBlockProducer::last_slot_id() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return 0;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    if (header == nullptr)
    {
        return 0;
    }
    return header->commit_index.load(std::memory_order_acquire);
}

int DataBlockProducer::get_metrics(DataBlockMetrics &out_metrics) const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_get_metrics(header, &out_metrics) : -1;
}

int DataBlockProducer::reset_metrics() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_reset_metrics(header) : -1;
}

// ─── LoopPolicy / ContextMetrics (HEP-CORE-0008) — DataBlockProducer ────────

void DataBlockProducer::set_loop_policy(LoopPolicy policy,
                                         std::chrono::milliseconds period) noexcept
{
    if (pImpl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->loop_policy = policy;
    pImpl->period_ms   = (policy == LoopPolicy::MaxRate)
                             ? 0ULL
                             : static_cast<uint64_t>(period.count());
    pImpl->metrics_.period_ms = pImpl->period_ms;
}

const ContextMetrics &DataBlockProducer::metrics() const noexcept
{
    static const ContextMetrics kEmpty{};
    if (pImpl == nullptr)
    {
        return kEmpty;
    }
    return pImpl->metrics_;
}

void DataBlockProducer::clear_metrics() noexcept
{
    if (pImpl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    const auto saved_period   = pImpl->metrics_.period_ms;
    pImpl->metrics_            = ContextMetrics{};
    pImpl->metrics_.period_ms  = saved_period;
    pImpl->t_iter_start_       = {};
}

// ─── Channel Identity Accessors (DataBlockProducer) ───

namespace
{
std::string read_id_field(const char *field, size_t max_len) noexcept
{
    size_t len = strnlen(field, max_len);
    return std::string{field, len};
}
} // anonymous namespace

std::string DataBlockProducer::hub_uid() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
        return {};
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->hub_uid, sizeof(h->hub_uid)) : std::string{};
}

std::string DataBlockProducer::hub_name() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return {};
    }
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->hub_name, sizeof(h->hub_name)) : std::string{};
}

std::string DataBlockProducer::producer_uid() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return {};
    }
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->producer_uid, sizeof(h->producer_uid))
                          : std::string{};
}

std::string DataBlockProducer::producer_name() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return {};
    }
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->producer_name, sizeof(h->producer_name))
                          : std::string{};
}

// ============================================================================
// Structure Re-Mapping API (Placeholder)
// ============================================================================

// NOLINT annotations: these are placeholder stubs for future broker-coordinated remapping
uint64_t DataBlockProducer::request_structure_remap(
    const std::optional<schema::SchemaInfo> &new_flexzone_schema,  // NOLINT(bugprone-easily-swappable-parameters)
    const std::optional<schema::SchemaInfo> &new_datablock_schema)
{
    (void)new_flexzone_schema;
    (void)new_datablock_schema;
    (void)pImpl; // Will be used in future implementation
    throw std::runtime_error(
        "DataBlockProducer::request_structure_remap: "
        "Structure remapping requires broker coordination - not yet implemented. "
        "This is a placeholder API for future functionality. "
        "See CHECKSUM_ARCHITECTURE.md §7.1 for protocol details.");
}

void DataBlockProducer::commit_structure_remap(
    uint64_t request_id,
    const std::optional<schema::SchemaInfo> &new_flexzone_schema,  // NOLINT(bugprone-easily-swappable-parameters)
    const std::optional<schema::SchemaInfo> &new_datablock_schema)
{
    (void)request_id;
    (void)new_flexzone_schema;
    (void)new_datablock_schema;
    (void)pImpl; // Will be used in future implementation
    throw std::runtime_error(
        "DataBlockProducer::commit_structure_remap: "
        "Structure remapping requires broker coordination - not yet implemented. "
        "This is a placeholder API for future functionality. "
        "See CHECKSUM_ARCHITECTURE.md §7.1 for protocol details.");
}

bool DataBlockProducer::update_checksum_flexible_zone() noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? update_checksum_flexible_zone_impl(pImpl->dataBlock.get())
               : false;
}

SharedSpinLock DataBlockProducer::get_spinlock(size_t index)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        throw std::runtime_error("DataBlockProducer::get_spinlock: producer is invalid.");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(index);
    return {state, fmt::format("{}:spinlock:{}", name(), index)};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency
uint32_t DataBlockProducer::spinlock_count() const noexcept
{
    return static_cast<uint32_t>(detail::MAX_SHARED_SPINLOCKS);
}

std::span<std::byte> DataBlockProducer::flexible_zone_span() noexcept
{
    return detail::get_flex_zone_span_mutable(pImpl.get());
}

bool DataBlockProducer::update_checksum_slot(size_t slot_index) noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? update_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
               : false;
}

std::unique_ptr<SlotWriteHandle> DataBlockProducer::acquire_write_slot(int timeout_ms) noexcept
{
    if (pImpl == nullptr)
    {
        LOGGER_WARN("acquire_write_slot: pImpl is null");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock.get());
    if (header == nullptr || slot_count == 0)
    {
        LOGGER_WARN("acquire_write_slot: header={} slot_count={}",
                    (void*)header, slot_count);
        return nullptr;
    }

    // ── ContextMetrics: Domain 2 — iteration-start timestamp (HEP-CORE-0008) ──
    const auto t_entry = ContextMetrics::Clock::now();

    const ConsumerSyncPolicy policy = header->consumer_sync_policy;
    if (policy == ConsumerSyncPolicy::Sequential || policy == ConsumerSyncPolicy::Sequential_sync)
    {
        auto start_time = pylabhub::platform::monotonic_time_ns();
        int iteration = 0;
        while (true)
        {
            uint64_t write_idx = header->write_index.load(std::memory_order_acquire);
            uint64_t read_idx = header->read_index.load(std::memory_order_acquire);
            if (write_idx - read_idx < static_cast<uint64_t>(slot_count))
            {
                break;
            }
            if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
            {
                header->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            backoff(iteration++);
        }
    }

    // Single-writer invariant: the broker enforces exactly one registered producer per channel.
    // DataBlockProducer is therefore always driven by a single writer thread.
    //
    // SHM-C2 design: write_index is advanced FIRST (fetch_add) to atomically claim a
    // unique slot_id, then acquire_write() confirms the physical slot is ready.
    //
    // SHM-C2 fix (Phase 2 drain-hold): if readers are still active when the Phase 2
    // timeout expires, acquire_write() resets its timer and keeps waiting rather than
    // releasing write_lock and restoring COMMITTED. While DRAINING is held, no new readers
    // can enter the slot — existing readers will eventually drain — so slot_id can never
    // be burned by a Phase 2 timeout. acquire_write() only returns SLOT_ACQUIRE_TIMEOUT
    // for Phase 1 failures (zombie write_lock reclaim), which are impossible under normal
    // single-writer operation.
    //
    // NOTE — single-writer only: this design assumes exactly one producer. For future
    // multi-writer support, two changes are required before touching this code:
    //   1. update_commit_index() must be a max-CAS (not plain store) to handle out-of-order
    //      commits from concurrent writers.
    //   2. Consumer read sequencing (latest slot_id tracking, Sequential ordering) must
    //      be redesigned for the multi-writer commit ordering that results.
    // Do not extend to multi-writer without fully resolving both protocols first.
    uint64_t slot_id = header->write_index.fetch_add(1, std::memory_order_acq_rel); // claim unique slot_id
    auto slot_index = static_cast<size_t>(slot_id % slot_count);

    SlotRWState *rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
    if (rw_state == nullptr)
    {
        return nullptr; // Should not happen: slot_id % slot_count always in range
    }

    // drain_hold=true: Phase 2 timeout resets timer and keeps waiting (no burn).
    // Phase 1 timeout (zombie writer) still releases and returns SLOT_ACQUIRE_TIMEOUT.
    SlotAcquireResult acquire_res = acquire_write(rw_state, header, timeout_ms, /*drain_hold=*/true);
    if (acquire_res != SLOT_ACQUIRE_OK)
    {
        // Phase 1 timeout (zombie write_lock) — slot_id is burned but this path is
        // unreachable under normal single-writer operation.
        return nullptr;
    }

    // Ring-buffer iteration step is logical slot size (slot_stride_bytes), not physical.
    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes();
    char *buf = pImpl->dataBlock->structured_data_buffer();
    if (buf == nullptr || slot_stride_bytes == 0)
    {
        // Release write lock if buffer is invalid before returning
        rw_state->write_lock.store(0, std::memory_order_release);
        return nullptr;
    }

    // ── ContextMetrics: record successful acquisition (HEP-CORE-0008 §4.1) ────
    // t_acquired is declared here (not inside the inner block) so it is also available
    // when initialising the per-handle t_slot_acquired_ field below.
    const auto t_acquired = ContextMetrics::Clock::now();
    {
        auto &m = pImpl->metrics_;
        const ContextMetrics::Clock::time_point t_zero{};
        if (pImpl->t_iter_start_ != t_zero)
        {
            // Subsequent iteration: compute start-to-start interval.
            const auto elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    t_acquired - pImpl->t_iter_start_).count());
            m.last_iteration_us = elapsed_us;
            if (elapsed_us > m.max_iteration_us)
                m.max_iteration_us = elapsed_us;
            m.context_elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    t_acquired - m.context_start_time).count());
            if (pImpl->loop_policy == LoopPolicy::FixedRate && pImpl->period_ms > 0 &&
                elapsed_us > pImpl->period_ms * 1000ULL)
                ++m.overrun_count;
        }
        else
        {
            // First acquisition: set session start time.
            m.context_start_time = t_acquired;
        }
        m.last_slot_wait_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - t_entry).count());
        ++m.iteration_count;
        pImpl->t_iter_start_  = t_acquired;
    }

    auto impl = std::make_unique<SlotWriteHandleImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->header = header;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = slot_buffer_ptr(buf, slot_index, slot_stride_bytes);
    impl->buffer_size = slot_stride_bytes;
    impl->rw_state = rw_state;
    impl->t_slot_acquired_ = t_acquired; // per-handle anchor for last_slot_work_us
    // flexible_ptr and flexible_size are no longer directly used in SlotWriteHandleImpl
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) -- OOM would terminate; noexcept design choice
    return std::unique_ptr<SlotWriteHandle>(new SlotWriteHandle(std::move(impl)));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency with class
bool DataBlockProducer::release_write_slot(SlotWriteHandle &handle) noexcept
{
    if (handle.pImpl == nullptr)
    {
        return false;
    }
    DataBlockProducerImpl *owner = handle.pImpl->owner;
    if (owner == nullptr)
    {
        return false;
    }
    return release_write_handle(*handle.pImpl);
}

void DataBlockProducer::update_heartbeat() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    if (header != nullptr)
    {
        detail::update_producer_heartbeat_impl(header, pylabhub::platform::get_pid());
    }
}

void DataBlockProducer::check_consumer_health() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || pImpl->dataBlock->header() == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();

    for (size_t i = 0; i < detail::MAX_CONSUMER_HEARTBEATS; ++i)
    {
        uint64_t consumer_pid =
            header->consumer_heartbeats[i].consumer_pid.load(std::memory_order_acquire);
        if (consumer_pid != 0)
        {
            if (!pylabhub::platform::is_process_alive(consumer_pid))
            {
                LOGGER_WARN(
                    "DataBlock '{}': Detected dead consumer PID {}. Clearing heartbeat slot {}.",
                    pImpl->name, consumer_pid, i);
                uint64_t expected_pid = consumer_pid;
                if (header->consumer_heartbeats[i].consumer_pid.compare_exchange_strong(
                        expected_pid, 0, std::memory_order_acq_rel))
                {
                    header->active_consumer_count.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

// ============================================================================
// Slot Handles (Primitive Data Transfer API) - Implementations
// ============================================================================
// Lifetime contract: SlotWriteHandle and SlotConsumeHandle hold pointers into the
// DataBlock's shared memory. Callers must release or destroy all handles before
// destroying the DataBlockProducer or DataBlockConsumer. Otherwise the handle
// destructor will access freed memory (use-after-free).
namespace
{
bool release_write_handle(SlotWriteHandleImpl &impl)
{
    if (impl.released)
    {
        return true;
    }

    // ── ContextMetrics: Domain 2 — measure work time (HEP-CORE-0008 §4.2) ────
    // Uses per-handle t_slot_acquired_ (not owner->t_iter_start_) so the RAII
    // multi-iteration path records the correct work time even when owner->t_iter_start_
    // has already been overwritten by the next acquire_write_slot() call.
    if (impl.owner != nullptr)
    {
        const ContextMetrics::Clock::time_point t_zero{};
        if (impl.t_slot_acquired_ != t_zero)
        {
            impl.owner->metrics_.last_slot_work_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    ContextMetrics::Clock::now() - impl.t_slot_acquired_).count());
        }
    }

    bool success = true;

    // Perform checksum updates if policy requires and committed. On checksum failure the slot is already committed (visible to readers); we log and return false.
    if (impl.committed && impl.owner != nullptr && impl.owner->checksum_policy != ChecksumPolicy::None &&
        impl.header != nullptr &&
        static_cast<ChecksumType>(impl.header->checksum_type) != ChecksumType::Unset)
    {
        if (!update_checksum_slot_impl(impl.dataBlock, impl.slot_index))
        {
            LOGGER_WARN("DataBlock '{}': release_write_slot failed — checksum update failed for slot_index={} slot_id={}.",
                        impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index, impl.slot_id);
            success = false;
        }

        // Update flexible zone checksums (Phase 2: single flex zone)
        if (impl.dataBlock->layout().flexible_zone_size > 0)
        {
            if (!update_checksum_flexible_zone_impl(impl.dataBlock))
            {
                LOGGER_WARN("DataBlock '{}': release_write_slot failed — flexible zone checksum update failed for slot_index={}.",
                            impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index);
                success = false;
            }
        }
    }

    // Commit the write (make it visible to readers)
    if (impl.committed && impl.rw_state != nullptr && impl.header != nullptr)
    {
        commit_write(impl.rw_state, impl.header, impl.slot_id);
        detail::update_producer_heartbeat_impl(impl.header, pylabhub::platform::get_pid());
        // Release write_lock so the slot can be reused on wrap-around (lap2+)
        impl.rw_state->write_lock.store(0, std::memory_order_release);
    }
    else if (impl.rw_state != nullptr)
    {
        // Abort path: slot was not committed, so return it to FREE.
        // State must transition to FREE before the write_lock is released.
        // See release_write() in data_block_slot_ops.cpp for the ordering rationale.
        impl.rw_state->slot_state.store(SlotRWState::SlotState::FREE,
                                        std::memory_order_release);
        impl.rw_state->write_lock.store(0, std::memory_order_release);

        // Restore write_index: the slot_id claimed by acquire_write_slot() was
        // never committed, so the ring position must be returned.  Without this,
        // Sequential-policy producers that discard slots will eventually believe
        // the ring is full (write_idx - read_idx >= slot_count) even though all
        // slots are FREE.
        //
        // Safe under the single-writer invariant: no concurrent fetch_add() can
        // race with this decrement.  The consumer's read_index is unaffected
        // because commit_index was never advanced for this slot_id.
        if (impl.header != nullptr)
        {
            impl.header->write_index.fetch_sub(1, std::memory_order_release);
        }
    }

    // No thread guard exit needed here, as acquire_write doesn't use it.
    impl.released = true;
    return success;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- structured release flow; refactor would fragment logic
bool release_consume_handle(SlotConsumeHandleImpl &impl)
{
    if (impl.released)
    {
        return true;
    }

    // ── ContextMetrics: Domain 2 — measure work time (HEP-CORE-0008 §4.2) ────
    // Uses per-handle t_slot_acquired_ so both explicit-release and RAII-destructor paths
    // record the correct work time regardless of when the next acquire_consume_slot() runs.
    if (impl.owner != nullptr)
    {
        const ContextMetrics::Clock::time_point t_zero{};
        if (impl.t_slot_acquired_ != t_zero)
        {
            impl.owner->metrics_.last_slot_work_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    ContextMetrics::Clock::now() - impl.t_slot_acquired_).count());
        }
    }

    bool success = true;

    // 1. Validate captured generation to detect wrap-around (if reader was pre-empted)
    if (impl.rw_state != nullptr && impl.header != nullptr)
    {
        if (!validate_read_impl(impl.rw_state, impl.header, impl.captured_generation))
        {
            LOGGER_WARN("DataBlock '{}': release_consume_slot failed — slot validation failed (wrap-around or slot overwritten) for slot_index={} slot_id={}.",
                        impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index, impl.slot_id);
            success = false;
        }
    }
    else
    {
        success = false; // Invalid state
    }

    // 2. Perform checksum verification if policy requires
    if (success && impl.owner != nullptr && impl.owner->checksum_policy != ChecksumPolicy::None &&
        impl.header != nullptr &&
        static_cast<ChecksumType>(impl.header->checksum_type) != ChecksumType::Unset)
    {
        if (!verify_checksum_slot_impl(impl.dataBlock, impl.slot_index))
        {
            LOGGER_WARN("DataBlock '{}': release_consume_slot failed — slot checksum verification failed for slot_index={} slot_id={}.",
                        impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index, impl.slot_id);
            success = false;
        }
        // Verify flexible zone checksums (Phase 2: single flex zone)
        if (impl.dataBlock->layout().flexible_zone_size > 0)
        {
            if (!verify_checksum_flexible_zone_impl(impl.dataBlock))
            {
                LOGGER_WARN("DataBlock '{}': release_consume_slot failed — flexible zone checksum verification failed for slot_index={}.",
                            impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index);
                success = false;
            }
        }
    }

    // 3. Release the read lock
    if (impl.rw_state != nullptr && impl.header != nullptr)
    {
        release_read(impl.rw_state, impl.header);
    }
    else
    {
        success = false; // Cannot release if state is invalid
    }

    // 4. Advance read position for Sequential / Sequential_sync
    if (success && impl.header != nullptr)
    {
        SharedMemoryHeader *header = impl.header;
        const ConsumerSyncPolicy policy = header->consumer_sync_policy;
        const uint64_t next = impl.slot_id + 1;
        if (policy == ConsumerSyncPolicy::Sequential)
        {
            header->read_index.store(next, std::memory_order_release);
        }
        else if (policy == ConsumerSyncPolicy::Sequential_sync && impl.consumer_heartbeat_slot >= 0 &&
                 impl.consumer_heartbeat_slot < static_cast<int>(MAX_CONSUMER_HEARTBEATS))
        {
            consumer_next_read_slot_ptr(header, static_cast<size_t>(impl.consumer_heartbeat_slot))
                ->store(next, std::memory_order_release);
            // read_index = min of all consumer positions (only count registered slots)
            uint64_t min_pos = next;
            for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
            {
                if (header->consumer_heartbeats[i].consumer_pid.load(std::memory_order_acquire) != 0)
                {
                    uint64_t pos =
                        consumer_next_read_slot_ptr(header, i)->load(std::memory_order_acquire);
                    if (pos < min_pos)
                    {
                        min_pos = pos;
                    }
                }
            }
            header->read_index.store(min_pos, std::memory_order_release);
        }
    }

    // Update last_consumed_slot_id only when explicitly consumed (not on exception-path destructor
    // release). This allows Latest_only consumers to re-read the same slot after exception recovery.
    if (impl.mark_as_consumed && impl.owner != nullptr)
    {
        impl.owner->last_consumed_slot_id = impl.slot_id;
    }

    impl.released = true;
    return success;
}
} // namespace

SlotWriteHandle::SlotWriteHandle() : pImpl(nullptr) {}

SlotWriteHandle::SlotWriteHandle(std::unique_ptr<SlotWriteHandleImpl> impl) : pImpl(std::move(impl))
{
}

SlotWriteHandle::~SlotWriteHandle() noexcept
{
    if (pImpl)
    {
        (void)release_write_handle(*pImpl);
    }
}

SlotWriteHandle::SlotWriteHandle(SlotWriteHandle &&other) noexcept = default;

SlotWriteHandle &SlotWriteHandle::operator=(SlotWriteHandle &&other) noexcept = default;

size_t SlotWriteHandle::slot_index() const noexcept
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotWriteHandle::slot_id() const noexcept
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<std::byte> SlotWriteHandle::buffer_span() noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || pImpl->buffer_size == 0)
    {
        return {};
    }
    return {reinterpret_cast<std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<std::byte> SlotWriteHandle::flexible_zone_span() noexcept
{
    return (pImpl != nullptr && pImpl->owner != nullptr) 
        ? detail::get_flex_zone_span_mutable(pImpl->owner)
        : std::span<std::byte>{};
}

bool SlotWriteHandle::write(const void *src, size_t len, size_t offset) noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || len == 0)
    {
        return false;
    }
    if (offset + len > pImpl->buffer_size)
    {
        return false;
    }
    std::memcpy(pImpl->buffer_ptr + offset, src, len);
    return true;
}

bool SlotWriteHandle::commit(size_t bytes_written) noexcept
{
    if (pImpl == nullptr || pImpl->header == nullptr)
    {
        return false;
    }
    if (bytes_written > pImpl->buffer_size)
    {
        return false;
    }
    pImpl->bytes_written = bytes_written;
    pImpl->committed = true;

    return true;
}

bool SlotWriteHandle::update_checksum_slot() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return update_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotWriteHandle::update_checksum_flexible_zone() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return update_checksum_flexible_zone_impl(pImpl->dataBlock);
}

SlotConsumeHandle::SlotConsumeHandle() : pImpl(nullptr) {}

SlotConsumeHandle::SlotConsumeHandle(std::unique_ptr<SlotConsumeHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

SlotConsumeHandle::~SlotConsumeHandle() noexcept
{
    if (pImpl)
    {
        (void)release_consume_handle(*pImpl);
    }
}

SlotConsumeHandle::SlotConsumeHandle(SlotConsumeHandle &&other) noexcept = default;

SlotConsumeHandle &SlotConsumeHandle::operator=(SlotConsumeHandle &&other) noexcept = default;

size_t SlotConsumeHandle::slot_index() const noexcept
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotConsumeHandle::slot_id() const noexcept
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<const std::byte> SlotConsumeHandle::buffer_span() const noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || pImpl->buffer_size == 0)
    {
        return {};
    }
    return {reinterpret_cast<const std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<const std::byte> SlotConsumeHandle::flexible_zone_span() const noexcept
{
    return (pImpl != nullptr && pImpl->owner != nullptr)
        ? detail::get_flex_zone_span_const(pImpl->owner)
        : std::span<const std::byte>{};
}

bool SlotConsumeHandle::read(void *dst, size_t len, size_t offset) const noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || len == 0)
    {
        return false;
    }
    if (offset + len > pImpl->buffer_size)
    {
        return false;
    }
    std::memcpy(dst, pImpl->buffer_ptr + offset, len);
    return true;
}

bool SlotConsumeHandle::verify_checksum_slot() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return verify_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotConsumeHandle::verify_checksum_flexible_zone() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return verify_checksum_flexible_zone_impl(pImpl->dataBlock);
}

bool SlotConsumeHandle::validate_read() const noexcept
{
    if (pImpl == nullptr || pImpl->rw_state == nullptr)
    {
        return false;
    }
    return validate_read_impl(pImpl->rw_state, pImpl->header, pImpl->captured_generation);
}

// ============================================================================
// DataBlockConsumer
// ============================================================================
DataBlockConsumer::DataBlockConsumer() : pImpl(nullptr) {}

DataBlockConsumer::DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockConsumer::~DataBlockConsumer() noexcept = default;

DataBlockConsumer::DataBlockConsumer(DataBlockConsumer &&other) noexcept = default;

DataBlockConsumer &DataBlockConsumer::operator=(DataBlockConsumer &&other) noexcept = default;

SharedSpinLock DataBlockConsumer::get_spinlock(size_t index)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        throw std::runtime_error("DataBlockConsumer::get_spinlock: consumer is invalid.");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(index);
    return {state, fmt::format("{}:spinlock:{}", name(), index)};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency
uint32_t DataBlockConsumer::spinlock_count() const noexcept
{
    return static_cast<uint32_t>(detail::MAX_SHARED_SPINLOCKS);
}

bool DataBlockConsumer::verify_checksum_flexible_zone() const noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? verify_checksum_flexible_zone_impl(pImpl->dataBlock.get())
               : false;
}

std::span<const std::byte> DataBlockConsumer::flexible_zone_span() const noexcept
{
    return detail::get_flex_zone_span_const(pImpl.get());
}

bool DataBlockConsumer::verify_checksum_slot(size_t slot_index) const noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? verify_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
               : false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- structured multi-step slot acquire flow
std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(int timeout_ms) noexcept
{
    if (pImpl == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock.get());
    if (header == nullptr || slot_count == 0)
    {
        return nullptr;
    }

    // ── ContextMetrics: Domain 2 — iteration-start timestamp (HEP-CORE-0008) ──
    const auto t_entry = ContextMetrics::Clock::now();

    const ConsumerSyncPolicy policy = header->consumer_sync_policy;
    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;
    uint64_t slot_id = INVALID_SLOT_ID;
    size_t slot_index = 0;
    SlotRWState *rw_state = nullptr;
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res{};

    // Sequential_sync: heartbeat is registered at construction time (find_datablock_consumer_impl).
    // Read position is initialized there too. If heartbeat registration failed (pool exhausted),
    // the warning was already logged once at construction — only increment metric here to avoid
    // flooding the log on every acquire call.
    if (policy == ConsumerSyncPolicy::Sequential_sync && pImpl->heartbeat_slot < 0)
    {
        detail::increment_metric_slot_acquire_errors(header);
        return nullptr;
    }

    while (true)
    {
        const uint64_t next_to_read =
            get_next_slot_to_read(header, pImpl->last_consumed_slot_id, pImpl->heartbeat_slot);

        if (next_to_read != INVALID_SLOT_ID)
        {
            slot_index = static_cast<size_t>(next_to_read % slot_count);
            rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
            if (rw_state == nullptr)
            {
                return nullptr;
            }
            acquire_res = acquire_read(rw_state, header, &captured_generation);
            if (acquire_res == SLOT_ACQUIRE_OK)
            {
                slot_id = next_to_read;
                break;
            }
            if (acquire_res != SLOT_ACQUIRE_NOT_READY)
            {
                return nullptr;
            }
        }

        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            header->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        backoff(iteration++);
    }

    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes(); // ring iteration step = logical
    const char *buf = pImpl->dataBlock->structured_data_buffer();
    if (buf == nullptr || slot_stride_bytes == 0)
    {
        release_read(rw_state, header);
        return nullptr;
    }

    // ── ContextMetrics: record successful acquisition (HEP-CORE-0008 §4.1) ────
    // t_acquired declared here (not inside the inner block) so it is also available
    // when initialising the per-handle t_slot_acquired_ field below.
    const auto t_acquired = ContextMetrics::Clock::now();
    {
        auto &m = pImpl->metrics_;
        const ContextMetrics::Clock::time_point t_zero{};
        if (pImpl->t_iter_start_ != t_zero)
        {
            const auto elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    t_acquired - pImpl->t_iter_start_).count());
            m.last_iteration_us = elapsed_us;
            if (elapsed_us > m.max_iteration_us)
                m.max_iteration_us = elapsed_us;
            m.context_elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    t_acquired - m.context_start_time).count());
            if (pImpl->loop_policy == LoopPolicy::FixedRate && pImpl->period_ms > 0 &&
                elapsed_us > pImpl->period_ms * 1000ULL)
                ++m.overrun_count;
        }
        else
        {
            m.context_start_time = t_acquired;
        }
        m.last_slot_wait_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t_acquired - t_entry).count());
        ++m.iteration_count;
        pImpl->t_iter_start_   = t_acquired;
    }

    auto handle_impl = make_slot_consume_handle_impl(
        pImpl.get(), pImpl->dataBlock.get(), header, slot_id, slot_index,
        buf, slot_stride_bytes, rw_state, captured_generation,
        (policy == ConsumerSyncPolicy::Sequential_sync) ? pImpl->heartbeat_slot : -1);
    handle_impl->t_slot_acquired_ = t_acquired; // per-handle anchor for last_slot_work_us

    // Note: last_consumed_slot_id is NOT updated here. It is updated only when the slot is
    // explicitly consumed via release_consume_slot() (mark_as_consumed=true). Exception-path
    // release (via ~SlotConsumeHandle) does NOT update it, allowing the same slot to be
    // re-read after exception recovery. See release_consume_handle() for details.
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) -- OOM would terminate; noexcept design choice
    return std::unique_ptr<SlotConsumeHandle>(new SlotConsumeHandle(std::move(handle_impl)));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- slot_id then timeout_ms; intentional ordering
std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(uint64_t slot_id,
                                                                           int timeout_ms) noexcept
{
    if (pImpl == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock.get());
    if (header == nullptr || slot_count == 0)
    {
        return nullptr;
    }

    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;

    // Wait until this slot_id is committed
    while (header->commit_index.load(std::memory_order_acquire) < slot_id)
    {
        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            header->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        backoff(iteration++);
    }

    auto slot_index = static_cast<size_t>(slot_id % slot_count);
    SlotRWState *rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res = acquire_read(rw_state, header, &captured_generation);
    if (acquire_res != SLOT_ACQUIRE_OK)
    {
        return nullptr;
    }

    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes(); // ring iteration step = logical
    const char *buf = pImpl->dataBlock->structured_data_buffer();
    if (buf == nullptr || slot_stride_bytes == 0)
    {
        release_read(rw_state, header);
        return nullptr;
    }

    auto handle_impl = make_slot_consume_handle_impl(
        pImpl.get(), pImpl->dataBlock.get(), header, slot_id, slot_index,
        buf, slot_stride_bytes, rw_state, captured_generation,
        (header->consumer_sync_policy == ConsumerSyncPolicy::Sequential_sync) ? pImpl->heartbeat_slot : -1);
    handle_impl->t_slot_acquired_ = ContextMetrics::Clock::now(); // per-handle anchor for last_slot_work_us

    // Note: last_consumed_slot_id updated only on explicit release_consume_slot() call.
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) -- OOM would terminate; noexcept design choice
    return std::unique_ptr<SlotConsumeHandle>(new SlotConsumeHandle(std::move(handle_impl)));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency with class
bool DataBlockConsumer::release_consume_slot(SlotConsumeHandle &handle) noexcept
{
    if (handle.pImpl == nullptr)
    {
        return false;
    }
    DataBlockConsumerImpl *owner = handle.pImpl->owner;
    if (owner == nullptr)
    {
        return false;
    }
    handle.pImpl->mark_as_consumed = true;
    return release_consume_handle(*handle.pImpl);
}

int DataBlockConsumer::register_heartbeat()
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || pImpl->dataBlock->header() == nullptr)
    {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();
    uint64_t pid = pylabhub::platform::get_pid();

    for (size_t i = 0; i < detail::MAX_CONSUMER_HEARTBEATS; ++i)
    {
        uint64_t expected = 0;
        if (header->consumer_heartbeats[i].consumer_pid.compare_exchange_strong(
                expected, pid, std::memory_order_acq_rel))
        {
            // CAS succeeded: we own this slot. Write identity fields only after
            // taking ownership to avoid corrupting another consumer's data.
            // Ordering: uid/name writes happen before last_heartbeat_ns.store(release),
            // so readers who acquire-load last_heartbeat_ns will see a consistent identity.
            std::memcpy(header->consumer_heartbeats[i].consumer_uid, pImpl->consumer_uid_buf,
                        sizeof(pImpl->consumer_uid_buf));
            std::memcpy(header->consumer_heartbeats[i].consumer_name, pImpl->consumer_name_buf,
                        sizeof(pImpl->consumer_name_buf));
            header->active_consumer_count.fetch_add(1, std::memory_order_relaxed);
            header->consumer_heartbeats[i].last_heartbeat_ns.store(
                pylabhub::platform::monotonic_time_ns(), std::memory_order_release);
            pImpl->heartbeat_slot = static_cast<int>(i);
            return static_cast<int>(i);
        }
        // CAS failed — slot is owned by another consumer; leave it untouched.
    }
    return -1; // No available slot
}

void DataBlockConsumer::update_heartbeat(int slot)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr ||
        pImpl->dataBlock->header() == nullptr || slot < 0 ||
        slot >= static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();
    header->consumer_heartbeats[slot].last_heartbeat_ns.store(
        pylabhub::platform::monotonic_time_ns(), std::memory_order_release);
}

void DataBlockConsumer::update_heartbeat() noexcept
{
    if (pImpl == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    int slot = pImpl->heartbeat_slot;
    if (slot >= 0)
    {
        // Directly update without delegating (avoid recursive lock)
        if (pImpl->dataBlock != nullptr && pImpl->dataBlock->header() != nullptr)
        {
            auto *header = pImpl->dataBlock->header();
            if (slot < static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
            {
                header->consumer_heartbeats[slot].last_heartbeat_ns.store(
                    pylabhub::platform::monotonic_time_ns(), std::memory_order_release);
            }
        }
    }
}

void DataBlockConsumer::unregister_heartbeat(int slot)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr ||
        pImpl->dataBlock->header() == nullptr || slot < 0 ||
        slot >= static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();
    uint64_t pid = pylabhub::platform::get_pid();
    uint64_t expected = pid; // Expected to be the current process's PID
    if (header->consumer_heartbeats[slot].consumer_pid.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel))
    {
        header->active_consumer_count.fetch_sub(1, std::memory_order_relaxed);
        std::memset(header->consumer_heartbeats[slot].consumer_uid, 0,
                    sizeof(header->consumer_heartbeats[slot].consumer_uid));
        std::memset(header->consumer_heartbeats[slot].consumer_name, 0,
                    sizeof(header->consumer_heartbeats[slot].consumer_name));
        pImpl->heartbeat_slot = -1;
    }
}

int DataBlockConsumer::get_metrics(DataBlockMetrics &out_metrics) const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_get_metrics(header, &out_metrics) : -1;
}

int DataBlockConsumer::reset_metrics() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_reset_metrics(header) : -1;
}

// ─── LoopPolicy / ContextMetrics (HEP-CORE-0008) — DataBlockConsumer ────────

void DataBlockConsumer::set_loop_policy(LoopPolicy policy,
                                         std::chrono::milliseconds period) noexcept
{
    if (pImpl == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    pImpl->loop_policy = policy;
    pImpl->period_ms   = (policy == LoopPolicy::MaxRate)
                             ? 0ULL
                             : static_cast<uint64_t>(period.count());
    pImpl->metrics_.period_ms = pImpl->period_ms;
}

const ContextMetrics &DataBlockConsumer::metrics() const noexcept
{
    static const ContextMetrics kEmpty{};
    if (pImpl == nullptr)
    {
        return kEmpty;
    }
    return pImpl->metrics_;
}

void DataBlockConsumer::clear_metrics() noexcept
{
    if (pImpl == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    const auto saved_period   = pImpl->metrics_.period_ms;
    pImpl->metrics_            = ContextMetrics{};
    pImpl->metrics_.period_ms  = saved_period;
    pImpl->t_iter_start_       = {};
}

// ─── Channel Identity Accessors (DataBlockConsumer) ───

std::string DataBlockConsumer::hub_uid() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
        return {};
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->hub_uid, sizeof(h->hub_uid)) : std::string{};
}

std::string DataBlockConsumer::hub_name() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
        return {};
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->hub_name, sizeof(h->hub_name)) : std::string{};
}

std::string DataBlockConsumer::producer_uid() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
        return {};
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->producer_uid, sizeof(h->producer_uid))
                          : std::string{};
}

std::string DataBlockConsumer::producer_name() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
        return {};
    auto *h = pImpl->dataBlock->header();
    return (h != nullptr) ? read_id_field(h->producer_name, sizeof(h->producer_name))
                          : std::string{};
}

std::string DataBlockConsumer::consumer_uid() const noexcept
{
    if (pImpl == nullptr)
        return {};
    return read_id_field(pImpl->consumer_uid_buf, sizeof(pImpl->consumer_uid_buf));
}

std::string DataBlockConsumer::consumer_name() const noexcept
{
    if (pImpl == nullptr)
        return {};
    return read_id_field(pImpl->consumer_name_buf, sizeof(pImpl->consumer_name_buf));
}

// ============================================================================
// Structure Re-Mapping API (Placeholder)
// ============================================================================

// NOLINT annotations: these are placeholder stubs for future broker-coordinated remapping
void DataBlockConsumer::release_for_remap()
{
    (void)pImpl; // Will be used in future implementation
    throw std::runtime_error(
        "DataBlockConsumer::release_for_remap: "
        "Structure remapping requires broker coordination - not yet implemented. "
        "This is a placeholder API for future functionality. "
        "See CHECKSUM_ARCHITECTURE.md §7.1 for protocol details.");
}

void DataBlockConsumer::reattach_after_remap(
    const std::optional<schema::SchemaInfo> &new_flexzone_schema,  // NOLINT(bugprone-easily-swappable-parameters)
    const std::optional<schema::SchemaInfo> &new_datablock_schema)
{
    (void)new_flexzone_schema;
    (void)new_datablock_schema;
    (void)pImpl; // Will be used in future implementation
    throw std::runtime_error(
        "DataBlockConsumer::reattach_after_remap: "
        "Structure remapping requires broker coordination - not yet implemented. "
        "This is a placeholder API for future functionality. "
        "See CHECKSUM_ARCHITECTURE.md §7.1 for protocol details.");
}


/** Single control surface for attach validation: layout checksum + optional config match.
 *  Call after validate_header_layout_hash(header). Returns false if layout checksum fails
 *  or if expected_config is non-null and header does not match it. */
static bool validate_attach_layout_and_config(const SharedMemoryHeader *header,
                                              const DataBlockConfig *expected_config)
{
    if (!validate_layout_checksum(header))
    {
        LOGGER_WARN("[DataBlock] Layout checksum validation failed during consumer attachment.");
        return false;
    }
    if (expected_config == nullptr)
    {
        return true;
    }
    // Round the expected flex_zone_size to PAGE_ALIGNMENT (same rounding applied at creation time)
    // so that consumers passing sizeof(MyFlexZone) match the rounded value stored in the header.
    const size_t rounded_expected_flex =
        (expected_config->flex_zone_size + detail::PAGE_ALIGNMENT - 1) &
        ~(detail::PAGE_ALIGNMENT - 1);
    const bool flex_ok = header->flexible_zone_size == rounded_expected_flex;
    const bool cap_ok = header->ring_buffer_capacity == expected_config->ring_buffer_capacity;
    const bool page_ok =
        header->physical_page_size == static_cast<uint32_t>(to_bytes(expected_config->physical_page_size));
    const bool stride_ok = detail::get_slot_stride_bytes(header) ==
                           static_cast<uint32_t>(expected_config->effective_logical_unit_size());
    const bool checksum_ok =
        header->checksum_type == static_cast<uint8_t>(expected_config->checksum_type);
    if (!flex_ok || !cap_ok || !page_ok || !stride_ok || !checksum_ok)
    {
        LOGGER_WARN("[DataBlock] Config mismatch during consumer attachment: flex_zone={}, cap={}, "
                    "page={}, stride={}, checksum={}",
                    flex_ok, cap_ok, page_ok, stride_ok, checksum_ok);
        return false;
    }
    return true;
}

// ============================================================================
// Factory Functions
// ============================================================================

// Internal implementation that accepts optional schema info
// Internal implementation that creates producer with dual schema support (Phase 4)
std::unique_ptr<DataBlockProducer>
create_datablock_producer_impl(const std::string &name, DataBlockPolicy policy,
                               const DataBlockConfig &config,
                               const pylabhub::schema::SchemaInfo *flexzone_schema,
                               const pylabhub::schema::SchemaInfo *datablock_schema)
{
    if (!lifecycle_initialized())
    {
        throw std::runtime_error(
            "DataBlock: Data Exchange Hub module not initialized. Create a LifecycleGuard in main() "
            "with pylabhub::hub::GetLifecycleModule() (and typically Logger, CryptoUtils) before creating producers.");
    }
    (void)policy; // Reserved for future policy-specific behavior
    auto impl = std::make_unique<DataBlockProducerImpl>();
    impl->name = name;
    impl->dataBlock = std::make_unique<DataBlock>(name, config);
    impl->checksum_policy = config.checksum_policy;
    
    // Single flex zone (Phase 2 refactoring)
    auto layout = DataBlockLayout::from_config(config);
    impl->flex_zone_offset = layout.flexible_zone_offset;
    impl->flex_zone_size = layout.flexible_zone_size;

    auto *header = impl->dataBlock->header();
    if (header != nullptr)
    {
        // Phase 4: Store BOTH schemas if provided
        if (flexzone_schema != nullptr)
        {
            std::memcpy(header->flexzone_schema_hash, flexzone_schema->hash.data(), detail::CHECKSUM_BYTES);
            LOGGER_DEBUG("[DataBlock:{}] FlexZone schema stored: {} v{}, hash={}...", name,
                         flexzone_schema->name, flexzone_schema->version.to_string(),
                         fmt::format("{:02x}{:02x}{:02x}{:02x}", flexzone_schema->hash[0],
                                     flexzone_schema->hash[1], flexzone_schema->hash[2], flexzone_schema->hash[3]));
        }
        else
        {
            std::memset(header->flexzone_schema_hash, 0, detail::CHECKSUM_BYTES);
        }
        
        if (datablock_schema != nullptr)
        {
            std::memcpy(header->datablock_schema_hash, datablock_schema->hash.data(), detail::CHECKSUM_BYTES);
            header->schema_version = datablock_schema->version.pack();
            LOGGER_DEBUG("[DataBlock:{}] DataBlock schema stored: {} v{}, hash={}...", name,
                         datablock_schema->name, datablock_schema->version.to_string(),
                         fmt::format("{:02x}{:02x}{:02x}{:02x}", datablock_schema->hash[0],
                                     datablock_schema->hash[1], datablock_schema->hash[2], datablock_schema->hash[3]));
        }
        else
        {
            std::memset(header->datablock_schema_hash, 0, detail::CHECKSUM_BYTES);
            header->schema_version = 0;
        }

        // Write channel identity fields from config
        auto write_id_str = [](char *dst, size_t dst_size, const std::string &src) {
            size_t len = src.size() < dst_size ? src.size() : dst_size - 1;
            std::memcpy(dst, src.c_str(), len);
            dst[len] = '\0';
        };
        write_id_str(header->hub_uid, sizeof(header->hub_uid), config.hub_uid);
        write_id_str(header->hub_name, sizeof(header->hub_name), config.hub_name);
        write_id_str(header->producer_uid, sizeof(header->producer_uid), config.producer_uid);
        write_id_str(header->producer_name, sizeof(header->producer_name), config.producer_name);
    }

    return std::make_unique<DataBlockProducer>(std::move(impl));
}

// Non-template wrappers removed - redundant, no schema benefit.
// Recovery and future C API call create_datablock_producer_impl directly.
// Template API in header calls impl inline.

// Phase 4: Internal implementation with dual schema validation
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer_impl(const std::string &name, uint64_t shared_secret,
                             const DataBlockConfig *expected_config,
                             const pylabhub::schema::SchemaInfo *flexzone_schema,
                             const pylabhub::schema::SchemaInfo *datablock_schema,
                             const char *consumer_uid,
                             const char *consumer_name)
{
    if (!lifecycle_initialized())
    {
        throw std::runtime_error(
            "DataBlock: Data Exchange Hub module not initialized. Create a LifecycleGuard in main() "
            "with pylabhub::hub::GetLifecycleModule() (and typically Logger, CryptoUtils) before finding consumers.");
    }
    auto impl = std::make_unique<DataBlockConsumerImpl>();
    impl->name = name;
    impl->dataBlock = std::make_unique<DataBlock>(name);

    auto *header = impl->dataBlock->header();
    if (header == nullptr)
    {
        return nullptr;
    }

    // Validate shared secret (first 8 bytes store capability for discovery)
    if (std::memcmp(header->shared_secret, &shared_secret, sizeof(shared_secret)) != 0)
    {
        return nullptr;
    }

    // Validate header layout (producer and consumer must have same SharedMemoryHeader ABI)
    try
    {
        validate_header_layout_hash(header);
    }
    catch (const pylabhub::schema::SchemaValidationException &)
    {
        header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
        LOGGER_WARN("[DataBlock:{}] Header layout mismatch during consumer attachment (ABI "
                    "incompatibility)",
                    name);
        return nullptr;
    }
    // Validate layout checksum + config (single control surface: see validate_attach_layout_and_config)
    if (!validate_attach_layout_and_config(header, expected_config))
    {
        LOGGER_WARN("[DataBlock:{}] Layout checksum or config mismatch during consumer attachment.",
                    name);
        return nullptr;
    }
    impl->checksum_policy = header->checksum_policy;
    
    // Single flex zone (Phase 2 refactoring)
    auto layout = DataBlockLayout::from_header(header);
    impl->flex_zone_offset = layout.flexible_zone_offset;
    impl->flex_zone_size = layout.flexible_zone_size;

    // Phase 4: Validate BOTH schemas if provided
    if (flexzone_schema != nullptr)
    {
        // Check if producer stored flexzone schema
        bool has_flexzone_schema = std::any_of(
            header->flexzone_schema_hash,
            header->flexzone_schema_hash + detail::CHECKSUM_BYTES,
            [](uint8_t byte) { return byte != 0; });

        if (!has_flexzone_schema)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[DataBlock:{}] Producer did not store FlexZone schema, but consumer expects '{}'",
                        name, flexzone_schema->name);
            return nullptr;
        }

        // Compare flexzone schema hashes
        if (std::memcmp(header->flexzone_schema_hash, flexzone_schema->hash.data(), detail::CHECKSUM_BYTES) != 0)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] FlexZone schema hash mismatch! Expected '{}' v{}", 
                         name, flexzone_schema->name, flexzone_schema->version.to_string());
            return nullptr;
        }
        
        LOGGER_DEBUG("[DataBlock:{}] FlexZone schema validated: {} v{}", 
                     name, flexzone_schema->name, flexzone_schema->version.to_string());
    }

    if (datablock_schema != nullptr)
    {
        // Check if producer stored datablock schema
        bool has_datablock_schema = std::any_of(
            header->datablock_schema_hash,
            header->datablock_schema_hash + detail::CHECKSUM_BYTES,
            [](uint8_t byte) { return byte != 0; });

        if (!has_datablock_schema)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[DataBlock:{}] Producer did not store DataBlock schema, but consumer expects '{}'",
                        name, datablock_schema->name);
            return nullptr;
        }

        // Compare datablock schema hashes
        if (std::memcmp(header->datablock_schema_hash, datablock_schema->hash.data(), detail::CHECKSUM_BYTES) != 0)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] DataBlock schema hash mismatch! Expected '{}' v{}",
                         name, datablock_schema->name, datablock_schema->version.to_string());
            return nullptr;
        }
        
        // Validate schema version compatibility
        auto stored_version = pylabhub::schema::SchemaVersion::unpack(header->schema_version);
        if (stored_version.major != datablock_schema->version.major)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] Incompatible DataBlock schema version! Producer: {}, Consumer: {}",
                         name, stored_version.to_string(), datablock_schema->version.to_string());
            return nullptr;
        }
        
        LOGGER_DEBUG("[DataBlock:{}] DataBlock schema validated: {} v{}", 
                     name, datablock_schema->name, datablock_schema->version.to_string());
    }

    // Store consumer identity before moving impl — register_heartbeat reads these
    if (consumer_uid != nullptr)
    {
        std::strncpy(impl->consumer_uid_buf, consumer_uid, sizeof(impl->consumer_uid_buf) - 1);
        impl->consumer_uid_buf[sizeof(impl->consumer_uid_buf) - 1] = '\0';
    }
    if (consumer_name != nullptr)
    {
        std::strncpy(impl->consumer_name_buf, consumer_name, sizeof(impl->consumer_name_buf) - 1);
        impl->consumer_name_buf[sizeof(impl->consumer_name_buf) - 1] = '\0';
    }

    // Create consumer first, then register heartbeat
    auto consumer = std::make_unique<DataBlockConsumer>(std::move(impl));
    
    // Register consumer heartbeat — enforced for all consumer sync policies.
    // Heartbeat slot provides liveness signal for all consumers (broker visibility).
    // For Sequential_sync: also serves as read-position cursor index in reserved_header.
    int heartbeat_slot = consumer->register_heartbeat();

    if (heartbeat_slot < 0)
    {
        LOGGER_WARN("[DataBlock:{}] Consumer heartbeat registration failed — heartbeat pool "
                    "may be exhausted (max={}). Consumer will have no liveness signal.",
                    name, detail::MAX_CONSUMER_HEARTBEATS);
    }
    else
    {
        LOGGER_DEBUG("[DataBlock:{}] Consumer registered heartbeat slot {}", name, heartbeat_slot);

        // Sequential_sync: initialize per-consumer read position at join time (join-at-latest).
        // Read position is stored in reserved_header at the heartbeat slot index.
        // Previously done lazily in acquire_consume_slot; moved here so position is valid
        // from the moment the consumer is created, before the first acquire call.
        if (header->consumer_sync_policy == ConsumerSyncPolicy::Sequential_sync)
        {
            uint64_t join_at = header->commit_index.load(std::memory_order_acquire);
            consumer_next_read_slot_ptr(header, static_cast<size_t>(heartbeat_slot))
                ->store((join_at != INVALID_SLOT_ID) ? join_at : 0, std::memory_order_release);
            LOGGER_DEBUG("[DataBlock:{}] Sequential_sync consumer join-at position: {}", name,
                         (join_at != INVALID_SLOT_ID) ? join_at : 0ULL);
        }
    }

    return consumer;
}

// ============================================================================
// attach_datablock_as_writer_impl
// ============================================================================
// Attaches to an existing shared memory segment as a writer (WriteAttach mode).
// Used for the hub/broker architecture: the hub creates the segment and the source
// process attaches R/W without initializing or owning the segment.
// Validation mirrors find_datablock_consumer_impl (secret + schema).
std::unique_ptr<DataBlockProducer>
attach_datablock_as_writer_impl(const std::string &name,
                                uint64_t shared_secret,
                                const DataBlockConfig *expected_config,
                                const pylabhub::schema::SchemaInfo *flexzone_schema,
                                const pylabhub::schema::SchemaInfo *datablock_schema)
{
    if (!lifecycle_initialized())
    {
        throw std::runtime_error(
            "DataBlock: Data Exchange Hub module not initialized. Create a LifecycleGuard in main() "
            "with pylabhub::hub::GetLifecycleModule() (and typically Logger, CryptoUtils) before attaching as writer.");
    }

    auto impl = std::make_unique<DataBlockProducerImpl>();
    impl->name = name;
    // Attach R/W to existing segment; no creation, no header initialization, no unlink.
    impl->dataBlock = std::make_unique<DataBlock>(name, DataBlockOpenMode::WriteAttach);

    auto *header = impl->dataBlock->header();
    if (header == nullptr)
    {
        return nullptr;
    }

    // Validate shared secret (first 8 bytes)
    if (std::memcmp(header->shared_secret, &shared_secret, sizeof(shared_secret)) != 0)
    {
        LOGGER_WARN("[DataBlock:{}] WriteAttach: shared_secret mismatch.", name);
        return nullptr;
    }

    // Validate header layout hash (ABI compatibility)
    try
    {
        validate_header_layout_hash(header);
    }
    catch (const pylabhub::schema::SchemaValidationException &)
    {
        header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
        LOGGER_WARN("[DataBlock:{}] WriteAttach: Header layout mismatch (ABI incompatibility).", name);
        return nullptr;
    }

    // Validate layout checksum + optional config
    if (!validate_attach_layout_and_config(header, expected_config))
    {
        LOGGER_WARN("[DataBlock:{}] WriteAttach: Layout checksum or config mismatch.", name);
        return nullptr;
    }
    impl->checksum_policy = header->checksum_policy;

    // Reconstruct flex zone layout
    auto layout = DataBlockLayout::from_header(header);
    impl->flex_zone_offset = layout.flexible_zone_offset;
    impl->flex_zone_size = layout.flexible_zone_size;

    // Validate FlexZone schema if provided
    if (flexzone_schema != nullptr)
    {
        bool has_flexzone = std::any_of(
            header->flexzone_schema_hash,
            header->flexzone_schema_hash + detail::CHECKSUM_BYTES,
            [](uint8_t byte) { return byte != 0; });
        if (!has_flexzone)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[DataBlock:{}] WriteAttach: Creator did not store FlexZone schema, but writer expects '{}'",
                        name, flexzone_schema->name);
            return nullptr;
        }
        if (std::memcmp(header->flexzone_schema_hash, flexzone_schema->hash.data(), detail::CHECKSUM_BYTES) != 0)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] WriteAttach: FlexZone schema hash mismatch! Expected '{}' v{}",
                         name, flexzone_schema->name, flexzone_schema->version.to_string());
            return nullptr;
        }
        LOGGER_DEBUG("[DataBlock:{}] WriteAttach: FlexZone schema validated: {} v{}",
                     name, flexzone_schema->name, flexzone_schema->version.to_string());
    }

    // Validate DataBlock schema if provided
    if (datablock_schema != nullptr)
    {
        bool has_datablock = std::any_of(
            header->datablock_schema_hash,
            header->datablock_schema_hash + detail::CHECKSUM_BYTES,
            [](uint8_t byte) { return byte != 0; });
        if (!has_datablock)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN("[DataBlock:{}] WriteAttach: Creator did not store DataBlock schema, but writer expects '{}'",
                        name, datablock_schema->name);
            return nullptr;
        }
        if (std::memcmp(header->datablock_schema_hash, datablock_schema->hash.data(), detail::CHECKSUM_BYTES) != 0)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] WriteAttach: DataBlock schema hash mismatch! Expected '{}' v{}",
                         name, datablock_schema->name, datablock_schema->version.to_string());
            return nullptr;
        }
        auto stored_version = pylabhub::schema::SchemaVersion::unpack(header->schema_version);
        if (stored_version.major != datablock_schema->version.major)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] WriteAttach: Incompatible DataBlock schema version! Creator: {}, Writer: {}",
                         name, stored_version.to_string(), datablock_schema->version.to_string());
            return nullptr;
        }
        LOGGER_DEBUG("[DataBlock:{}] WriteAttach: DataBlock schema validated: {} v{}",
                     name, datablock_schema->name, datablock_schema->version.to_string());
    }

    LOGGER_INFO("[DataBlock:{}] Attached as writer (WriteAttach mode).", name);
    return std::make_unique<DataBlockProducer>(std::move(impl));
}

// Non-template wrappers removed (see comment after create_datablock_producer_impl above).

} // namespace pylabhub::hub
