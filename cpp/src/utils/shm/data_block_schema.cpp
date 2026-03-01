// data_block_schema.cpp
//
// SharedMemoryHeader schema validation and layout checksum.
//
// Contains the four exported functions declared in utils/data_block.hpp:
//   get_shared_memory_header_schema_info()
//   validate_header_layout_hash()
//   store_layout_checksum()
//   validate_layout_checksum()
//
// These functions depend only on public headers — no private data_block_internal.hpp needed.
// The canonical schema field list lives next to SharedMemoryHeader in data_block.hpp
// (PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS). Update that list and the struct together.

#include "utils/crypto_utils.hpp"
#include "utils/logger.hpp"
#include "utils/data_block.hpp"
#include "utils/deterministic_checksum.hpp"
#include <cassert>

namespace pylabhub::hub
{

// ============================================================================
// SharedMemoryHeader schema (layout + protocol check)
// ============================================================================
pylabhub::schema::SchemaInfo get_shared_memory_header_schema_info()
{
    using pylabhub::schema::BLDSBuilder;
    using pylabhub::schema::SchemaInfo;
    using pylabhub::schema::SchemaVersion;

    BLDSBuilder builder{};
    // Header defines the list: PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS(OP) expands to
    //   OP(magic_number,"u32") OP(version_major,"u16") ... for every header field.
    // We define OP here: for each (member, type_id) call add_member(name, type_id, offset, size).
#define PYLABHUB_ADD_SCHEMA_FIELD(member, type_id)                                            \
    builder.add_member(#member, type_id, offsetof(SharedMemoryHeader, member),               \
                      sizeof(SharedMemoryHeader::member));
    PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS(PYLABHUB_ADD_SCHEMA_FIELD);
#undef PYLABHUB_ADD_SCHEMA_FIELD
    // Trailing fields: type_id depends on constants (must stay in same order as struct)
    builder.add_member("consumer_heartbeats",
                 fmt::format("ConsumerHeartbeat[{}]", detail::MAX_CONSUMER_HEARTBEATS),
                 offsetof(SharedMemoryHeader, consumer_heartbeats),
                 sizeof(SharedMemoryHeader::consumer_heartbeats));
    builder.add_member("spinlock_states",
                 fmt::format("SharedSpinLockState[{}]", detail::MAX_SHARED_SPINLOCKS),
                 offsetof(SharedMemoryHeader, spinlock_states),
                 sizeof(SharedMemoryHeader::spinlock_states));
    builder.add_member("flexible_zone_checksums",
                 fmt::format("FlexibleZoneChecksumEntry[{}]", detail::MAX_FLEXIBLE_ZONE_CHECKSUMS),
                 offsetof(SharedMemoryHeader, flexible_zone_checksums),
                 sizeof(SharedMemoryHeader::flexible_zone_checksums));
    builder.add_member("reserved_header",
                 fmt::format("u8[{}]", sizeof(SharedMemoryHeader::reserved_header)),
                 offsetof(SharedMemoryHeader, reserved_header),
                 sizeof(SharedMemoryHeader::reserved_header));

    SchemaInfo info{};
    info.name = "pylabhub.hub.SharedMemoryHeader";
    info.version = SchemaVersion{detail::HEADER_VERSION_MAJOR, detail::HEADER_VERSION_MINOR, 0};
    info.struct_size = sizeof(SharedMemoryHeader);
    info.blds = builder.build();
    info.compute_hash();
    return info;
}

void validate_header_layout_hash(const SharedMemoryHeader *header)
{
    if (header == nullptr)
    {
        throw std::invalid_argument("validate_header_layout_hash: header is null");
    }
    pylabhub::schema::SchemaInfo expected = get_shared_memory_header_schema_info();
    const uint8_t *stored = header->reserved_header + detail::HEADER_LAYOUT_HASH_OFFSET;
    if (std::memcmp(expected.hash.data(), stored, detail::HEADER_LAYOUT_HASH_SIZE) != 0)
    {
        std::array<uint8_t, detail::CHECKSUM_BYTES> actual_hash;
        std::memcpy(actual_hash.data(), stored, detail::HEADER_LAYOUT_HASH_SIZE);
        throw pylabhub::schema::SchemaValidationException(
            "SharedMemoryHeader layout mismatch: producer and consumer have different ABI "
            "(offset/size).",
            expected.hash, actual_hash);
    }
}

// ============================================================================
// Layout checksum (segment layout-defining values)
// ============================================================================
namespace
{
/** Layout checksum input: fixed order so producer and consumer hash the same bytes.
 *  Order: ring_buffer_capacity(4), physical_page_size(4), logical_unit_size(4),
 *  flexible_zone_size(8), checksum_type(1), policy(1), consumer_sync_policy(1), reserved(1). */
constexpr size_t LAYOUT_CHECKSUM_INPUT_BYTES = 24U;

inline void layout_checksum_fill(uint8_t *buf, const SharedMemoryHeader *header)
{
    if (buf == nullptr || header == nullptr)
    {
        return;
    }
    using pylabhub::utils::append_le_u32;
    using pylabhub::utils::append_le_u64;
    using pylabhub::utils::append_u8;
    size_t off = 0;
    append_le_u32(buf, off, header->ring_buffer_capacity);
    append_le_u32(buf, off, header->physical_page_size);
    append_le_u32(buf, off, header->logical_unit_size);
    append_le_u64(buf, off, static_cast<uint64_t>(header->flexible_zone_size));
    append_u8(buf, off, header->checksum_type);
    append_u8(buf, off, static_cast<uint8_t>(header->policy));
    append_u8(buf, off, static_cast<uint8_t>(header->consumer_sync_policy));
    append_u8(buf, off, 0); // reserved
    assert(off == LAYOUT_CHECKSUM_INPUT_BYTES);
}
} // namespace

void store_layout_checksum(SharedMemoryHeader *header)
{
    if (header == nullptr)
    {
        return;
    }
    std::array<uint8_t, LAYOUT_CHECKSUM_INPUT_BYTES> buf{};
    layout_checksum_fill(buf.data(), header);
    uint8_t *out = header->reserved_header + detail::LAYOUT_CHECKSUM_OFFSET;
    if (!pylabhub::crypto::compute_blake2b(out, buf.data(), buf.size()))
    {
        LOGGER_ERROR("[DataBlock] store_layout_checksum: compute_blake2b failed; storing zeros.");
        std::memset(out, 0, detail::LAYOUT_CHECKSUM_SIZE);
    }
}

bool validate_layout_checksum(const SharedMemoryHeader *header)
{
    if (header == nullptr)
    {
        return false;
    }
    std::array<uint8_t, LAYOUT_CHECKSUM_INPUT_BYTES> buf{};
    layout_checksum_fill(buf.data(), header);
    std::array<uint8_t, detail::CHECKSUM_BYTES> computed;
    if (!pylabhub::crypto::compute_blake2b(computed.data(), buf.data(), buf.size()))
    {
        return false;
    }
    const uint8_t *stored = header->reserved_header + detail::LAYOUT_CHECKSUM_OFFSET;
    return std::memcmp(computed.data(), stored, detail::LAYOUT_CHECKSUM_SIZE) == 0;
}

} // namespace pylabhub::hub
