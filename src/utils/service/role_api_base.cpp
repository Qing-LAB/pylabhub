/**
 * @file role_api_base.cpp
 * @brief RoleAPIBase implementation — unified role API (pure C++).
 */
#include "utils/role_api_base.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/script_engine.hpp"        // ScriptEngine, InvokeInbox, ThreadEngineGuard
#include "utils/zmq_poll_loop.hpp"        // ZmqPollLoop, PeriodicTask

#include "utils/config/checksum_config.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_producer.hpp"
#include "utils/logger.hpp"
#include "utils/metrics_json.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"
#include "utils/shared_memory_spinlock.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace pylabhub::scripting
{

// ============================================================================
// Impl
// ============================================================================

struct RoleAPIBase::Impl
{
    explicit Impl(RoleHostCore *c) : core(c) {}

    RoleHostCore    *core;
    hub::Producer   *producer{nullptr};
    hub::Consumer   *consumer{nullptr};

    // Direct queue handles — the surviving abstraction post-L3.γ. While
    // hub::Producer / hub::Consumer still exist, these are cached mirrors
    // populated by set_producer() / set_consumer() and point at the same
    // objects the producer/consumer wrap internally. All data-plane
    // forwards (write_acquire/commit/discard, read_acquire/release,
    // flexzone, etc.) go through these directly.
    hub::QueueWriter *tx_queue{nullptr};
    hub::QueueReader *rx_queue{nullptr};
    hub::InboxQueue          *inbox_queue{nullptr};
    ScriptEngine             *engine{nullptr};
    hub::BrokerRequestComm *broker_channel{nullptr};

    std::string role_tag;   // "prod", "cons", "proc"
    hub::ChecksumPolicy checksum_policy{hub::ChecksumPolicy::Enforced};
    bool stop_on_script_error{false};
    std::string uid;
    std::string name;
    std::string channel;
    std::string out_channel;
    std::string log_level;
    std::string script_dir;
    std::string role_dir;

    // Broker registration state (for explicit deregister on shutdown).
    std::string registered_producer_channel;   ///< Non-empty if REG_REQ succeeded.
    std::string registered_consumer_channel;   ///< Non-empty if CONSUMER_REG_REQ succeeded.

    // Inbox client cache (keyed by target_uid).
    // Uses RoleHostCore::open_inbox() for atomic check-and-create.

    // last_seq: no local copy — forwarded directly to consumer->last_seq().

    // Thread manager.
    std::vector<std::thread> managed_threads_;
};

// ============================================================================
// Lifecycle
// ============================================================================

RoleAPIBase::RoleAPIBase(RoleHostCore &core)
    : pImpl(std::make_unique<Impl>(&core))
{}

RoleAPIBase::~RoleAPIBase() = default;
RoleAPIBase::RoleAPIBase(RoleAPIBase &&) noexcept = default;
RoleAPIBase &RoleAPIBase::operator=(RoleAPIBase &&) noexcept = default;

// ============================================================================
// Host wiring
// ============================================================================

void RoleAPIBase::set_role_tag(std::string tag)       { pImpl->role_tag = std::move(tag); }
void RoleAPIBase::set_producer(hub::Producer *p)
{
    pImpl->producer = p;
    pImpl->tx_queue = p ? p->queue_writer() : nullptr;
}
void RoleAPIBase::set_consumer(hub::Consumer *c)
{
    pImpl->consumer = c;
    pImpl->rx_queue = c ? c->queue_reader() : nullptr;
}
void RoleAPIBase::set_inbox_queue(hub::InboxQueue *q) { pImpl->inbox_queue = q; }
void RoleAPIBase::set_uid(std::string uid)            { pImpl->uid = std::move(uid); }
void RoleAPIBase::set_name(std::string name)          { pImpl->name = std::move(name); }
void RoleAPIBase::set_channel(std::string c)          { pImpl->channel = std::move(c); }
void RoleAPIBase::set_out_channel(std::string c)      { pImpl->out_channel = std::move(c); }
void RoleAPIBase::set_log_level(std::string l)        { pImpl->log_level = std::move(l); }
void RoleAPIBase::set_script_dir(std::string d)       { pImpl->script_dir = std::move(d); }
void RoleAPIBase::set_role_dir(std::string d)         { pImpl->role_dir = std::move(d); }
void RoleAPIBase::set_checksum_policy(hub::ChecksumPolicy p) { pImpl->checksum_policy = p; }
void RoleAPIBase::set_engine(ScriptEngine *e)          { pImpl->engine = e; }
void RoleAPIBase::set_stop_on_script_error(bool v)    { pImpl->stop_on_script_error = v; }

// ============================================================================
// Thread manager
// ============================================================================

void RoleAPIBase::spawn_thread(const std::string &name, std::function<void()> body)
{
    // Capture `this` — engine and tag are accessed via pImpl at call time.
    // RoleAPIBase outlives all managed threads (join_all_threads called before dtor).
    pImpl->managed_threads_.emplace_back(
        [this, name, body = std::move(body)]()
        {
            // ThreadEngineGuard registers this thread with the engine for
            // cross-thread dispatch (Lua: thread-local state; Python: queue).
            auto *eng = pImpl->engine;
            ThreadEngineGuard guard(*eng);
            LOGGER_INFO("[{}/{}] thread started", pImpl->role_tag, name);
            body();
            LOGGER_INFO("[{}/{}] thread exiting", pImpl->role_tag, name);
        });
}

void RoleAPIBase::join_all_threads()
{
    // Join in reverse spawn order (last spawned = first joined).
    for (auto it = pImpl->managed_threads_.rbegin();
         it != pImpl->managed_threads_.rend(); ++it)
    {
        if (it->joinable())
            it->join();
    }
    pImpl->managed_threads_.clear();
}

size_t RoleAPIBase::thread_count() const
{
    return pImpl->managed_threads_.size();
}

// ============================================================================
// set_broker_comm
// ============================================================================

void RoleAPIBase::set_broker_comm(hub::BrokerRequestComm *bc)
{
    pImpl->broker_channel = bc;
}

// ============================================================================
// Broker protocol helpers (require ctrl thread running)
// ============================================================================

std::optional<nlohmann::json>
RoleAPIBase::register_producer_channel(const nlohmann::json &opts, int timeout_ms)
{
    auto *bc = pImpl->broker_channel;
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] register_producer_channel: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }
    auto result = bc->register_channel(opts, timeout_ms);
    if (!result.has_value())
        LOGGER_ERROR("[{}] REG_REQ failed for channel '{}'",
                     pImpl->role_tag, opts.value("channel_name", "?"));
    else
        LOGGER_INFO("[{}] Registered producer channel '{}' with broker",
                    pImpl->role_tag, opts.value("channel_name", "?"));
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::discover_channel(const std::string &channel, int timeout_ms)
{
    auto *bc = pImpl->broker_channel;
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] discover_channel: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }
    auto result = bc->discover_channel(channel, {}, timeout_ms);
    if (!result.has_value())
        LOGGER_ERROR("[{}] DISC_REQ failed for channel '{}'", pImpl->role_tag, channel);
    else
        LOGGER_INFO("[{}] Discovered channel '{}' from broker", pImpl->role_tag, channel);
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::register_consumer(const nlohmann::json &opts, int timeout_ms)
{
    auto *bc = pImpl->broker_channel;
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] register_consumer: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }
    auto result = bc->register_consumer(opts, timeout_ms);
    if (!result.has_value())
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ failed for channel '{}'",
                     pImpl->role_tag, opts.value("channel_name", "?"));
    else
        LOGGER_INFO("[{}] Registered consumer on channel '{}' with broker",
                    pImpl->role_tag, opts.value("channel_name", "?"));
    return result;
}

bool RoleAPIBase::deregister_producer_channel(const std::string &channel, int timeout_ms)
{
    auto *bc = pImpl->broker_channel;
    if (!bc || !bc->is_connected())
        return false;
    return bc->deregister_channel(channel, timeout_ms);
}

bool RoleAPIBase::deregister_consumer(const std::string &channel, int timeout_ms)
{
    auto *bc = pImpl->broker_channel;
    if (!bc || !bc->is_connected())
        return false;
    return bc->deregister_consumer(channel, timeout_ms);
}

// ============================================================================
// Control thread — broker communication (heartbeat, notifications, requests)
// ============================================================================
//
// Private helpers called from within the ctrl thread. Access pImpl members
// directly — no bare pointers leaked into lambdas.

void RoleAPIBase::deregister_from_broker()
{
    auto *bc = pImpl->broker_channel;
    if (!bc || !bc->is_connected())
        return;

    if (!pImpl->registered_producer_channel.empty())
    {
        LOGGER_INFO("[{}] ctrl: deregistering producer channel '{}' from broker",
                    pImpl->role_tag, pImpl->registered_producer_channel);
        deregister_producer_channel(pImpl->registered_producer_channel);
        pImpl->registered_producer_channel.clear();
    }

    if (!pImpl->registered_consumer_channel.empty())
    {
        LOGGER_INFO("[{}] ctrl: deregistering consumer from channel '{}' from broker",
                    pImpl->role_tag, pImpl->registered_consumer_channel);
        deregister_consumer(pImpl->registered_consumer_channel);
        pImpl->registered_consumer_channel.clear();
    }
}

void RoleAPIBase::on_heartbeat_tick_()
{
    auto *bc  = pImpl->broker_channel;
    auto *eng = pImpl->engine;
    if (!bc)
        return;

    std::string hb_channel = pImpl->out_channel.empty()
                                 ? pImpl->channel
                                 : pImpl->out_channel;

    LOGGER_TRACE("[{}] ctrl: sending heartbeat for '{}'",
                 pImpl->role_tag, hb_channel);
    bc->send_heartbeat(hb_channel, snapshot_metrics_json());

    if (eng && eng->has_callback("on_heartbeat"))
        eng->invoke("on_heartbeat");
}

void RoleAPIBase::on_metrics_report_tick_()
{
    auto *bc = pImpl->broker_channel;
    if (!bc)
        return;

    LOGGER_TRACE("[{}] ctrl: sending metrics report", pImpl->role_tag);
    bc->send_metrics_report(pImpl->channel, pImpl->uid, snapshot_metrics_json());
}

bool RoleAPIBase::start_ctrl_thread(
    const hub::BrokerRequestComm::Config &connect_cfg,
    const CtrlThreadConfig &cfg)
{
    auto *bc   = pImpl->broker_channel;
    auto *core = pImpl->core;

    if (!bc)
    {
        LOGGER_ERROR("[{}] start_ctrl_thread: no BrokerRequestComm set", pImpl->role_tag);
        return false;
    }

    // ── Step 1: Connect DEALER socket to broker ────────────────────────────
    if (!connect_cfg.broker_endpoint.empty())
    {
        if (!bc->connect(connect_cfg))
        {
            LOGGER_ERROR("[{}] start_ctrl_thread: broker connect failed", pImpl->role_tag);
            return false;
        }
    }
    else
    {
        LOGGER_WARN("[{}] start_ctrl_thread: no broker endpoint configured — "
                    "running without broker", pImpl->role_tag);
    }

    // ── Step 2: Wire notification + hub-dead callbacks ──────────────────────
    LOGGER_TRACE("[{}] ctrl: wiring notification and hub-dead callbacks", pImpl->role_tag);

    bc->on_notification([core, this](const std::string &type, const nlohmann::json &body) {
        LOGGER_TRACE("[{}] ctrl: notification received: {}", pImpl->role_tag, type);
        IncomingMessage msg;
        msg.event   = type;
        msg.details = body;
        core->enqueue_message(std::move(msg));
    });

    const auto &tag = pImpl->role_tag;
    bc->on_hub_dead([core, tag]() {
        LOGGER_WARN("[{}] hub-dead: broker connection lost", tag);
        core->set_stop_reason(RoleHostCore::StopReason::HubDead);
        core->request_stop();
    });

    // ── Step 3: Spawn ctrl thread (heartbeat + poll loop) ──────────────────
    LOGGER_INFO("[{}] ctrl: starting control thread (heartbeat={}ms)",
                pImpl->role_tag, cfg.heartbeat_interval_ms);

    spawn_thread("ctrl", [this, cfg]()
    {
        auto *bc   = pImpl->broker_channel;
        auto *core = pImpl->core;

        // Heartbeat: iteration-gated (stops if script stuck → broker detects dead).
        bc->set_periodic_task(
            [this] { on_heartbeat_tick_(); },
            cfg.heartbeat_interval_ms,
            [core] { return core->iteration_count(); });

        // Metrics report: time-only (fires even when idle).
        if (cfg.report_metrics)
        {
            bc->set_periodic_task(
                [this] { on_metrics_report_tick_(); },
                cfg.heartbeat_interval_ms,
                nullptr);
        }

        LOGGER_TRACE("[{}] ctrl: entering poll loop", pImpl->role_tag);
        bc->run_poll_loop([core] {
            return core->is_running() && !core->is_shutdown_requested();
        });
        LOGGER_TRACE("[{}] ctrl: poll loop exited", pImpl->role_tag);
    });

    // ── Step 4: Register with broker (main thread, blocks) ─────────────────
    // The ctrl thread is now running and can process commands.

    // Append inbox metadata to registration payload if inbox is configured.
    auto append_inbox = [&](nlohmann::json &opts) {
        if (cfg.inbox.has_value() && cfg.inbox->has_inbox())
        {
            if (pImpl->inbox_queue)
                opts["inbox_endpoint"] = pImpl->inbox_queue->actual_endpoint();
            opts["inbox_schema_json"] = cfg.inbox->schema_fields_json;
            opts["inbox_packing"]     = cfg.inbox->packing;
            opts["inbox_checksum"]    = cfg.inbox->checksum;
        }
    };

    if (!cfg.producer_reg_opts.empty())
    {
        LOGGER_INFO("[{}] ctrl: registering producer channel with broker",
                    pImpl->role_tag);
        nlohmann::json reg = cfg.producer_reg_opts;
        append_inbox(reg);
        auto result = register_producer_channel(reg);
        if (!result.has_value())
        {
            LOGGER_ERROR("[{}] Broker producer registration failed", pImpl->role_tag);
            return false;
        }
        pImpl->registered_producer_channel = reg.value("channel_name", "");
    }

    if (!cfg.consumer_reg_opts.empty())
    {
        LOGGER_INFO("[{}] ctrl: registering consumer with broker", pImpl->role_tag);
        nlohmann::json reg = cfg.consumer_reg_opts;
        append_inbox(reg);
        auto result = register_consumer(reg);
        if (!result.has_value())
        {
            LOGGER_ERROR("[{}] Broker consumer registration failed", pImpl->role_tag);
            return false;
        }
        pImpl->registered_consumer_channel = reg.value("channel_name", "");
    }

    LOGGER_INFO("[{}] ctrl: broker communication ready", pImpl->role_tag);
    return true;
}

// ============================================================================
// Inbox drain
// ============================================================================

void RoleAPIBase::drain_inbox_sync()
{
    auto *iq = pImpl->inbox_queue;
    auto *eng = pImpl->engine;
    if (!iq || !eng)
        return;

    while (true)
    {
        const auto *item = iq->recv_one(std::chrono::milliseconds{0});
        if (!item)
            break;

        eng->invoke_on_inbox(InvokeInbox{
            item->data, iq->item_size(),
            item->sender_id, item->seq});

        iq->send_ack(0);
    }
}

// ============================================================================
// Event callback wiring
// ============================================================================

void RoleAPIBase::wire_event_callbacks()
{
    auto *core = pImpl->core;
    const std::string &tag = pImpl->role_tag;

    // For processor, channel names distinguish input vs output in events.
    const std::string &in_ch  = pImpl->channel;
    const std::string &out_ch_name = pImpl->out_channel;
    const bool is_dual = (pImpl->consumer != nullptr && pImpl->producer != nullptr);

    // ── Consumer-side callbacks ──────────────────────────────────────────
    if (auto *c = pImpl->consumer)
    {
        c->on_channel_closing([core, tag, in_ch, is_dual]() {
            LOGGER_INFO("[{}] CHANNEL_CLOSING_NOTIFY received, queuing event", tag);
            IncomingMessage msg;
            msg.event = "channel_closing";
            if (is_dual)
                msg.details["channel"] = in_ch;
            core->enqueue_message(std::move(msg));
        });

        c->on_force_shutdown([core, tag]() {
            LOGGER_WARN("[{}] FORCE_SHUTDOWN received, forcing immediate shutdown", tag);
            core->request_stop();
        });
    }

    // ── Producer-side callbacks ──────────────────────────────────────────
    if (auto *p = pImpl->producer)
    {
        p->on_channel_closing([core, tag, out_ch_name, is_dual]() {
            IncomingMessage msg;
            msg.event = "channel_closing";
            if (is_dual)
                msg.details["channel"] = out_ch_name;
            core->enqueue_message(std::move(msg));
        });

        p->on_force_shutdown([core]() {
            core->request_stop();
        });
    }

    // Note: broker notifications (CONSUMER_DIED_NOTIFY, CHANNEL_EVENT_NOTIFY,
    // hub-dead) are all handled by BrokerRequestComm's on_notification() and
    // on_hub_dead() callbacks wired in start_ctrl_thread(). No Messenger
    // callback wiring is needed.
}

// ============================================================================
// retry_acquire — shared inner retry logic
// ============================================================================

void *retry_acquire(
    const AcquireContext &ctx,
    RoleHostCore &core,
    const std::function<void *(std::chrono::milliseconds)> &try_once)
{
    while (true)
    {
        void *ptr = try_once(ctx.short_timeout);
        if (ptr != nullptr)
            return ptr;

        if (ctx.is_max_rate)
            return nullptr; // MaxRate: single attempt

        // Check shutdown between retries.
        if (!core.is_running() ||
            core.is_shutdown_requested() ||
            core.is_critical_error())
        {
            return nullptr;
        }
        if (core.is_process_exit_requested())
            return nullptr;

        // First cycle (deadline == max): retry indefinitely until success or shutdown.
        if (ctx.deadline != std::chrono::steady_clock::time_point::max())
        {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    ctx.deadline - std::chrono::steady_clock::now());
            if (remaining <= ctx.short_timeout_us)
                return nullptr; // not enough time to retry
        }
        // else: retry
    }
}

// ============================================================================
// run_data_loop — shared loop frame
// ============================================================================

void RoleAPIBase::run_data_loop(const LoopConfig &cfg, RoleCycleOps &ops)
{
    auto &core = *pImpl->core;
    const std::string &tag = pImpl->role_tag;

    // ── Timing setup ────────────────────────────────────────────────────
    const auto policy     = cfg.loop_timing;
    const double period_us = cfg.period_us;
    const bool is_max_rate = (policy == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, cfg.queue_io_wait_timeout_ratio);
    const auto short_timeout    = std::chrono::duration_cast<std::chrono::milliseconds>(
        short_timeout_us + std::chrono::microseconds{999});

    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::time_point::max();

    // ── Outer loop ──────────────────────────────────────────────────────
    while (core.is_running() &&
           !core.is_shutdown_requested() &&
           !core.is_critical_error())
    {
        if (core.is_process_exit_requested())
            break;

        const auto cycle_start = Clock::now();

        // ── Step A: Role-specific acquire ────────────────────────────
        AcquireContext ctx{short_timeout, short_timeout_us, deadline, is_max_rate};
        bool has_data = ops.acquire(ctx);

        // ── Step B: Deadline wait ────────────────────────────────────
        if (!is_max_rate && has_data &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        // ── Step B': Shutdown check after potential sleep ────────────
        if (!core.is_running() ||
            core.is_shutdown_requested() ||
            core.is_critical_error())
        {
            ops.cleanup_on_shutdown();
            break;
        }
        if (core.is_process_exit_requested())
        {
            ops.cleanup_on_shutdown();
            break;
        }

        // ── Step C: Drain messages + inbox ──────────────────────────
        auto msgs = core.drain_messages();
        drain_inbox_sync();

        // ── Step D+E: Role-specific invoke + commit ─────────────────
        if (!ops.invoke_and_commit(msgs))
            break;

        // ── Step F: Metrics ─────────────────────────────────────────
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        core.set_last_cycle_work_us(work_us);
        core.inc_iteration_count();
        if (deadline != Clock::time_point::max() && now > deadline)
            core.inc_loop_overrun();

        // ── Step G: Next deadline ───────────────────────────────────
        deadline = compute_next_deadline(policy, deadline, cycle_start, period_us);
    }

    // ── Post-loop cleanup ───────────────────────────────────────────
    ops.cleanup_on_exit();

    LOGGER_INFO("[{}] run_data_loop exiting: running={} shutdown={} critical={}",
                tag, core.is_running(), core.is_shutdown_requested(),
                core.is_critical_error());
}

// ============================================================================
// Identity
// ============================================================================

const std::string &RoleAPIBase::role_tag() const   { return pImpl->role_tag; }
const std::string &RoleAPIBase::uid() const        { return pImpl->uid; }
const std::string &RoleAPIBase::name() const       { return pImpl->name; }
const std::string &RoleAPIBase::channel() const    { return pImpl->channel; }
const std::string &RoleAPIBase::out_channel() const { return pImpl->out_channel; }
const std::string &RoleAPIBase::log_level() const  { return pImpl->log_level; }
const std::string &RoleAPIBase::script_dir() const { return pImpl->script_dir; }
const std::string &RoleAPIBase::role_dir() const   { return pImpl->role_dir; }

std::string RoleAPIBase::logs_dir() const
{
    return pImpl->role_dir.empty() ? std::string{} : pImpl->role_dir + "/logs";
}

std::string RoleAPIBase::run_dir() const
{
    return pImpl->role_dir.empty() ? std::string{} : pImpl->role_dir + "/run";
}

hub::ChecksumPolicy RoleAPIBase::checksum_policy() const { return pImpl->checksum_policy; }
bool RoleAPIBase::stop_on_script_error() const { return pImpl->stop_on_script_error; }

// ============================================================================
// Control
// ============================================================================

void RoleAPIBase::log(const std::string &level, const std::string &msg)
{
    if (level == "debug" || level == "Debug")
        LOGGER_DEBUG("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
    else if (level == "warn" || level == "Warn" || level == "warning")
        LOGGER_WARN("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
    else if (level == "error" || level == "Error")
        LOGGER_ERROR("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
    else
        LOGGER_INFO("[{}/{}] {}", pImpl->role_tag, pImpl->uid, msg);
}

void RoleAPIBase::stop()                        { pImpl->core->request_stop(); }
void RoleAPIBase::set_critical_error()          { pImpl->core->set_critical_error(); }
bool RoleAPIBase::critical_error() const        { return pImpl->core->is_critical_error(); }
std::string RoleAPIBase::stop_reason() const    { return pImpl->core->stop_reason_string(); }

// ============================================================================
// Band pub/sub (HEP-CORE-0030)
// ============================================================================

std::optional<nlohmann::json> RoleAPIBase::band_join(const std::string &channel)
{
    if (!pImpl->broker_channel)
        return std::nullopt;
    return pImpl->broker_channel->band_join(channel);
}

bool RoleAPIBase::band_leave(const std::string &channel)
{
    if (!pImpl->broker_channel)
        return false;
    return pImpl->broker_channel->band_leave(channel);
}

void RoleAPIBase::band_broadcast(const std::string &channel,
                                  const nlohmann::json &body)
{
    if (pImpl->broker_channel)
        pImpl->broker_channel->band_broadcast(channel, body);
}

std::optional<nlohmann::json> RoleAPIBase::band_members(const std::string &channel)
{
    if (!pImpl->broker_channel)
        return std::nullopt;
    return pImpl->broker_channel->band_members(channel);
}

// ============================================================================
// Inbox client management
// ============================================================================

std::optional<RoleAPIBase::InboxOpenResult>
RoleAPIBase::open_inbox_client(const std::string &target_uid)
{
    if (!pImpl->core || !pImpl->broker_channel)
        return std::nullopt;

    hub::SchemaSpec result_spec;
    std::string result_packing;

    auto entry = pImpl->core->open_inbox(target_uid,
        [&]() -> std::optional<RoleHostCore::InboxCacheEntry>
        {
            auto info = pImpl->broker_channel->query_role_info(target_uid, 1000);
            if (!info.has_value())
                return std::nullopt;

            auto inbox_schema = info->value("inbox_schema", nlohmann::json{});
            if (!inbox_schema.is_object() || !inbox_schema.contains("fields"))
                return std::nullopt;

            auto inbox_packing  = info->value("inbox_packing", std::string{});
            auto inbox_endpoint = info->value("inbox_endpoint", std::string{});
            auto inbox_checksum = info->value("inbox_checksum", std::string{});

            hub::SchemaSpec spec;
            try
            {
                spec = hub::parse_schema_json(inbox_schema);
            }
            catch (const std::exception &e)
            {
                LOGGER_WARN("[api] open_inbox('{}'): schema parse error: {}",
                            target_uid, e.what());
                return std::nullopt;
            }

            size_t item_size = hub::compute_schema_size(spec, inbox_packing);

            auto zmq_fields = hub::schema_spec_to_zmq_fields(spec);

            auto client_ptr = hub::InboxClient::connect_to(
                inbox_endpoint, pImpl->uid,
                std::move(zmq_fields), inbox_packing);
            if (!client_ptr)
            {
                LOGGER_WARN("[api] open_inbox('{}'): connect failed", target_uid);
                return std::nullopt;
            }
            if (!client_ptr->start())
            {
                LOGGER_WARN("[api] open_inbox('{}'): start failed", target_uid);
                return std::nullopt;
            }
            client_ptr->set_checksum_policy(
                config::string_to_checksum_policy(inbox_checksum));

            result_spec = std::move(spec);
            result_packing = inbox_packing;

            return RoleHostCore::InboxCacheEntry{
                std::shared_ptr<hub::InboxClient>(std::move(client_ptr)),
                "InboxSlot", item_size};
        });

    if (!entry)
        return std::nullopt;

    return InboxOpenResult{
        entry->client, std::move(result_spec),
        std::move(result_packing), entry->item_size};
}

bool RoleAPIBase::wait_for_role(const std::string &uid, int timeout_ms)
{
    if (!pImpl->broker_channel)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pImpl->broker_channel->query_role_presence(uid, kPollMs))
            return true;
    }
    return false;
}

void RoleAPIBase::close_all_inbox_clients()
{
    pImpl->core->clear_inbox_cache();
}

// ============================================================================
// Output side — flat data-plane verbs
// ============================================================================

void *RoleAPIBase::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    return pImpl->tx_queue ? pImpl->tx_queue->write_acquire(timeout) : nullptr;
}

void RoleAPIBase::write_commit() noexcept
{
    if (pImpl->tx_queue) pImpl->tx_queue->write_commit();
}

void RoleAPIBase::write_discard() noexcept
{
    if (pImpl->tx_queue) pImpl->tx_queue->write_discard();
}

size_t RoleAPIBase::write_item_size() const noexcept
{
    return pImpl->tx_queue ? pImpl->tx_queue->item_size() : 0;
}

bool RoleAPIBase::sync_flexzone_checksum()
{
    if (!pImpl->tx_queue) return false;
    pImpl->tx_queue->sync_flexzone_checksum();
    return true;
}

void *RoleAPIBase::flexzone(ChannelSide side)
{
    if (side == ChannelSide::Tx)
        return pImpl->tx_queue ? pImpl->tx_queue->flexzone() : nullptr;
    return pImpl->rx_queue ? pImpl->rx_queue->flexzone() : nullptr;
}

size_t RoleAPIBase::flexzone_size(ChannelSide side) const noexcept
{
    if (side == ChannelSide::Tx)
        return pImpl->tx_queue ? pImpl->tx_queue->flexzone_size() : 0;
    return pImpl->rx_queue ? pImpl->rx_queue->flexzone_size() : 0;
}

bool RoleAPIBase::update_flexzone_checksum()
{
    if (!pImpl->tx_queue)
        return false;
    pImpl->tx_queue->sync_flexzone_checksum();
    return true;
}

uint64_t RoleAPIBase::out_slots_written() const { return pImpl->core->out_slots_written(); }
uint64_t RoleAPIBase::out_drop_count() const    { return pImpl->core->out_drop_count(); }

size_t RoleAPIBase::out_capacity() const
{
    return pImpl->tx_queue ? pImpl->tx_queue->capacity() : 0;
}

std::string RoleAPIBase::out_policy() const
{
    return pImpl->tx_queue ? pImpl->tx_queue->policy_info() : std::string{};
}

// ============================================================================
// Input side — flat data-plane verbs
// ============================================================================

const void *RoleAPIBase::read_acquire(std::chrono::milliseconds timeout) noexcept
{
    return pImpl->rx_queue ? pImpl->rx_queue->read_acquire(timeout) : nullptr;
}

void RoleAPIBase::read_release() noexcept
{
    if (pImpl->rx_queue) pImpl->rx_queue->read_release();
}

size_t RoleAPIBase::read_item_size() const noexcept
{
    return pImpl->rx_queue ? pImpl->rx_queue->item_size() : 0;
}

uint64_t RoleAPIBase::in_slots_received() const { return pImpl->core->in_slots_received(); }

uint64_t RoleAPIBase::last_seq() const
{
    // Forward directly to QueueReader — single source of truth.
    // Thread-safe: ShmQueue uses plain uint64 (aligned, no torn read),
    // ZmqQueue uses atomic<uint64_t> with relaxed ordering.
    return pImpl->rx_queue ? pImpl->rx_queue->last_seq() : 0;
}


size_t RoleAPIBase::in_capacity() const
{
    return pImpl->rx_queue ? pImpl->rx_queue->capacity() : 0;
}

std::string RoleAPIBase::in_policy() const
{
    return pImpl->rx_queue ? pImpl->rx_queue->policy_info() : std::string{};
}

void RoleAPIBase::set_verify_checksum(bool enable)
{
    // set_verify_checksum(slot,fz) is not yet on the QueueReader base
    // interface — still routes through hub::Consumer until L3.γ migrates
    // the hub-layer API into QueueReader.
    if (pImpl->consumer)
        pImpl->consumer->set_verify_checksum(enable, false);
}

// ============================================================================
// Diagnostics
// ============================================================================

uint64_t RoleAPIBase::script_error_count() const { return pImpl->core->script_error_count(); }
uint64_t RoleAPIBase::loop_overrun_count() const { return pImpl->core->loop_overrun_count(); }
uint64_t RoleAPIBase::last_cycle_work_us() const { return pImpl->core->last_cycle_work_us(); }

uint64_t RoleAPIBase::ctrl_queue_dropped() const
{
    return 0; // P2C ctrl queue removed — no drops to track
}

// ============================================================================
// Schema sizes
// ============================================================================

size_t RoleAPIBase::slot_logical_size(std::optional<ChannelSide> side) const
{
    const bool has_tx = pImpl->core->has_out_slot();
    const bool has_rx = pImpl->core->has_in_slot();

    if (side.has_value())
        return (*side == ChannelSide::Tx)
            ? (has_tx ? pImpl->core->out_slot_logical_size() : 0)
            : (has_rx ? pImpl->core->in_slot_logical_size()  : 0);

    if (has_tx && has_rx)
        throw std::runtime_error("slot_logical_size: side parameter required for processor");
    return has_tx ? pImpl->core->out_slot_logical_size()
         : has_rx ? pImpl->core->in_slot_logical_size()
         : 0;
}

size_t RoleAPIBase::flexzone_logical_size(std::optional<ChannelSide> side) const
{
    // Returns the C struct size (logical) for the flexzone schema.
    // Recomputed from the stored SchemaSpec — core stores the physical (page-aligned) size,
    // not the logical size. The spec + packing are the source of truth.
    auto compute = [](const hub::SchemaSpec &spec) -> size_t {
        if (!spec.has_schema) return 0;
        auto [layout, sz] = hub::compute_field_layout(
            hub::to_field_descs(spec.fields), spec.packing);
        return sz;
    };

    const bool has_tx = pImpl->core->has_out_fz();
    const bool has_rx = pImpl->core->has_in_fz();

    if (side.has_value())
    {
        return (*side == ChannelSide::Tx)
            ? compute(pImpl->core->out_fz_spec())
            : compute(pImpl->core->in_fz_spec());
    }

    if (has_tx && has_rx)
        throw std::runtime_error("flexzone_logical_size: side parameter required for processor");

    if (has_tx) return compute(pImpl->core->out_fz_spec());
    if (has_rx) return compute(pImpl->core->in_fz_spec());
    return 0;
}

// ============================================================================
// Spinlocks
// ============================================================================

hub::SharedSpinLock RoleAPIBase::get_spinlock(size_t index, std::optional<ChannelSide> side)
{
    const bool has_tx = pImpl->tx_queue != nullptr;
    const bool has_rx = pImpl->rx_queue != nullptr;

    if (side.has_value())
    {
        if (*side == ChannelSide::Tx)
        {
            if (!has_tx)
                throw std::runtime_error("get_spinlock(Tx): no Tx queue connected");
            return pImpl->tx_queue->get_spinlock(index);
        }
        if (!has_rx)
            throw std::runtime_error("get_spinlock(Rx): no Rx queue connected");
        return pImpl->rx_queue->get_spinlock(index);
    }

    // No side specified — auto-select for single-side roles, error for dual.
    if (has_tx && has_rx)
        throw std::runtime_error(
            "get_spinlock: side parameter required for processor "
            "(use ChannelSide::Tx or ChannelSide::Rx)");
    if (has_tx)
        return pImpl->tx_queue->get_spinlock(index);
    if (has_rx)
        return pImpl->rx_queue->get_spinlock(index);
    throw std::runtime_error("get_spinlock: no Tx or Rx queue connected");
}

uint32_t RoleAPIBase::spinlock_count(std::optional<ChannelSide> side) const
{
    const bool has_tx = pImpl->tx_queue != nullptr;
    const bool has_rx = pImpl->rx_queue != nullptr;

    if (side.has_value())
    {
        if (*side == ChannelSide::Tx)
            return has_tx ? pImpl->tx_queue->spinlock_count() : 0;
        return has_rx ? pImpl->rx_queue->spinlock_count() : 0;
    }

    // No side specified — auto-select for single-side roles, error for dual.
    if (has_tx && has_rx)
        throw std::runtime_error(
            "spinlock_count: side parameter required for processor "
            "(use ChannelSide::Tx or ChannelSide::Rx)");
    if (has_tx)
        return pImpl->tx_queue->spinlock_count();
    if (has_rx)
        return pImpl->rx_queue->spinlock_count();
    return 0;
}

// ============================================================================
// Custom metrics
// ============================================================================

void RoleAPIBase::report_metric(const std::string &key, double value)
{
    pImpl->core->report_metric(key, value);
}

void RoleAPIBase::report_metrics(const std::unordered_map<std::string, double> &kv)
{
    pImpl->core->report_metrics(kv);
}

void RoleAPIBase::clear_custom_metrics()
{
    pImpl->core->clear_custom_metrics();
}

// ============================================================================
// Metrics snapshot — data-driven structure
// ============================================================================

nlohmann::json RoleAPIBase::snapshot_metrics_json() const
{
    nlohmann::json result;
    const bool has_in  = (pImpl->rx_queue != nullptr);
    const bool has_out = (pImpl->tx_queue != nullptr);

    // Queue metrics: key depends on which sides exist.
    if (has_in && has_out)
    {
        nlohmann::json iq, oq;
        hub::queue_metrics_to_json(iq, pImpl->rx_queue->metrics());
        hub::queue_metrics_to_json(oq, pImpl->tx_queue->metrics());
        result["in_queue"] = std::move(iq);
        result["out_queue"] = std::move(oq);
    }
    else if (has_out)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->tx_queue->metrics());
        result["queue"] = std::move(q);
    }
    else if (has_in)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->rx_queue->metrics());
        result["queue"] = std::move(q);
    }

    // Loop metrics.
    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, pImpl->core->loop_metrics());
        result["loop"] = std::move(lm);
    }

    // Role metrics — core counters always present, queue-specific gated on pointers.
    nlohmann::json role;
    role["out_slots_written"]  = pImpl->core->out_slots_written();
    role["in_slots_received"]  = pImpl->core->in_slots_received();
    role["out_drop_count"]     = pImpl->core->out_drop_count();
    role["script_error_count"] = pImpl->core->script_error_count();

    role["ctrl_queue_dropped"] = 0; // P2C ctrl queue removed
    result["role"] = std::move(role);

    // Inbox metrics.
    if (pImpl->inbox_queue)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, pImpl->inbox_queue->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    // Custom metrics.
    {
        auto cm = pImpl->core->custom_metrics_snapshot();
        if (!cm.empty())
            result["custom"] = nlohmann::json(cm);
    }

    return result;
}

// ============================================================================
// Shared script state — delegates to RoleHostCore
// ============================================================================

void RoleAPIBase::set_shared_data(const std::string &key, StateValue value)
{
    pImpl->core->set_shared_data(key, std::move(value));
}

std::optional<RoleAPIBase::StateValue> RoleAPIBase::get_shared_data(const std::string &key) const
{
    return pImpl->core->get_shared_data(key);
}

void RoleAPIBase::remove_shared_data(const std::string &key)
{
    pImpl->core->remove_shared_data(key);
}

void RoleAPIBase::clear_shared_data()
{
    pImpl->core->clear_shared_data();
}

// ============================================================================
// Infrastructure access
// ============================================================================

RoleHostCore *RoleAPIBase::core() const     { return pImpl->core; }
hub::Producer *RoleAPIBase::producer() const { return pImpl->producer; }
hub::Consumer *RoleAPIBase::consumer() const { return pImpl->consumer; }
hub::InboxQueue *RoleAPIBase::inbox_queue() const { return pImpl->inbox_queue; }

bool RoleAPIBase::has_tx_side() const noexcept { return pImpl->tx_queue != nullptr; }
bool RoleAPIBase::has_rx_side() const noexcept { return pImpl->rx_queue != nullptr; }

hub::QueueMetrics RoleAPIBase::queue_metrics(ChannelSide side) const noexcept
{
    if (side == ChannelSide::Tx)
        return pImpl->tx_queue ? pImpl->tx_queue->metrics() : hub::QueueMetrics{};
    return pImpl->rx_queue ? pImpl->rx_queue->metrics() : hub::QueueMetrics{};
}

} // namespace pylabhub::scripting
