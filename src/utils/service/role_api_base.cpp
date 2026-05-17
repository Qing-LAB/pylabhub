/**
 * @file role_api_base.cpp
 * @brief RoleAPIBase implementation — unified role API (pure C++).
 */
#include "utils/role_api_base.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/role_handler.hpp"         // Wave-B M4c: handler-mode ctrl threads
#include "utils/script_engine.hpp"        // ScriptEngine, InvokeInbox, ThreadEngineGuard
#include "utils/zmq_poll_loop.hpp"        // ZmqPollLoop, PeriodicTask

#include "utils/config/checksum_config.hpp"
#include "utils/format_tools.hpp"

#include "utils/hub_inbox_queue.hpp"

#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/logger.hpp"
#include "utils/metrics_json.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"
#include "utils/shared_memory_spinlock.hpp"
#include "utils/thread_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
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
    Impl(RoleHostCore *c, std::string rt, std::string id)
        : core(c),
          role_tag(std::move(rt)),
          uid(std::move(id)),
          thread_mgr_(std::make_unique<pylabhub::utils::ThreadManager>(
              role_tag, uid))
    {}

    RoleHostCore    *core;

    // Abstract queue ownership — RoleAPIBase sees the unified interface only.
    // Factories (ShmQueue::create_*, ZmqQueue::push_to / pull_from) return
    // concrete types; build_tx_queue/build_rx_queue upcast into these handles
    // so that every path downstream goes through QueueWriter/QueueReader.
    std::unique_ptr<hub::QueueWriter> tx_queue;
    std::unique_ptr<hub::QueueReader> rx_queue;
    hub::InboxQueue          *inbox_queue{nullptr};
    ScriptEngine             *engine{nullptr};

    // ── Thread-local / set-once-before-spawn state ─────────────────────
    //
    // Every field below this banner is either:
    //   (a) set in the RoleAPIBase ctor (role_tag, uid), or
    //   (b) set via the role host's startup wiring (build_tx_queue,
    //       set_channel, set_name, set_engine, ...) which runs BEFORE
    //       `start_handler_threads()` spawns any other thread.
    //
    // The thread spawn itself is the happens-before synchronization
    // point — readers on the ctrl thread (e.g. `on_heartbeat_tick_`
    // reading `out_channel` / `channel` / `uid`) observe the final
    // value with no extra protection.  Adding a field here is a claim
    // that it will NEVER be mutated after `start_ctrl_thread()`.  If
    // that claim ever breaks, move the field into `Shared` below.
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

    // Role-side CurveZMQ keypair (Wave-B M4a) — read by
    // `RoleHandler::start_connections` to populate BRC::Config.  Empty
    // = plaintext (no CURVE).  Set-once before start_connections.
    std::string auth_client_pubkey;
    std::string auth_client_seckey;

    // ── Shared state — touched outside the worker thread ───────────────
    //
    // Anything that may be read OR mutated from threads other than the
    // worker (ctrl thread's notification dispatch / heartbeat /
    // future paths) lives below.  Access ONLY via the controlled-
    // access methods — never touch the fields directly.  The internal
    // mutex protects every field in this sub-struct.
    //
    // Locking discipline (HEP-CORE-0031 spirit applied to in-process
    // state): the mutex MUST NEVER be held across a `do_request` to
    // the broker.  `do_request` blocks the worker thread waiting on
    // the ctrl thread to send + receive a reply; if the ctrl thread
    // ever tries to take this mutex, the two threads deadlock.  All
    // methods below take the lock for a short critical section only
    // (single field read/write/swap).  Deregister flows use
    // `take_*_channel()` to atomically swap-with-empty under the
    // lock and pass the captured value to the broker RPC outside
    // the lock.
    struct Shared
    {
        mutable std::mutex mu;
        std::string producer_channel;   ///< Non-empty if REG_REQ succeeded.
        std::string consumer_channel;   ///< Non-empty if CONSUMER_REG_REQ succeeded.

        void set_producer_channel(std::string name) noexcept
        {
            std::lock_guard<std::mutex> g(mu);
            producer_channel = std::move(name);
        }
        void set_consumer_channel(std::string name) noexcept
        {
            std::lock_guard<std::mutex> g(mu);
            consumer_channel = std::move(name);
        }
        /// Atomic read-and-clear.  Caller passes the returned string
        /// to the broker RPC — by that point the field is already
        /// empty so no other path can race a concurrent mutator.
        std::string take_producer_channel()
        {
            std::lock_guard<std::mutex> g(mu);
            std::string out;
            out.swap(producer_channel);
            return out;
        }
        std::string take_consumer_channel()
        {
            std::lock_guard<std::mutex> g(mu);
            std::string out;
            out.swap(consumer_channel);
            return out;
        }
    } shared;

    // Inbox client cache (keyed by target_uid).
    // Uses RoleHostCore::open_inbox() for atomic check-and-create.

    // last_seq: no local copy — forwarded directly to consumer->last_seq().

    // The role's sole thread manager. All role-scope threads
    // (worker / ctrl / inbox / future) live under this one instance —
    // same dynamic lifecycle module "ThreadManager:" + role_tag, same
    // bounded join, same process-wide leak aggregator. Constructed
    // eagerly in the Impl ctor from role_tag + uid.
    std::unique_ptr<pylabhub::utils::ThreadManager> thread_mgr_;

    // RoleHandler-mode network surface (Wave-B M4c; sole path after M4f).
    //   (a) `handler_` non-null + `ctrl_threads_started_ == true`:
    //       handler-mode active; N ctrl threads polling N BRCs.
    //   (b) `handler_` null + `ctrl_threads_started_ == false`:
    //       validate-only run, or post-stop_handler_threads.
    // `start_handler_threads` flips `ctrl_threads_started_` to true on
    // success; a second call is refused (single-shot per instance).
    std::unique_ptr<RoleHandler> handler_;
    bool                         ctrl_threads_started_{false};

    // A2 (Wave-B M8 prep): per-connection liveness bitmask.  Bit `i`
    // == 1 means handler->connections()[i] has not observed a
    // ZMQ_EVENT_DISCONNECTED since start_handler_threads.  Sized by
    // bit width: 64 connections is comfortably above any plausible
    // role-side topology (single-hub = 1, dual-hub processor = 2,
    // hypothetical many-hub federation = small N).  Atomic for
    // any-thread reads + ctrl-thread writes via on_hub_dead lambda.
    // Zero before start_handler_threads; bits 0..n-1 set on spawn;
    // bits cleared individually as each connection dies.
    std::atomic<std::uint64_t>   connection_alive_mask_{0};

    // Optional hook for role-specific metrics injection.
    std::function<void(nlohmann::json &)> metrics_hook;

    // Wave-B M4d/M4e/M4f: route a Class A/B/D operation to the right
    // BrokerRequestComm via the handler's index.  Returns nullptr if
    // the handler isn't attached (validate-only / post-stop), if the
    // handler's lookup misses (e.g. DISC_REQ for a channel not in our
    // presence list, or band_* for a band we haven't joined), or if
    // start_handler_threads hasn't run.  Class A/B/D call sites
    // handle nullptr via the existing `if (!bc || !bc->is_connected())`
    // guards — same shape as the pre-M4 `if (!bc)` guards they replaced.
    //
    // Per HEP-CORE-0033 §18.3:
    //   - Class A (channel-bound): handler->brc_for_channel(channel)
    //     hashes via channel_index_ → presence → connection.
    //   - Class B (role-bound): handler->brc_for_role() returns the
    //     first connection's BRC (any connection suffices for role-
    //     scope queries).  Full multi-hub fall-through is M5+ scope.
    //   - Class D (band-bound): handler->brc_for_band(band) hashes
    //     via band_index_ (populated by on_band_joined() on a
    //     successful band_join RPC).

    [[nodiscard]] hub::BrokerRequestComm *
    resolve_bc_for_channel(const std::string &channel) const noexcept
    {
        return handler_ ? handler_->brc_for_channel(channel) : nullptr;
    }

    [[nodiscard]] hub::BrokerRequestComm *
    resolve_bc_for_role() const noexcept
    {
        return handler_ ? handler_->brc_for_role() : nullptr;
    }

    [[nodiscard]] hub::BrokerRequestComm *
    resolve_bc_for_band(const std::string &band) const noexcept
    {
        return handler_ ? handler_->brc_for_band(band) : nullptr;
    }
};

// ============================================================================
// Lifecycle
// ============================================================================

RoleAPIBase::RoleAPIBase(RoleHostCore &core,
                         std::string   role_tag,
                         std::string   uid)
    : pImpl(std::make_unique<Impl>(&core, std::move(role_tag), std::move(uid)))
{
    // ThreadManager ctor throws std::invalid_argument if either identity
    // string is empty — effectively propagating the compile-time-plus-
    // runtime-checked invariant up to RoleAPIBase's caller.
}

RoleAPIBase::~RoleAPIBase() = default;
RoleAPIBase::RoleAPIBase(RoleAPIBase &&) noexcept = default;
RoleAPIBase &RoleAPIBase::operator=(RoleAPIBase &&) noexcept = default;

// ============================================================================
// Host wiring
// ============================================================================

// set_role_tag and set_uid removed — identity is ctor-only.

namespace
{
// Compute 8-byte schema tag from first 8 bytes of hash (empty → nullopt).
std::optional<std::array<uint8_t, 8>>
make_schema_tag(const std::string &hash)
{
    if (hash.size() < 8) return std::nullopt;
    std::array<uint8_t, 8> tag{};
    std::memcpy(tag.data(), hash.data(), 8);
    return tag;
}
} // namespace

bool RoleAPIBase::build_tx_queue(const hub::TxQueueOptions &opts)
{
    pImpl->tx_queue.reset();

    // Identity read from RoleAPIBase state (single source of truth —
    // opts does not carry channel_name/role_uid/role_name).  Tx uses
    // out_channel when set (processor out side), falling back to
    // channel for single-direction producers that set_channel() only.
    const std::string &tx_channel =
        pImpl->out_channel.empty() ? pImpl->channel : pImpl->out_channel;

    // Prefer SHM when configured and a slot schema is available; fall back to
    // ZMQ when data_transport == "zmq". The factories return concrete types,
    // which upcast into the abstract unique_ptr<QueueWriter> at the storage
    // boundary. After this function, nothing below RoleAPIBase sees concrete
    // ShmQueue / ZmqQueue.
    std::unique_ptr<hub::QueueWriter> writer;
    if (opts.has_shm && opts.slot_spec.has_schema)
    {
        auto shm = hub::ShmQueue::create_writer(
            tx_channel,
            hub::schema_spec_to_zmq_fields(opts.slot_spec), opts.slot_spec.packing,
            hub::schema_spec_to_zmq_fields(opts.fz_spec),   opts.fz_spec.packing,
            opts.shm_config.ring_buffer_capacity,
            opts.shm_config.physical_page_size,
            opts.shm_config.shared_secret,
            opts.shm_config.policy,
            opts.shm_config.consumer_sync_policy,
            opts.shm_config.checksum_policy,
            /*checksum_slot=*/false, /*checksum_fz=*/false,
            opts.always_clear_slot,
            opts.shm_config.hub_uid,
            opts.shm_config.hub_name,
            nullptr, nullptr,
            opts.shm_config.producer_uid,
            opts.shm_config.producer_name);
        if (!shm)
        {
            LOGGER_ERROR("[{}] ShmQueue create_writer failed for '{}'",
                         pImpl->role_tag, tx_channel);
            return false;
        }
        writer.reset(shm.release()); // upcast: ShmQueue* → QueueWriter*
    }
    else if (opts.data_transport == "zmq")
    {
        if (opts.zmq_node_endpoint.empty())
        {
            LOGGER_ERROR("[{}] data_transport='zmq' but zmq_node_endpoint is empty",
                         pImpl->role_tag);
            return false;
        }
        // Compose a stable ThreadManager owner_id from role identity +
        // direction. Uniqueness across the process is guaranteed by the
        // role uid (which is a per-instance UID by construction).
        std::string inst_id = opts.instance_id.empty()
                                  ? (pImpl->role_tag + ":" + pImpl->uid + ":tx")
                                  : opts.instance_id;
        // Schema tag (frame-validation 8-byte identity) is derived
        // from slot_spec + fz_spec.  Auto-computed here so callers
        // aren't carrying a redundant schema_hash field in opts.
        const auto schema_hash = hub::compute_schema_hash(opts.slot_spec,
                                                           opts.fz_spec);
        writer = hub::ZmqQueue::push_to(
            opts.zmq_node_endpoint,
            hub::schema_spec_to_zmq_fields(opts.slot_spec),
            opts.slot_spec.packing,
            opts.zmq_bind, make_schema_tag(schema_hash),
            /*sndhwm=*/0, opts.zmq_buffer_depth, opts.zmq_overflow_policy,
            /*send_retry_interval_ms=*/10, std::move(inst_id));
        if (!writer)
            return false;
        if (!writer->start())
        {
            LOGGER_ERROR("[{}] ZMQ PUSH start() failed for '{}'",
                         pImpl->role_tag, opts.zmq_node_endpoint);
            return false;
        }
    }
    else
    {
        return false;
    }

    writer->set_checksum_policy(opts.checksum_policy);
    writer->set_flexzone_checksum(opts.flexzone_checksum);
    pImpl->tx_queue = std::move(writer);
    return true;
}

bool RoleAPIBase::build_rx_queue(const hub::RxQueueOptions &opts)
{
    pImpl->rx_queue.reset();

    // Identity read from RoleAPIBase state (single source of truth).
    // Rx uses `channel` (which consumer/processor-in set via
    // set_channel(in_channel)).
    const std::string &rx_channel = pImpl->channel;

    std::unique_ptr<hub::QueueReader> reader;
    if (!opts.shm_name.empty() && opts.shm_shared_secret != 0 && opts.slot_spec.has_schema)
    {
        auto shm = hub::ShmQueue::create_reader(
            opts.shm_name, opts.shm_shared_secret,
            hub::schema_spec_to_zmq_fields(opts.slot_spec), opts.slot_spec.packing,
            rx_channel,
            /*verify_slot=*/false, /*verify_fz=*/false,
            pImpl->uid, pImpl->name);
        if (!shm)
            return false;
        reader.reset(shm.release());
    }
    else if (opts.data_transport == "zmq")
    {
        if (opts.zmq_node_endpoint.empty())
        {
            LOGGER_ERROR("[{}] data_transport='zmq' but zmq_node_endpoint is empty",
                         pImpl->role_tag);
            return false;
        }
        std::string inst_id = opts.instance_id.empty()
                                  ? (pImpl->role_tag + ":" + pImpl->uid + ":rx")
                                  : opts.instance_id;
        // Expected schema hash auto-computed from specs (single source
        // of truth — no redundant expected_schema_hash field in opts).
        const auto expected_hash = hub::compute_schema_hash(opts.slot_spec,
                                                             opts.fz_spec);
        reader = hub::ZmqQueue::pull_from(
            opts.zmq_node_endpoint,
            hub::schema_spec_to_zmq_fields(opts.slot_spec),
            opts.slot_spec.packing,
            /*bind=*/false, opts.zmq_buffer_depth,
            make_schema_tag(expected_hash), std::move(inst_id));
        if (!reader)
            return false;
        if (!reader->start())
        {
            LOGGER_ERROR("[{}] ZMQ PULL start() failed for '{}'",
                         pImpl->role_tag, opts.zmq_node_endpoint);
            return false;
        }
    }
    else
    {
        return false;
    }

    reader->set_checksum_policy(opts.checksum_policy);
    reader->set_flexzone_checksum(opts.flexzone_checksum);
    pImpl->rx_queue = std::move(reader);
    return true;
}

bool RoleAPIBase::start_tx_queue()
{
    return pImpl->tx_queue && pImpl->tx_queue->start();
}

bool RoleAPIBase::start_rx_queue()
{
    return pImpl->rx_queue && pImpl->rx_queue->start();
}

void RoleAPIBase::reset_tx_queue_metrics()
{
    if (pImpl->tx_queue) pImpl->tx_queue->init_metrics();
}

void RoleAPIBase::reset_rx_queue_metrics()
{
    if (pImpl->rx_queue) pImpl->rx_queue->init_metrics();
}

void RoleAPIBase::sync_tx_flexzone_checksum()
{
    if (pImpl->tx_queue) pImpl->tx_queue->sync_flexzone_checksum();
}

bool RoleAPIBase::tx_has_shm() const noexcept
{
    return pImpl->tx_queue && pImpl->tx_queue->is_shm_backed();
}

bool RoleAPIBase::rx_has_shm() const noexcept
{
    return pImpl->rx_queue && pImpl->rx_queue->is_shm_backed();
}

void RoleAPIBase::close_queues()
{
    if (pImpl->tx_queue) pImpl->tx_queue->stop();
    if (pImpl->rx_queue) pImpl->rx_queue->stop();
    pImpl->tx_queue.reset();
    pImpl->rx_queue.reset();
}

void RoleAPIBase::set_inbox_queue(hub::InboxQueue *q) { pImpl->inbox_queue = q; }
// set_uid removed — see note above.
void RoleAPIBase::set_name(std::string name)          { pImpl->name = std::move(name); }
void RoleAPIBase::set_channel(std::string c)          { pImpl->channel = std::move(c); }
void RoleAPIBase::set_out_channel(std::string c)      { pImpl->out_channel = std::move(c); }
void RoleAPIBase::set_log_level(std::string l)        { pImpl->log_level = std::move(l); }
void RoleAPIBase::set_script_dir(std::string d)       { pImpl->script_dir = std::move(d); }
void RoleAPIBase::set_role_dir(std::string d)         { pImpl->role_dir = std::move(d); }
void RoleAPIBase::set_checksum_policy(hub::ChecksumPolicy p) { pImpl->checksum_policy = p; }
void RoleAPIBase::set_engine(ScriptEngine *e)          { pImpl->engine = e; }
void RoleAPIBase::set_stop_on_script_error(bool v)    { pImpl->stop_on_script_error = v; }
void RoleAPIBase::set_metrics_hook(std::function<void(nlohmann::json &)> hook)
{
    pImpl->metrics_hook = std::move(hook);
}

void RoleAPIBase::set_auth(std::string client_pubkey, std::string client_seckey)
{
    pImpl->auth_client_pubkey = std::move(client_pubkey);
    pImpl->auth_client_seckey = std::move(client_seckey);
}

const std::string &RoleAPIBase::auth_client_pubkey() const
{
    return pImpl->auth_client_pubkey;
}

const std::string &RoleAPIBase::auth_client_seckey() const
{
    return pImpl->auth_client_seckey;
}

// ============================================================================
// Thread manager
// ============================================================================

pylabhub::utils::ThreadManager &RoleAPIBase::thread_manager()
{
    // Always valid — the Impl ctor constructs thread_mgr_ from the
    // ctor-required role_tag + uid. No runtime "did you init?" check.
    return *pImpl->thread_mgr_;
}

// ============================================================================
// stop_ctrl_for_teardown — non-destructive signal of every BRC poll loop
// ============================================================================

void RoleAPIBase::stop_ctrl_for_teardown() noexcept
{
    if (!pImpl->handler_) return;   // validate-only run, or post-stop
    for (const auto &conn : pImpl->handler_->connections())
    {
        if (auto *brc = conn.brc.get())
            brc->stop();
    }
}

// ============================================================================
// install_heartbeat — Wave-B M5: post-REG cadence negotiation + tick install
// ============================================================================

void RoleAPIBase::install_heartbeat(int role_cfg_ms,
                                     std::optional<int> hub_max_ms_opt) noexcept
{
    // HEP-CORE-0023 §2.5 — hub's heartbeat_interval_ms is the **maximum
    // tolerated silence** (timeout ceiling).  Role's configured cadence is
    // its preferred (typically faster) pace.  Effective cadence is
    // min(role, hub); a slower role triggers a warning + downgrade so the
    // role doesn't get reaped by hub-side liveness.
    int effective_interval_ms = role_cfg_ms;
    if (hub_max_ms_opt.has_value())
    {
        const int hub_max = *hub_max_ms_opt;
        if (role_cfg_ms > hub_max)
        {
            LOGGER_WARN(
                "[{}] heartbeat: configured interval {} ms exceeds hub's "
                "tolerated max {} ms — resetting to hub max to avoid "
                "liveness timeout (HEP-CORE-0023 §2.5)",
                pImpl->role_tag, role_cfg_ms, hub_max);
            effective_interval_ms = hub_max;
        }
        else
        {
            LOGGER_INFO(
                "[{}] heartbeat: aligned with hub — role cadence {} ms, "
                "hub max {} ms",
                pImpl->role_tag, role_cfg_ms, hub_max);
        }
    }

    // Pick the BRC that runs the periodic-task timer: connections()[0].brc
    // — the master ctrl thread.  on_heartbeat_tick_ then routes each
    // per-presence emission per-channel via resolve_bc_for_channel, so
    // heartbeats end up on the correct BRC even in dual-hub processor (M8).
    hub::BrokerRequestComm *bc_tick = nullptr;
    if (pImpl->handler_)
    {
        const auto &conns = pImpl->handler_->connections();
        if (!conns.empty()) bc_tick = conns[0].brc.get();
    }
    if (!bc_tick)
    {
        LOGGER_ERROR("[{}] install_heartbeat: no BRC available — "
                     "start_handler_threads not called?",
                     pImpl->role_tag);
        return;
    }

    auto *core_post = pImpl->core;
    bc_tick->set_periodic_task(
        [this] { on_heartbeat_tick_(); },
        effective_interval_ms,
        [core_post] { return core_post->iteration_count(); });

    LOGGER_INFO("[{}] heartbeat: periodic tick installed at {}ms",
                pImpl->role_tag, effective_interval_ms);
}

void RoleAPIBase::append_inbox_to_reg(nlohmann::json &opts,
                                       const config::InboxConfig &inbox_cfg) const
{
    if (!inbox_cfg.has_inbox())
        return;
    // Per legacy `append_inbox` lambda in start_ctrl_thread: inbox_queue
    // is optional even when inbox is configured (the role host wires it
    // separately via set_inbox_queue); skip the endpoint field rather
    // than emitting an empty string if no queue exists.
    if (pImpl->inbox_queue)
        opts["inbox_endpoint"] = pImpl->inbox_queue->actual_endpoint();
    opts["inbox_schema_json"] = inbox_cfg.schema_fields_json;
    opts["inbox_packing"]     = inbox_cfg.packing;
    opts["inbox_checksum"]    = inbox_cfg.checksum;
}

std::optional<int>
RoleAPIBase::extract_hub_heartbeat_max(const nlohmann::json &reg_ack_body) noexcept
{
    if (reg_ack_body.contains("heartbeat") &&
        reg_ack_body["heartbeat"].is_object() &&
        reg_ack_body["heartbeat"].contains("heartbeat_interval_ms") &&
        reg_ack_body["heartbeat"]["heartbeat_interval_ms"].is_number_integer())
    {
        return reg_ack_body["heartbeat"]["heartbeat_interval_ms"].get<int>();
    }
    return std::nullopt;
}

// ============================================================================
// Broker protocol helpers (require ctrl thread running)
// ============================================================================

std::optional<nlohmann::json>
RoleAPIBase::register_producer_channel(const nlohmann::json &opts, int timeout_ms)
{
    // Class A (channel-bound) — route via handler's channel_index_.
    const std::string ch = opts.value("channel_name", std::string{});
    auto *bc = pImpl->resolve_bc_for_channel(ch);
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] register_producer_channel: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }
    auto result = bc->register_channel(opts, timeout_ms);
    // Per HEP-CORE-0007 §12.3, a request-reply method's optional<json>
    // now carries the broker's response body (success OR error).  nullopt
    // means no response (timeout/disconnect).  Status field discriminates
    // the success vs error branch; error_code (HEP-0007 §12.4a taxonomy)
    // tells the caller what specifically failed.
    if (!result.has_value())
        LOGGER_ERROR("[{}] REG_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->role_tag, opts.value("channel_name", "?"));
    else if (result->value("status", std::string{}) != "success")
        LOGGER_ERROR("[{}] REG_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->role_tag, opts.value("channel_name", "?"),
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
    {
        LOGGER_INFO("[{}] Registered producer channel '{}' with broker",
                    pImpl->role_tag, opts.value("channel_name", "?"));
        // Wave-B M5: record the registered channel so deregister_from_broker's
        // atomic take-and-clear can dereg it on teardown.  Pre-M5 legacy
        // start_ctrl_thread did this manually after a successful REG; auto-
        // recording here makes the API safer — every caller (handler-mode
        // M5+ role hosts AND the legacy start_ctrl_thread path) gets dereg
        // bookkeeping wired without an extra step.  Idempotent: calling
        // shared.set_producer_channel twice with the same name is a no-op
        // beyond the second write.
        pImpl->shared.set_producer_channel(opts.value("channel_name", std::string{}));
    }
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::discover_channel(const std::string &channel, int timeout_ms)
{
    // Class A — DISC_REQ asks the broker "what do you know about
    // channel X?", which is valid for ANY channel name, not just
    // ones in our presence list.  Prefer routing via the channel's
    // BRC when we have a presence on it (multi-hub: send to the
    // hub that owns the channel); fall back to brc_for_role() (any
    // connection) for channels we're not registered on — single-hub
    // roles get a deterministic route; dual-hub roles query the
    // first connection (full multi-hub fall-through is M-future scope).
    auto *bc = pImpl->resolve_bc_for_channel(channel);
    if (!bc) bc = pImpl->resolve_bc_for_role();
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] discover_channel: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }
    auto result = bc->discover_channel(channel, {}, timeout_ms);
    if (!result.has_value())
        LOGGER_ERROR("[{}] DISC_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->role_tag, channel);
    else if (result->value("status", std::string{}) != "success")
        LOGGER_ERROR("[{}] DISC_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->role_tag, channel,
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
        LOGGER_INFO("[{}] Discovered channel '{}' from broker", pImpl->role_tag, channel);
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::register_consumer(const nlohmann::json &opts, int timeout_ms)
{
    // Wave-B M4d: Class A — route via handler when active.
    const std::string ch = opts.value("channel_name", std::string{});
    auto *bc = pImpl->resolve_bc_for_channel(ch);
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] register_consumer: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }
    auto result = bc->register_consumer(opts, timeout_ms);
    if (!result.has_value())
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->role_tag, opts.value("channel_name", "?"));
    else if (result->value("status", std::string{}) != "success")
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->role_tag, opts.value("channel_name", "?"),
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
    {
        LOGGER_INFO("[{}] Registered consumer on channel '{}' with broker",
                    pImpl->role_tag, opts.value("channel_name", "?"));
        // Wave-B M5: see register_producer_channel for the auto-record
        // rationale — every caller (handler-mode or legacy) gets dereg
        // bookkeeping without an extra step.
        pImpl->shared.set_consumer_channel(opts.value("channel_name", std::string{}));
    }
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::deregister_producer_channel(const std::string &channel, int timeout_ms)
{
    // Wave-B M4d: Class A — route via handler when active.
    auto *bc = pImpl->resolve_bc_for_channel(channel);
    if (!bc || !bc->is_connected())
        return std::nullopt;
    return bc->deregister_channel(channel, timeout_ms);
}

std::optional<nlohmann::json>
RoleAPIBase::deregister_consumer(const std::string &channel, int timeout_ms)
{
    // Wave-B M4d: Class A — route via handler when active.
    auto *bc = pImpl->resolve_bc_for_channel(channel);
    if (!bc || !bc->is_connected())
        return std::nullopt;
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
    // No outer guard: the per-method dereg helpers below
    // (`deregister_producer_channel`, `deregister_consumer`) each
    // route via `resolve_bc_for_channel(channel)` and early-return
    // on null/disconnected BRC.  State-clearing
    // (`shared.take_*_channel()`) proceeds even when the broker
    // connection is down — the role host is tearing down anyway,
    // and we don't want stale registered-channel state to outlive
    // a broker that went away first.

    // Atomic take-and-clear of any registered channel names; the
    // broker RPC then runs OUTSIDE the lock on the captured locals.
    // Holding the mutex across `do_request` would block the worker
    // thread waiting on the ctrl thread to send + receive the reply,
    // and any future ctrl-thread path that takes the same mutex
    // would deadlock — see `Impl::Shared` docstring.
    auto prod = pImpl->shared.take_producer_channel();
    if (!prod.empty())
    {
        LOGGER_INFO("[{}] ctrl: deregistering producer channel '{}' from broker",
                    pImpl->role_tag, prod);
        (void)deregister_producer_channel(prod);
    }

    auto cons = pImpl->shared.take_consumer_channel();
    if (!cons.empty())
    {
        LOGGER_INFO("[{}] ctrl: deregistering consumer from channel '{}' from broker",
                    pImpl->role_tag, cons);
        (void)deregister_consumer(cons);
    }
}

void RoleAPIBase::on_heartbeat_tick_()
{
    auto *eng = pImpl->engine;

    // Skip the entire tick (incl. user callback) if the handler isn't
    // attached.  The handler's connections() vector is non-empty by
    // start_handler_threads contract, so when handler_ is non-null
    // each per-channel resolve will produce a valid BRC.
    if (!pImpl->handler_)
        return;

    // Per HEP-CORE-0019 §2.3 Phase 6 + HEP-CORE-0033 §19: heartbeat is
    // per-presence — one emission per `(channel, role_type)` row the role
    // holds, AND each emission carries metrics shaped for THAT presence
    // (not the role-wide aggregate).  A processor emits 2 heartbeats per
    // cycle, each with its own per-presence metrics snapshot — the
    // consumer-presence payload carries rx-queue + in_slots_received,
    // the producer-presence payload carries tx-queue + out_slots_written
    // + out_drop_count.  Closes audit H2 (2026-05-15).
    //
    // Wave-B M4d: BC is resolved per-channel inside the lambda so that
    // dual-hub processors (M8) route each presence's heartbeat to its
    // own broker.  Single-hub roles resolve to the same BRC every time.
    auto emit = [&](const std::string &ch, const char *role_type) {
        if (ch.empty()) return;
        auto *bc = pImpl->resolve_bc_for_channel(ch);
        if (!bc) return;
        const auto metrics = snapshot_metrics_for_presence(role_type);
        LOGGER_TRACE("[{}] ctrl: sending heartbeat for '{}' (uid='{}' role_type='{}')",
                     pImpl->role_tag, ch, pImpl->uid, role_type);
        bc->send_heartbeat(ch, pImpl->uid, role_type, metrics);
    };

    if (pImpl->role_tag == "proc")
    {
        emit(pImpl->channel,     "consumer");
        emit(pImpl->out_channel, "producer");
    }
    else if (pImpl->role_tag == "cons")
    {
        emit(pImpl->channel, "consumer");
    }
    else
    {
        // "prod" and any unknown role_tag default to a single producer
        // presence on `channel` (or `out_channel` if `channel` is unset —
        // matches the pre-Phase-6 fallback for producers that only call
        // `set_out_channel`).
        emit(pImpl->channel.empty() ? pImpl->out_channel : pImpl->channel,
             "producer");
    }

    if (eng && eng->has_callback("on_heartbeat"))
        eng->invoke("on_heartbeat");
}

// M1.4 (2026-05-11): `on_metrics_report_tick_` deleted.  Metrics
// piggyback on `on_heartbeat_tick_` via `send_heartbeat(...metrics)`
// per HEP-CORE-0019 §2.3 Phase 6.  The dedicated tick was redundant
// (heartbeat tick already calls `snapshot_metrics_json()` at the
// same cadence).

// Wave-B M4f (2026-05-16): `start_ctrl_thread` + `set_broker_comm` +
// the `pImpl->broker_channel` raw-pointer view + `CtrlThreadConfig`
// struct DELETED.  Pre-M5/M6/M7 the legacy single-BRC path co-existed
// with handler-mode as a fallback view for role hosts not yet migrated;
// after M5/M6/M7 every production role binary runs on handler-mode end-
// to-end and the legacy path is dead.  Class A/B/D routing helpers
// (`resolve_bc_for_channel/role/band`) now return nullptr when no
// handler is attached — no fallback path.

// ============================================================================
// Handler-mode control plane (Wave-B M4c)
// ============================================================================

bool RoleAPIBase::start_handler_threads(std::unique_ptr<RoleHandler> handler)
{
    // ── Atomicity guard ──────────────────────────────────────────────────
    if (pImpl->ctrl_threads_started_)
    {
        // WARN (not ERROR) — graceful refusal; state preserved.
        LOGGER_WARN("[{}] start_handler_threads: ctrl threads already "
                    "started; refusing re-entry (single-shot per instance)",
                    pImpl->role_tag);
        return false;
    }
    if (handler == nullptr)
    {
        LOGGER_ERROR("[{}] start_handler_threads: handler is null",
                     pImpl->role_tag);
        return false;
    }

    const std::size_t n_conn = handler->connection_count();
    const std::size_t n_pres = handler->presence_count();

    LOGGER_INFO("[{}] start_handler_threads: ENTRY — {} presence(s) on {} "
                "unique hub(s) (uid='{}' name='{}')",
                pImpl->role_tag, n_pres, n_conn,
                pImpl->uid, pImpl->name);

    // ── Phase 1: Allocate + connect each BRC (via handler) ───────────────
    LOGGER_INFO("[{}] start_handler_threads: Phase 1 — connecting {} BRC(s)",
                pImpl->role_tag, n_conn);
    if (!handler->start_connections(*this))
    {
        LOGGER_ERROR("[{}] start_handler_threads: Phase 1 FAILED — "
                     "handler->start_connections returned false",
                     pImpl->role_tag);
        return false;
    }
    LOGGER_INFO("[{}] start_handler_threads: Phase 1 OK — {} BRC(s) connected",
                pImpl->role_tag, n_conn);

    // Take ownership of the handler now — Phase 1 succeeded, so the
    // handler's BRCs are live + we will spawn threads for them.
    pImpl->handler_ = std::move(handler);

    auto *core = pImpl->core;
    const std::string &tag_local = pImpl->role_tag;

    // ── Phase 2: Per-BRC notification + hub-dead callbacks ───────────────
    LOGGER_INFO("[{}] start_handler_threads: Phase 2 — wiring notification "
                "and hub-dead callbacks on {} BRC(s)",
                pImpl->role_tag, n_conn);
    for (std::size_t i = 0; i < pImpl->handler_->connections().size(); ++i)
    {
        auto *brc = pImpl->handler_->connections()[i].brc.get();
        if (brc == nullptr) continue;  // defensive; should be non-null

        // Notification routing: enqueue every notification to the
        // role's core message queue.  Per-presence tagging (using
        // M4b's find_presence_from_notification) is deferred to M5+ —
        // the script side already reads channel_name from the body.
        brc->on_notification(
            [core, tag_local, i](const std::string &type,
                                  const nlohmann::json &body)
            {
                LOGGER_TRACE("[{}/handler_ctrl_{}] notification: {}",
                             tag_local, i, type);
                IncomingMessage msg;
                msg.event     = type;
                msg.notification_id = pylabhub::scripting::parse_notification_id(type);
                msg.details   = body;
                core->enqueue_message(std::move(msg));
            });

        // Hub-dead policy (A2, Wave-B M8 prep — HEP-CORE-0023 §2.5):
        //   * Master (i == 0): mark dead in bitmask + set HubDead +
        //     request_stop.  Role exits because the master ctrl
        //     thread is the one driving the heartbeat timer; without
        //     it the broker will reap the role anyway, and any peer
        //     work depends on the role still running.
        //   * Peer (i > 0): mark dead in bitmask + WARN only.  Role
        //     keeps running on the master; the role host or script
        //     can poll `api.is_connection_alive(i)` to decide
        //     whether to exit, fail RPCs that need that hub, or
        //     just continue with degraded service.
        // Bitmask write is relaxed (single producer per slot — the
        // ctrl thread for connection i is the only writer).
        auto *alive_mask = &pImpl->connection_alive_mask_;
        brc->on_hub_dead(
            [core, alive_mask, tag_local, i]()
            {
                alive_mask->fetch_and(~(std::uint64_t{1} << i),
                                       std::memory_order_relaxed);
                if (i == 0)
                {
                    LOGGER_WARN("[{}/handler_ctrl_{}] hub-dead: MASTER "
                                "broker connection lost — role will exit",
                                tag_local, i);
                    core->set_stop_reason(RoleHostCore::StopReason::HubDead);
                    core->request_stop();
                }
                else
                {
                    LOGGER_WARN("[{}/handler_ctrl_{}] hub-dead: PEER "
                                "broker connection lost — role continues "
                                "on master (HEP-CORE-0023 §2.5)",
                                tag_local, i);
                }
            });
    }
    LOGGER_INFO("[{}] start_handler_threads: Phase 2 OK", pImpl->role_tag);

    // ── Phase 3: Spawn one ctrl thread per HubConnection ─────────────────
    LOGGER_INFO("[{}] start_handler_threads: Phase 3 — spawning {} ctrl "
                "thread(s) (first = master per HEP-CORE-0031 §4.2.1)",
                pImpl->role_tag, n_conn);

    auto &tm = thread_manager();
    bool master_spawned = false;
    for (std::size_t i = 0; i < pImpl->handler_->connections().size(); ++i)
    {
        auto *brc = pImpl->handler_->connections()[i].brc.get();
        if (brc == nullptr) continue;

        pylabhub::utils::ThreadManager::SpawnOptions opts;
        opts.is_master = !master_spawned;

        const std::string slot_name = "handler_ctrl_" + std::to_string(i);
        const std::string endpoint  =
            pImpl->handler_->connections()[i].broker_endpoint;

        LOGGER_INFO("[{}] start_handler_threads: spawning '{}' for "
                    "hub='{}' role=[{}]",
                    pImpl->role_tag, slot_name, endpoint,
                    opts.is_master ? "MASTER" : "peer");

        const bool spawn_ok = tm.spawn(
            slot_name,
            [brc, slot_name, core, tag_local]
            (pylabhub::utils::ThreadManager::SlotContext &ctx)
            {
                LOGGER_INFO("[{}/{}] poll thread started",
                            tag_local, slot_name);
                ctx.with_active_loop(
                    [brc, core, &ctx]
                    {
                        brc->run_poll_loop(
                            [core, &ctx] {
                                return core->is_running() &&
                                       !ctx.shutdown_requested();
                            });
                    });
                LOGGER_INFO("[{}/{}] poll thread exiting",
                            tag_local, slot_name);
            },
            opts);

        if (!spawn_ok)
        {
            LOGGER_ERROR("[{}] start_handler_threads: tm.spawn('{}', "
                         "is_master={}) FAILED — rolling back",
                         pImpl->role_tag, slot_name, opts.is_master);
            // Roll back: signal any spawned BRCs, drain, release.
            // `stop_handler_threads()` does the full sequence + clears
            // state flags so subsequent re-entry isn't blocked by the
            // partial setup.
            stop_handler_threads();
            return false;
        }

        master_spawned = true;
    }
    LOGGER_INFO("[{}] start_handler_threads: Phase 3 OK — {} ctrl thread(s) spawned",
                pImpl->role_tag, n_conn);

    // A2 (Wave-B M8 prep): mark every spawned connection alive in the
    // bitmask.  Done AFTER spawn (so on_hub_dead lambdas observe a
    // sane initial state if a connection dies racing the spawn loop).
    // Bit width sanity-check: handler::connections().size() must fit
    // in uint64; LOG + cap to 64 if exceeded (silently truncating
    // would make is_connection_alive(i) lie for i >= 64).
    if (n_conn > 64)
    {
        LOGGER_ERROR("[{}] start_handler_threads: connection count {} > 64; "
                     "A2 per-connection liveness bitmask only supports up to "
                     "64.  Tracking the first 64; the remainder will report "
                     "is_connection_alive=false from start.",
                     pImpl->role_tag, n_conn);
    }
    const std::size_t bits = (n_conn > 64) ? 64 : n_conn;
    const std::uint64_t init_mask = (bits == 64)
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << bits) - 1);
    pImpl->connection_alive_mask_.store(init_mask, std::memory_order_relaxed);

    // Wave-B M4f (2026-05-16): the previous Phase 4 set
    // `pImpl->broker_channel = handler->connections()[0].brc.get()`
    // as a legacy fallback view for unmigrated call sites.  After
    // M4d/e migrated every Class A/B/D site through the routing
    // helpers AND M5/M6/M7 retired the legacy role-host startup
    // path, no caller dereferences `broker_channel` — the field
    // (and its set/clear pair) was deleted.

    pImpl->ctrl_threads_started_ = true;

    LOGGER_INFO("[{}] start_handler_threads: COMPLETE — handler-mode "
                "active ({} ctrl thread(s), {} presence(s))",
                pImpl->role_tag, n_conn, n_pres);
    return true;
}

void RoleAPIBase::stop_handler_threads() noexcept
{
    if (pImpl->handler_ == nullptr && !pImpl->ctrl_threads_started_)
    {
        // Never started, or already stopped — idempotent no-op.
        return;
    }

    LOGGER_INFO("[{}] stop_handler_threads: ENTRY", pImpl->role_tag);

    // Wave-B M4f (2026-05-16): the previous Phase 1 cleared the
    // legacy `broker_channel` fallback view before BRCs were
    // destroyed; the field is gone, so no clear is needed.  Numbering
    // of the remaining phases preserved for log grep continuity.

    // ── Phase 2: Signal each BRC to exit its poll loop ───────────────────
    if (pImpl->handler_ != nullptr)
    {
        const std::size_t n_conn = pImpl->handler_->connections().size();
        LOGGER_INFO("[{}] stop_handler_threads: Phase 2 — stopping {} "
                    "BRC poll loop(s)",
                    pImpl->role_tag, n_conn);
        for (auto &c : pImpl->handler_->connections())
        {
            if (c.brc) c.brc->stop();
        }
    }

    // ── Phase 3: Drain the ThreadManager (HEP-CORE-0031 §4.1) ────────────
    LOGGER_INFO("[{}] stop_handler_threads: Phase 3 — draining ThreadManager",
                pImpl->role_tag);
    try
    {
        auto &tm = thread_manager();
        tm.request_shutdown_all();
        (void)tm.wait_for_quiescence(std::chrono::seconds(5));
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] stop_handler_threads: ThreadManager drain "
                     "threw — {}",
                     pImpl->role_tag, e.what());
    }

    // ── Phase 4: Disconnect + release each BRC, release the handler ──────
    if (pImpl->handler_ != nullptr)
    {
        LOGGER_INFO("[{}] stop_handler_threads: Phase 4 — releasing BRCs "
                    "+ handler",
                    pImpl->role_tag);
        pImpl->handler_->stop_connections();
        pImpl->handler_.reset();
    }

    pImpl->ctrl_threads_started_ = false;

    LOGGER_INFO("[{}] stop_handler_threads: COMPLETE", pImpl->role_tag);
}

RoleHandler *RoleAPIBase::handler() const noexcept
{
    return pImpl->handler_.get();
}

bool RoleAPIBase::is_connection_alive(std::size_t i) const noexcept
{
    if (i >= 64) return false;   // beyond the bitmask
    if (!pImpl->handler_) return false;
    if (i >= pImpl->handler_->connections().size()) return false;
    const auto mask = pImpl->connection_alive_mask_.load(std::memory_order_relaxed);
    return (mask & (std::uint64_t{1} << i)) != 0;
}

std::size_t RoleAPIBase::connections_alive_count() const noexcept
{
    if (!pImpl->handler_) return 0;
    const auto mask = pImpl->connection_alive_mask_.load(std::memory_order_relaxed);
    // Mask out any bits beyond actual connections (defensive — should
    // be 0 already since init_mask only sets bits 0..n-1).
    const auto n = pImpl->handler_->connections().size();
    const auto cap = (n >= 64) ? ~std::uint64_t{0}
                                : ((std::uint64_t{1} << n) - 1);
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::size_t>(__builtin_popcountll(mask & cap));
#else
    auto m = mask & cap;
    std::size_t c = 0;
    while (m) { c += m & 1; m >>= 1; }
    return c;
#endif
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

    // on_inbox is an OPTIONAL callback per HEP-CORE-0011: a role with
    // an inbox queue configured is not required to handle inbox
    // messages in script.  Skip the dispatch (and drain the queue by
    // ack'ing) when the script/plugin doesn't export on_inbox.
    // Without this guard, Lua/Python/Native's missing-callback→Error
    // path would fire on every legitimate delivery.
    const bool has_handler = eng->has_callback("on_inbox");

    while (true)
    {
        const auto *item = iq->recv_one(std::chrono::milliseconds{0});
        if (!item)
            break;

        if (has_handler)
        {
            eng->invoke_on_inbox(InvokeInbox{
                item->data, iq->item_size(),
                item->sender_id, item->seq});
        }

        iq->send_ack(0);
    }
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
    // Class D — band_join is the BOOTSTRAP case: the handler's
    // band_index_ is empty until on_band_joined() below registers it
    // on a successful broker round-trip.  So `resolve_bc_for_band`
    // returns nullptr here on first-time join.  Per HEP-CORE-0033
    // §18.3, the role picks which hub to join the band on; pre-M5
    // single-presence roles have one choice (any connection works),
    // so route the initial REQ via `resolve_bc_for_role()` (returns
    // connections()[0].brc).  M5+ multi-presence (processor) callers
    // wanting to join a band on a specific hub will need explicit
    // `api.in_hub.band_join` / `api.out_hub.band_join` accessors
    // (out of scope here; same pattern as Class C).
    auto *bc = pImpl->resolve_bc_for_band(channel);   // null on first-time join
    if (!bc) bc = pImpl->resolve_bc_for_role();       // bootstrap fallback
    if (!bc)
        return std::nullopt;
    auto result = bc->band_join(channel);

    // On success, register this band in the handler's band_index_ so
    // subsequent band_* calls route to the same BRC.  Pre-M5 single-
    // presence roles always pick presences[0] as the band owner
    // (only one choice).  M5+ multi-presence roles will need to
    // decide which presence owns the band based on caller context
    // (in_hub vs out_hub); the on_band_joined() call signature
    // already accepts a Presence*, so the decision moves to the
    // role-host layer without changing this code.
    if (result.has_value() && pImpl->handler_)
    {
        const auto &presences = pImpl->handler_->presences();
        if (!presences.empty())
            pImpl->handler_->on_band_joined(channel, &presences.front());
    }
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::band_leave(const std::string &channel)
{
    // Wave-B M4e: Class D — route via handler when active.
    auto *bc = pImpl->resolve_bc_for_band(channel);
    if (!bc)
        return std::nullopt;
    auto result = bc->band_leave(channel);

    // On success, remove the band → presence mapping so later
    // band_* calls don't dangle on a stale route.  Safe to call
    // when the band isn't indexed (no-op).
    if (result.has_value() && pImpl->handler_)
        pImpl->handler_->on_band_left(channel);
    return result;
}

void RoleAPIBase::band_broadcast(const std::string &channel,
                                  const nlohmann::json &body)
{
    // Wave-B M4e: Class D — route via handler when active.
    if (auto *bc = pImpl->resolve_bc_for_band(channel))
        bc->band_broadcast(channel, body);
}

std::optional<nlohmann::json> RoleAPIBase::band_members(const std::string &channel)
{
    // Wave-B M4e: Class D — route via handler when active.
    auto *bc = pImpl->resolve_bc_for_band(channel);
    if (!bc)
        return std::nullopt;
    return bc->band_members(channel);
}

// ============================================================================
// Inbox client management
// ============================================================================

std::optional<RoleAPIBase::InboxOpenResult>
RoleAPIBase::open_inbox_client(const std::string &target_uid)
{
    if (!pImpl->core)
        return std::nullopt;
    // Wave-B M4e: Class B (role-bound) — inbox discovery is a
    // role-scope query (ROLE_INFO_REQ per HEP-CORE-0033 §18.2 +
    // HEP-CORE-0027 §4.2).  Route via the first connection; pre-M5
    // single-hub callers see identical behaviour.
    auto *bc_role = pImpl->resolve_bc_for_role();
    if (!bc_role)
        return std::nullopt;

    hub::SchemaSpec result_spec;
    std::string result_packing;

    auto entry = pImpl->core->open_inbox(target_uid,
        [&]() -> std::optional<RoleHostCore::InboxCacheEntry>
        {
            auto info = bc_role->query_role_info(target_uid, 1000);
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
    // Class B — query_role_presence is a role-scope query; route via
    // the first connection (handler->brc_for_role()).
    auto *bc = pImpl->resolve_bc_for_role();
    if (!bc)
        return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;
    while (std::chrono::steady_clock::now() < deadline)
    {
        // Per HEP-CORE-0007 §12.3 (post-Bucket-C contract):
        // `query_role_presence` returns the broker's response body
        // (`{present: true, channel, role}` on found, `{present: false}`
        // on not-found, or `{present: false, error}` on missing role_uid)
        // or `nullopt` on transport failure.
        auto resp = bc->query_role_presence(uid, kPollMs);
        if (resp.has_value() && resp->value("present", false))
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
    // Routes to QueueReader::set_verify_checksum(slot, fz); slot-only as per
    // the pre-L3.γ contract (the old Consumer::set_verify_checksum also
    // passed false for fz).
    if (pImpl->rx_queue)
        pImpl->rx_queue->set_verify_checksum(enable, false);
}

// ============================================================================
// Diagnostics
// ============================================================================

uint64_t RoleAPIBase::script_error_count() const { return pImpl->core->script_error_count(); }
uint64_t RoleAPIBase::loop_overrun_count() const { return pImpl->core->loop_overrun_count(); }
uint64_t RoleAPIBase::last_cycle_work_us() const { return pImpl->core->last_cycle_work_us(); }

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

    const bool has_tx = pImpl->core->has_tx_fz();
    const bool has_rx = pImpl->core->has_rx_fz();

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

nlohmann::json
RoleAPIBase::snapshot_metrics_for_presence(const std::string &role_type) const
{
    // Per-presence emission shape per HEP-CORE-0019 §2.3 Phase 6.
    // Producer-presence carries tx-side queue + producer-side role
    // counters; consumer-presence carries rx-side queue + consumer-side
    // role counters.  Role-wide fields (loop, inbox, custom) appear on
    // every presence.
    nlohmann::json result;

    // Queue: only the direction relevant to this presence.  Single key
    // ("queue", not "in_queue"/"out_queue") — the broker keys the row
    // by (channel, uid, role_type), so direction is already implicit.
    if (role_type == "consumer" && pImpl->rx_queue)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->rx_queue->metrics());
        result["queue"] = std::move(q);
    }
    else if (role_type == "producer" && pImpl->tx_queue)
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, pImpl->tx_queue->metrics());
        result["queue"] = std::move(q);
    }

    // Loop metrics: role-wide (one loop drives both directions of a
    // processor).  Appears on every presence — admin queries can
    // dedupe across rows or just read from either.
    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, pImpl->core->loop_metrics());
        result["loop"] = std::move(lm);
    }

    // Role-level counters: include only side-relevant fields.
    // `script_error_count` is direction-agnostic so it appears on both
    // (script errors do not have a producer/consumer side).
    nlohmann::json role;
    role["script_error_count"] = pImpl->core->script_error_count();
    if (role_type == "consumer")
    {
        role["in_slots_received"] = pImpl->core->in_slots_received();
    }
    else if (role_type == "producer")
    {
        role["out_slots_written"] = pImpl->core->out_slots_written();
        role["out_drop_count"]    = pImpl->core->out_drop_count();
    }
    result["role"] = std::move(role);

    // Inbox metrics: per-role, not per-presence (one inbox per role).
    if (pImpl->inbox_queue)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, pImpl->inbox_queue->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    // Custom metrics: per-role (the report_metric() API doesn't
    // distinguish sides today).
    {
        auto cm = pImpl->core->custom_metrics_snapshot();
        if (!cm.empty())
            result["custom"] = nlohmann::json(cm);
    }

    // Role-specific metrics hook — fires once per call.  Hooks that
    // inject side-specific data can inspect `result["queue"]` /
    // `result["role"]` to disambiguate which presence is being built.
    if (pImpl->metrics_hook)
        pImpl->metrics_hook(result);

    return result;
}

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

    // Role-specific metrics hook.
    if (pImpl->metrics_hook)
        pImpl->metrics_hook(result);

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
