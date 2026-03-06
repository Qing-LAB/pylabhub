#pragma once
/**
 * @file hub_script_api.hpp
 * @brief HubScriptAPI, ChannelInfo, and HubTickInfo — Python-accessible types for hub scripts.
 *
 * These types are exposed to Python via the `hub_script_api` embedded module defined in
 * hub_script_api.cpp.  Hub scripts receive a `HubScriptAPI` reference in their callbacks:
 *
 * @code
 * def on_start(api):
 *     api.log("info", f"Hub '{api.hub_name()}' started")
 *
 * def on_tick(api, tick):
 *     for ch in api.ready_channels():
 *         if ch.consumer_count() == 0:
 *             ch.request_close()
 *
 * def on_stop(api):
 *     api.log("info", "stopping")
 * @endcode
 *
 * ## Thread safety
 *
 * All methods on HubScriptAPI and ChannelInfo are called only from the tick thread, which
 * holds the GIL.  `pending_closes_` is read by the tick thread after `on_tick` returns,
 * before the GIL is released — no extra synchronisation is needed.
 *
 * `HubScriptAPI::shutdown()` stores to an atomic flag; safe from any thread.
 */

#include "utils/broker_service.hpp"
#include "plh_service.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace pylabhub
{

// ---------------------------------------------------------------------------
// ChannelInfo — typed, read-only snapshot of one channel
// ---------------------------------------------------------------------------

/**
 * @brief Typed read-only snapshot of a single channel, exposed to Python.
 *
 * Instances are created fresh per `on_tick` call from the snapshot returned by
 * `BrokerService::query_channel_snapshot()`.  `request_close()` records the channel
 * name in the parent `HubScriptAPI::pending_closes_` list for post-tick processing.
 */
class ChannelInfo
{
public:
    explicit ChannelInfo(broker::ChannelSnapshotEntry snap, class HubScriptAPI* api)
        : snap_(std::move(snap)), api_(api)
    {
    }

    /** Channel name. */
    const std::string& name() const noexcept { return snap_.name; }

    /** Status string: @c "Ready" | @c "PendingReady" | @c "Closing". */
    const std::string& status() const noexcept { return snap_.status; }

    /** Number of registered consumers. */
    int consumer_count() const noexcept { return snap_.consumer_count; }

    /** PID of the producer process (0 if not available). */
    uint64_t producer_pid() const noexcept { return snap_.producer_pid; }

    /** 64-char hex schema hash, or empty when no schema is declared. */
    const std::string& schema_hash() const noexcept { return snap_.schema_hash; }

    /** Human-readable name of the producer actor (empty if not set). */
    const std::string& producer_actor_name() const noexcept { return snap_.producer_actor_name; }

    /** UID of the producer actor (empty if not set). */
    const std::string& producer_actor_uid() const noexcept { return snap_.producer_actor_uid; }

    /**
     * @brief Mark this channel for close after `on_tick` returns.
     *
     * The C++ tick runner calls `BrokerService::request_close_channel()` for every
     * channel marked here, sending CHANNEL_CLOSING_NOTIFY to all parties.
     */
    void request_close();

private:
    broker::ChannelSnapshotEntry snap_;
    HubScriptAPI*                api_{nullptr};
};

// ---------------------------------------------------------------------------
// HubTickInfo — per-tick statistics, read-only
// ---------------------------------------------------------------------------

/**
 * @brief Per-tick metadata passed as the second argument to `on_tick(api, tick)`.
 */
class HubTickInfo
{
public:
    uint64_t tick_count() const noexcept { return tick_count_; }
    uint64_t elapsed_ms() const noexcept { return elapsed_ms_; }
    uint64_t uptime_ms()  const noexcept { return uptime_ms_; }
    int channels_ready()   const noexcept { return channels_ready_; }
    int channels_pending() const noexcept { return channels_pending_; }
    int channels_closing() const noexcept { return channels_closing_; }

    // Mutable setters for the C++ tick runner.
    void set_tick_count(uint64_t v)  noexcept { tick_count_ = v; }
    void set_elapsed_ms(uint64_t v)  noexcept { elapsed_ms_ = v; }
    void set_uptime_ms(uint64_t v)   noexcept { uptime_ms_  = v; }
    void set_channels_ready(int v)   noexcept { channels_ready_   = v; }
    void set_channels_pending(int v) noexcept { channels_pending_ = v; }
    void set_channels_closing(int v) noexcept { channels_closing_ = v; }

private:
    uint64_t tick_count_{0};
    uint64_t elapsed_ms_{0};
    uint64_t uptime_ms_{0};
    int      channels_ready_{0};
    int      channels_pending_{0};
    int      channels_closing_{0};
};

// ---------------------------------------------------------------------------
// HubScriptAPI — primary API object passed to every hub script callback
// ---------------------------------------------------------------------------

/**
 * @class HubScriptAPI
 * @brief Hub management API exposed to Python hub scripts.
 *
 * Each hub has a single `HubScriptAPI` instance, created by `HubScript` and passed to
 * `on_start`, `on_tick`, and `on_stop` callbacks.
 *
 * All Python-callable methods are exposed via the `hub_script_api` embedded module
 * (see hub_script_api.cpp).
 */
class HubScriptAPI
{
public:
    HubScriptAPI() = default;

    /// @internal Set by HubScript before callbacks fire.
    void set_broker(broker::BrokerService* broker) noexcept { broker_ = broker; }
    /// @internal Set by HubScript before callbacks fire.
    void set_shutdown_flag(std::atomic<bool>* flag) noexcept { shutdown_flag_ = flag; }

    // -----------------------------------------------------------------------
    // Hub identity
    // -----------------------------------------------------------------------

    /** Hub name in reverse-domain format (e.g. "asu.lab.main"). */
    std::string hub_name() const;

    /** Stable hub UID (format: "HUB-{NAME}-{8HEX}"). */
    std::string hub_uid() const;

    // -----------------------------------------------------------------------
    // Logging
    // -----------------------------------------------------------------------

    /**
     * @brief Log a message at the given level from hub script.
     *
     * @param level  One of: "debug", "info", "warn", "error"
     * @param msg    Message text.
     */
    void log(const std::string& level, const std::string& msg) const;

    // -----------------------------------------------------------------------
    // Control
    // -----------------------------------------------------------------------

    /**
     * @brief Request a graceful hub shutdown.
     *
     * Equivalent to calling `pylabhub.shutdown()` from the admin shell.
     */
    void shutdown();

    // -----------------------------------------------------------------------
    // Channel access
    // -----------------------------------------------------------------------

    /**
     * @brief Return all channels as typed ChannelInfo objects.
     *
     * Uses the snapshot built before `on_tick` was called; does NOT query
     * the broker again mid-tick.
     */
    std::vector<ChannelInfo> channels() const;

    /**
     * @brief Return only channels with status == "Ready".
     */
    std::vector<ChannelInfo> ready_channels() const;

    /**
     * @brief Return only channels with status == "PendingReady".
     */
    std::vector<ChannelInfo> pending_channels() const;

    /**
     * @brief Look up a channel by name.
     * @throws std::runtime_error if the channel is not found.
     */
    ChannelInfo channel(const std::string& name) const;

    // -----------------------------------------------------------------------
    // Hub federation (HEP-CORE-0022)
    // -----------------------------------------------------------------------

    /**
     * @brief Send a hub-targeted message to a direct federation peer.
     *
     * Queued and sent via BrokerService during the next broker run() poll iteration.
     * The payload must be a valid JSON-encoded string.
     *
     * @param target_hub_uid  UID of the destination hub (direct neighbor only).
     * @param channel         Context channel name (informational).
     * @param payload_json    JSON payload string.
     */
    void notify_hub(const std::string& target_hub_uid,
                    const std::string& channel,
                    const std::string& payload_json);

    // -----------------------------------------------------------------------
    // Internal — used by HubScript tick runner
    // -----------------------------------------------------------------------

    /// Hub event pushed from the broker thread (thread-safe).
    struct HubEvent
    {
        enum class Type { Connected, Disconnected, Message } type;
        std::string hub_uid;      ///< Connected/Disconnected: peer hub UID; Message: sender hub UID
        std::string channel;      ///< Message only
        std::string payload;      ///< Message only
    };

    /// Called from broker thread — push "connected" event.
    void push_hub_connected(const std::string& hub_uid);
    /// Called from broker thread — push "disconnected" event.
    void push_hub_disconnected(const std::string& hub_uid);
    /// Called from broker thread — push "message" event.
    void push_hub_message(const std::string& channel,
                          const std::string& payload,
                          const std::string& source_hub_uid);

    /// Take (move-out) all pending hub events (tick thread only).
    std::vector<HubEvent> take_hub_events();

    /// Set the current channel snapshot (refreshed every tick by HubScript).
    void set_snapshot(broker::ChannelSnapshot snap) { snapshot_ = std::move(snap); }

    /// Take (move-out) the list of channels to close after on_tick returns.
    std::vector<std::string> take_pending_closes()
    {
        std::vector<std::string> out;
        out.swap(pending_closes_);
        return out;
    }

    /// Called by ChannelInfo::request_close() — appends channel name.
    void mark_for_close(const std::string& name) { pending_closes_.push_back(name); }

private:
    broker::BrokerService*      broker_{nullptr};
    std::atomic<bool>*          shutdown_flag_{nullptr};
    broker::ChannelSnapshot     snapshot_;
    std::vector<std::string>    pending_closes_;

    mutable std::mutex          m_hub_event_mu_;
    std::vector<HubEvent>       pending_hub_events_;
};

} // namespace pylabhub
