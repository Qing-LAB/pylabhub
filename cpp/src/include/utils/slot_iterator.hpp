/**
 * @file slot_iterator.hpp
 * @brief Non-terminating iterator for slot acquisition in RAII layer
 * 
 * @copyright Copyright (c) 2024-2026 PyLabHub Project
 * 
 * Part of Phase 3: C++ RAII Layer
 * Provides non-terminating iteration over datablock slots with Result-based error handling.
 * 
 * Design Philosophy:
 * - Iterator never ends on Timeout or NoSlot (user breaks explicitly)
 * - Each iteration yields Result<SlotRef, SlotAcquireError>
 * - User checks result.is_ok() and handles errors
 * - Fatal errors (producer/consumer destroyed) end iteration
 * - Integrates with TransactionContext for lifecycle management
 */

#pragma once

#include "utils/result.hpp"
#include "utils/slot_ref.hpp"
#include "utils/transaction_context.hpp"
#include <chrono>
#include <iterator>
#include <memory>

namespace pylabhub::hub
{

/**
 * @class SlotIterator
 * @brief Non-terminating iterator for slot acquisition
 * 
 * @tparam DataBlockT The data type stored in slots
 * @tparam IsWrite true for producer (write), false for consumer (read)
 * 
 * SlotIterator implements a C++20 range-based for loop interface that:
 * 1. **Never ends on timeout/no-slot** - Returns error Result, continues iteration
 * 2. **Yields Result objects** - User must check result.is_ok()
 * 3. **Ends on fatal errors** - Producer/consumer destroyed, unrecoverable errors
 * 4. **User breaks explicitly** - Based on flexzone flags, events, application logic
 * 
 * Usage Pattern:
 * @code
 * for (auto result : ctx.slots(100ms)) {
 *     if (!result.is_ok()) {
 *         if (result.error() == SlotAcquireError::Timeout) {
 *             process_events();
 *         }
 *         if (check_shutdown_flag()) break;
 *         continue;
 *     }
 *     
 *     auto& slot = result.content();
 *     slot.get().data = produce();
 *     ctx.commit();
 * }
 * @endcode
 * 
 * Iterator Semantics:
 * - begin() returns self
 * - operator++() acquires next slot
 * - operator*() returns current Result
 * - operator==(sentinel) checks if iteration should end (fatal error only)
 * 
 * Thread Safety: Not thread-safe. Each thread uses its own context and iterator.
 */
template <typename DataBlockT, bool IsWrite>
class SlotIterator
{
  public:
    using ContextType = TransactionContext<void, DataBlockT, IsWrite>; // Will be specialized
    using SlotRefType = std::conditional_t<IsWrite, WriteSlotRef<DataBlockT>, ReadSlotRef<DataBlockT>>;
    using ResultType = Result<SlotRefType, SlotAcquireError>;
    using HandleType = std::conditional_t<IsWrite, SlotWriteHandle, SlotConsumeHandle>;

    // Iterator traits for C++20 ranges
    using iterator_category = std::input_iterator_tag;
    using value_type = ResultType;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type *;
    using reference = value_type &;

    // ====================================================================
    // Construction
    // ====================================================================

    /**
     * @brief Construct slot iterator
     * @param handle Producer or Consumer handle
     * @param timeout Timeout for each slot acquisition attempt
     * 
     * Does not acquire slot immediately - first slot acquired on first operator++
     */
    SlotIterator(std::conditional_t<IsWrite, DataBlockProducer *, DataBlockConsumer *> handle,
                 std::chrono::milliseconds timeout)
        : m_handle(handle), m_timeout(timeout), m_done(false)
    {
        // Don't acquire first slot here - will be acquired in operator++
        // This follows the pattern where begin() returns iterator pointing "before first"
    }

    // Default constructor for sentinel
    SlotIterator() : m_handle(nullptr), m_timeout(0), m_done(true) {}

    // ====================================================================
    // Iterator Interface
    // ====================================================================

    /**
     * @brief Advance to next slot (acquire next slot)
     * @return Reference to self
     * 
     * Attempts to acquire next slot with the specified timeout.
     * Updates m_current_result with:
     * - Ok(SlotRef) if slot acquired successfully
     * - Error(Timeout) if acquisition timed out (iteration continues)
     * - Error(NoSlot) if no slot available (iteration continues)
     * - Error(Error) if fatal error occurred (iteration ends)
     */
    SlotIterator &operator++()
    {
        if (m_done)
        {
            return *this;
        }

        try
        {
            acquire_next_slot();
        }
        catch (const std::exception &)
        {
            // Fatal error - end iteration
            m_current_result = ResultType::error(SlotAcquireError::Error, -1);
            m_done = true;
        }

        return *this;
    }

    /**
     * @brief Post-increment (not recommended for iterators)
     */
    SlotIterator operator++(int)
    {
        SlotIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    /**
     * @brief Dereference to get current Result
     * @return Reference to current Result<SlotRef, SlotAcquireError>
     */
    reference operator*() { return m_current_result; }

    /**
     * @brief Const dereference
     */
    reference operator*() const { return m_current_result; }

    /**
     * @brief Arrow operator (for Result methods)
     */
    pointer operator->() { return &m_current_result; }

    /**
     * @brief Compare with sentinel (end condition)
     * @return true if iteration should end (fatal error or done)
     * 
     * Only returns true for fatal errors, never for Timeout/NoSlot.
     */
    bool operator==(std::default_sentinel_t) const noexcept { return m_done; }

    /**
     * @brief Inequality with sentinel
     */
    bool operator!=(std::default_sentinel_t) const noexcept { return !m_done; }

    // ====================================================================
    // Range Interface (C++20)
    // ====================================================================

    /**
     * @brief Begin iterator (returns self after acquiring first slot)
     * @return Iterator positioned at first slot attempt
     */
    SlotIterator begin()
    {
        if (!m_done && !m_first_acquired)
        {
            ++(*this); // Acquire first slot
            m_first_acquired = true;
        }
        return *this;
    }

    /**
     * @brief End sentinel
     * @return Default sentinel (iterator ends when operator== returns true)
     */
    std::default_sentinel_t end() const noexcept { return std::default_sentinel; }

  private:
    // ====================================================================
    // Slot Acquisition
    // ====================================================================

    /**
     * @brief Acquire next slot (producer version)
     */
    void acquire_next_slot() requires IsWrite
    {
        if (!m_handle)
        {
            m_current_result = ResultType::error(SlotAcquireError::Error, -2);
            m_done = true;
            return;
        }

        // Cast timeout to ms for acquire_write_slot
        auto timeout_ms = static_cast<int>(m_timeout.count());

        // Try to acquire write slot
        auto slot_handle = m_handle->acquire_write_slot(timeout_ms);

        if (slot_handle)
        {
            // Success - wrap in SlotRef
            m_current_slot = std::move(slot_handle);
            m_current_result = ResultType::ok(SlotRefType(m_current_slot.get()));
        }
        else
        {
            // Timeout or no slot - distinguish if possible
            // For now, treat nullptr as Timeout (acquisition APIs return nullptr on timeout)
            m_current_result = ResultType::error(SlotAcquireError::Timeout);
        }
    }

    /**
     * @brief Acquire next slot (consumer version)
     */
    void acquire_next_slot() requires(!IsWrite)
    {
        if (!m_handle)
        {
            m_current_result = ResultType::error(SlotAcquireError::Error, -2);
            m_done = true;
            return;
        }

        // Cast timeout to ms for acquire_consume_slot
        auto timeout_ms = static_cast<int>(m_timeout.count());

        // Try to acquire consume slot
        auto slot_handle = m_handle->acquire_consume_slot(timeout_ms);

        if (slot_handle)
        {
            // Success - wrap in SlotRef
            m_current_slot = std::move(slot_handle);
            m_current_result = ResultType::ok(SlotRefType(m_current_slot.get()));
        }
        else
        {
            // Timeout or no slot
            m_current_result = ResultType::error(SlotAcquireError::Timeout);
        }
    }

    // ====================================================================
    // Member Variables
    // ====================================================================

    std::conditional_t<IsWrite, DataBlockProducer *, DataBlockConsumer *> m_handle;
    std::chrono::milliseconds m_timeout;
    ResultType m_current_result;
    bool m_done;
    bool m_first_acquired = false;

    // Current slot handle (owned by iterator, released on next iteration or destruction)
    std::unique_ptr<HandleType> m_current_slot;
};

} // namespace pylabhub::hub

// ====================================================================
// TransactionContext::slots() Implementation
// ====================================================================

namespace pylabhub::hub
{

template <typename FlexZoneT, typename DataBlockT, bool IsWrite>
SlotIterator<DataBlockT, IsWrite>
TransactionContext<FlexZoneT, DataBlockT, IsWrite>::slots(std::chrono::milliseconds timeout)
{
    return SlotIterator<DataBlockT, IsWrite>(m_handle, timeout);
}

} // namespace pylabhub::hub
