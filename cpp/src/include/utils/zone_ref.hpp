/**
 * @file zone_ref.hpp
 * @brief Type-safe wrapper for flexible zone access in RAII layer
 * 
 * @copyright Copyright (c) 2024-2026 PyLabHub Project
 * 
 * Part of Phase 3: C++ RAII Layer
 * Provides type-safe access to flexible zones with compile-time and runtime validation.
 * 
 * Design Philosophy:
 * - Wraps Producer/Consumer flexible zone access
 * - Provides typed .get() access with size validation
 * - Offers raw memory access as opt-in capability
 * - Enforces trivial copyability for shared memory safety
 * - Supports void specialization for no-flexzone mode
 */

#pragma once

#include "utils/data_block.hpp"
#include <concepts>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace pylabhub::hub
{

/**
 * @class ZoneRef
 * @brief Type-safe reference to a flexible zone
 * 
 * @tparam FlexZoneT The data type stored in the flexible zone (must be trivially copyable)
 * @tparam IsMutable True for write access, false for read-only access
 * 
 * ZoneRef wraps the low-level flexible_zone_span() API and provides:
 * 1. **Typed access**: `.get()` returns `FlexZoneT&` with size validation
 * 2. **Raw access**: `.raw_access()` returns `std::span<std::byte>` for advanced use
 * 3. **Void specialization**: ZoneRef<void> for no-flexzone mode
 * 
 * Phase 2 Note: Currently only single flex zone supported (index always 0).
 * Future versions may support multiple zones via index parameter.
 * 
 * Usage (Producer):
 * @code
 * WriteZoneRef<MetaData> zone = ctx.flexzone();
 * zone.get().status = Status::Active;  // Type-safe write
 * @endcode
 * 
 * Usage (Consumer):
 * @code
 * ReadZoneRef<MetaData> zone = ctx.flexzone();
 * if (zone.get().shutdown_requested) break;  // Type-safe read
 * @endcode
 * 
 * No Flexzone Mode:
 * @code
 * WriteZoneRef<void> zone = ctx.flexzone();  // or just omit flexzone access
 * // zone.get() not available for void type
 * @endcode
 * 
 * Thread Safety: ZoneRef instances are not thread-safe. Each thread should use
 * its own transaction context and zone references.
 */
template <typename FlexZoneT, bool IsMutable>
class ZoneRef
{
  public:
    using value_type = std::conditional_t<IsMutable, FlexZoneT, const FlexZoneT>;
    using span_type = std::conditional_t<IsMutable, std::span<std::byte>, std::span<const std::byte>>;

    // Compile-time check: FlexZoneT must be trivially copyable for shared memory
    // (unless it's void, which is allowed for no-flexzone mode)
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for safe shared memory access. "
                  "Types with vtables, std::string, std::vector, etc. are not allowed. "
                  "Use FlexZoneT=void for no-flexzone mode.");

    // ====================================================================
    // Construction (Internal - used by TransactionContext)
    // ====================================================================

    /**
     * @brief Construct from DataBlockProducer (mutable access)
     * @param producer Pointer to producer (non-null)
     */
    explicit ZoneRef(DataBlockProducer *producer) requires IsMutable : m_producer(producer)
    {
        if (!producer)
        {
            throw std::invalid_argument("ZoneRef: producer cannot be null");
        }
    }

    /**
     * @brief Construct from DataBlockConsumer (const access)
     * @param consumer Pointer to consumer (non-null)
     */
    explicit ZoneRef(DataBlockConsumer *consumer) requires(!IsMutable) : m_consumer(consumer)
    {
        if (!consumer)
        {
            throw std::invalid_argument("ZoneRef: consumer cannot be null");
        }
    }

    // Movable but not copyable (references underlying producer/consumer)
    ZoneRef(ZoneRef &&) noexcept = default;
    ZoneRef &operator=(ZoneRef &&) noexcept = default;
    ZoneRef(const ZoneRef &) = delete;
    ZoneRef &operator=(const ZoneRef &) = delete;

    // ====================================================================
    // Typed Access (disabled for void specialization)
    // ====================================================================

    /**
     * @brief Get typed reference to flexible zone data
     * @return Reference to FlexZoneT stored in flexible zone
     * @throws std::runtime_error if zone size < sizeof(FlexZoneT)
     * @throws std::runtime_error if no flexible zone configured
     * 
     * This is the primary interface for type-safe access. The returned reference
     * is valid for the lifetime of the ZoneRef.
     * 
     * Size validation ensures the zone is large enough for FlexZoneT.
     * 
     * Note: Not available when FlexZoneT=void (use raw_access() or omit flexzone).
     */
    [[nodiscard]] value_type &get() requires(!std::is_void_v<FlexZoneT>)
    {
        span_type raw = raw_access();

        if (raw.empty())
        {
            throw std::runtime_error("ZoneRef::get(): no flexible zone configured");
        }

        if (raw.size() < sizeof(FlexZoneT))
        {
            throw std::runtime_error("ZoneRef::get(): flexible zone size (" + std::to_string(raw.size()) +
                                     " bytes) is smaller than sizeof(FlexZoneT) (" +
                                     std::to_string(sizeof(FlexZoneT)) + " bytes)");
        }

        // Cast to typed reference
        if constexpr (IsMutable)
        {
            return *reinterpret_cast<FlexZoneT *>(raw.data());
        }
        else
        {
            return *reinterpret_cast<const FlexZoneT *>(raw.data());
        }
    }

    /**
     * @brief Get const typed reference (available for both mutable and const zones)
     * @return Const reference to FlexZoneT
     * @throws std::runtime_error if zone size < sizeof(FlexZoneT)
     * @throws std::runtime_error if no flexible zone configured
     */
    [[nodiscard]] const FlexZoneT &get() const requires(!std::is_void_v<FlexZoneT>)
    {
        auto raw = raw_access();

        if (raw.empty())
        {
            throw std::runtime_error("ZoneRef::get(): no flexible zone configured");
        }

        if (raw.size() < sizeof(FlexZoneT))
        {
            throw std::runtime_error("ZoneRef::get(): flexible zone size (" + std::to_string(raw.size()) +
                                     " bytes) is smaller than sizeof(FlexZoneT) (" +
                                     std::to_string(sizeof(FlexZoneT)) + " bytes)");
        }

        return *reinterpret_cast<const FlexZoneT *>(raw.data());
    }

    // ====================================================================
    // Raw Memory Access (Opt-In)
    // ====================================================================

    /**
     * @brief Get raw memory span for advanced usage (mutable version)
     * @return Mutable span of bytes covering the flexible zone (empty if not configured or moved-from)
     *
     * **Use with caution**: This bypasses type safety. User is responsible for:
     * - Correct interpretation of memory layout
     * - Not exceeding span boundaries
     * - Maintaining data structure invariants
     *
     * Returns empty span if no flexible zone is configured or if ZoneRef was moved from.
     *
     * Only available after transaction entry validation (schema, checksums, etc.)
     */
    [[nodiscard]] std::span<std::byte> raw_access() noexcept requires IsMutable
    {
        if (!m_producer)
        {
            return {};
        }
        return m_producer->flexible_zone_span();
    }

    /**
     * @brief Get raw memory span for advanced usage (const version)
     * @return Const span of bytes covering the flexible zone (empty if not configured or moved-from)
     */
    [[nodiscard]] std::span<const std::byte> raw_access() const noexcept
    {
        if constexpr (IsMutable)
        {
            if (!m_producer)
            {
                return {};
            }
            return m_producer->flexible_zone_span();
        }
        else
        {
            if (!m_consumer)
            {
                return {};
            }
            return m_consumer->flexible_zone_span();
        }
    }

    // ====================================================================
    // Zone Metadata
    // ====================================================================

    /**
     * @brief Check if flexible zone is configured
     * @return true if zone exists and has non-zero size
     */
    [[nodiscard]] bool has_zone() const noexcept
    {
        return !raw_access().empty();
    }

    /**
     * @brief Get flexible zone size in bytes
     * @return Size of flexible zone (0 if not configured)
     */
    [[nodiscard]] size_t size() const noexcept
    {
        return raw_access().size();
    }

  private:
    // Storage: either producer (mutable) or consumer (const)
    DataBlockProducer *m_producer = nullptr;    // Valid when IsMutable == true
    DataBlockConsumer *m_consumer = nullptr;    // Valid when IsMutable == false
};

// ====================================================================
// Convenience Type Aliases
// ====================================================================

/**
 * @brief Type alias for mutable zone reference (producer side)
 * @tparam T The data type stored in the flexible zone (or void for no-flexzone)
 */
template <typename T>
using WriteZoneRef = ZoneRef<T, true>;

/**
 * @brief Type alias for const zone reference (consumer side)
 * @tparam T The data type stored in the flexible zone (or void for no-flexzone)
 */
template <typename T>
using ReadZoneRef = ZoneRef<T, false>;

} // namespace pylabhub::hub
