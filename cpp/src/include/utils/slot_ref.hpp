/**
 * @file slot_ref.hpp
 * @brief Type-safe wrapper for data slot access in RAII layer
 * 
 * @copyright Copyright (c) 2024-2026 PyLabHub Project
 * 
 * Part of Phase 3: C++ RAII Layer
 * Provides type-safe access to datablock slots with compile-time and runtime validation.
 * 
 * Design Philosophy:
 * - Wraps existing SlotWriteHandle/SlotConsumeHandle primitives
 * - Provides typed .get() access with size validation
 * - Offers raw memory access as opt-in capability
 * - Enforces trivial copyability for shared memory safety
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
 * @class SlotRef
 * @brief Type-safe reference to a datablock slot
 * 
 * @tparam DataBlockT The data type stored in the slot (must be trivially copyable)
 * @tparam IsMutable True for write access, false for read-only access
 * 
 * SlotRef wraps the low-level SlotWriteHandle (producer) or SlotConsumeHandle (consumer)
 * and provides:
 * 1. **Typed access**: `.get()` returns `DataBlockT&` with size validation
 * 2. **Raw access**: `.raw_access()` returns `std::span<std::byte>` for advanced use
 * 3. **Metadata**: Slot ID and index
 * 
 * Usage (Producer):
 * @code
 * WriteSlotRef<MyData> slot = ...;
 * slot.get().payload = 42;  // Type-safe write
 * @endcode
 * 
 * Usage (Consumer):
 * @code
 * ReadSlotRef<MyData> slot = ...;
 * int value = slot.get().payload;  // Type-safe read
 * @endcode
 * 
 * Raw Access (Advanced):
 * @code
 * auto raw_span = slot.raw_access();
 * // Manual interpretation at user's risk
 * @endcode
 * 
 * Thread Safety: SlotRef instances are not thread-safe. Each thread should use
 * its own transaction context and slot references.
 */
template <typename DataBlockT, bool IsMutable>
class SlotRef
{
  public:
    using value_type = std::conditional_t<IsMutable, DataBlockT, const DataBlockT>;
    using span_type = std::conditional_t<IsMutable, std::span<std::byte>, std::span<const std::byte>>;

    // Compile-time check: DataBlockT must be trivially copyable for shared memory
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for safe shared memory access. "
                  "Types with vtables, std::string, std::vector, etc. are not allowed.");

    // ====================================================================
    // Construction (Internal - used by TransactionContext)
    // ====================================================================

    /**
     * @brief Construct from SlotWriteHandle (producer/mutable)
     * @param handle Pointer to write handle (non-null)
     */
    explicit SlotRef(SlotWriteHandle *handle) requires IsMutable : m_write_handle(handle)
    {
        if (!handle)
        {
            throw std::invalid_argument("SlotRef: write handle cannot be null");
        }
    }

    /**
     * @brief Construct from SlotConsumeHandle (consumer/const)
     * @param handle Pointer to consume handle (non-null)
     */
    explicit SlotRef(SlotConsumeHandle *handle) requires(!IsMutable) : m_read_handle(handle)
    {
        if (!handle)
        {
            throw std::invalid_argument("SlotRef: read handle cannot be null");
        }
    }

    // Movable but not copyable (references underlying handle)
    SlotRef(SlotRef &&) noexcept = default;
    SlotRef &operator=(SlotRef &&) noexcept = default;
    SlotRef(const SlotRef &) = delete;
    SlotRef &operator=(const SlotRef &) = delete;

    // ====================================================================
    // Typed Access
    // ====================================================================

    /**
     * @brief Get typed reference to slot data
     * @return Reference to DataBlockT stored in slot
     * @throws std::runtime_error if slot size < sizeof(DataBlockT)
     * 
     * This is the primary interface for type-safe access. The returned reference
     * is valid for the lifetime of the SlotRef.
     * 
     * Size validation ensures the slot is large enough for DataBlockT.
     */
    [[nodiscard]] value_type &get()
    {
        span_type raw = raw_access();

        if (raw.size() < sizeof(DataBlockT))
        {
            throw std::runtime_error("SlotRef::get(): slot size (" + std::to_string(raw.size()) +
                                     " bytes) is smaller than sizeof(DataBlockT) (" +
                                     std::to_string(sizeof(DataBlockT)) + " bytes)");
        }

        // Cast to typed reference
        if constexpr (IsMutable)
        {
            return *reinterpret_cast<DataBlockT *>(raw.data());
        }
        else
        {
            return *reinterpret_cast<const DataBlockT *>(raw.data());
        }
    }

    /**
     * @brief Get const typed reference (available for both mutable and const slots)
     * @return Const reference to DataBlockT
     * @throws std::runtime_error if slot size < sizeof(DataBlockT)
     */
    [[nodiscard]] const DataBlockT &get() const
    {
        auto raw = raw_access();

        if (raw.size() < sizeof(DataBlockT))
        {
            throw std::runtime_error("SlotRef::get(): slot size (" + std::to_string(raw.size()) +
                                     " bytes) is smaller than sizeof(DataBlockT) (" +
                                     std::to_string(sizeof(DataBlockT)) + " bytes)");
        }

        return *reinterpret_cast<const DataBlockT *>(raw.data());
    }

    // ====================================================================
    // Raw Memory Access (Opt-In)
    // ====================================================================

    /**
     * @brief Get raw memory span for advanced usage (mutable version)
     * @return Mutable span of bytes covering the slot
     * 
     * **Use with caution**: This bypasses type safety. User is responsible for:
     * - Correct interpretation of memory layout
     * - Not exceeding span boundaries
     * - Maintaining data structure invariants
     * 
     * Only available after transaction entry validation (schema, checksums, etc.)
     */
    [[nodiscard]] std::span<std::byte> raw_access() requires IsMutable
    {
        if (!m_write_handle)
        {
            throw std::logic_error("SlotRef::raw_access(): write handle is null");
        }
        return m_write_handle->buffer_span();
    }

    /**
     * @brief Get raw memory span for advanced usage (const version)
     * @return Const span of bytes covering the slot
     */
    [[nodiscard]] std::span<const std::byte> raw_access() const
    {
        if constexpr (IsMutable)
        {
            if (!m_write_handle)
            {
                throw std::logic_error("SlotRef::raw_access(): write handle is null");
            }
            return m_write_handle->buffer_span();
        }
        else
        {
            if (!m_read_handle)
            {
                throw std::logic_error("SlotRef::raw_access(): read handle is null");
            }
            return m_read_handle->buffer_span();
        }
    }

    // ====================================================================
    // Slot Metadata
    // ====================================================================

    /**
     * @brief Get unique slot ID (monotonically increasing)
     * @return Slot ID assigned by producer
     */
    [[nodiscard]] uint64_t slot_id() const noexcept
    {
        if constexpr (IsMutable)
        {
            return m_write_handle ? m_write_handle->slot_id() : 0;
        }
        else
        {
            return m_read_handle ? m_read_handle->slot_id() : 0;
        }
    }

    /**
     * @brief Get slot index in ring buffer
     * @return Zero-based slot index
     */
    [[nodiscard]] size_t slot_index() const noexcept
    {
        if constexpr (IsMutable)
        {
            return m_write_handle ? m_write_handle->slot_index() : 0;
        }
        else
        {
            return m_read_handle ? m_read_handle->slot_index() : 0;
        }
    }

  private:
    // Storage: either write handle (producer) or read handle (consumer)
    SlotWriteHandle *m_write_handle = nullptr;     // Valid when IsMutable == true
    SlotConsumeHandle *m_read_handle = nullptr;    // Valid when IsMutable == false
};

// ====================================================================
// Convenience Type Aliases
// ====================================================================

/**
 * @brief Type alias for mutable slot reference (producer side)
 * @tparam T The data type stored in the slot
 */
template <typename T>
using WriteSlotRef = SlotRef<T, true>;

/**
 * @brief Type alias for const slot reference (consumer side)
 * @tparam T The data type stored in the slot
 */
template <typename T>
using ReadSlotRef = SlotRef<T, false>;

} // namespace pylabhub::hub
