#pragma once
/**
 * @file schema_blds.hpp
 * @brief BLDS (Basic Layout Description String) schema generation for DataBlock.
 *
 * This module provides compile-time schema generation for C++ structs used in
 * shared memory DataBlocks. The schema is represented as a BLDS string that
 * captures the struct layout (member names, types, offsets, sizes) and is
 * hashed with BLAKE2b-256 to create a unique schema identifier.
 *
 * Design Goals:
 * - Compile-time schema generation (no runtime reflection overhead)
 * - Canonical representation (same struct layout = same BLDS = same hash)
 * - Platform-independent (handles endianness, alignment, padding)
 * - Extensible (supports nested structs, arrays, basic types)
 *
 * BLDS Format:
 *   BLDS := MEMBER_LIST
 *   MEMBER_LIST := MEMBER (";" MEMBER)*
 *   MEMBER := MEMBER_NAME ":" TYPE_ID [ "@" OFFSET ":" SIZE ]
 *   TYPE_ID := FUNDAMENTAL_TYPE | ARRAY_TYPE | STRUCT_HASH
 *   FUNDAMENTAL_TYPE := "f32" | "f64" | "i8" | "i16" | "i32" | "i64" | "u8" | ...
 *   ARRAY_TYPE := TYPE_ID "[" COUNT "]"
 *   STRUCT_HASH := "_" BLAKE2B_HEX
 *
 * Default practice: every member includes both MEMBER_NAME and TYPE_ID. When layout
 * validation is required (e.g. SharedMemoryHeader protocol), also include
 * "@offset:size" so the hash reflects memory layout and producer/consumer can
 * verify identical ABI.
 *
 * Example (type-only): "timestamp_ns:u64;temperature:f32;pressure:f32;humidity:f32"
 * Example (with layout): "magic_number:u32@0:4;version_major:u16@4:2"
 *   Hash: BLAKE2b-256 of BLDS string
 *
 * @see HEP-CORE-0002-DataHub-FINAL.md Section 11 (Schema Validation)
 */
#include "utils/crypto_utils.hpp"
#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <type_traits>
#include <cstdint>
#include <cstddef>

namespace pylabhub::schema
{

// ============================================================================
// Type ID Mapping (C++ types → BLDS type identifiers)
// ============================================================================

/**
 * @brief Maps C++ fundamental types to BLDS type identifiers.
 * @details Uses template specialization for compile-time type mapping.
 */
template <typename T> struct BLDSTypeID;

// Floating-point types
template <> struct BLDSTypeID<float>
{
    static constexpr const char *value = "f32";
};
template <> struct BLDSTypeID<double>
{
    static constexpr const char *value = "f64";
};

// Signed integer types
template <> struct BLDSTypeID<int8_t>
{
    static constexpr const char *value = "i8";
};
template <> struct BLDSTypeID<int16_t>
{
    static constexpr const char *value = "i16";
};
template <> struct BLDSTypeID<int32_t>
{
    static constexpr const char *value = "i32";
};
template <> struct BLDSTypeID<int64_t>
{
    static constexpr const char *value = "i64";
};

// Unsigned integer types
template <> struct BLDSTypeID<uint8_t>
{
    static constexpr const char *value = "u8";
};
template <> struct BLDSTypeID<uint16_t>
{
    static constexpr const char *value = "u16";
};
template <> struct BLDSTypeID<uint32_t>
{
    static constexpr const char *value = "u32";
};
template <> struct BLDSTypeID<uint64_t>
{
    static constexpr const char *value = "u64";
};

// Boolean type
template <> struct BLDSTypeID<bool>
{
    static constexpr const char *value = "b";
};

// Character types
template <> struct BLDSTypeID<char>
{
    static constexpr const char *value = "c";
};

// std::atomic (layout same as underlying type; for protocol checking use underlying type id)
template <typename T> struct BLDSTypeID<std::atomic<T>> : BLDSTypeID<T>
{
};

// Array specialization
template <typename T, size_t N> struct BLDSTypeID<T[N]>
{
    static std::string value()
    {
        if constexpr (std::is_same_v<T, char>)
        {
            // Special case: char arrays are strings
            return "c[" + std::to_string(N) + "]";
        }
        else
        {
            return std::string(BLDSTypeID<T>::value) + "[" + std::to_string(N) + "]";
        }
    }
};

// std::array specialization
template <typename T, size_t N> struct BLDSTypeID<std::array<T, N>>
{
    static std::string value()
    {
        return std::string(BLDSTypeID<T>::value) + "[" + std::to_string(N) + "]";
    }
};

// ============================================================================
// Schema Version
// ============================================================================

/**
 * @brief Semantic version for schema evolution.
 */
struct SchemaVersion
{
    uint16_t major = 1; ///< Breaking changes (incompatible)
    uint16_t minor = 0; ///< Non-breaking additions (backward-compatible)
    uint16_t patch = 0; ///< Bug fixes (fully compatible)

    std::string to_string() const
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    /**
     * @brief Packs version into uint32_t for storage in SharedMemoryHeader.
     * @details Format: [major:10bits][minor:10bits][patch:12bits]
     *          Maximum values: major=1023, minor=1023, patch=4095
     */
    uint32_t pack() const
    {
        return (static_cast<uint32_t>(major & 0x3FF) << 22) |
               (static_cast<uint32_t>(minor & 0x3FF) << 12) |
               (static_cast<uint32_t>(patch & 0xFFF));
    }

    /**
     * @brief Unpacks version from uint32_t stored in SharedMemoryHeader.
     */
    static SchemaVersion unpack(uint32_t packed)
    {
        SchemaVersion v;
        v.major = static_cast<uint16_t>((packed >> 22) & 0x3FF);
        v.minor = static_cast<uint16_t>((packed >> 12) & 0x3FF);
        v.patch = static_cast<uint16_t>(packed & 0xFFF);
        return v;
    }
};

// ============================================================================
// Schema Info
// ============================================================================

/**
 * @brief Complete schema information for a DataBlock structure.
 *
 * `packing` is part of the fingerprint (HEP-CORE-0034 §6.3): two SchemaInfos
 * with identical BLDS but different packing have different hashes.  C++
 * template producers populate `packing` from `SchemaRegistry<T>::packing`
 * (set by `PYLABHUB_SCHEMA_BEGIN` / `PYLABHUB_SCHEMA_BEGIN_PACKED`); the
 * file loader populates it from the JSON `slot.packing` / `flexzone.packing`
 * fields.
 */
struct SchemaInfo
{
    std::string name;                       ///< Schema name (e.g., "SensorHub.SensorData")
    std::string blds;                       ///< BLDS string representation
    std::array<uint8_t, 32> hash{};         ///< BLAKE2b-256 hash of canonical(BLDS, packing)
    SchemaVersion version;                  ///< Semantic version
    size_t struct_size = 0;                 ///< sizeof(T) for validation
    std::string packing{"aligned"};         ///< "aligned" | "packed" (HEP-CORE-0034 §6.3)

    /**
     * @brief Computes the BLAKE2b-256 hash of the canonical schema string.
     * @details Canonical form is `blds + "|pack:" + packing`.  Hash is
     *          deterministic and platform-independent.  Two schemas with
     *          identical BLDS but different packing produce different hashes
     *          (HEP-CORE-0034 Phase 1 fingerprint correction).
     *          Uses libsodium via crypto_utils module.
     */
    void compute_hash()
    {
        std::string canonical;
        canonical.reserve(blds.size() + 16);
        canonical.append(blds);
        canonical.append("|pack:");
        canonical.append(packing);
        hash = pylabhub::crypto::compute_blake2b_array(canonical.data(), canonical.size());
    }

    /**
     * @brief Checks if this schema matches another (by hash comparison).
     */
    [[nodiscard]] bool matches(const SchemaInfo &other) const { return hash == other.hash; }

    /**
     * @brief Checks if this schema matches a stored hash.
     */
    [[nodiscard]] bool matches_hash(const std::array<uint8_t, 32> &other_hash) const
    {
        return hash == other_hash;
    }
};

// ============================================================================
// Schema Builder (for manual schema construction)
// ============================================================================
//
// Default practice: every schema entry must include both member name and field type
// (type_id). For shared-memory / ABI validation, also include offset and size so the
// hash reflects layout. Use the 4-argument add_member() for header/layout schemas.
// ============================================================================

/**
 * @brief Builder for constructing BLDS strings manually.
 * @details Used internally by schema generation macros. Every member must supply
 *          both name and type_id; for layout validation also supply offset and size.
 */
class BLDSBuilder
{
  public:
    BLDSBuilder() = default;

    /**
     * @brief Adds a member with name and type only (no layout).
     * @param name Member name (required).
     * @param type_id BLDS type identifier, e.g. "u64", "f32[4]" (required).
     * @details Produces "name:type_id". Prefer add_member(name, type_id, offset, size)
     *          for shared-memory/ABI schemas so the hash includes layout.
     */
    void add_member(const std::string &name, const std::string &type_id)
    {
        if (!blds_.empty())
        {
            blds_ += ";";
        }
        blds_ += name + ":" + type_id;
    }

    /**
     * @brief Adds a member with name, type, and layout (default for ABI/layout schemas).
     * @param name Member name (required).
     * @param type_id BLDS type identifier (required).
     * @param offset Byte offset of the member within the struct.
     * @param size Size in bytes of the member.
     * @details Produces "name:type_id@offset:size". Use this for SharedMemoryHeader
     *          and any schema used for protocol/ABI validation.
     */
    void add_member(const std::string &name, const std::string &type_id, size_t offset, size_t size)
    {
        if (!blds_.empty())
        {
            blds_ += ";";
        }
        blds_ += name + ":" + type_id + "@" + std::to_string(offset) + ":" + std::to_string(size);
    }

    /**
     * @brief Returns the constructed BLDS string.
     */
    std::string build() const { return blds_; }

  private:
    std::string blds_;
};

// ============================================================================
// Schema Generation (template-based introspection)
// ============================================================================

/**
 * @brief Generates schema information for a C++ struct.
 * @details Uses compile-time introspection to build BLDS string.
 *
 * @tparam T The struct type to generate schema for.
 * @param name Schema name (e.g., "SensorHub.SensorData").
 * @param version Schema version.
 * @return SchemaInfo with BLDS string and hash.
 *
 * @note This requires the struct to be registered with PYLABHUB_SCHEMA_*() macros.
 *
 * @example
 * auto schema = generate_schema_info<SensorData>(
 *     "SensorHub.SensorData",
 *     SchemaVersion{1, 0, 0}
 * );
 */
template <typename T>
SchemaInfo generate_schema_info(const std::string &name, const SchemaVersion &version);

// Forward declaration for schema registry
template <typename T> struct SchemaRegistry;

// ============================================================================
// SchemaRegistry Detection Trait
// ============================================================================

/**
 * @brief Trait: true when T has been registered with PYLABHUB_SCHEMA_BEGIN/MEMBER/END.
 *
 * Used in `validate_named_schema<DataT, FlexT>()` (schema_library.hpp) to
 * conditionally enable hash-level schema verification alongside size checks.
 * Without the macro registration, only `sizeof(T)` is compared against the
 * schema's `struct_size`; the BLDS hash is not checked.
 *
 * @tparam T  Any C++ type.
 */
template <typename T, typename = void>
struct has_schema_registry : std::false_type
{
};
template <typename T>
struct has_schema_registry<T, std::void_t<decltype(SchemaRegistry<T>::generate_blds())>>
    : std::true_type
{
};
template <typename T>
inline constexpr bool has_schema_registry_v = has_schema_registry<T>::value;

// ============================================================================
// Schema Registration Macros
// ============================================================================

/**
 * @def PYLABHUB_SCHEMA_BEGIN(StructName)
 * @brief Begins schema registration for a natural-aligned struct.
 *
 * Produces `SchemaRegistry<StructName>::packing == "aligned"`.  Use this
 * for plain C++ structs that follow natural alignment — the default for
 * almost every producer.
 *
 * @example
 * PYLABHUB_SCHEMA_BEGIN(SensorData)
 *     PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
 *     PYLABHUB_SCHEMA_MEMBER(temperature)
 *     PYLABHUB_SCHEMA_MEMBER(pressure)
 * PYLABHUB_SCHEMA_END(SensorData)
 *
 * @see PYLABHUB_SCHEMA_BEGIN_PACKED — for `#pragma pack(1)` / `__attribute__((packed))` structs
 */
#define PYLABHUB_SCHEMA_BEGIN(StructName)                                                          \
    PYLABHUB_SCHEMA_BEGIN_IMPL_(StructName, "aligned")

/**
 * @def PYLABHUB_SCHEMA_BEGIN_PACKED(StructName)
 * @brief Begins schema registration for a `_pack_=1` (packed) struct.
 *
 * Produces `SchemaRegistry<StructName>::packing == "packed"`.  Use this
 * only when the struct itself is declared packed via
 * `#pragma pack(push, 1) ... #pragma pack(pop)` or
 * `__attribute__((packed))`.  The wire fingerprint includes `packing`
 * (HEP-CORE-0034 §6.3), so a packed C++ producer is wire-compatible only
 * with consumers that explicitly declare `"packing": "packed"` in JSON.
 *
 * @note A `static_assert` in `PYLABHUB_SCHEMA_END` verifies that
 *       `sizeof(StructName)` matches the size computed under the declared
 *       packing.  If the assertion fires, either the `_PACKED` macro was
 *       used on a non-packed struct, or the regular macro was used on a
 *       packed struct — fix the macro to match the struct declaration.
 *
 * @example
 * #pragma pack(push, 1)
 * struct WireFrame {
 *     double  ts;
 *     uint8_t flags;
 *     int32_t value;
 * };
 * #pragma pack(pop)
 *
 * PYLABHUB_SCHEMA_BEGIN_PACKED(WireFrame)
 *     PYLABHUB_SCHEMA_MEMBER(ts)
 *     PYLABHUB_SCHEMA_MEMBER(flags)
 *     PYLABHUB_SCHEMA_MEMBER(value)
 * PYLABHUB_SCHEMA_END(WireFrame)
 */
#define PYLABHUB_SCHEMA_BEGIN_PACKED(StructName)                                                   \
    PYLABHUB_SCHEMA_BEGIN_IMPL_(StructName, "packed")

#define PYLABHUB_SCHEMA_BEGIN_IMPL_(StructName, PackingStr)                                        \
    namespace pylabhub::schema                                                                     \
    {                                                                                              \
    template <> struct SchemaRegistry<StructName>                                                  \
    {                                                                                              \
        using StructType = StructName;                                                             \
        static constexpr const char *packing = PackingStr;                                         \
        static std::string generate_blds()                                                         \
        {                                                                                          \
            BLDSBuilder builder;

/**
 * @def PYLABHUB_SCHEMA_MEMBER(member_name)
 * @brief Registers a struct member for schema generation.
 */
#define PYLABHUB_SCHEMA_MEMBER(member_name)                                                        \
    builder.add_member(#member_name, get_type_id(&StructType::member_name));

/**
 * @def PYLABHUB_SCHEMA_END(StructName)
 * @brief Ends schema registration for a struct.
 * @note StructName parameter must match the one in PYLABHUB_SCHEMA_BEGIN.
 */
#define PYLABHUB_SCHEMA_END(StructName)                                                            \
    return builder.build();                                                                        \
    }                                                                                              \
                                                                                                   \
  private:                                                                                         \
    template <typename T> static std::string get_type_id(T StructType::*)                          \
    {                                                                                              \
        if constexpr (std::is_array_v<T>)                                                          \
        {                                                                                          \
            using ElemType = std::remove_extent_t<T>;                                              \
            constexpr size_t N = std::extent_v<T>;                                                 \
            return BLDSTypeID<ElemType[N]>::value();                                               \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            return BLDSTypeID<T>::value;                                                           \
        }                                                                                          \
    }                                                                                              \
    }                                                                                              \
    ;                                                                                              \
    }

// ============================================================================
// Schema Info Generation Implementation
// ============================================================================

template <typename T>
SchemaInfo generate_schema_info(const std::string &name, const SchemaVersion &version)
{
    SchemaInfo info;
    info.name = name;
    info.version = version;
    info.struct_size = sizeof(T);

    // Generate BLDS string using SchemaRegistry specialization
    info.blds = SchemaRegistry<T>::generate_blds();

    // Pull declared packing from the macro-generated specialization
    // (HEP-CORE-0034 §6.3 — packing is part of the fingerprint).
    info.packing = SchemaRegistry<T>::packing;

    // Compute BLAKE2b-256 hash over canonical(BLDS, packing)
    info.compute_hash();

    return info;
}

// ============================================================================
// Validation Helpers
// ============================================================================

/**
 * @brief Exception thrown when schema validation fails.
 */
class SchemaValidationException : public std::runtime_error
{
  public:
    SchemaValidationException(const std::string &msg, const std::array<uint8_t, 32> &expected,
                              const std::array<uint8_t, 32> &actual)
        : std::runtime_error(msg), expected_hash(expected), actual_hash(actual)
    {
    }

    std::array<uint8_t, 32> expected_hash;
    std::array<uint8_t, 32> actual_hash;
};

/**
 * @brief Validates that two schemas match (by hash comparison).
 * @throws SchemaValidationException if hashes don't match.
 */
inline void validate_schema_match(const SchemaInfo &expected, const SchemaInfo &actual,
                                  const std::string &context = "Schema validation")
{
    if (!expected.matches(actual))
    {
        throw SchemaValidationException(context + ": Schema mismatch detected. Expected schema '" +
                                            expected.name + "', got '" + actual.name + "'",
                                        expected.hash, actual.hash);
    }
}

/**
 * @brief Validates that a schema matches a stored hash.
 * @throws SchemaValidationException if hash doesn't match.
 */
inline void validate_schema_hash(const SchemaInfo &schema,
                                 const std::array<uint8_t, 32> &stored_hash,
                                 const std::string &context = "Schema validation")
{
    if (!schema.matches_hash(stored_hash))
    {
        throw SchemaValidationException(
            context + ": Schema hash mismatch for '" + schema.name + "'", stored_hash, schema.hash);
    }
}

} // namespace pylabhub::schema
