/**
 * @file transaction_context.hpp
 * @brief Transaction context for type-safe RAII layer
 *
 * @copyright Copyright (c) 2024-2026 PyLabHub Project
 *
 * Part of Phase 3: C++ RAII Layer
 * Provides the core context-centric transaction API.
 *
 * Design Philosophy:
 * - Context represents session-level state (typed access, slot iteration, lifecycle)
 * - Context is NOT the current slot (slots are acquired via iterator)
 * - Validation is performed once at creation/attach time (template factory functions),
 *   not repeated per transaction — the type system enforces correctness.
 * - Context lifetime = transaction scope (RAII)
 */

#pragma once

#include "utils/data_block.hpp"
#include "utils/result.hpp"
#include "utils/slot_ref.hpp"
#include "utils/zone_ref.hpp"
#include <chrono>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace pylabhub::hub
{

// Forward declaration (implemented at end of slot_iterator.hpp)
template <typename DataBlockT, bool IsWrite>
class SlotIterator;

/**
 * @class TransactionContext
 * @brief Context for a type-safe transaction with typed flexzone and slot access
 *
 * @tparam FlexZoneT Type of flexible zone data (or void for no flexzone)
 * @tparam DataBlockT Type of datablock slot data
 * @tparam IsWrite true for producer (write), false for consumer (read)
 *
 * TransactionContext is the primary public interface of the RAII layer. It:
 * 1. **Provides flexzone access**: `ctx.flexzone()` → ZoneRef<FlexZoneT>
 * 2. **Provides slot iteration**: `ctx.slots(timeout)` → SlotIterator
 * 3. **Manages lifecycle**: RAII ensures slot cleanup on scope exit
 *
 * Schema and layout correctness are guaranteed by the template factory functions
 * (`create_datablock_producer<F,D>`, `find_datablock_consumer<F,D>`) which
 * perform schema hash validation at creation/attach time. There is no redundant
 * runtime re-validation inside transactions.
 *
 * Usage (Producer):
 * @code
 * producer.with_transaction<MetaData, Payload>(timeout, [](auto& ctx) {
 *     ctx.flexzone().get().status = Status::Active;
 *
 *     for (auto& result : ctx.slots(timeout)) {
 *         if (!result.is_ok()) {
 *             if (result.error() == SlotAcquireError::Timeout) {
 *                 process_events();
 *             }
 *             continue;
 *         }
 *
 *         auto& slot = result.content();
 *         slot.get().data = produce();
 *     }
 * });
 * @endcode
 *
 * Usage (Consumer):
 * @code
 * consumer.with_transaction<MetaData, Payload>(timeout, [](auto& ctx) {
 *     for (auto& result : ctx.slots(timeout)) {
 *         if (!result.is_ok()) continue;
 *
 *         process(result.content().get());
 *     }
 * });
 * @endcode
 *
 * Thread Safety: TransactionContext is not thread-safe. Each thread should
 * create its own transaction context. The underlying Producer/Consumer are
 * thread-safe (internal mutex).
 */
template <typename FlexZoneT, typename DataBlockT, bool IsWrite>
class TransactionContext
{
  public:
    // Type aliases for clarity
    using ZoneRefType = std::conditional_t<IsWrite, WriteZoneRef<FlexZoneT>, ReadZoneRef<FlexZoneT>>;
    using SlotRefType = std::conditional_t<IsWrite, WriteSlotRef<DataBlockT>, ReadSlotRef<DataBlockT>>;
    using HandleType = std::conditional_t<IsWrite, DataBlockProducer, DataBlockConsumer>;

    // Compile-time checks
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory (or void for no flexzone)");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");

    // ====================================================================
    // Construction (Internal - called by with_transaction)
    // ====================================================================

    /**
     * @brief Construct transaction context
     * @param handle Producer or Consumer handle (must not be null)
     * @param timeout Default timeout for slot operations
     * @throws std::invalid_argument if handle is null
     */
    explicit TransactionContext(HandleType *handle, std::chrono::milliseconds timeout)
        : m_handle(handle), m_default_timeout(timeout)
    {
        if (!handle)
        {
            throw std::invalid_argument("TransactionContext: handle cannot be null");
        }
    }

    // Non-copyable, movable
    TransactionContext(const TransactionContext &) = delete;
    TransactionContext &operator=(const TransactionContext &) = delete;
    TransactionContext(TransactionContext &&) noexcept = default;
    TransactionContext &operator=(TransactionContext &&) noexcept = default;

    // ====================================================================
    // Flexible Zone Access
    // ====================================================================

    /**
     * @brief Get reference to flexible zone
     * @return ZoneRef for typed or raw access to flexible zone
     *
     * Returns WriteZoneRef (producer) or ReadZoneRef (consumer).
     * For FlexZoneT=void, the returned ZoneRef provides only raw_access().
     * For typed FlexZoneT, use zone.get() for type-safe access.
     *
     * Example:
     * @code
     * auto zone = ctx.flexzone();
     * zone.get().status = Status::Active;  // Type-safe
     * @endcode
     */
    [[nodiscard]] ZoneRefType flexzone()
    {
        return ZoneRefType(m_handle);
    }

    /**
     * @brief Get const reference to flexible zone (always read-only)
     */
    [[nodiscard]] ReadZoneRef<FlexZoneT> flexzone() const
    {
        return ReadZoneRef<FlexZoneT>(m_handle);
    }

    // ====================================================================
    // Slot Iteration
    // ====================================================================

    /**
     * @brief Get slot iterator for this transaction
     * @param timeout Timeout for each slot acquisition attempt
     * @return SlotIterator yielding Result<SlotRef, SlotAcquireError>
     *
     * Returns a non-terminating iterator that yields Result objects.
     * User must check result.is_ok() and handle timeout/error cases.
     *
     * Iterator continues until:
     * - Fatal error (producer/consumer destroyed)
     * - User breaks explicitly (based on flexzone flags, events, etc.)
     *
     * Example:
     * @code
     * for (auto& result : ctx.slots(100ms)) {
     *     if (!result.is_ok()) {
     *         if (result.error() == SlotAcquireError::Timeout) {
     *             process_events();
     *         }
     *         continue;
     *     }
     *     auto& slot = result.content();
     *     slot.get().value = compute();
     * }
     * @endcode
     */
    [[nodiscard]] SlotIterator<DataBlockT, IsWrite> slots(std::chrono::milliseconds timeout);

    /**
     * @brief Get slot iterator with default timeout
     * @return SlotIterator with the timeout specified at context creation
     */
    [[nodiscard]] SlotIterator<DataBlockT, IsWrite> slots()
    {
        return slots(m_default_timeout);
    }

    // ====================================================================
    // Transaction Operations (Producer only)
    // ====================================================================

    /**
     * @brief Publish current slot (producer only)
     * @throws std::logic_error if no slot is active
     * @throws std::runtime_error if publish fails
     *
     * Makes the current slot visible to consumers: marks committed, updates checksums,
     * advances commit_index, releases write lock. Size committed = sizeof(DataBlockT).
     *
     * This is an explicit control path for advanced use. Most callers can rely on the
     * auto-publish behavior: when the SlotIterator exits normally (break or end of range),
     * the current slot is automatically published. An exception in the loop body triggers
     * automatic rollback (slot released without publish).
     *
     * ctx.publish() and auto-publish are both safe to use; publish() is idempotent.
     */
    void publish() requires IsWrite
    {
        if (!m_current_write_slot)
        {
            throw std::logic_error("TransactionContext::publish(): no active write slot");
        }

        bool success = m_current_write_slot->commit(sizeof(DataBlockT));
        if (!success)
        {
            throw std::runtime_error("TransactionContext::publish(): publish failed");
        }

        if (m_handle)
        {
            m_handle->release_write_slot(*m_current_write_slot);
        }

        // Clear the raw pointer — slot is released. SlotIterator's unique_ptr still holds
        // the handle but release_write_handle() will detect impl.released==true and no-op.
        m_current_write_slot = nullptr;
    }

    // ====================================================================
    // Flexible Zone Checksum Control (Producer only)
    // ====================================================================

    /**
     * @brief Immediately update the flexzone checksum (producer only)
     *
     * Computes and stores the BLAKE2b checksum of the flexible zone right now,
     * under the producer mutex. Use this when you want explicit control over when
     * the checksum is updated rather than relying on the auto-update at
     * with_transaction exit.
     *
     * No-op if FlexZoneT is void or the handle is null.
     */
    void publish_flexzone() requires IsWrite
    {
        if constexpr (!std::is_void_v<FlexZoneT>)
        {
            if (m_handle)
            {
                (void)m_handle->update_checksum_flexible_zone();
            }
        }
    }

    /**
     * @brief Suppress the automatic flexzone checksum update at with_transaction exit.
     *
     * By default, with_transaction updates the flexzone checksum on normal (non-exception)
     * exit. Call this to opt out — useful when you did not modify the flexzone content
     * and want to avoid an unnecessary checksum recomputation, or when you want to
     * leave the existing checksum deliberately unchanged.
     *
     * Has no effect when called during exception propagation (auto-update is already
     * suppressed on the exception path).
     */
    void suppress_flexzone_checksum() requires IsWrite
    {
        m_suppress_flexzone_checksum = true;
    }

    /**
     * @brief Returns true if flexzone checksum auto-update is suppressed.
     * Called by with_transaction after the lambda returns to decide whether to update.
     */
    [[nodiscard]] bool is_flexzone_checksum_suppressed() const noexcept requires IsWrite
    {
        return m_suppress_flexzone_checksum;
    }

    // ====================================================================
    // Heartbeat (Convenience)
    // ====================================================================

    /**
     * @brief Update heartbeat (convenience wrapper)
     *
     * Forwards to producer/consumer update_heartbeat().
     * Useful when inside a long-running transaction loop without slot activity.
     */
    void update_heartbeat()
    {
        if (m_handle)
        {
            m_handle->update_heartbeat();
        }
    }

    // ====================================================================
    // Internal Access (for SlotIterator)
    // ====================================================================

    /**
     * @brief Get underlying handle (internal use by SlotIterator)
     */
    [[nodiscard]] HandleType *handle() const noexcept { return m_handle; }

  private:
    HandleType *m_handle;                         // Producer or Consumer
    std::chrono::milliseconds m_default_timeout;  // Default timeout for slot ops

    // Non-owning pointer to the current write slot (owned by SlotIterator).
    // Set by SlotIterator via the pointer-to-pointer mechanism in slots().
    // Cleared by publish() after release, and by SlotIterator on destruction.
    SlotWriteHandle *m_current_write_slot = nullptr; // For producer; null for consumer

    // When true, with_transaction will not auto-update the flexzone checksum on exit.
    // Set via suppress_flexzone_checksum(). Only meaningful for IsWrite=true.
    bool m_suppress_flexzone_checksum = false;
};

// ====================================================================
// Convenience Type Aliases
// ====================================================================

/**
 * @brief Transaction context for producer (write access)
 * @tparam FlexZoneT Type of flexible zone (or void)
 * @tparam DataBlockT Type of datablock slot
 */
template <typename FlexZoneT, typename DataBlockT>
using WriteTransactionContext = TransactionContext<FlexZoneT, DataBlockT, true>;

/**
 * @brief Transaction context for consumer (read access)
 * @tparam FlexZoneT Type of flexible zone (or void)
 * @tparam DataBlockT Type of datablock slot
 */
template <typename FlexZoneT, typename DataBlockT>
using ReadTransactionContext = TransactionContext<FlexZoneT, DataBlockT, false>;

} // namespace pylabhub::hub
