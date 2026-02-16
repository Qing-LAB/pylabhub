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
 * - Auto-publish on normal loop exit (break/end of range): if the iterator is destroyed without
 *   an active exception, the current slot is automatically published. If destroyed during
 *   exception propagation (stack unwinding), the slot is released without publish (abort/rollback).
 * - Automatic heartbeat update on every operator++() call (both producer and consumer),
 *   covering both successful acquires and timeout/no-slot iterations.
 * - Explicit ctx.publish() is also supported for advanced control.
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
 * 5. **Auto-publishes on normal exit** - When destroyed without active exceptions (break/end
 *    of range), the current unpublished write slot is automatically published. On exception
 *    propagation, the slot is released without publish (RAII rollback).
 * 6. **Auto-heartbeat between iterations** - operator++() updates the producer/consumer
 *    heartbeat before each slot acquisition attempt. This covers the slot-acquisition
 *    spin (timeout/retry loops). It does NOT cover time spent inside the loop body —
 *    if user code runs longer than the heartbeat stale threshold, call
 *    ctx.update_heartbeat() explicitly within the loop body.
 *
 * Usage Pattern:
 * @code
 * for (auto& result : ctx.slots(100ms)) {
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
 *     break; // auto-publish fires when iterator is destroyed at loop exit
 *     // OR call ctx.publish() explicitly for advanced control
 * }
 * @endcode
 *
 * Thread Safety: Not thread-safe. Each thread uses its own context and iterator.
 */
template <typename DataBlockT, bool IsWrite>
class SlotIterator
{
  public:
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
     * @brief Construct slot iterator for producer (write)
     * @param handle Producer handle
     * @param timeout Timeout for each slot acquisition attempt
     * @param ctx_write_slot_ptr Optional pointer to TransactionContext::m_current_write_slot.
     *        When set, enables ctx.publish() to access the current slot handle. Set by
     *        TransactionContext::slots() — do not set manually.
     */
    SlotIterator(DataBlockProducer *handle, std::chrono::milliseconds timeout,
                 SlotWriteHandle **ctx_write_slot_ptr = nullptr) requires IsWrite
        : m_handle(handle), m_timeout(timeout), m_done(false),
          m_ctx_write_slot_ptr(ctx_write_slot_ptr)
    {
    }

    /**
     * @brief Construct slot iterator for consumer (read)
     * @param handle Consumer handle
     * @param timeout Timeout for each slot acquisition attempt
     */
    SlotIterator(DataBlockConsumer *handle, std::chrono::milliseconds timeout) requires(!IsWrite)
        : m_handle(handle), m_timeout(timeout), m_done(false)
    {
    }

    // Default constructor for sentinel
    SlotIterator() : m_handle(nullptr), m_timeout(0), m_done(true) {}

    /**
     * @brief Destructor: auto-publish on normal exit for write iterators.
     *
     * If the iterator is destroyed without active exception propagation (std::uncaught_exceptions()
     * == 0), the current unpublished write slot is automatically published. This handles
     * the common pattern of breaking out of the slot loop after writing.
     *
     * If destroyed during exception propagation (stack unwinding), the slot is released
     * without publish — data is discarded (RAII rollback).
     */
    ~SlotIterator()
    {
        if constexpr (IsWrite)
        {
            if (m_current_slot && std::uncaught_exceptions() == 0)
            {
                // Auto-publish: makes the written data visible to consumers.
                // Idempotent — safe even if ctx.publish() was already called.
                (void)m_current_slot->commit(sizeof(DataBlockT));
            }
            // Clear ctx's raw pointer to prevent dangling reference after destruction
            if (m_ctx_write_slot_ptr)
            {
                *m_ctx_write_slot_ptr = nullptr;
            }
        }
        else
        {
            // Consumer: on normal exit (no exception), mark the slot as explicitly consumed so
            // last_consumed_slot_id is advanced. On exception propagation, we let the unique_ptr
            // destructor release without marking, preserving the slot for re-reading.
            if (m_current_slot && m_handle && std::uncaught_exceptions() == 0)
            {
                (void)m_handle->release_consume_slot(*m_current_slot);
                // m_current_slot's pImpl->released=true; destructor below will no-op.
            }
        }
        // m_current_slot unique_ptr destructor handles the actual release.
        // For write: release_write_handle() checks the committed flag to determine data visibility.
        // For read: release_consume_handle() is called (no-op if already released above).
    }

    // Move constructor: nullify source's ctx pointer to prevent double-clear
    SlotIterator(SlotIterator &&other) noexcept
        : m_handle(other.m_handle),
          m_timeout(other.m_timeout),
          m_current_result(std::move(other.m_current_result)),
          m_done(other.m_done),
          m_first_acquired(other.m_first_acquired),
          m_current_slot(std::move(other.m_current_slot))
    {
        if constexpr (IsWrite)
        {
            m_ctx_write_slot_ptr = other.m_ctx_write_slot_ptr;
            other.m_ctx_write_slot_ptr = nullptr; // Prevent moved-from dtor from clearing ctx ptr
        }
        other.m_done = true; // Mark moved-from as done (safe sentinel state)
    }

    SlotIterator &operator=(SlotIterator &&other) noexcept
    {
        if (this != &other)
        {
            // Auto-publish current slot if we're being replaced (same logic as destructor)
            if constexpr (IsWrite)
            {
                if (m_current_slot && std::uncaught_exceptions() == 0)
                {
                    (void)m_current_slot->commit(sizeof(DataBlockT));
                }
                if (m_ctx_write_slot_ptr)
                {
                    *m_ctx_write_slot_ptr = nullptr;
                }
            }
            else
            {
                if (m_current_slot && m_handle && std::uncaught_exceptions() == 0)
                {
                    (void)m_handle->release_consume_slot(*m_current_slot);
                }
            }

            m_handle = other.m_handle;
            m_timeout = other.m_timeout;
            m_current_result = std::move(other.m_current_result);
            m_done = other.m_done;
            m_first_acquired = other.m_first_acquired;
            m_current_slot = std::move(other.m_current_slot);

            if constexpr (IsWrite)
            {
                m_ctx_write_slot_ptr = other.m_ctx_write_slot_ptr;
                other.m_ctx_write_slot_ptr = nullptr;
            }
            other.m_done = true;
        }
        return *this;
    }

    SlotIterator(const SlotIterator &) = delete;
    SlotIterator &operator=(const SlotIterator &) = delete;

    // ====================================================================
    // Iterator Interface
    // ====================================================================

    /**
     * @brief Advance to next slot (acquire next slot)
     * @return Reference to self
     *
     * Attempts to acquire next slot with the specified timeout.
     * If a current write slot is held and has NOT been published (neither via ctx.publish()
     * nor auto-publish), advancing to the next slot releases the old slot without publish
     * (data is aborted/discarded). This is the intended behavior — call ctx.publish()
     * or rely on auto-publish (break) to make data visible.
     *
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

        // Auto-heartbeat: update liveness signal between iterations (at start of each ++).
        // Covers slot-acquisition spin (timeout/retry). User code inside the loop body is
        // NOT covered — call ctx.update_heartbeat() if the body may run longer than the stale threshold.
        if (m_handle)
        {
            m_handle->update_heartbeat();
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
     * @brief Post-increment (advance only; move-only type cannot return prior state)
     */
    void operator++(int) { ++(*this); }

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
     * @note SlotIterator is move-only; begin() moves from *this. After calling begin(),
     *       the original object is in a moved-from state. Range-for handles this correctly.
     */
    SlotIterator begin()
    {
        if (!m_done && !m_first_acquired)
        {
            ++(*this); // Acquire first slot
            m_first_acquired = true;
        }
        return std::move(*this);
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
     *
     * Clears ctx's raw slot pointer before acquiring (old slot is about to be replaced).
     * After successful acquisition, sets ctx's pointer to the new slot handle.
     */
    void acquire_next_slot() requires IsWrite
    {
        if (!m_handle)
        {
            m_current_result = ResultType::error(SlotAcquireError::Error, -2);
            m_done = true;
            return;
        }

        // Clear ctx's raw pointer before replacing the current slot.
        // Old slot will be released (with or without commit) when m_current_slot is replaced.
        if (m_ctx_write_slot_ptr)
        {
            *m_ctx_write_slot_ptr = nullptr;
        }

        // Cast timeout to ms for acquire_write_slot
        auto timeout_ms = static_cast<int>(m_timeout.count());

        // Try to acquire write slot
        auto slot_handle = m_handle->acquire_write_slot(timeout_ms);

        if (slot_handle)
        {
            // Success - wrap in SlotRef and notify ctx
            m_current_slot = std::move(slot_handle);
            if (m_ctx_write_slot_ptr)
            {
                *m_ctx_write_slot_ptr = m_current_slot.get();
            }
            m_current_result = ResultType::ok(SlotRefType(m_current_slot.get()));
        }
        else
        {
            // Timeout or no slot — no change to m_current_slot (old slot still held)
            m_current_result = ResultType::error(SlotAcquireError::Timeout);
        }
    }

    /**
     * @brief Acquire next slot (consumer version)
     *
     * Explicitly releases the previous slot via release_consume_slot() before acquiring the next.
     * This updates last_consumed_slot_id (Latest_only) so the same slot is not re-read on the
     * next with_transaction call. On exception paths the iterator is destroyed directly (not via
     * operator++) and the slot is released without marking, preserving it for exception recovery.
     */
    void acquire_next_slot() requires(!IsWrite)
    {
        if (!m_handle)
        {
            m_current_result = ResultType::error(SlotAcquireError::Error, -2);
            m_done = true;
            return;
        }

        // Explicitly consume and release the previous slot before acquiring the next.
        // This marks last_consumed_slot_id so Latest_only consumers don't re-read the same slot.
        if (m_current_slot)
        {
            (void)m_handle->release_consume_slot(*m_current_slot);
            m_current_slot.reset(); // Clear stale handle; destructor will no-op (already released)
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
            // Timeout or no slot; m_current_slot is already null from reset() above
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

    // Pointer-to-pointer into TransactionContext::m_current_write_slot (non-owning).
    // Enables ctx.publish() to access the current slot handle. Set by TransactionContext::slots().
    // Only meaningful when IsWrite=true; always nullptr for consumer.
    SlotWriteHandle **m_ctx_write_slot_ptr = nullptr;

    // Current slot handle (owned by iterator; released on next iteration or destruction)
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
    if constexpr (IsWrite)
    {
        // Pass pointer to m_current_write_slot so ctx.publish() can access the current slot
        return SlotIterator<DataBlockT, IsWrite>(m_handle, timeout, &m_current_write_slot);
    }
    else
    {
        return SlotIterator<DataBlockT, IsWrite>(m_handle, timeout);
    }
}

} // namespace pylabhub::hub
