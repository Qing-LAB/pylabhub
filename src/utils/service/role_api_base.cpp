/**
 * @file role_api_base.cpp
 * @brief RoleAPIBase implementation — unified role API (pure C++).
 */
#include "utils/role_api_base.hpp"
#include "utils/broker_request_channel.hpp"
#include "utils/script_engine.hpp"        // ScriptEngine, InvokeInbox, ThreadEngineGuard
#include "utils/zmq_poll_loop.hpp"        // ZmqPollLoop, PeriodicTask

#include "utils/config/checksum_config.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_producer.hpp"
#include "utils/logger.hpp"
#include "utils/messenger.hpp"
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
    hub::Messenger  *messenger{nullptr};
    hub::InboxQueue          *inbox_queue{nullptr};
    ScriptEngine             *engine{nullptr};
    hub::BrokerRequestChannel *broker_channel{nullptr};

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
void RoleAPIBase::set_producer(hub::Producer *p)      { pImpl->producer = p; }
void RoleAPIBase::set_consumer(hub::Consumer *c)      { pImpl->consumer = c; }
void RoleAPIBase::set_messenger(hub::Messenger *m)    { pImpl->messenger = m; }
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
    auto *eng = pImpl->engine;
    const std::string &tag = pImpl->role_tag;

    pImpl->managed_threads_.emplace_back(
        [eng, tag, name, body = std::move(body)]()
        {
            // ThreadEngineGuard registers this thread with the engine for
            // cross-thread dispatch (Lua: thread-local state; Python: queue).
            ThreadEngineGuard guard(*eng);
            LOGGER_INFO("[{}/{}] thread started", tag, name);
            body();
            LOGGER_INFO("[{}/{}] thread exiting", tag, name);
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
// set_broker_channel
// ============================================================================

void RoleAPIBase::set_broker_channel(hub::BrokerRequestChannel *bc)
{
    pImpl->broker_channel = bc;
}

// ============================================================================
// Broker thread — runs BrokerRequestChannel poll loop + heartbeat
// ============================================================================

void RoleAPIBase::start_broker_thread(const BrokerThreadConfig &cfg)
{
    auto *core = pImpl->core;
    auto *bc   = pImpl->broker_channel;
    auto *eng  = pImpl->engine;
    const auto &ch     = pImpl->channel;
    const auto &out_ch = pImpl->out_channel;
    const auto &uid    = pImpl->uid;

    if (!bc)
    {
        LOGGER_ERROR("[{}] start_broker_thread: no BrokerRequestChannel set",
                     pImpl->role_tag);
        return;
    }

    // Wire broker notification callback → core_.enqueue_message().
    // Channel notifications (JOIN/LEAVE/MSG_NOTIFY) and broker lifecycle
    // notifications all arrive here and become entries in the script's msgs.
    bc->on_notification([core](const std::string &type, const nlohmann::json &body) {
        IncomingMessage msg;
        msg.event = type;
        msg.details = body;
        core->enqueue_message(std::move(msg));
    });

    // Wire hub-dead callback.
    const auto &tag = pImpl->role_tag;
    bc->on_hub_dead([core, tag]() {
        LOGGER_WARN("[{}] hub-dead: BrokerRequestChannel connection lost", tag);
        core->set_stop_reason(RoleHostCore::StopReason::HubDead);
        core->request_stop();
    });

    spawn_thread("broker", [=, this]()
    {
        const bool has_hb = eng->has_callback("on_heartbeat");
        std::string hb_channel = out_ch.empty() ? ch : out_ch;

        // Heartbeat: iteration-gated (stops if script is stuck → broker detects dead).
        bc->set_periodic_task(
            [bc, eng, has_hb, hb_channel, this]
            {
                bc->send_heartbeat(hb_channel, snapshot_metrics_json());
                if (has_hb)
                    eng->invoke("on_heartbeat");
            },
            cfg.heartbeat_interval_ms,
            [core] { return core->iteration_count(); });

        // Metrics report: time-only (fires even when idle).
        if (cfg.report_metrics)
        {
            bc->set_periodic_task(
                [bc, this, ch, uid]
                {
                    bc->send_metrics_report(ch, uid, snapshot_metrics_json());
                },
                cfg.heartbeat_interval_ms,
                nullptr);
        }

        bc->run_poll_loop([core] {
            return core->is_running() && !core->is_shutdown_requested();
        });
    });
}

// ============================================================================
// P2C comm thread — polls Producer/Consumer sockets
// ============================================================================

void RoleAPIBase::start_comm_thread()
{
    auto *core = pImpl->core;
    auto *prod = pImpl->producer;
    auto *cons = pImpl->consumer;
    const auto &tag = pImpl->role_tag;
    const auto &uid = pImpl->uid;

    spawn_thread("comm", [=]()
    {
        ZmqPollLoop loop{
            [core] {
                return core->is_running() && !core->is_shutdown_requested();
            },
            tag + ":comm:" + uid};

        if (prod)
        {
            auto *h = prod->peer_ctrl_socket_handle();
            if (h)
            {
                loop.sockets.push_back(
                    {zmq::socket_ref(zmq::from_handle, h),
                     [prod] { prod->handle_peer_events_nowait(); }});
            }
        }
        if (cons)
        {
            auto *h = cons->ctrl_zmq_socket_handle();
            if (h)
            {
                loop.sockets.push_back(
                    {zmq::socket_ref(zmq::from_handle, h),
                     [cons] { cons->handle_ctrl_events_nowait(); }});
            }
        }
        if (cons)
        {
            auto *ds = cons->data_zmq_socket_handle();
            if (ds)
            {
                loop.sockets.push_back(
                    {zmq::socket_ref(zmq::from_handle, ds),
                     [cons] { cons->handle_data_events_nowait(); }});
            }
        }

        loop.run();
    });
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

        // Always register on_zmq_data — harmless if no data arrives on the
        // socket (eliminates SHM-vs-ZMQ conditional; see tech draft §7.3).
        c->on_zmq_data([core, tag](std::span<const std::byte> data) {
            LOGGER_DEBUG("[{}] zmq_data: data_message size={}", tag, data.size());
            IncomingMessage msg;
            msg.data.assign(data.begin(), data.end());
            core->enqueue_message(std::move(msg));
        });

        c->on_producer_message(
            [core, tag](std::string_view type, std::span<const std::byte> data) {
                LOGGER_INFO("[{}] ctrl_msg: producer_message type='{}' size={}",
                            tag, type, data.size());
                IncomingMessage msg;
                msg.event = "producer_message";
                msg.details["type"] = std::string(type);
                msg.details["data"] = format_tools::bytes_to_hex(
                    {reinterpret_cast<const char *>(data.data()), data.size()});
                core->enqueue_message(std::move(msg));
            });

        c->on_channel_error(
            [core, tag, is_dual](const std::string &event, const nlohmann::json &details) {
                LOGGER_INFO("[{}] broker_notify: channel_event event='{}' details={}",
                            tag, event, details.dump());
                IncomingMessage msg;
                msg.event = "channel_event";
                msg.details = details;
                msg.details["detail"] = event;
                if (is_dual)
                    msg.details["source"] = "in_channel";
                core->enqueue_message(std::move(msg));
            });

        c->on_peer_dead([core, tag]() {
            LOGGER_WARN("[{}] peer-dead: upstream producer silent; triggering shutdown", tag);
            core->set_stop_reason(RoleHostCore::StopReason::PeerDead);
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

        p->on_consumer_joined([core, tag](const std::string &identity) {
            auto hex = format_tools::bytes_to_hex(identity);
            LOGGER_INFO("[{}] peer_event: consumer_joined identity={}", tag, hex);
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = std::move(hex);
            core->enqueue_message(std::move(msg));
        });

        p->on_consumer_left([core, tag](const std::string &identity) {
            auto hex = format_tools::bytes_to_hex(identity);
            LOGGER_INFO("[{}] peer_event: consumer_left identity={}", tag, hex);
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = std::move(hex);
            core->enqueue_message(std::move(msg));
        });

        p->on_consumer_message(
            [core, tag](const std::string &identity, std::span<const std::byte> data) {
                LOGGER_INFO("[{}] zmq_data: consumer_message size={}", tag, data.size());
                IncomingMessage msg;
                msg.sender = identity;
                msg.data.assign(data.begin(), data.end());
                core->enqueue_message(std::move(msg));
            });

        p->on_peer_dead([core, tag]() {
            LOGGER_WARN("[{}] peer-dead: downstream consumer silent; triggering shutdown", tag);
            core->set_stop_reason(RoleHostCore::StopReason::PeerDead);
            core->request_stop();
        });
    }

    // ── Messenger-level callbacks (per-channel) ─────────────────────────
    if (auto *m = pImpl->messenger)
    {
        // on_consumer_died and on_channel_error are producer-side messenger
        // events — only register when we have a producer (i.e., producer-only
        // or processor roles). Consumer-only roles don't need these.
        if (pImpl->producer)
        {
            std::string out_ch = pImpl->out_channel.empty()
                                     ? pImpl->channel : pImpl->out_channel;

            if (!out_ch.empty())
            {
                m->on_consumer_died(out_ch,
                    [core, tag](uint64_t pid, std::string reason) {
                        LOGGER_INFO("[{}] broker_notify: consumer_died pid={} reason={}",
                                    tag, pid, reason);
                        IncomingMessage msg;
                        msg.event = "consumer_died";
                        msg.details["pid"] = pid;
                        msg.details["reason"] = std::move(reason);
                        core->enqueue_message(std::move(msg));
                    });

                m->on_channel_error(out_ch,
                    [core, tag, is_dual](std::string event, nlohmann::json details) {
                        LOGGER_INFO("[{}] broker_notify: channel_event event='{}' details={}",
                                    tag, event, details.dump());
                        IncomingMessage msg;
                        msg.event = "channel_event";
                        msg.details = std::move(details);
                        msg.details["detail"] = std::move(event);
                        if (is_dual)
                            msg.details["source"] = "out_channel";
                        core->enqueue_message(std::move(msg));
                    });
            }
        }

        m->on_hub_dead([core, tag]() {
            LOGGER_WARN("[{}] hub-dead: broker connection lost; triggering shutdown", tag);
            core->set_stop_reason(RoleHostCore::StopReason::HubDead);
            core->request_stop();
        });
    }

    // ── Processor dual-messenger: hub-dead on both ──────────────────────
    // For processor, the in_messenger and out_messenger may be different.
    // The role host sets messenger to out_messenger. If the processor has
    // a separate in_messenger, it must wire hub_dead on that one separately
    // (the role host handles this since it owns both messenger instances).
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
    if (!pImpl->core || !pImpl->messenger)
        return std::nullopt;

    hub::SchemaSpec result_spec;
    std::string result_packing;

    auto entry = pImpl->core->open_inbox(target_uid,
        [&]() -> std::optional<RoleHostCore::InboxCacheEntry>
        {
            auto info = pImpl->messenger->query_role_info(target_uid, 1000);
            if (!info.has_value())
                return std::nullopt;

            if (!info->inbox_schema.is_object() ||
                !info->inbox_schema.contains("fields"))
                return std::nullopt;

            hub::SchemaSpec spec;
            try
            {
                spec = hub::parse_schema_json(info->inbox_schema);
            }
            catch (const std::exception &e)
            {
                LOGGER_WARN("[api] open_inbox('{}'): schema parse error: {}",
                            target_uid, e.what());
                return std::nullopt;
            }

            size_t item_size = hub::compute_schema_size(spec, info->inbox_packing);

            auto zmq_fields = hub::schema_spec_to_zmq_fields(spec);

            auto client_ptr = hub::InboxClient::connect_to(
                info->inbox_endpoint, pImpl->uid,
                std::move(zmq_fields), info->inbox_packing);
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
                config::string_to_checksum_policy(info->inbox_checksum));

            result_spec = std::move(spec);
            result_packing = info->inbox_packing;

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
    if (!pImpl->messenger)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pImpl->messenger->query_role_presence(uid, kPollMs))
            return true;
    }
    return false;
}

void RoleAPIBase::close_all_inbox_clients()
{
    pImpl->core->clear_inbox_cache();
}

// ============================================================================
// Output side
// ============================================================================

void *RoleAPIBase::write_flexzone()
{
    return pImpl->producer ? pImpl->producer->write_flexzone() : nullptr;
}

const void *RoleAPIBase::read_flexzone() const
{
    return pImpl->producer ? pImpl->producer->read_flexzone() : nullptr;
}

size_t RoleAPIBase::flexzone_size() const
{
    return pImpl->producer ? pImpl->producer->flexzone_size() : 0;
}

bool RoleAPIBase::update_flexzone_checksum()
{
    if (!pImpl->producer)
        return false;
    pImpl->producer->sync_flexzone_checksum();
    return true;
}

uint64_t RoleAPIBase::out_slots_written() const { return pImpl->core->out_slots_written(); }
uint64_t RoleAPIBase::out_drop_count() const    { return pImpl->core->out_drop_count(); }

size_t RoleAPIBase::out_capacity() const
{
    return pImpl->producer ? pImpl->producer->queue_capacity() : 0;
}

std::string RoleAPIBase::out_policy() const
{
    return pImpl->producer ? pImpl->producer->queue_policy_info() : std::string{};
}

// ============================================================================
// Input side
// ============================================================================

uint64_t RoleAPIBase::in_slots_received() const { return pImpl->core->in_slots_received(); }

uint64_t RoleAPIBase::last_seq() const
{
    // Forward directly to QueueReader — single source of truth.
    // Thread-safe: ShmQueue uses plain uint64 (aligned, no torn read),
    // ZmqQueue uses atomic<uint64_t> with relaxed ordering.
    return pImpl->consumer ? pImpl->consumer->last_seq() : 0;
}


size_t RoleAPIBase::in_capacity() const
{
    return pImpl->consumer ? pImpl->consumer->queue_capacity() : 0;
}

std::string RoleAPIBase::in_policy() const
{
    return pImpl->consumer ? pImpl->consumer->queue_policy_info() : std::string{};
}

void RoleAPIBase::set_verify_checksum(bool enable)
{
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
    uint64_t total = 0;
    if (pImpl->producer) total += pImpl->producer->ctrl_queue_dropped();
    if (pImpl->consumer) total += pImpl->consumer->ctrl_queue_dropped();
    return total;
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
    const bool has_tx = pImpl->producer != nullptr;
    const bool has_rx = pImpl->consumer != nullptr;

    if (side.has_value())
    {
        if (*side == ChannelSide::Tx)
        {
            if (!has_tx)
                throw std::runtime_error("get_spinlock(Tx): no producer connected");
            return pImpl->producer->get_spinlock(index);
        }
        if (!has_rx)
            throw std::runtime_error("get_spinlock(Rx): no consumer connected");
        return pImpl->consumer->get_spinlock(index);
    }

    // No side specified — auto-select for single-side roles, error for dual.
    if (has_tx && has_rx)
        throw std::runtime_error(
            "get_spinlock: side parameter required for processor "
            "(use ChannelSide::Tx or ChannelSide::Rx)");
    if (has_tx)
        return pImpl->producer->get_spinlock(index);
    if (has_rx)
        return pImpl->consumer->get_spinlock(index);
    throw std::runtime_error("get_spinlock: no producer or consumer connected");
}

uint32_t RoleAPIBase::spinlock_count(std::optional<ChannelSide> side) const
{
    const bool has_tx = pImpl->producer != nullptr;
    const bool has_rx = pImpl->consumer != nullptr;

    if (side.has_value())
    {
        if (*side == ChannelSide::Tx)
            return has_tx ? pImpl->producer->spinlock_count() : 0;
        return has_rx ? pImpl->consumer->spinlock_count() : 0;
    }

    // No side specified — auto-select for single-side roles, error for dual.
    if (has_tx && has_rx)
        throw std::runtime_error(
            "spinlock_count: side parameter required for processor "
            "(use ChannelSide::Tx or ChannelSide::Rx)");
    if (has_tx)
        return pImpl->producer->spinlock_count();
    if (has_rx)
        return pImpl->consumer->spinlock_count();
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
    const bool has_in  = (pImpl->consumer != nullptr);
    const bool has_out = (pImpl->producer != nullptr);

    // Queue metrics: key depends on which sides exist.
    if (has_in && has_out)
    {
        nlohmann::json iq, oq;
        hub::queue_metrics_to_json(iq, pImpl->consumer->queue_metrics());
        hub::queue_metrics_to_json(oq, pImpl->producer->queue_metrics());
        result["in_queue"] = std::move(iq);
        result["out_queue"] = std::move(oq);
    }
    else if (has_out)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->producer->queue_metrics());
        result["queue"] = std::move(q);
    }
    else if (has_in)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->consumer->queue_metrics());
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

    if (has_in && has_out)
    {
        role["ctrl_queue_dropped"] = {
            {"input",  pImpl->consumer->ctrl_queue_dropped()},
            {"output", pImpl->producer->ctrl_queue_dropped()}
        };
    }
    else
    {
        uint64_t dropped = 0;
        if (has_out) dropped += pImpl->producer->ctrl_queue_dropped();
        if (has_in)  dropped += pImpl->consumer->ctrl_queue_dropped();
        role["ctrl_queue_dropped"] = dropped;
    }
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

} // namespace pylabhub::scripting
