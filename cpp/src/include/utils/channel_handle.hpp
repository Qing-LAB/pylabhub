#pragma once
/**
 * @file channel_handle.hpp
 * @brief RAII handle owning the producer or consumer side of a channel.
 *
 * A ChannelHandle is created by Messenger::create_channel() (producer) or
 * Messenger::connect_channel() (consumer). It owns the P2C (producer-to-consumer)
 * ZMQ sockets and, optionally, a DataBlock handle for shared-memory bulk data.
 *
 * **Threading**: ChannelHandle is single-threaded. The ZMQ sockets live in the
 * thread that called create_channel / connect_channel. Do not call ChannelHandle
 * methods concurrently or from a different thread.
 *
 * **Socket pattern summary**:
 *
 * | Pattern  | Producer socket(s) | Consumer socket(s) |
 * |----------|--------------------|--------------------|
 * | PubSub   | ROUTER ctrl (bind) + XPUB data (bind) | DEALER ctrl (conn) + SUB data (conn) |
 * | Pipeline | ROUTER ctrl (bind) + PUSH data (bind) | DEALER ctrl (conn) + PULL data (conn) |
 * | Bidir    | ROUTER ctrl (bind) (data+ctrl combined) | DEALER ctrl (conn) |
 *
 * Framing (universal):
 *  - Data frame:    ['A', <raw bytes>]          (2 ZMQ frames)
 *  - Control frame: ['C', <type str>, <body>]   (3 ZMQ frames)
 *  - ROUTER always prepends/receives an identity frame before the type byte.
 */
#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ChannelHandleImpl;

class PYLABHUB_UTILS_EXPORT ChannelHandle
{
  public:
    ChannelHandle();
    ~ChannelHandle();

    ChannelHandle(const ChannelHandle &) = delete;
    ChannelHandle &operator=(const ChannelHandle &) = delete;
    ChannelHandle(ChannelHandle &&) noexcept;
    ChannelHandle &operator=(ChannelHandle &&) noexcept;

    // ── Data send ──────────────────────────────────────────────────────────────

    /**
     * @brief Send raw data bytes.
     *
     * Producer (PubSub/Pipeline): broadcasts ['A', data] on the data socket.
     * Producer (Bidir): sends [identity, 'A', data] on the ctrl ROUTER socket.
     *   @p identity must be a valid ZMQ identity string for Bidir; empty = returns false.
     * Consumer (Bidir): sends ['A', data] on the ctrl DEALER socket.
     * Consumer (PubSub/Pipeline): not applicable (returns false).
     */
    bool send(const void *data, size_t size, const std::string &identity = {});

    // ── Data receive ───────────────────────────────────────────────────────────

    /**
     * @brief Receive raw data bytes.
     *
     * Consumer (PubSub/Pipeline): receives ['A', data] from data socket.
     * Consumer (Bidir): receives ['A', data] from ctrl DEALER socket.
     *   Non-data frames ('C') are discarded and the call retries until data
     *   arrives or the timeout expires.
     * Producer (Bidir): receives [identity, 'A', data] from ctrl ROUTER socket.
     *   @p out_identity is filled with the sender's ZMQ identity.
     * Producer (PubSub/Pipeline): not applicable (returns false).
     *
     * @param buf          Output buffer; resized to fit the received payload.
     * @param timeout_ms   Max time to wait for a message.
     * @param out_identity If non-null, filled with the sender's ZMQ identity
     *                     (meaningful for ROUTER sockets on producer Bidir).
     * @return true on success, false on timeout or closed handle.
     */
    bool recv(std::vector<std::byte> &buf,
              int                    timeout_ms   = 5000,
              std::string           *out_identity = nullptr);

    // ── Control messages ───────────────────────────────────────────────────────

    /**
     * @brief Send a control frame on the ctrl socket.
     *
     * Sends ['C', "CTRL", <data>] on the ctrl socket.
     * Consumer (DEALER): sends to the producer.
     * Producer (ROUTER): @p identity must be provided to address a specific consumer.
     */
    bool send_ctrl(const void *data, size_t size, const std::string &identity = {});

    /**
     * @brief Receive a control frame from the ctrl socket.
     *
     * @param buf          Receives the raw control body (3rd frame for DEALER,
     *                     4th frame for ROUTER).
     * @param out_type     If non-null, filled with the control type string (2nd frame
     *                     for DEALER, 3rd for ROUTER, e.g. "CTRL", "HELLO", "HELLO_ACK").
     * @param out_identity If non-null, filled with sender ZMQ identity (ROUTER only).
     */
    bool recv_ctrl(std::vector<std::byte> &buf,
                   int                    timeout_ms   = 5000,
                   std::string           *out_type     = nullptr,
                   std::string           *out_identity = nullptr);

    // ── Introspection ─────────────────────────────────────────────────────────

    ChannelPattern     pattern()      const;
    bool               has_shm()      const;
    const std::string &channel_name() const;

    /**
     * @brief Returns the shared memory segment name.
     *
     * For producer handles: equals channel_name() when has_shm() is true; empty otherwise.
     * For consumer handles: the shm_name returned by the broker on discovery.
     */
    [[nodiscard]] const std::string &shm_name() const;

    /**
     * @brief Send a control frame with a caller-supplied type string.
     *
     * Like send_ctrl(), but allows specifying a custom @p type (e.g. "HELLO", "BYE").
     * Consumer (DEALER): sends ['C', type, data].
     * Producer (ROUTER): @p identity must be non-empty; sends [identity, 'C', type, data].
     */
    bool send_typed_ctrl(std::string_view type, const void *data, size_t size,
                         const std::string &identity = {});

    /**
     * @brief Returns false if the handle is empty (default-constructed or moved-from)
     *        or has been invalidated by a CHANNEL_CLOSING_NOTIFY.
     */
    bool is_valid() const;

    /**
     * @brief Mark the handle invalid (called by Messenger on CHANNEL_CLOSING_NOTIFY).
     */
    void invalidate();

    // ── Internal factory (used by Messenger) ─────────────────────────────────

    /**
     * @brief Internal constructor used by Messenger::create_channel /
     *        connect_channel.  Not for direct use by application code.
     */
    explicit ChannelHandle(std::unique_ptr<ChannelHandleImpl> impl);

    // ── Internal raw socket access (for hub_producer / hub_consumer only) ──────
    // Returns a type-erased pointer to the underlying zmq::socket_t.
    // channel_handle_internals.hpp casts these back to zmq::socket_t*.
    // Each socket must be used by exactly ONE thread after start().
    void *internal_ctrl_socket_ptr() noexcept;
    void *internal_data_socket_ptr() noexcept;

  private:
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<ChannelHandleImpl> pImpl;
};

} // namespace pylabhub::hub
