#pragma once
/**
 * @file hub_consumer.hpp
 * @brief Passive consumer wrapper: owns the channel's read-side queue
 *        (ShmQueue or ZmqQueue) and exposes a QueueReader handle.
 *
 * Post-L3.γ: all active-mode surface (shm_thread, Queue/RealTime modes,
 * user callbacks) has been removed. hub::Consumer is now a thin factory
 * + owner for the unified QueueReader; role hosts drive the data loop
 * directly through the QueueReader returned by queue_reader().
 *
 * Scheduled for full removal in A6.3 — queue ownership migrates into
 * RoleAPIBase::Impl at that point.
 *
 * One Consumer instance per channel per process.
 */

#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_zmq_queue.hpp"  // ZmqQueue — returned by queue() accessor (HEP-CORE-0021)
#include "utils/schema_library.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// ConsumerOptions
// ============================================================================

struct ConsumerOptions
{
    std::string channel_name;

    std::string expected_schema_hash{};

    uint64_t shm_shared_secret{0};
    std::optional<DataBlockConfig> expected_shm_config{};

    std::string consumer_uid{};
    std::string consumer_name{};

    int timeout_ms{5000};

    std::string expected_schema_id{};

    // HEP-CORE-0021: ZMQ Endpoint Registry
    std::vector<ZmqSchemaField> zmq_schema{};
    std::string zmq_packing{"aligned"};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};

    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};

    std::string queue_type{};

    std::string shm_name{};
    std::string data_transport{"shm"};
    std::string zmq_node_endpoint{};
};

// ============================================================================
// Consumer
// ============================================================================

struct ConsumerImpl;

class PYLABHUB_UTILS_EXPORT Consumer
{
  public:
    [[nodiscard]] static std::optional<Consumer>
    create(const ConsumerOptions &opts);

    ~Consumer();
    Consumer(Consumer &&) noexcept;
    Consumer &operator=(Consumer &&) noexcept;
    Consumer(const Consumer &) = delete;
    Consumer &operator=(const Consumer &) = delete;

    // ── Introspection ─────────────────────────────────────────────────────────

    [[nodiscard]] bool               is_valid() const;
    [[nodiscard]] const std::string &channel_name() const;
    [[nodiscard]] ChannelPattern     pattern() const;
    [[nodiscard]] bool               has_shm() const;

    // ── SHM identity (delegated to internal DataBlock via ShmQueue) ───────────

    [[nodiscard]] uint32_t spinlock_count() const noexcept;
    [[nodiscard]] SharedSpinLock get_spinlock(size_t index);
    [[nodiscard]] std::string hub_uid() const noexcept;
    [[nodiscard]] std::string hub_name() const noexcept;
    [[nodiscard]] std::string producer_uid() const noexcept;
    [[nodiscard]] std::string producer_name() const noexcept;
    [[nodiscard]] std::string consumer_uid() const noexcept;
    [[nodiscard]] std::string consumer_name() const noexcept;

    // ── Transport accessors (HEP-CORE-0021) ───────────────────────────────────

    [[nodiscard]] const std::string &data_transport() const noexcept;
    [[nodiscard]] const std::string &zmq_node_endpoint() const noexcept;
    [[nodiscard]] ZmqQueue *queue() noexcept;

    // ── Unified QueueReader handle (L3.γ bridge) ──────────────────────────────
    [[nodiscard]] QueueReader *queue_reader() noexcept;

    // ── Queue data operations (forwarded to QueueReader) ─────────────────────

    [[nodiscard]] const void *read_acquire(std::chrono::milliseconds timeout) noexcept;
    void read_release() noexcept;
    [[nodiscard]] uint64_t last_seq() const noexcept;
    [[nodiscard]] size_t queue_item_size() const noexcept;
    [[nodiscard]] size_t queue_capacity() const noexcept;
    [[nodiscard]] QueueMetrics queue_metrics() const noexcept;
    void reset_queue_metrics() noexcept;

    // ── Queue lifecycle ─────────────────────────────────────────────────────

    bool start_queue();
    void stop_queue();

    // ── Channel data operations (flexzone, checksum — SHM-specific) ─────────

    [[nodiscard]] void *flexzone() noexcept;
    [[nodiscard]] size_t flexzone_size() const noexcept;
    void set_verify_checksum(bool slot, bool fz) noexcept;
    [[nodiscard]] std::string queue_policy_info() const;

    /// Closes queues and SHM. Called by destructor. Idempotent.
    void close();

  private:
    explicit Consumer(std::unique_ptr<ConsumerImpl> impl);
    std::unique_ptr<ConsumerImpl> pImpl;
};

} // namespace pylabhub::hub
