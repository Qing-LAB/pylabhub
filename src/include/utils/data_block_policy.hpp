#pragma once
/**
 * @file data_block_policy.hpp
 * @brief Layer 1 policy enums for DataBlock behavior contracts.
 *
 * Contains all enum types that govern how a DataBlock segment is created,
 * accessed, synchronized, and checksummed. These are pure enum definitions
 * with no dependencies beyond <cstdint>.
 *
 * Also provides parse_consumer_sync_policy() — a shared inline parser used by
 * ProducerConfig, ProcessorConfig, and ConsumerConfig to avoid duplication.
 *
 * Included automatically by data_block_config.hpp and data_block.hpp.
 * Use data_block_policy.hpp directly only when policy types are needed
 * without the full DataBlockConfig or DataBlock implementation.
 *
 * See docs/HEP/HEP-CORE-0002-DataHub-FINAL.md and
 *     docs/HEP/HEP-CORE-0008-LoopPolicy-and-IterationMetrics.md.
 */
#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub::hub
{

// ============================================================================
// Buffer Management Policy
// ============================================================================

/**
 * @enum DataBlockPolicy
 * @brief Defines the buffer management strategy for a DataBlock segment.
 *
 * **Where set:** DataBlockConfig::policy; passed to create_datablock_producer().
 * **Where stored:** SharedMemoryHeader::policy (immutable after creation).
 * **Where checked:** data_block.cpp — controls whether the writer advances past
 *   the read index (RingBuffer) or blocks when all slots are consumed (Single/Double).
 *
 * | Value        | Slot count | Overwrite behaviour                                |
 * |--------------|------------|----------------------------------------------------|
 * | Single       | exactly 1  | Writer blocks until the reader finishes; simplest  |
 * |              |            | synchronisation contract. Matches ConsumerSyncPolicy|
 * |              |            | Sequential.                                     |
 * | DoubleBuffer | exactly 2  | Front/back swap; writer alternates freely; reader  |
 * |              |            | always receives the latest committed slot.         |
 * | RingBuffer   | N ≥ 3      | FIFO ring; overwrite policy determined by          |
 * |              |            | ConsumerSyncPolicy (Latest_only = lossy streaming; |
 * |              |            | Sequential / Sequential_sync = ordered, blocking).  |
 * | Unset        | —          | Sentinel — must never be stored in the SHM header. |
 *
 * **Design doc:** HEP-CORE-0002-DataHub-FINAL.md §3.1
 */
enum class DataBlockPolicy : uint32_t
{
    Single       = 0,
    DoubleBuffer = 1,
    RingBuffer   = 2,
    Unset        = 255 ///< Sentinel: must not be stored in header
};

// ============================================================================
// Consumer Synchronization Policy
// ============================================================================

/**
 * @enum ConsumerSyncPolicy
 * @brief How readers advance through slots and when the writer may overwrite.
 *
 * **Where set:** DataBlockConfig::consumer_sync_policy; passed to create_datablock_producer().
 * **Where stored:** SharedMemoryHeader::consumer_sync_policy (immutable after creation).
 * **Where checked:** data_block.cpp — acquire_consume_slot() reads this to decide
 *   whether to advance a per-consumer index or a shared global read_index, and
 *   acquire_write_slot() uses it to determine when (and whether) to block.
 *
 * | Value         | Consumers | Read order | Writer blocks when…          |
 * |---------------|-----------|------------|-------------------------------|
 * | Latest_only   | Any       | Skip-ahead | Never — writer overwrites     |
 * |               |           | (newest)   | freely; reader may miss slots |
 * | Sequential      | Exactly 1 | Sequential | Ring is full: write_index −   |
 * |               |           |            | read_index ≥ capacity         |
 * | Sequential_sync | Multiple  | Sequential | Slowest consumer has not yet  |
 * |               |           | (per-cons) | consumed the oldest slot      |
 * | Unset         | —         | —          | Sentinel: must not be stored  |
 *
 * **Design doc:** HEP-CORE-0002-DataHub-FINAL.md §3.2
 */
enum class ConsumerSyncPolicy : uint32_t
{
    Latest_only   = 0, ///< Writer overwrites freely; reader jumps to newest committed slot
    Sequential        = 1, ///< Ordered, single consumer; writer blocks when ring is full
    Sequential_sync   = 2, ///< Ordered, multiple consumers; writer blocks on the slowest reader
    Unset         = 255 ///< Sentinel: must not be stored in header
};

// ============================================================================
// Open Mode
// ============================================================================

/**
 * @enum DataBlockOpenMode
 * @brief Role and ownership contract when creating or attaching a DataBlock segment.
 *
 * **Where set:** Passed explicitly to create_datablock_producer() (→ Create) and
 *   find_datablock_consumer() (→ ReadAttach). WriteAttach is used for secondary
 *   producers that share an already-created segment.
 * **Where checked:** data_block.cpp DataBlockImpl constructor — determines whether
 *   to call shm_open(O_CREAT), ftruncate, mmap(PROT_WRITE), or mmap(PROT_READ).
 *
 * | Value       | Creates SHM? | Writes? | Unlinks on dtor? | Typical caller         |
 * |-------------|-------------|---------|-------------------|------------------------|
 * | Create      | Yes          | Yes     | Yes               | Primary producer / hub |
 * | WriteAttach | No (attaches)| Yes     | No                | Secondary producer     |
 * | ReadAttach  | No (attaches)| No      | No                | Consumer               |
 *
 * **Design doc:** HEP-CORE-0002-DataHub-FINAL.md §2.1
 */
enum class DataBlockOpenMode : uint8_t
{
    Create,      ///< Allocate + initialise SHM; unlinks on destruction (primary producer)
    WriteAttach, ///< Attach read-write to existing segment; no init, no unlink
    ReadAttach   ///< Attach read-only to existing segment; no init, no unlink
};

// ============================================================================
// Checksum Policy
// ============================================================================

/**
 * @enum ChecksumType
 * @brief Hash algorithm used for DataBlock slot and flexible-zone checksums.
 *
 * **Where set:** DataBlockConfig::checksum_type (default: BLAKE2b). Written to
 *   SharedMemoryHeader::checksum_type at segment creation; immutable afterwards.
 * **Where checked:** data_block.cpp — update_checksum_* and verify_checksum_* select
 *   the hash function. Currently only BLAKE2b is implemented; Unset is a sentinel.
 *
 * Checksum storage space is always reserved in the SHM layout regardless of
 * ChecksumPolicy, so a segment can switch from None to Enforced without reformatting.
 *
 * Implementation: BLAKE2b-256 via libsodium crypto_generichash_blake2b().
 *
 * **Design doc:** HEP-CORE-0002-DataHub-FINAL.md §4.3
 */
enum class ChecksumType
{
    BLAKE2b = 0,   ///< BLAKE2b-256 via libsodium (only value currently implemented)
    Unset   = 255  ///< Sentinel: must not be stored in header
};

/**
 * @enum ChecksumPolicy
 * @brief When DataBlock slot checksums are computed and verified.
 *
 * **Where set:** DataBlockConfig::checksum_policy (default: Enforced). Immutable
 *   after segment creation.
 * **Where checked:** data_block.cpp — release_write_slot() and release_consume_slot()
 *   read this to decide whether to call update/verify automatically.
 * **Actor override:** ValidationPolicy::Checksum (actor_config.hpp) overrides this
 *   at the actor-script layer: "update" maps to Manual, "enforce" maps to Enforced,
 *   "none" maps to None for the specific actor role.
 *
 * | Value    | Producer (write release)       | Consumer (consume release)          |
 * |----------|-------------------------------|--------------------------------------|
 * | None     | No checksum call               | No checksum call                     |
 * | Manual   | Caller calls update_checksum_* | Caller calls verify_checksum_*       |
 * | Enforced | Auto-update on slot commit     | Auto-verify; error if mismatch       |
 *
 * **Design doc:** HEP-CORE-0002-DataHub-FINAL.md §4.3
 */
enum class ChecksumPolicy
{
    None,    ///< Checksum storage present but not used — no compute, no verify
    Manual,  ///< Caller explicitly calls update_checksum_* / verify_checksum_*
    Enforced ///< Auto-update on release_write_slot; auto-verify on release_consume_slot
};

// ============================================================================
// Loop Timing Policy (HEP-CORE-0008)
// ============================================================================

/**
 * @enum LoopPolicy
 * @brief Slot-acquisition pacing strategy in a write or read loop.
 *
 * This is the **DataBlock layer** policy — distinct from RoleConfig::LoopTimingPolicy
 * (actor_config.hpp), which is the **actor layer** policy for deadline advancement.
 * Both policies can be active simultaneously on the same actor role.
 *
 * **Where set:**
 *   - DataBlockProducer::set_loop_policy() / DataBlockConsumer::set_loop_policy()
 *   - Actor layer: parsed from RoleConfig::loop_policy
 *     (JSON: `"loop_policy": "max_rate"` | `"fixed_rate"`; default: max_rate)
 *     and applied in actor_host.cpp before the loop starts.
 * **Where checked:** data_block.cpp — acquire_write_slot() and acquire_consume_slot()
 *   apply the sleep and update ContextMetrics::overrun_count.
 * **Where read back:** TransactionContext::metrics() (pass-through to DataBlockImpl).
 *
 * | Value        | Behaviour                                                   |
 * |--------------|-------------------------------------------------------------|
 * | MaxRate      | No sleep. Returns immediately when a slot is available.     |
 * |              | Runs at the maximum rate the SHM ring can sustain.         |
 * |              | Default for high-throughput sensor capture.                |
 * | FixedRate    | After each slot release: sleeps for                        |
 * |              |   max(0, period_ms − elapsed_since_last_acquire)           |
 * |              | Increments ContextMetrics::overrun_count when              |
 * |              |   elapsed ≥ period_ms (iteration took longer than target). |
 * |              | Requires period_ms > 0 in DataBlockConfig.                 |
 * | MixTriggered | Reserved — trigger-based mode, not implemented.            |
 *
 * **Contrast with RoleConfig::LoopTimingPolicy:**
 *   - LoopPolicy (this enum): controls the *sleep inside* acquire_*_slot().
 *     It is a DataBlock-level knob that slows down slot consumption.
 *   - LoopTimingPolicy: controls *when the next deadline is computed* in the actor
 *     write loop (after on_iteration returns). FixedRate resets from now();
 *     FixedRateWithCompensation advances from the previous deadline, catching up after overruns.
 *
 * **Design doc:** HEP-CORE-0008-LoopPolicy-and-IterationMetrics.md §2.1
 */
enum class LoopPolicy : uint8_t
{
    MaxRate,      ///< No sleep — acquire slots as fast as possible (default)
    FixedRate,    ///< Start-to-start period: sleep(max(0, period_ms − elapsed)); tracks overruns
    MixTriggered, ///< Reserved — trigger-based mode, not yet implemented
};

} // namespace pylabhub::hub

// ============================================================================
// Shared config parsers — used by ProducerConfig, ProcessorConfig, ConsumerConfig
// ============================================================================

namespace pylabhub
{

/**
 * @brief Parse a JSON string value into a ConsumerSyncPolicy enum.
 *
 * Shared by all three role config parsers (producer, processor, consumer) to
 * avoid duplicating the same string → enum logic in each .cpp file.
 *
 * @param s        JSON string value (e.g. "sequential", "latest_only").
 * @param context  Caller context for error message (e.g. "Producer config").
 * @return Parsed ConsumerSyncPolicy.
 * @throws std::runtime_error on unknown value.
 */
inline hub::ConsumerSyncPolicy parse_consumer_sync_policy(
    const std::string &s, const char *context = "config")
{
    if (s == "latest_only")      return hub::ConsumerSyncPolicy::Latest_only;
    if (s == "sequential")       return hub::ConsumerSyncPolicy::Sequential;
    if (s == "sequential_sync")  return hub::ConsumerSyncPolicy::Sequential_sync;
    throw std::runtime_error(
        std::string(context) + ": invalid 'reader_sync_policy' = '" + s +
        "' (expected 'latest_only', 'sequential', or 'sequential_sync')");
}

} // namespace pylabhub
