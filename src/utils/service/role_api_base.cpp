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
    // Audit S1+O4 (2026-05-17): the former `Shared` struct held
    // `producer_channel` / `consumer_channel` strings as implicit
    // "have I registered?" state.  That state is now per-presence via
    // `Presence::registration_state` (HEP-CORE-0023 §2 + Round-2
    // R2.1 S1).  `deregister_from_broker` iterates
    // `handler_->presences()` directly and dispatches DEREGs for any
    // presence in Registered / RegRequestPending state.

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

    // Route a Class A/B/D operation to the right BrokerRequestComm
    // via the handler's index (HEP-CORE-0033 §18.3).  Returns nullptr if
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
    //     first connection's BRC as a primitive.  Multi-hub
    //     fall-through (audit A3) is composed at the CALL SITE — see
    //     `wait_for_role` and `open_inbox_client` which iterate
    //     `handler_->connections()` directly so a target registered
    //     on hub[i>0] is discoverable.  Do NOT use `resolve_bc_for_role`
    //     for Class B queries on multi-presence roles; iterate
    //     `connections()` instead.
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

    // Audit S1+O4 (2026-05-17): mark the Presence as RegRequestPending
    // BEFORE dispatching the broker RPC.  This makes the in-flight
    // window observable (external readers see "we're trying" rather
    // than the previous ambiguous "Shared::producer_channel is still
    // empty").  Transitions to Registered on success or back to
    // Unregistered on failure below.
    Presence *presence = pImpl->handler_
        ? pImpl->handler_->find_presence_for_channel(ch) : nullptr;
    if (presence)
        presence->registration_state.store(
            RegistrationState::RegRequestPending,
            std::memory_order_release);

    auto result = bc->register_channel(opts, timeout_ms);
    // Per HEP-CORE-0007 §12.3, a request-reply method's optional<json>
    // now carries the broker's response body (success OR error).  nullopt
    // means no response (timeout/disconnect).  Status field discriminates
    // the success vs error branch; error_code (HEP-0007 §12.4a taxonomy)
    // tells the caller what specifically failed.
    const bool registered =
        result.has_value() &&
        result->value("status", std::string{}) == "success";

    if (!result.has_value())
        LOGGER_ERROR("[{}] REG_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->role_tag, opts.value("channel_name", "?"));
    else if (!registered)
        LOGGER_ERROR("[{}] REG_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->role_tag, opts.value("channel_name", "?"),
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
        LOGGER_INFO("[{}] Registered producer channel '{}' with broker",
                    pImpl->role_tag, opts.value("channel_name", "?"));

    if (presence)
        presence->registration_state.store(
            registered ? RegistrationState::Registered
                        : RegistrationState::Unregistered,
            std::memory_order_release);
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
    // Class A — route via handler when active.
    const std::string ch = opts.value("channel_name", std::string{});
    auto *bc = pImpl->resolve_bc_for_channel(ch);
    if (!bc || !bc->is_connected())
    {
        LOGGER_ERROR("[{}] register_consumer: broker comm not connected", pImpl->role_tag);
        return std::nullopt;
    }

    // Audit S1+O4 (2026-05-17): per-presence FSM marker, same pattern
    // as `register_producer_channel`.
    Presence *presence = pImpl->handler_
        ? pImpl->handler_->find_presence_for_channel(ch) : nullptr;
    if (presence)
        presence->registration_state.store(
            RegistrationState::RegRequestPending,
            std::memory_order_release);

    auto result = bc->register_consumer(opts, timeout_ms);
    const bool registered =
        result.has_value() &&
        result->value("status", std::string{}) == "success";

    if (!result.has_value())
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->role_tag, opts.value("channel_name", "?"));
    else if (!registered)
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->role_tag, opts.value("channel_name", "?"),
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
        LOGGER_INFO("[{}] Registered consumer on channel '{}' with broker",
                    pImpl->role_tag, opts.value("channel_name", "?"));

    if (presence)
        presence->registration_state.store(
            registered ? RegistrationState::Registered
                        : RegistrationState::Unregistered,
            std::memory_order_release);
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::deregister_producer_channel(const std::string &channel, int timeout_ms)
{
    // Class A — route via handler when active.
    auto *bc = pImpl->resolve_bc_for_channel(channel);
    if (!bc || !bc->is_connected())
        return std::nullopt;
    auto result = bc->deregister_channel(channel, timeout_ms);

    // Audit S1+O4 (2026-05-17): drop the presence's registration
    // state.  Mark Deregistered regardless of broker outcome — the
    // role's intent is "I no longer want to be registered"; even if
    // the broker is offline (no response), our local state should
    // reflect that we've ceased participation.
    if (Presence *p = pImpl->handler_
            ? pImpl->handler_->find_presence_for_channel(channel) : nullptr)
        p->registration_state.store(RegistrationState::Deregistered,
                                     std::memory_order_release);
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::deregister_consumer(const std::string &channel, int timeout_ms)
{
    // Class A — route via handler when active.
    auto *bc = pImpl->resolve_bc_for_channel(channel);
    if (!bc || !bc->is_connected())
        return std::nullopt;
    auto result = bc->deregister_consumer(channel, timeout_ms);
    // Audit S1+O4: same FSM drop as the producer path.
    if (Presence *p = pImpl->handler_
            ? pImpl->handler_->find_presence_for_channel(channel) : nullptr)
        p->registration_state.store(RegistrationState::Deregistered,
                                     std::memory_order_release);
    return result;
}

// ============================================================================
// Control thread — broker communication (heartbeat, notifications, requests)
// ============================================================================
//
// Private helpers called from within the ctrl thread. Access pImpl members
// directly — no bare pointers leaked into lambdas.

void RoleAPIBase::deregister_from_broker()
{
    // Audit S1+O4 (2026-05-17): iterate `handler_->presences()` and
    // dereg every presence whose registration state is Registered
    // (or RegRequestPending — best-effort dereg of a partially-
    // completed registration).  Pre-S1 this walked two strings on
    // `Impl::Shared` (producer_channel / consumer_channel) protected
    // by a mutex; the new shape sources truth from the per-presence
    // atomic state instead.
    //
    // Ordering: producer presences first, consumer presences second
    // — preserves the pre-S1 semantics ("drain output before input"
    // so consumers don't observe data after they've left the
    // channel).  Each per-method dereg helper routes via
    // `resolve_bc_for_channel(channel)` and early-returns on
    // null/disconnected BRC, so we don't need to gate on broker
    // liveness here.
    if (!pImpl->handler_) return;

    auto needs_dereg = [](const Presence &p) noexcept {
        const auto s = p.registration_state.load(std::memory_order_acquire);
        return s == RegistrationState::Registered ||
               s == RegistrationState::RegRequestPending;
    };

    // Pass 1: producer presences.
    for (const auto &p : pImpl->handler_->presences())
    {
        if (p.role_kind != RoleKind::Producer) continue;
        if (!needs_dereg(p)) continue;
        LOGGER_INFO("[{}] ctrl: deregistering producer channel '{}' from broker",
                    pImpl->role_tag, p.channel);
        (void)deregister_producer_channel(p.channel);
    }

    // Pass 2: consumer presences.
    for (const auto &p : pImpl->handler_->presences())
    {
        if (p.role_kind != RoleKind::Consumer) continue;
        if (!needs_dereg(p)) continue;
        LOGGER_INFO("[{}] ctrl: deregistering consumer from channel '{}' from broker",
                    pImpl->role_tag, p.channel);
        (void)deregister_consumer(p.channel);
    }
}

void RoleAPIBase::on_heartbeat_tick_()
{
    auto *eng = pImpl->engine;

    // V1 safety gate (2026-05-18): heartbeat ticks run on the master
    // ctrl thread via `BrokerRequestComm::set_periodic_task`.  A
    // slow-waker scenario could fire this tick after Phase 3a has
    // invalidated the context — bail before touching `pImpl->handler_`,
    // which Phase 4 may have destroyed.  See RoleHostCore "Flag
    // contract" header block for the discipline.
    if (pImpl->core != nullptr && !pImpl->core->context_valid())
    {
        LOGGER_WARN("[{}] heartbeat tick fired after context "
                    "invalidated — bailing (V1 safety gate)",
                    pImpl->role_tag);
        return;
    }

    // Skip the entire tick (incl. user callback) if the handler isn't
    // attached.  The handler's connections() vector is non-empty by
    // start_handler_threads contract, so when handler_ is non-null
    // each per-channel resolve will produce a valid BRC.
    if (!pImpl->handler_)
        return;

    // Per HEP-CORE-0019 §2.3 Phase 6 + HEP-CORE-0033 §19.3 step 3:
    // heartbeat is per-presence — iterate `handler_->presences()` and
    // emit one heartbeat per row.  Each emission carries metrics
    // shaped for THAT presence's role kind (not the role-wide
    // aggregate).  A dual-hub processor's two presences (Consumer on
    // in_hub + Producer on out_hub) emit one heartbeat each to their
    // own hubs.  Single-hub roles' presence lists have 1 entry and
    // emit 1 heartbeat.  Closes audit H2 (2026-05-15) + audit C2
    // (`REVIEW_Connection_Inbox_Band_2026-05-17.md` — replaces
    // pre-existing `role_tag` string branching that bypassed the
    // presence list).
    //
    // BC is resolved per-channel via `resolve_bc_for_channel` so each
    // presence's heartbeat lands on its own hub's BRC (HEP-CORE-0033
    // §18.3 Class A dispatch).
    for (const auto &p : pImpl->handler_->presences())
    {
        if (p.channel.empty()) continue;
        auto *bc = pImpl->resolve_bc_for_channel(p.channel);
        if (!bc) continue;
        const char *role_type = to_wire_string(p.role_kind);
        const auto metrics    = snapshot_metrics_for_presence(role_type);
        LOGGER_TRACE("[{}] ctrl: sending heartbeat for '{}' "
                     "(uid='{}' role_type='{}')",
                     pImpl->role_tag, p.channel, pImpl->uid, role_type);
        bc->send_heartbeat(p.channel, pImpl->uid, role_type, metrics);
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

    // ── Phase 1.5: Initialize connection-alive bitmask BEFORE Phase 2 ────
    //
    // Audit S3 (2026-05-19, REVIEW_Connection_Inbox_Band §S3): the
    // bitmask must be initialized to `(1<<N)-1` (all-alive) BEFORE
    // the Phase 2 lambdas capture `&pImpl->connection_alive_mask_`
    // and BEFORE Phase 3 spawns ctrl threads.  Pre-fix, the init
    // happened at the END of `start_handler_threads`, so any
    // on_hub_dead lambda firing during the init window (e.g.,
    // broker crashes mid-startup) read `mask = 0` and the policy
    // violation gate at the lambda body's `(prev & bit) == 0`
    // check would suppress the HUB_DEAD enqueue — a real
    // connection death gets misclassified as a duplicate fire.
    // Moving init here closes the window; the mask is initialised
    // exactly once per (uid, role lifetime) pair, before any code
    // that captures it.
    //
    // Bit width sanity: `handler.connections().size()` must fit in
    // uint64; LOG + cap to 64 if exceeded (silently truncating
    // would make is_connection_alive(i) lie for i >= 64).
    if (n_conn > 64)
    {
        LOGGER_ERROR("[{}] start_handler_threads: connection count {} > 64; "
                     "A2 per-connection liveness bitmask only supports up to "
                     "64.  Tracking the first 64; the remainder will report "
                     "is_connection_alive=false from start.",
                     pImpl->role_tag, n_conn);
    }
    {
        const std::size_t bits = (n_conn > 64) ? 64 : n_conn;
        const std::uint64_t init_mask = (bits == 64)
            ? ~std::uint64_t{0}
            : ((std::uint64_t{1} << bits) - 1);
        // Release ordering: pairs with the acquire load in
        // `is_connection_alive` / `connections_alive_count` and the
        // acq_rel fetch_and inside the on_hub_dead lambda body
        // (audit M3, 2026-05-18).  Stored BEFORE the lambdas
        // capture &pImpl->connection_alive_mask_ — happens-before
        // is established by the same `pImpl` pointer being live
        // across both stores (one-thread init phase).
        pImpl->connection_alive_mask_.store(init_mask, std::memory_order_release);
    }

    // ── Phase 2: Per-BRC notification + hub-dead callbacks ───────────────
    LOGGER_INFO("[{}] start_handler_threads: Phase 2 — wiring notification "
                "and hub-dead callbacks on {} BRC(s)",
                pImpl->role_tag, n_conn);
    for (std::size_t i = 0; i < pImpl->handler_->connections().size(); ++i)
    {
        auto *brc = pImpl->handler_->connections()[i].brc.get();
        if (brc == nullptr) continue;  // defensive; should be non-null

        // Audit C3 (2026-05-17): capture this connection's broker
        // endpoint by value so the lambda can stamp every inbound
        // message with its origin.  `(broker_endpoint, broker_pubkey)`
        // is the role-side dedup key per HEP-CORE-0033 §19.2, so
        // endpoint alone is unique among a role's connections (a role
        // never holds two HubConnection rows pointing at the same
        // endpoint).  This populates `IncomingMessage::source_hub_uid`
        // per HEP-CORE-0023 §7 + HEP-CORE-0033 §18.3 + §19.4 so a
        // dual-hub processor's script can tell which hub a
        // notification came from.
        std::string conn_endpoint =
            pImpl->handler_->connections()[i].broker_endpoint;

        // Notification routing: enqueue every notification to the
        // role's core message queue.  Per-presence tagging (using
        // M4b's find_presence_from_notification) is deferred to M5+ —
        // the script side already reads channel_name from the body.
        //
        // V1 (2026-05-18): gate on `core->context_valid()` so a
        // slow-waker firing this lambda after `stop_handler_threads`
        // Phase 3a has flipped the flag bails with a WARN instead of
        // touching `core->enqueue_message` on a tearing-down core.
        // See RoleHostCore "Flag contract" for the full discipline.
        brc->on_notification(
            [core, tag_local, i, conn_endpoint = std::move(conn_endpoint)](
                const std::string &type,
                const nlohmann::json &body)
            {
                if (!core->context_valid())
                {
                    LOGGER_WARN("[{}/handler_ctrl_{}] notification '{}' "
                                "fired after context invalidated — "
                                "bailing (V1 safety gate)",
                                tag_local, i, type);
                    return;
                }
                LOGGER_TRACE("[{}/handler_ctrl_{}] notification: {}",
                             tag_local, i, type);
                IncomingMessage msg;
                msg.event           = type;
                msg.notification_id = pylabhub::scripting::parse_notification_id(type);
                msg.details         = body;
                msg.source_hub_uid  = conn_endpoint;
                core->enqueue_message(std::move(msg));
            });

        // Hub-dead policy (audit D1/D2, 2026-05-18 — supersedes the
        // original A2 master/peer asymmetric direct-stop policy):
        //
        // The ctrl-thread lambda below now ENQUEUES a synthetic
        // HUB_DEAD `IncomingMessage` (with `source_hub_uid` set and
        // `details["is_master"]` flagged) and lets the worker-thread
        // dispatcher (`kNotificationTable[HubDead]` →
        // `invoke_user_hub_dead` or `default_hub_dead` in
        // cycle_ops.hpp) apply the user override or framework
        // default.  Default:
        //   * Master (is_master=true)  → set HubDead + request_stop.
        //     Required because the master ctrl thread drives the
        //     heartbeat timer; without it the broker reaps the role.
        //   * Peer (is_master=false)   → no-op (role keeps running
        //     on master; preserves pre-D1 peer-non-fatal behavior).
        //
        // If the script defines `on_hub_dead`, that callback REPLACES
        // the default action (uniform with `on_channel_closing` per
        // user design call 2026-05-15T03:29).  Scripts wishing to
        // exit on peer-death must define on_hub_dead and call
        // `api.stop()` inside it; scripts wishing to survive a
        // master-death must define on_hub_dead and avoid `api.stop()`.
        //
        // alive_mask bit-clearing happens here unconditionally
        // (master + peer) so `api.is_connection_alive(i)` and
        // `api.connections_alive_count()` reflect truth as soon as
        // the lambda runs — script callbacks see correct state.
        // Bitmask write is relaxed (single producer per slot — the
        // ctrl thread for connection i is the only writer).
        auto *alive_mask = &pImpl->connection_alive_mask_;
        // Audit R3.3 (2026-05-17): capture handler + dead-connection
        // pointer so the lambda can transition presences pointing at
        // this connection from `Registered` → `Deregistered`.  The
        // broker on the dead end has already reaped (or will reap)
        // these presences via heartbeat-timeout; making the role's
        // FSM mirror that truth means:
        //   (a) `deregister_from_broker` walking
        //       `handler_->presences()` filtering by registration
        //       state will skip the dead-connection presences
        //       (saving the per-presence DEREG-request blocking
        //       timeout during teardown).
        //   (b) external readers asking `presence.registration_state`
        //       see the truthful state instead of "Registered" on a
        //       broker that is gone.
        // `handler_` is owned by pImpl which outlives the lambda
        // (the lambda lives inside the BRC owned by handler_, which
        // pImpl owns).  Capturing the raw pointer is safe — there's
        // no lifetime hole.
        RoleHandler *handler_ptr = pImpl->handler_.get();
        const HubConnection *dead_conn =
            &pImpl->handler_->connections()[i];
        // Capture the broker endpoint at lambda-creation time so the
        // synthetic HUB_DEAD msg can carry `source_hub_uid` matching
        // the connection's stable identifier (HEP-0033 §19.2).  Reading
        // the endpoint from `pImpl->handler_->connections()[i]` inside
        // the lambda would race with handler teardown; capturing it
        // here keeps the value alive as long as the lambda exists.
        const std::string dead_endpoint =
            pImpl->handler_->connections()[i].broker_endpoint;
        const bool is_master_conn = (i == 0);
        brc->on_hub_dead(
            [core, alive_mask, tag_local, i, handler_ptr, dead_conn,
             dead_endpoint, is_master_conn]()
            {
                // V1 safety gate (2026-05-18): slow-waker may fire
                // this lambda after `stop_handler_threads` Phase 3a
                // has invalidated the context and Phase 4 has
                // destroyed `handler_` (or is about to).  Bail out
                // BEFORE dereferencing `handler_ptr` / `dead_conn`
                // (which would point into the destroyed handler).
                // See RoleHostCore "Flag contract" for the
                // discipline.  Note we still update the alive_mask
                // bit unconditionally — it lives on the
                // longer-lived Impl (which the wait_for_quiescence
                // protects via the lambda capture lifetime; the
                // mask is just a plain atomic int, no destructor
                // hazard).
                // acq_rel pairs with the release-ordered init store
                // (Phase 4) and the acquire-ordered reads in
                // is_connection_alive / connections_alive_count
                // (audit M3, 2026-05-18).
                // Audit S1 (2026-05-18) — fetch the PRIOR bit value so
                // we can detect a repeat fire.  pylabhub policy:
                // disconnect is TERMINAL (BRC sets reconnect_ivl=-1
                // at socket init).  In a correctly-configured build
                // this lambda runs ≤1 time per connection lifetime.
                // The defensive gate below logs ERROR + bails if it
                // fires a second time — surfaces any future config
                // drift (DRAFT-API enable, code edit re-enabling
                // reconnect, etc.) loudly instead of silently piling
                // duplicate HUB_DEAD msgs into incoming_queue_ (which
                // would push real notifications out at cap=64).
                const std::uint64_t bit  = std::uint64_t{1} << i;
                const std::uint64_t prev = alive_mask->fetch_and(
                                       ~bit, std::memory_order_acq_rel);
                if ((prev & bit) == 0)
                {
                    LOGGER_ERROR("[{}/handler_ctrl_{}] on_hub_dead fired "
                                 "AGAIN for an already-dead connection "
                                 "— pylabhub policy VIOLATION "
                                 "(disconnect is terminal, "
                                 "reconnect_ivl must be -1; see "
                                 "HEP-CORE-0023 §2.5).  Suppressing "
                                 "repeat msg enqueue.",
                                 tag_local, i);
                    return;
                }
                if (!core->context_valid())
                {
                    LOGGER_WARN("[{}/handler_ctrl_{}] hub-dead fired "
                                "after context invalidated — bailing "
                                "(V1 safety gate)",
                                tag_local, i);
                    return;
                }
                // Transition presence FSM out of Registered for any
                // presence rooted on this dead connection AND erase
                // band_index_ entries pointing at it.  The DisconnectReap
                // struct carries both counts so we can log the full
                // impact + (S4-6, Part C/E) enqueue per-band
                // on_band_lost events once the typed callback is wired.
                const auto reap =
                    handler_ptr->mark_connection_disconnected(dead_conn);
                LOGGER_WARN("[{}/handler_ctrl_{}] hub-dead: {} broker "
                            "connection lost ({} presence(s) marked "
                            "Deregistered, {} band(s) routing lost) — "
                            "dispatching HUB_DEAD to worker (default: {})",
                            tag_local, i,
                            is_master_conn ? "MASTER" : "PEER",
                            reap.presences_transitioned,
                            reap.bands_lost.size(),
                            is_master_conn ? "stop role" :
                                             "continue on master");
                // S4-6 (HEP-CORE-0030 amendment 2026-05-19, Part E):
                // enqueue one synthetic BAND_LOST IncomingMessage per
                // band whose routing was on the dead connection.  The
                // worker thread's dispatcher (kNotificationTable
                // `BandLost` row) fires the script's on_band_lost
                // override OR the no-op default — scripts that care
                // about per-band hub-dead notifications subscribe.
                for (const auto &band : reap.bands_lost)
                {
                    IncomingMessage bl;
                    bl.event           = "BAND_LOST";
                    bl.notification_id = NotificationId::BandLost;
                    bl.details         = nlohmann::json::object();
                    bl.details["band"]   = band;
                    bl.details["reason"] = "hub_dead";
                    bl.source_hub_uid  = dead_endpoint;
                    core->enqueue_message(std::move(bl));
                }
                // Enqueue synthetic HUB_DEAD notification so the
                // worker-thread dispatcher
                // (`kNotificationTable[HubDead]` in cycle_ops.hpp)
                // applies the framework default OR fires the
                // script's `on_hub_dead` callback.  Audit D1/D2
                // (2026-05-18) — uniform "callback replaces default"
                // pattern matching `on_channel_closing`.
                IncomingMessage msg;
                msg.event           = "HUB_DEAD";
                msg.notification_id = NotificationId::HubDead;
                msg.details         = nlohmann::json::object();
                msg.details["is_master"] = is_master_conn;
                msg.details["reason"]    = "ctrl_thread_on_hub_dead";
                msg.source_hub_uid  = dead_endpoint;
                core->enqueue_message(std::move(msg));
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

    // Audit S3 (2026-05-19): connection_alive_mask_ is now initialised
    // in Phase 1.5 (BEFORE Phase 2 wires the lambdas that capture it
    // + BEFORE Phase 3 spawns ctrl threads).  No init needed here.

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

    // ── Phase 3a: Invalidate the callback-safety beacon (V1, 2026-05-18) ─
    //
    // Between the quiescence wait (Phase 3) and the actual destruction
    // (Phase 4 below), flip the role's `context_valid_` flag to false.
    // Every ctrl-thread callback (on_notification, on_hub_dead, the
    // heartbeat periodic-tick) opens with `if (!core->context_valid())
    // bail` — so a slow-waker thread whose callback fires AFTER this
    // point sees an invalidated context and exits cleanly with a WARN
    // log instead of dereferencing the about-to-be-destroyed
    // `handler_`.  See `RoleHostCore`'s "Flag contract" header block
    // for the full discipline and the distinction from `is_running()`.
    //
    // Placed LATE on purpose: callbacks during normal teardown
    // (between Phase 2's `brc.stop()` and Phase 3's wait completing)
    // are still observed by the bracket+wait mechanism and still do
    // their full work (mark presences Deregistered, fire DEREG_REQ,
    // etc.).  Only the rare slow-waker case — where Phase 3 timed out
    // — relies on this fallback gate.
    if (pImpl->core != nullptr)
        pImpl->core->set_context_invalid();

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
    // Acquire pairs with the release store in start_handler_threads
    // Phase 4 init (audit M3, 2026-05-18) and with the release-ordered
    // bit-clearing from `on_hub_dead` lambdas via fetch_and.
    const auto mask = pImpl->connection_alive_mask_.load(std::memory_order_acquire);
    return (mask & (std::uint64_t{1} << i)) != 0;
}

std::size_t RoleAPIBase::connections_alive_count() const noexcept
{
    if (!pImpl->handler_) return 0;
    // Acquire pairs with the release store in start_handler_threads
    // Phase 4 init (audit M3, 2026-05-18) and with the release-ordered
    // bit-clearing from `on_hub_dead` lambdas via fetch_and.
    const auto mask = pImpl->connection_alive_mask_.load(std::memory_order_acquire);
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
void RoleAPIBase::set_critical_error(std::string_view msg)
{
    // Audit S2 (2026-05-18) — uniform "[role_tag/uid] CRITICAL: <msg>"
    // log line for non-empty messages, BEFORE flipping state so log
    // scrapers see the message adjacent to the stop event.  All three
    // engines (Python / Lua / Native C) route through here so the log
    // format is identical regardless of language.
    if (!msg.empty())
    {
        LOGGER_ERROR("[{}/{}] CRITICAL: {}",
                     pImpl->role_tag, pImpl->uid, msg);
    }
    pImpl->core->set_critical_error();
}
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

    // On SUCCESS only, register this band in the handler's band_index_
    // so subsequent band_* calls route to the same BRC.  Audit R3.5
    // (2026-05-17): pre-fix this checked only `result.has_value()`,
    // which treats a broker-error response (`{status: error, ...}`)
    // as success — polluting band_index_ with an entry that the
    // broker doesn't actually have us joined on.  Per HEP-CORE-0007
    // §12.3 a request-reply optional<json> carries the broker's
    // response body for BOTH success and error cases; only the
    // status="success" branch represents an admitted join.
    //
    // Pre-M5 single-presence roles always pick presences[0] as the
    // band owner (only one choice).  M5+ multi-presence roles will
    // need to decide which presence owns the band based on caller
    // context (in_hub vs out_hub); the on_band_joined() call
    // signature already accepts a Presence*, so the decision moves
    // to the role-host layer without changing this code.
    const bool joined =
        result.has_value() &&
        result->value("status", std::string{}) == "success";
    if (joined && pImpl->handler_)
    {
        const auto &presences = pImpl->handler_->presences();
        if (!presences.empty())
            pImpl->handler_->on_band_joined(channel, &presences.front());
    }
    // S4-3 (2026-05-19, HEP-CORE-0030 amendment): role-side bookkeeping
    // mirrors broker truth on `{status: error}`.  A broker error means
    // either the join attempt was rejected OR the role is somehow not
    // a member from the broker's view — either way, any stale
    // `band_index_` entry for this band is wrong and must be removed.
    // (`on_band_left` is a no-op when the entry doesn't exist, so this
    // is safe for the fresh-join-error case.)  `nullopt` (transport
    // timeout) leaves bookkeeping unchanged per the principle that
    // automatic recovery is the script's domain, not the framework's.
    const bool broker_rejected =
        result.has_value() &&
        result->value("status", std::string{}) == "error";
    if (broker_rejected && pImpl->handler_)
    {
        LOGGER_DEBUG("[{}] band_join('{}') broker_rejected — "
                     "clearing any stale band_index_ entry",
                     pImpl->role_tag, channel);
        pImpl->handler_->on_band_left(channel);
    }
    return result;
}

std::optional<nlohmann::json>
RoleAPIBase::band_leave(const std::string &channel)
{
    // Class D (band-bound) — route via handler's band_index_
    // (HEP-CORE-0033 §18.3).
    auto *bc = pImpl->resolve_bc_for_band(channel);
    if (!bc)
        return std::nullopt;
    auto result = bc->band_leave(channel);

    // S4-4 (2026-05-19, HEP-CORE-0030 amendment): role-side bookkeeping
    // mirrors broker truth on BOTH `status: success` AND `status:
    // error`.  Pre-fix this only erased on `success` per audit R3.2
    // (2026-05-17) which preserved routing on broker-error under the
    // reasoning "broker still has us in" — that reasoning was wrong
    // for the canonical error code (broker_proto 5 returns typed
    // `NOT_A_MEMBER` when the sender isn't a member, which means the
    // role's `band_index_` entry is stale and should be erased).
    // The broker-authority principle: role state mirrors the
    // broker's verdict, whatever it is.  `on_band_left` is a no-op
    // when no entry exists, so the cleanup is idempotent for cases
    // where the role thought it was joined but wasn't.  `nullopt`
    // (transport timeout) leaves bookkeeping unchanged — script's
    // domain to recover.
    const bool broker_responded =
        result.has_value();
    if (broker_responded && pImpl->handler_)
    {
        const std::string &status = result->value("status", std::string{});
        if (status == "success")
        {
            pImpl->handler_->on_band_left(channel);
        }
        else if (status == "error")
        {
            const std::string &code = result->value("error_code", std::string{});
            LOGGER_DEBUG("[{}] band_leave('{}') broker_rejected "
                         "(error_code='{}') — clearing band_index_ "
                         "entry per broker-authority principle",
                         pImpl->role_tag, channel, code);
            pImpl->handler_->on_band_left(channel);
        }
    }
    return result;
}

void RoleAPIBase::band_broadcast(const std::string &channel,
                                  const nlohmann::json &body)
{
    // Class D (band-bound) — route via handler's band_index_
    // (HEP-CORE-0033 §18.3).  Audit V3 (2026-05-18): WARN log on the
    // not-joined path — pre-fix this was a complete silent no-op (no
    // log on either the role or broker side, since the request never
    // reached the wire).  A caller broadcasting to an unjoined band
    // got zero observable feedback that nothing happened.  Now the
    // role-side at least logs the misrouting so operators see the
    // problem in the log.  Fire-and-forget semantics still preserved
    // — no return value to set.
    if (auto *bc = pImpl->resolve_bc_for_band(channel))
    {
        bc->band_broadcast(channel, body);
    }
    else
    {
        LOGGER_WARN("[{}] band_broadcast('{}') dropped — band not in this "
                    "role's index (must call band_join first)",
                    pImpl->role_tag, channel);
    }
}

std::optional<nlohmann::json> RoleAPIBase::band_members(const std::string &channel)
{
    // Class D (band-bound) — route via handler's band_index_
    // (HEP-CORE-0033 §18.3).
    auto *bc = pImpl->resolve_bc_for_band(channel);
    if (!bc)
        return std::nullopt;
    return bc->band_members(channel);
}

bool RoleAPIBase::is_in_band(const std::string &channel) const noexcept
{
    // S4-7 (HEP-CORE-0030 amendment 2026-05-19): role's cached view
    // of its own band membership.  Reads `band_index_` directly; no
    // broker round-trip.  See header docstring for the cache vs
    // authority distinction.
    if (pImpl->handler_ == nullptr) return false;
    return pImpl->handler_->is_in_band(channel);
}

// ============================================================================
// Inbox client management
// ============================================================================

std::optional<RoleAPIBase::InboxOpenResult>
RoleAPIBase::open_inbox_client(const std::string &target_uid)
{
    if (!pImpl->core)
        return std::nullopt;
    // Class B (role-bound) inbox discovery (ROLE_INFO_REQ per
    // HEP-CORE-0033 §18.2 + HEP-CORE-0027 §4.2).  Iterate
    // connections per §18.3 — first non-empty answer wins.  Target
    // role's inbox lives on whichever hub it registered with; we
    // don't know which hub a priori, so fall through.
    if (!pImpl->handler_) return std::nullopt;
    const auto &conns = pImpl->handler_->connections();
    if (conns.empty()) return std::nullopt;

    hub::SchemaSpec result_spec;
    std::string result_packing;

    auto entry = pImpl->core->open_inbox(target_uid,
        [&]() -> std::optional<RoleHostCore::InboxCacheEntry>
        {
            // Iterate connections; first query that returns a body
            // with an inbox_schema wins.  A "not found" response
            // (broker doesn't know this role) returns body without
            // inbox_schema → fall to next connection.
            nlohmann::json info;
            bool found = false;
            for (const auto &conn : conns)
            {
                auto *bc = conn.brc.get();
                if (!bc) continue;
                auto resp = bc->query_role_info(target_uid, 1000);
                if (!resp.has_value()) continue;     // transport failure
                auto sch = resp->value("inbox_schema", nlohmann::json{});
                if (sch.is_object() && sch.contains("fields"))
                {
                    info  = std::move(*resp);
                    found = true;
                    break;
                }
            }
            if (!found) return std::nullopt;

            auto inbox_schema = info.value("inbox_schema", nlohmann::json{});
            if (!inbox_schema.is_object() || !inbox_schema.contains("fields"))
                return std::nullopt;

            auto inbox_packing  = info.value("inbox_packing", std::string{});
            auto inbox_endpoint = info.value("inbox_endpoint", std::string{});
            auto inbox_checksum = info.value("inbox_checksum", std::string{});

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
    // Class B (role-bound) per HEP-CORE-0033 §18.3: iterate all
    // connections, first present-true wins.  For dual-hub processor
    // (connections to N hubs), a role registered on hub-N may be
    // invisible to a brc_for_role-only query (which returns
    // connection[0]).  We fall through each connection per poll
    // iteration until one reports the target present OR all answer
    // not-present.  Each connection's query has its own kPollMs
    // sub-budget; total poll period is N × kPollMs ≈ N × 200ms.
    if (!pImpl->handler_) return false;
    const auto &conns = pImpl->handler_->connections();
    if (conns.empty()) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;
    while (std::chrono::steady_clock::now() < deadline)
    {
        // Per HEP-CORE-0007 §12.3 (post-Bucket-C contract):
        // `query_role_presence` returns the broker's response body
        // (`{present: true, channel, role}` on found, `{present: false}`
        // on not-found, or `{present: false, error}` on missing role_uid)
        // or `nullopt` on transport failure.  Iterate connections;
        // first present-true wins.
        for (const auto &conn : conns)
        {
            auto *bc = conn.brc.get();
            if (!bc) continue;
            auto resp = bc->query_role_presence(uid, kPollMs);
            if (resp.has_value() && resp->value("present", false))
                return true;
            // Short-circuit on deadline mid-iteration so a slow first
            // connection doesn't lock us out of trying later ones.
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
        }
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
