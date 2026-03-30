#pragma once
/**
 * @file hub_inbox_queue.hpp
 * @brief InboxQueue (ROUTER receiver) and InboxClient (DEALER sender) for typed role-to-role messaging.
 *
 * ## Overview
 * A role declares an inbox by configuring `inbox_schema` in its JSON config.
 * The inbox endpoint is registered with the broker at startup (REG_REQ `inbox_endpoint` field).
 * Any role can discover the inbox endpoint via ROLE_INFO_REQ (Phase 4) and connect an InboxClient.
 *
 * ## Wire format (MessagePack array, 4 elements — same as ZmqQueue)
 *   [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N fields)]
 *   - magic      : 0x51484C50 ('PLHQ')
 *   - schema_tag : first 8 bytes of BLAKE2b-256(BLDS) (optional identity guard)
 *   - seq        : monotonic sender counter
 *   - payload    : N typed field values (scalar or bin)
 *
 * ## ZMQ framing (ROUTER-DEALER)
 * InboxClient sets ZMQ_IDENTITY to the sender's pylabhub UID before connecting.
 * ROUTER receives: [identity_frame, empty_frame, payload_frame]
 * ROUTER sends ACK: [identity_frame, empty_frame, ack_code_byte]
 * DEALER receives ACK: ["", ack_code_byte]  (ZMQ strips identity; app drains empty delimiter)
 *
 * ## Thread safety
 * InboxQueue is designed for use from a SINGLE inbox_thread_. All recv_one() and
 * send_ack() calls must come from the same thread (the ROUTER socket is not thread-safe).
 * InboxClient is also single-threaded: acquire()/send()/abort() from one caller thread.
 *
 * ## Lifecycle
 * Call start() before recv_one()/acquire(); call stop() before destruction.
 * stop() drains any pending recv/ack and closes the ZMQ context.
 */
#include "utils/hub_zmq_queue.hpp"   // ZmqSchemaField

#include "pylabhub_utils_export.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct InboxQueueImpl;
struct InboxClientImpl;

// ============================================================================
// InboxItem — result of InboxQueue::recv_one()
// ============================================================================

/**
 * @struct InboxItem
 * @brief One decoded inbox message.  Valid only until the next recv_one() call.
 */
struct PYLABHUB_UTILS_EXPORT InboxItem
{
    const void*  data{nullptr};    ///< Decoded payload buffer (InboxQueue-owned; item_size() bytes).
    std::string  sender_id;        ///< Pylabhub UID of the sender (from ZMQ identity frame).
    uint64_t     seq{0};           ///< Monotonic sender sequence number.
};

// ============================================================================
// InboxQueue — ZMQ ROUTER receiver
// ============================================================================

/**
 * @class InboxQueue
 * @brief ZMQ ROUTER socket that receives typed, schema-validated messages and sends ACK.
 *
 * Designed for use from a single inbox_thread_. Not thread-safe for concurrent callers.
 *
 * Typical usage in inbox_thread_:
 * @code
 *   while (running) {
 *       const auto* item = inbox_queue_->recv_one(std::chrono::milliseconds{100});
 *       if (!item) continue;
 *       uint8_t ack = 0;
 *       { py::gil_scoped_acquire g;
 *         try { call_on_inbox_(item); }
 *         catch (...) { ack = 3; } }
 *       inbox_queue_->send_ack(ack);
 *   }
 * @endcode
 */
class PYLABHUB_UTILS_EXPORT InboxQueue
{
public:
    /**
     * @brief Factory: create an InboxQueue that binds a ROUTER socket at @p endpoint.
     *
     * @param endpoint    ZMQ endpoint to bind (e.g. "tcp://0.0.0.0:5592" or "tcp://0.0.0.0:0").
     *                    Port 0 causes the OS to assign a free port; retrieve it via actual_endpoint().
     * @param schema      Field list — must be non-empty; returns nullptr on error.
     * @param packing     "aligned" or "packed". Must match InboxClient packing.
     * @param rcvhwm      ZMQ_RCVHWM: max queued incoming messages before ZMQ drops.
     *                    Default 1000 (ZMQ built-in default). 0 = unlimited.
     * @return            nullptr on schema or ZMQ setup error (logged internally).
     */
    [[nodiscard]] static std::unique_ptr<InboxQueue>
    bind_at(const std::string& endpoint, std::vector<ZmqSchemaField> schema,
            std::string packing = "aligned", int rcvhwm = 1000);

    ~InboxQueue();
    InboxQueue(InboxQueue&&) noexcept;
    InboxQueue& operator=(InboxQueue&&) noexcept;
    InboxQueue(const InboxQueue&) = delete;
    InboxQueue& operator=(const InboxQueue&) = delete;

    /**
     * @brief Bind ROUTER socket and start listening.
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure.
     */
    bool start();

    /**
     * @brief Stop listening and close socket + context.  Safe to call multiple times.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /**
     * @brief Actual bound endpoint after start().
     * For port-0 binds, returns the OS-assigned endpoint (e.g. "tcp://127.0.0.1:54321").
     */
    [[nodiscard]] std::string actual_endpoint() const;

    /** Size in bytes of one decoded item buffer. */
    [[nodiscard]] size_t item_size() const noexcept;

    /**
     * @brief Blocking receive of one inbox message with timeout.
     *
     * @param timeout  Maximum wait time. Returns nullptr on timeout or stop().
     * @return Pointer to the decoded InboxItem (owned by this InboxQueue; valid until
     *         the next recv_one() call).  nullptr on timeout or error.
     */
    [[nodiscard]] const InboxItem* recv_one(std::chrono::milliseconds timeout) noexcept;

    /**
     * @brief Send ACK for the last recv_one() result.
     *
     * Must be called exactly once after each successful recv_one() != nullptr.
     * Uses the sender_id stored internally from the last recv_one().
     *
     * @param code  0=OK, 1=queue_overflow, 2=schema_error, 3=handler_error.
     */
    void send_ack(uint8_t code) noexcept;

    /** Frames rejected (bad magic, schema mismatch, field type/size error). */
    [[nodiscard]] uint64_t recv_frame_error_count() const noexcept;
    /** ACK send failures (ZMQ errors). */
    [[nodiscard]] uint64_t ack_send_error_count() const noexcept;
    /** Sequence number gaps detected (sender restarts or dropped frames). */
    [[nodiscard]] uint64_t recv_gap_count() const noexcept;
    /** BLAKE2b checksum verification failures. */
    [[nodiscard]] uint64_t checksum_error_count() const noexcept;

    /** Set checksum policy. Enforced = auto verify on recv. None = skip. */
    void set_checksum_policy(ChecksumPolicy policy) noexcept;

    /// Snapshot inbox metrics.
    struct InboxMetricsSnapshot
    {
        uint64_t recv_frame_error_count{0};
        uint64_t ack_send_error_count{0};
        uint64_t recv_gap_count{0};
        uint64_t checksum_error_count{0};
    };
    [[nodiscard]] InboxMetricsSnapshot inbox_metrics() const noexcept
    {
        return {recv_frame_error_count(), ack_send_error_count(),
                recv_gap_count(), checksum_error_count()};
    }

private:
    explicit InboxQueue(std::unique_ptr<InboxQueueImpl> impl);
    std::unique_ptr<InboxQueueImpl> pImpl;
};

// ============================================================================
// InboxClient — ZMQ DEALER sender
// ============================================================================

/**
 * @class InboxClient
 * @brief ZMQ DEALER socket that sends typed, schema-validated messages to an InboxQueue.
 *
 * The ZMQ_IDENTITY is set to @p sender_uid before connecting, so the ROUTER
 * sees the sender's pylabhub UID as the routing identity.
 *
 * Typical usage:
 * @code
 *   auto client = InboxClient::connect_to(endpoint, my_uid, schema);
 *   client->start();
 *   void* buf = client->acquire();
 *   // fill buf with typed fields
 *   uint8_t ack = client->send(std::chrono::milliseconds{500});
 * @endcode
 */
class PYLABHUB_UTILS_EXPORT InboxClient
{
public:
    /**
     * @brief Factory: create an InboxClient that connects a DEALER to @p endpoint.
     *
     * @param endpoint    ZMQ endpoint of the ROUTER InboxQueue.
     * @param sender_uid  This client's pylabhub UID — set as ZMQ_IDENTITY before connect.
     * @param schema      Field list — must be non-empty; returns nullptr on error.
     * @param packing     "aligned" or "packed". Must match InboxQueue packing.
     * @return            nullptr on schema or ZMQ setup error (logged internally).
     */
    [[nodiscard]] static std::unique_ptr<InboxClient>
    connect_to(const std::string& endpoint, const std::string& sender_uid,
               std::vector<ZmqSchemaField> schema, std::string packing = "aligned");

    ~InboxClient();
    InboxClient(InboxClient&&) noexcept;
    InboxClient& operator=(InboxClient&&) noexcept;
    InboxClient(const InboxClient&) = delete;
    InboxClient& operator=(const InboxClient&) = delete;

    /**
     * @brief Set ZMQ_IDENTITY and connect DEALER socket to the configured endpoint.
     * @return true on success or if already running (idempotent).
     *         false only on actual startup failure.
     */
    bool start();

    /**
     * @brief Disconnect and close socket + context.  Safe to call multiple times.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /** Size in bytes of one send buffer. */
    [[nodiscard]] size_t item_size() const noexcept;

    /**
     * @brief Get the write buffer.  Fill it with typed field values before calling send().
     * The buffer is zero-initialized at factory time and between sends.
     * @return Pointer to the write buffer (item_size() bytes); nullptr if not running.
     */
    [[nodiscard]] void* acquire() noexcept;

    /**
     * @brief Encode and send the buffer contents; optionally wait for ACK.
     *
     * @param ack_timeout  If > 0 ms: block up to this duration for the ACK byte.
     *                     If 0 ms: fire-and-forget (returns 0 immediately).
     * @return ACK error code (0=OK, non-zero=error).  Returns 255 on send failure or ACK timeout.
     */
    uint8_t send(std::chrono::milliseconds ack_timeout = std::chrono::milliseconds{1000}) noexcept;

    /**
     * @brief Discard the current buffer contents without sending.  Next acquire() is fresh.
     */
    void abort() noexcept;

    /** Set checksum policy. Enforced = auto compute on send. None = send zeros. */
    void set_checksum_policy(ChecksumPolicy policy) noexcept;

private:
    explicit InboxClient(std::unique_ptr<InboxClientImpl> impl);
    std::unique_ptr<InboxClientImpl> pImpl;
};

} // namespace pylabhub::hub

/**
 * @brief Canonical field list for InboxQueue::InboxMetricsSnapshot serialization.
 * Same X-macro pattern as PYLABHUB_QUEUE_METRICS_FIELDS (see hub_queue.hpp).
 */
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define PYLABHUB_INBOX_METRICS_FIELDS(X) \
    X(recv_frame_error_count)            \
    X(ack_send_error_count)              \
    X(recv_gap_count)                    \
    X(checksum_error_count)
// NOLINTEND(cppcoreguidelines-macro-usage)
