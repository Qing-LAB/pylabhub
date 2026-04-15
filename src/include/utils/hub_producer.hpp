#pragma once
/**
 * @file hub_producer.hpp
 * @brief Passive producer wrapper: owns the channel's write-side queue
 *        (ShmQueue or ZmqQueue) and exposes a QueueWriter handle.
 *
 * Post-L3.γ: all active-mode surface (write_thread, Queue/RealTime modes,
 * user callbacks) has been removed. hub::Producer is now a thin factory
 * + owner for the unified QueueWriter; role hosts drive the data loop
 * directly through the QueueWriter returned by queue_writer().
 *
 * Scheduled for full removal in A6.3 — queue ownership migrates into
 * RoleAPIBase::Impl at that point.
 *
 * One Producer instance per channel per process.
 */
#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_zmq_queue.hpp"  // ZmqQueue — returned by queue() accessor (HEP-CORE-0021)
#include "utils/schema_library.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// ProducerOptions
// ============================================================================

struct ProducerOptions
{
    std::string    channel_name;
    ChannelPattern pattern{ChannelPattern::PubSub};

    bool            has_shm{false};
    DataBlockConfig shm_config{};

    std::string schema_hash{};
    uint32_t    schema_version{0};

    int timeout_ms{5000};

    std::string role_name{};
    std::string role_uid{};

    std::string schema_id{};

    // HEP-CORE-0021: ZMQ Endpoint Registry
    std::string data_transport{"shm"};
    std::string zmq_node_endpoint{};
    bool zmq_bind{true};
    std::vector<ZmqSchemaField> zmq_schema{};
    std::string zmq_packing{"aligned"};
    std::vector<ZmqSchemaField> fz_schema{};
    std::string fz_packing{"aligned"};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};
    OverflowPolicy zmq_overflow_policy{OverflowPolicy::Drop};

    // Queue abstraction
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};
    bool always_clear_slot{true};
};

// ============================================================================
// Producer
// ============================================================================

struct ProducerImpl;

class PYLABHUB_UTILS_EXPORT Producer
{
  public:
    [[nodiscard]] static std::optional<Producer>
    create(const ProducerOptions &opts);

    ~Producer();
    Producer(Producer &&) noexcept;
    Producer &operator=(Producer &&) noexcept;
    Producer(const Producer &) = delete;
    Producer &operator=(const Producer &) = delete;

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

    // ── ZMQ endpoint accessor (HEP-CORE-0021) ─────────────────────────────────

    [[nodiscard]] ZmqQueue *queue() noexcept;

    // ── Unified QueueWriter handle (L3.γ bridge) ──────────────────────────────
    [[nodiscard]] QueueWriter *queue_writer() noexcept;

    // ── Queue data operations (forwarded to QueueWriter) ─────────────────────

    [[nodiscard]] void *write_acquire(std::chrono::milliseconds timeout) noexcept;
    void write_commit() noexcept;
    void write_discard() noexcept;
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
    void set_checksum_options(bool slot, bool fz) noexcept;
    void set_always_clear_slot(bool enable) noexcept;
    void sync_flexzone_checksum() noexcept;
    [[nodiscard]] std::string queue_policy_info() const;

    /// Closes queues and SHM. Called by destructor. Idempotent.
    void close();

  private:
    explicit Producer(std::unique_ptr<ProducerImpl> impl);
    std::unique_ptr<ProducerImpl> pImpl;
};

} // namespace pylabhub::hub
