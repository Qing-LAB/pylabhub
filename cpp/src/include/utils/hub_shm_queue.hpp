#pragma once
/**
 * @file hub_shm_queue.hpp
 * @brief ShmQueue — shared-memory-backed Queue implementation.
 *
 * Wraps a DataBlockConsumer (read mode) or DataBlockProducer (write mode).
 * No ZMQ thread, no broker registration, no protocol.
 *
 * The caller is responsible for SHM setup (broker registration, DataBlock
 * creation/attachment) before constructing the ShmQueue.
 *
 * @par Thread safety
 * ShmQueue is NOT thread-safe; use from exactly one thread at a time.
 *
 * @par Lifecycle
 * start()/stop() are no-ops; the SHM objects are already attached.
 */
#include "utils/hub_queue.hpp"
#include "utils/data_block.hpp"

#include <memory>
#include <string>

namespace pylabhub::hub
{

struct ShmQueueImpl;

/**
 * @class ShmQueue
 * @brief Shared-memory Queue backed by DataBlockConsumer or DataBlockProducer.
 *
 * @par Read mode
 * Wraps a DataBlockConsumer.  Each read_acquire() call blocks until the next
 * committed slot is available, then returns a pointer to the slot data.
 * read_release() releases the read lock on that slot.
 *
 * @par Write mode
 * Wraps a DataBlockProducer.  write_acquire() acquires a free slot.
 * write_commit() commits it (makes it visible to consumers).
 * write_abort() releases it without committing.
 */
class PYLABHUB_UTILS_EXPORT ShmQueue final : public Queue
{
public:
    // ── Factories ─────────────────────────────────────────────────────────────

    /**
     * @brief Create a read-mode ShmQueue from a DataBlockConsumer.
     *
     * Takes ownership of the consumer.  The caller must not use the consumer
     * after passing it to this function.
     *
     * @param dbc           DataBlockConsumer to wrap (must not be null).
     * @param item_size     sizeof(DataT) — size of one slot in bytes.
     * @param flexzone_sz   sizeof(FlexZoneT), or 0 if no flexzone.
     * @param channel_name  Optional diagnostic name.
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    from_consumer(std::unique_ptr<DataBlockConsumer> dbc,
                  size_t item_size, size_t flexzone_sz = 0,
                  std::string channel_name = {});

    /**
     * @brief Create a write-mode ShmQueue from a DataBlockProducer.
     *
     * Takes ownership of the producer.
     *
     * @param dbp           DataBlockProducer to wrap (must not be null).
     * @param item_size     sizeof(DataT) — committed bytes per slot.
     * @param flexzone_sz   sizeof(FlexZoneT), or 0 if no flexzone.
     * @param channel_name  Optional diagnostic name.
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    from_producer(std::unique_ptr<DataBlockProducer> dbp,
                  size_t item_size, size_t flexzone_sz = 0,
                  std::string channel_name = {});

    /**
     * @brief Create a read-mode ShmQueue wrapping an existing DataBlockConsumer.
     *
     * Non-owning: the caller retains ownership of @p dbc, which must remain
     * valid for the lifetime of this ShmQueue.
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    from_consumer_ref(DataBlockConsumer& dbc, size_t item_size,
                      size_t flexzone_sz = 0, std::string channel_name = {});

    /**
     * @brief Create a write-mode ShmQueue wrapping an existing DataBlockProducer.
     *
     * Non-owning: the caller retains ownership of @p dbp, which must remain
     * valid for the lifetime of this ShmQueue.
     */
    [[nodiscard]] static std::unique_ptr<ShmQueue>
    from_producer_ref(DataBlockProducer& dbp, size_t item_size,
                      size_t flexzone_sz = 0, std::string channel_name = {});

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    ~ShmQueue() override;
    ShmQueue(ShmQueue&&) noexcept;
    ShmQueue& operator=(ShmQueue&&) noexcept;
    ShmQueue(const ShmQueue&) = delete;
    ShmQueue& operator=(const ShmQueue&) = delete;

    // ── Queue interface — reading ─────────────────────────────────────────────

    const void* read_acquire(std::chrono::milliseconds timeout) noexcept override;
    void        read_release() noexcept override;
    const void* read_flexzone() const noexcept override;

    // ── Queue interface — writing ─────────────────────────────────────────────

    void* write_acquire(std::chrono::milliseconds timeout) noexcept override;
    void  write_commit() noexcept override;
    void  write_abort() noexcept override;
    void* write_flexzone() noexcept override;

    // ── Queue interface — metadata ────────────────────────────────────────────

    size_t      item_size()     const noexcept override;
    size_t      flexzone_size() const noexcept override;
    std::string name()          const override;

    // start()/stop()/is_running() — inherited no-op implementations from Queue.

    /**
     * @brief Enable BLAKE2b checksum updates on write_commit().
     *
     * When enabled, write_commit() calls update_checksum_slot() and
     * optionally update_checksum_flexible_zone() after committing the slot
     * and before releasing it.  Only meaningful for write-mode ShmQueues.
     *
     * @param slot  Update slot checksum on commit.
     * @param fz    Update flexzone checksum on commit.
     */
    void set_checksum_options(bool slot, bool fz) noexcept;

private:
    explicit ShmQueue(std::unique_ptr<ShmQueueImpl> impl);
    std::unique_ptr<ShmQueueImpl> pImpl;
};

} // namespace pylabhub::hub
