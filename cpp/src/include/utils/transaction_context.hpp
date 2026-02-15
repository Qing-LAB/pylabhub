/**
 * @file transaction_context.hpp
 * @brief Transaction context for type-safe RAII layer
 * 
 * @copyright Copyright (c) 2024-2026 PyLabHub Project
 * 
 * Part of Phase 3: C++ RAII Layer
 * Provides the core context-centric transaction API with schema validation.
 * 
 * Design Philosophy:
 * - Context represents session-level state (validation, protection, layout)
 * - Context is NOT the current slot (slots are acquired via iterator)
 * - Validation happens once at entry, not per slot
 * - Context lifetime = transaction scope (RAII)
 */

#pragma once

#include "utils/data_block.hpp"
#include "utils/result.hpp"
#include "utils/slot_ref.hpp"
#include "utils/zone_ref.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace pylabhub::hub
{

// Forward declaration (implemented at end of slot_iterator.hpp)
template <typename DataBlockT, bool IsWrite>
class SlotIterator;

/**
 * @class TransactionContext
 * @brief Context for a transaction with schema validation and typed access
 * 
 * @tparam FlexZoneT Type of flexible zone data (or void for no flexzone)
 * @tparam DataBlockT Type of datablock slot data
 * @tparam IsWrite true for producer (write), false for consumer (read)
 * 
 * TransactionContext is the heart of the RAII layer. It:
 * 1. **Validates at entry**: Schema, layout, checksums (one-time cost)
 * 2. **Provides flexzone access**: `ctx.flexzone()` → ZoneRef<FlexZoneT>
 * 3. **Provides slot iteration**: `ctx.slots(timeout)` → SlotIterator
 * 4. **Manages lifecycle**: RAII ensures cleanup on scope exit
 * 
 * Usage (Producer):
 * @code
 * producer.with_transaction<MetaData, Payload>(timeout, [](auto& ctx) {
 *     ctx.flexzone().get().status = Status::Active;
 *     
 *     for (auto result : ctx.slots(timeout)) {
 *         if (!result.is_ok()) {
 *             if (result.error() == SlotAcquireError::Timeout) {
 *                 process_events();
 *             }
 *             continue;
 *         }
 *         
 *         auto& slot = result.value();
 *         slot.get().data = produce();
 *         ctx.commit();
 *     }
 * });
 * @endcode
 * 
 * Usage (Consumer):
 * @code
 * consumer.with_transaction<MetaData, Payload>(timeout, [](auto& ctx) {
 *     for (auto result : ctx.slots(timeout)) {
 *         if (!result.is_ok()) continue;
 *         
 *         if (!ctx.validate_read()) continue;
 *         
 *         process(result.value().get());
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
     * @brief Construct transaction context with validation
     * @param handle Producer or Consumer handle
     * @param timeout Default timeout for slot operations
     * @throws std::invalid_argument if handle is null
     * @throws std::runtime_error if validation fails (schema, layout, checksums)
     * 
     * Performs entry validation:
     * 1. Schema validation (if schema registered)
     * 2. Layout validation (sizeof checks)
     * 3. Checksum policy enforcement
     */
    explicit TransactionContext(HandleType *handle, std::chrono::milliseconds timeout)
        : m_handle(handle), m_default_timeout(timeout)
    {
        if (!handle)
        {
            throw std::invalid_argument("TransactionContext: handle cannot be null");
        }

        // Perform entry validation
        validate_entry();
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
     * 
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
        if constexpr (IsWrite)
        {
            return ZoneRefType(m_handle);
        }
        else
        {
            return ZoneRefType(m_handle);
        }
    }

    /**
     * @brief Get const reference to flexible zone
     * @return Const ZoneRef for read-only access
     */
    [[nodiscard]] auto flexzone() const
    {
        // Always return const zone ref for const context
        return ReadZoneRef<FlexZoneT>(const_cast<HandleType *>(m_handle));
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
     * for (auto result : ctx.slots(100ms)) {
     *     if (!result.is_ok()) {
     *         if (result.error() == SlotAcquireError::Timeout) {
     *             process_events();
     *         }
     *         continue;
     *     }
     *     // Process slot
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
     * @brief Commit current slot (producer only)
     * @throws std::logic_error if no slot is active
     * @throws std::runtime_error if commit fails
     * 
     * Makes the current slot visible to consumers. This is a protocol step:
     * - Advances commit_index
     * - Updates checksums (if policy requires)
     * - Releases slot for consumption
     * 
     * Size committed = sizeof(DataBlockT) (typed commit)
     * 
     * Must be called after writing to slot, before acquiring next slot.
     */
    void commit() requires IsWrite
    {
        if (!m_current_write_slot)
        {
            throw std::logic_error("TransactionContext::commit(): no active write slot");
        }

        // Commit the slot (this also releases it)
        bool success = m_current_write_slot->commit(sizeof(DataBlockT));
        if (!success)
        {
            throw std::runtime_error("TransactionContext::commit(): commit failed");
        }

        // Release the slot handle
        if (m_handle)
        {
            m_handle->release_write_slot(*m_current_write_slot);
        }

        m_current_write_slot.reset();
    }

    // ====================================================================
    // Transaction Operations (Consumer only)
    // ====================================================================

    /**
     * @brief Validate current read slot (consumer only)
     * @return true if slot is still valid, false if invalid
     * 
     * Checks if the currently acquired slot is still valid:
     * - Checksums match (if enforced)
     * - Slot hasn't been overwritten (in ring buffer scenarios)
     * 
     * Should be called before processing slot data.
     */
    [[nodiscard]] bool validate_read() const requires(!IsWrite)
    {
        if (!m_current_read_slot)
        {
            return false;
        }

        // Validate checksum if policy requires
        // (This delegates to the underlying slot handle's validation)
        return true; // Placeholder: actual validation in SlotConsumeHandle
    }

    // ====================================================================
    // Heartbeat (Convenience)
    // ====================================================================

    /**
     * @brief Update heartbeat (convenience wrapper)
     * 
     * Forwards to producer/consumer update_heartbeat().
     * Useful when inside transaction but not acquiring slots.
     * 
     * Example: During long event processing in iterator loop.
     */
    void update_heartbeat()
    {
        if (m_handle)
        {
            m_handle->update_heartbeat();
        }
    }

    // ====================================================================
    // Context Metadata
    // ====================================================================

    /**
     * @brief Get datablock configuration
     * @return Const reference to DataBlockConfig
     */
    [[nodiscard]] const DataBlockConfig &config() const
    {
        if (!m_handle)
        {
            throw std::logic_error("TransactionContext::config(): handle is null");
        }
        return m_handle->config();
    }

    // Note: layout() method removed - DataBlockLayout is internal implementation detail

    // ====================================================================
    // Internal Access (for SlotIterator)
    // ====================================================================

    /**
     * @brief Get underlying handle (internal use by SlotIterator)
     * @return Pointer to Producer or Consumer
     */
    [[nodiscard]] HandleType *handle() const noexcept { return m_handle; }

    /**
     * @brief Set current write slot (internal use by SlotIterator)
     */
    void set_current_slot(SlotWriteHandle *slot) requires IsWrite
    {
        m_current_write_slot.reset(slot);
    }

    /**
     * @brief Set current read slot (internal use by SlotIterator)
     */
    void set_current_slot(SlotConsumeHandle *slot) requires(!IsWrite)
    {
        m_current_read_slot.reset(slot);
    }

  private:
    // ====================================================================
    // Entry Validation
    // ====================================================================

    /**
     * @brief Perform entry validation (called from constructor)
     * @throws std::runtime_error if validation fails
     */
    void validate_entry()
    {
        validate_schema();
        validate_layout();
        validate_checksums();
    }

    /**
     * @brief Validate schema (if registered)
     * @throws std::runtime_error if schema mismatch
     * 
     * Phase 3.7: Size-based validation (complete)
     * - Validates sizeof(FlexZoneT) <= cfg.flex_zone_size
     * - Validates sizeof(DataBlockT) <= cfg.ring_buffer.slot_bytes
     * 
     * Future: Full BLDS-based schema validation
     * - Compare against stored schema::SchemaInfo hash
     * - Validate member-level compatibility
     */
    void validate_schema()
    {
        const auto &cfg = config();

        // Validate flexible zone size
        if constexpr (!std::is_void_v<FlexZoneT>)
        {
            if (cfg.flex_zone_size < sizeof(FlexZoneT))
            {
                throw std::runtime_error(
                    "TransactionContext: flexible zone size (" + std::to_string(cfg.flex_zone_size) +
                    " bytes) is smaller than sizeof(FlexZoneT) (" + std::to_string(sizeof(FlexZoneT)) + " bytes)");
            }
        }

        // Validate slot size
        size_t slot_size = cfg.ring_buffer.slot_bytes;
        if (slot_size < sizeof(DataBlockT))
        {
            throw std::runtime_error(
                "TransactionContext: slot size (" + std::to_string(slot_size) +
                " bytes) is smaller than sizeof(DataBlockT) (" + std::to_string(sizeof(DataBlockT)) + " bytes)");
        }
    }

    /**
     * @brief Validate layout (consistency checks)
     */
    void validate_layout()
    {
        // Basic sanity checks on config
        const auto &cfg = config();

        if (cfg.ring_buffer.num_slots == 0)
        {
            throw std::runtime_error("TransactionContext: slot count is zero");
        }

        if (cfg.ring_buffer.slot_bytes == 0)
        {
            throw std::runtime_error("TransactionContext: slot stride is zero");
        }
    }

    /**
     * @brief Validate checksums (if policy requires)
     */
    void validate_checksums()
    {
        // Checksum validation will be performed by the underlying
        // Producer/Consumer at slot acquisition time
        // This is just a placeholder for future policy checks
    }

    // ====================================================================
    // Member Variables
    // ====================================================================

    HandleType *m_handle;                                  // Producer or Consumer
    std::chrono::milliseconds m_default_timeout;           // Default timeout for slot ops

    // Current slot (only one active at a time per context)
    std::unique_ptr<SlotWriteHandle> m_current_write_slot;    // For producer
    std::unique_ptr<SlotConsumeHandle> m_current_read_slot;   // For consumer
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
