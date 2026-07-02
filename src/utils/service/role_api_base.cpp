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
#include "utils/security/attach_protocol.hpp"        // HEP-0041 1i-mig-4 (#272)
#include "utils/security/key_store.hpp"              // HEP-0041 1i-mig-4 (#272)
#include "utils/security/shm_capability_channel.hpp" // HEP-0041 1i-mig-4 (#272)
#include "utils/shared_memory_spinlock.hpp"
#include "utils/thread_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace pylabhub::scripting
{

namespace {

/// Internal helper: mutex-guarded per-channel snapshot cache for the
/// symmetric script-observable peer surfaces (HEP-CORE-0036 §I11).
/// One instance holds the producer-side allowlist observation; another
/// holds the consumer-side producers observation.  See HEP-CORE-0011
/// §"Cross-Engine Surface Parity" Read-only observation surface
/// principle for why the two caches stay independent instances rather
/// than being merged on a direction axis.
class PeerCache
{
public:
    /// Returns a copy of the per-channel snapshot, or an empty vector
    /// if absent.  Takes the lock internally; callers iterate freely.
    [[nodiscard]] std::vector<AllowedPeer>
    snapshot(const std::string &channel) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(channel);
        if (it == map_.end()) return {};
        return it->second;
    }

    /// Replace the per-channel entry with `peers`.  Takes the lock
    /// internally.
    void put(const std::string &channel,
             std::vector<AllowedPeer> peers)
    {
        std::lock_guard<std::mutex> lk(mu_);
        map_[channel] = std::move(peers);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<AllowedPeer>> map_;
};

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct RoleAPIBase::Impl
{
    Impl(RoleHostCore *c, std::string rt, std::string id)
        : core(c),
          short_tag(std::move(rt)),
          uid(std::move(id)),
          thread_mgr_(std::make_unique<pylabhub::utils::ThreadManager>(
              short_tag, uid))
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

    /// HEP-CORE-0041 1i-mig-4 (#272) — Consumer-side SHM capability owner.
    ///
    /// Populated by `apply_consumer_reg_ack`'s SHM branch after the
    /// §5.5 ZAP-CURVE dial succeeds and the producer sends the memfd
    /// via `SCM_RIGHTS`.  Owns the mmap'd anonymous SHM region; its
    /// dtor `munmap`s and `close`s the original received fd at role
    /// teardown (consumer-side mirror of producer's `shm_transport_`
    /// on RoleHostFrame).  D1 (designer decision): RoleAPIBase owns
    /// the consumer side (symmetric with RoleHostFrame owning the
    /// producer side).
    ///
    /// Destruction order: `shm_consumer` is declared AFTER `rx_queue`
    /// above, so it is destroyed FIRST (reverse declaration order).
    /// Safe because `rx_queue` dups the fd internally on
    /// `set_shm_capability_fd` (substep 1f fd-source factory at
    /// `find_datablock_consumer_from_fd_impl`) — closing
    /// `shm_consumer`'s original fd does not invalidate the queue's
    /// own dup or its mmap.  The queue closes its dup at its own
    /// destruction (next in reverse-order).
    ///
    /// Null when:
    ///   - Channel uses ZMQ transport (no SHM capability to receive).
    ///   - SHM channel not yet authorized (REG_ACK didn't arrive or
    ///     hadn't fired the SHM branch when the role tears down).
    std::unique_ptr<utils::security::IShmCapabilityConsumer> shm_consumer;

    // HEP-CORE-0036 §I11 — symmetric script-observable peer caches.
    // Both stay independent PeerCache instances per HEP-CORE-0011
    // §"Cross-Engine Surface Parity" Read-only observation surface
    // principle: producer-side and consumer-side observation are
    // independent surfaces; the internal caches mirror that
    // separation.  See the file-local PeerCache helper above for the
    // lock + snapshot/put contract.
    //
    //   * allowlist_cache (producer-side):
    //     channel-allowlist cache for `api.allowed_peers(channel)`
    //     polling + `on_allowlist_changed` callback.  The authoritative
    //     enforcement set lives in `ZmqQueue::peer_allowlist_snapshot()`
    //     — this cache is a SCRIPT-CONVENIENCE COPY enriched with
    //     role_uid (which PeerIdentity doesn't carry).  Updated only by
    //     `handle_channel_auth_notifies` after `set_peer_allowlist`
    //     succeeds, so the cache and the ZAP enforcement state move
    //     together.
    //
    //   * producer_peer_cache (consumer-side):
    //     per-channel snapshot of the AUTHORIZED PRODUCERS that the
    //     broker delivered via `CONSUMER_REG_ACK.producers[]` (HEP-0036
    //     §6.4).  Updated by `apply_consumer_reg_ack` after the queue's
    //     `apply_master_approval` succeeds, so the cache and the queue's
    //     producer_peers_ move together.  Empty when SHM transport
    //     (CONSUMER_REG_ACK has no `producers[]` field per §5.6).
    PeerCache allowlist_cache;
    PeerCache producer_peer_cache;

    // ── Thread-local / set-once-before-spawn state ─────────────────────
    //
    // Every field below this banner is either:
    //   (a) set in the RoleAPIBase ctor (short_tag, uid), or
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
    std::string short_tag;   // "prod", "cons", "proc"
    hub::ChecksumPolicy checksum_policy{hub::ChecksumPolicy::Enforced};
    bool stop_on_script_error{false};
    std::string uid;
    std::string name;
    std::string channel;
    std::string out_channel;
    std::string log_level;
    std::string script_dir;
    std::string role_dir;

    // Flexzone introspection cache.  Populated exactly once at setup
    // time by RoleHostFrame; read by script-API calls (flexzone_logical_size,
    // has_*_fz).  See FlexzoneInfoCache struct in role_api_base.hpp
    // for the linear-forward-time contract.  While core's fz_spec
    // storage is still the legacy authoritative source, the frame
    // dual-writes both paths.
    FlexzoneInfoCache fz_info_cache{};

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
    // same dynamic lifecycle module "ThreadManager:" + short_tag, same
    // bounded join, same process-wide leak aggregator. Constructed
    // eagerly in the Impl ctor from short_tag + uid.
    std::unique_ptr<pylabhub::utils::ThreadManager> thread_mgr_;

    /// HEP-CORE-0023 §2.5 telemetry — count of `HEARTBEAT_REQ` frames
    /// successfully emitted by this role across all presences.
    /// Incremented in `on_heartbeat_tick_` per presence-emission;
    /// emitted as a one-shot summary at `stop_handler_threads` to make
    /// observed cadence verifiable post-mortem (Pattern 4 rung 3 +
    /// future ops dashboards).  Reset on a new RoleAPIBase instance;
    /// not persisted across reconnects.  `relaxed` ordering is
    /// sufficient — we are counting, not synchronising.
    std::atomic<std::uint64_t> heartbeats_sent_{0};
    std::chrono::steady_clock::time_point heartbeat_install_at_{};

    /// HEP-CORE-0042 §7 — most-recent `instance_id` echoed from
    /// PRODUCER_REG_ACK.  Assigned by the broker at every REG or
    /// re-REG; the producer echoes it verbatim on every
    /// CHANNEL_AUTH_APPLIED_REQ (Phase 3a.3, pending).
    /// **Overwritten** on every subsequent REG_ACK — if the producer
    /// registers on multiple channels, or re-registers after a
    /// broker-side DEREG / heartbeat-timeout, the counter advances
    /// and the stored value is the LAST assignment.  This matches
    /// broker semantics: only the current `instance[P]` matches
    /// APPLIED_REQ's echo (any prior value is stale by definition).
    /// Zero means "no REG_ACK observed yet"; a nonzero value is
    /// guaranteed by §5.5.3 (broker echo) + the hard-error in
    /// `apply_producer_reg_ack` on absent/zero.
    /// `relaxed` ordering — same rationale as `heartbeats_sent_`
    /// above (single-writer producer, occasional read; no
    /// synchronization dependency between the two counters).
    std::atomic<std::uint64_t> producer_instance_id_{0};

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
                         std::string   short_tag,
                         std::string   uid)
    : pImpl(std::make_unique<Impl>(&core, std::move(short_tag), std::move(uid)))
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

// set_short_tag and set_uid removed — identity is ctor-only.

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
        // Audit (2026-05-20, demo-harness discovery): the flexzone spec
        // is OPTIONAL — when no `out_flexzone_schema` is configured
        // (null in JSON, or omitted entirely), `opts.fz_spec.fields`
        // is empty.  Pre-fix this passed an empty spec into
        // `schema_spec_to_zmq_fields`, which throws "fields must not
        // be empty" and kills the worker thread on startup.  Gate the
        // conversion: empty fields → empty descriptor vector (the
        // ShmQueue layer accepts it as "no flexzone allocated").
        auto fz_fields = opts.fz_spec.fields.empty()
            ? std::vector<hub::SchemaFieldDesc>{}
            : hub::schema_spec_to_zmq_fields(opts.fz_spec);

        // HEP-CORE-0041 1i-mig-2: capability-transport path is the only
        // path.  The role host's IShmCapabilityProducer (substep 1b
        // backend) pre-allocated the memfd at exactly
        // datablock_layout_total_size(config) and populates
        // opts.shm_capability_fd.  Build the queue in Standby, attach
        // the borrowed fd, then start() drives Configured → Active via
        // the substep 1f fd-source factory
        // (create_datablock_producer_from_fd_impl).  The legacy
        // secret-based path (`ShmQueue::create_writer(..., shared_secret, ...)`)
        // retired in #275-S3.
        if (opts.shm_capability_fd < 0)
        {
            LOGGER_ERROR("[{}] build_tx_queue: SHM channel '{}' missing "
                         "shm_capability_fd (HEP-CORE-0041 1i-mig-2 — role "
                         "host must populate the borrowed fd before "
                         "build_tx_queue)",
                         pImpl->short_tag, tx_channel);
            return false;
        }
        std::unique_ptr<hub::ShmQueue> shm;
        {
            shm = hub::ShmQueue::create_writer_standby(
                tx_channel,
                hub::schema_spec_to_zmq_fields(opts.slot_spec), opts.slot_spec.packing,
                std::move(fz_fields),                            opts.fz_spec.packing,
                opts.shm_config.ring_buffer_capacity,
                opts.shm_config.physical_page_size,
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
                LOGGER_ERROR("[{}] ShmQueue create_writer_standby failed for '{}' "
                             "(HEP-CORE-0041 1i-mig-2 capability path)",
                             pImpl->short_tag, tx_channel);
                return false;
            }
            if (!shm->set_shm_capability_fd(opts.shm_capability_fd))
            {
                LOGGER_ERROR("[{}] ShmQueue::set_shm_capability_fd refused "
                             "for '{}' fd={} (HEP-CORE-0041 1i-mig-2 — see "
                             "queue WARN for reason)",
                             pImpl->short_tag, tx_channel,
                             opts.shm_capability_fd);
                return false;
            }
            if (!shm->start())
            {
                LOGGER_ERROR("[{}] ShmQueue::start failed for '{}' on "
                             "capability path (fd={}; see queue ERROR for "
                             "reason)",
                             pImpl->short_tag, tx_channel,
                             opts.shm_capability_fd);
                return false;
            }
        }
        writer.reset(shm.release()); // upcast: ShmQueue* → QueueWriter*
    }
    else if (opts.data_transport == "zmq")
    {
        if (opts.zmq_node_endpoint.empty())
        {
            LOGGER_ERROR("[{}] data_transport='zmq' but zmq_node_endpoint is empty",
                         pImpl->short_tag);
            return false;
        }
        // Compose a stable ThreadManager owner_id from role identity +
        // direction. Uniqueness across the process is guaranteed by the
        // role uid (which is a per-instance UID by construction).
        std::string inst_id = opts.instance_id.empty()
                                  ? (pImpl->short_tag + ":" + pImpl->uid + ":tx")
                                  : opts.instance_id;
        // Schema tag (frame-validation 8-byte identity) is derived
        // from slot_spec + fz_spec.  Auto-computed here so callers
        // aren't carrying a redundant schema_hash field in opts.
        const auto schema_hash = hub::compute_schema_hash(opts.slot_spec,
                                                           opts.fz_spec);
        // HEP-CORE-0035 §2 + §4.6.5: CURVE is unconditional on every
        // role↔hub data path.  Producer (PUSH/bind side) presents its
        // identity keypair to libzmq + installs a PeerAdmission
        // handler on the same ZMQ context's ZAP REP (HEP-CORE-0036
        // §7).  The initial allowlist is empty until
        // `apply_master_approval(REG_ACK)` seeds it from
        // `REG_ACK.initial_allowlist` at S3 per HEP-CORE-0036 §3.5.5
        // + §6.7 Option B.  Runtime refreshes arrive when the
        // role-host's BRC handler pulls `GET_CHANNEL_AUTH_REQ` in
        // response to `CHANNEL_AUTH_CHANGED_NOTIFY` per §6.5
        // (amendment 2026-06-04 — snapshot-push `CHANNEL_AUTH_UPDATE`
        // retired).  Tracked under task #103 (AUTH-1).  zap_domain is
        // per-(role,channel,side).
        // HEP-CORE-0040 §172: identity keypair lives in the process
        // KeyStore under `kRoleIdentityName` (seeded by
        // `RoleConfig::load_keypair`).  `push_to` resolves the
        // name inside its CURVE-setup block — secret bytes never
        // materialize on this side of the call.
        writer = hub::ZmqQueue::push_to(
            opts.zmq_node_endpoint,
            hub::schema_spec_to_zmq_fields(opts.slot_spec),
            opts.slot_spec.packing,
            /*identity_key_name=*/pylabhub::utils::security::kRoleIdentityName,
            /*zap_domain=*/pImpl->uid + ":" + tx_channel + ":tx",
            /*bind=*/opts.zmq_bind,
            /*schema_tag=*/make_schema_tag(schema_hash),
            /*sndhwm=*/0,
            /*send_buffer_depth=*/opts.zmq_buffer_depth,
            /*overflow_policy=*/opts.zmq_overflow_policy,
            /*send_retry_interval_ms=*/10,
            /*instance_id=*/std::move(inst_id));
        if (!writer)
            return false;
        // HEP-CORE-0036 §3.5.1 + §3.5.5 S1: tx queue is built in
        // Standby here; no PUSH bind, no ZAP arm.  The role host
        // drives Standby → Active later at S3 via
        // `apply_producer_reg_ack(REG_ACK)` once the broker has
        // accepted the producer's registration.
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
    if (opts.data_transport == "shm" && opts.slot_spec.has_schema)
    {
        // HEP-CORE-0041 1i-mig-4 (#272) — capability-transport rx path.
        // Build the ShmQueue in Standby; the fd arrives later via
        // `apply_consumer_reg_ack`'s SHM dispatch (dial → §5.5
        // handshake → SCM_RIGHTS recv → `set_shm_capability_fd` +
        // `start`).  Symmetric with `build_tx_queue`'s capability
        // path above.
        //
        // Test-only fast path: `opts.shm_capability_fd >= 0`
        // pre-populated drives Standby → Active immediately, analogous
        // to the ZMQ pre-populated-`producer_peers` case below.
        // Production never sets this field at build time — the
        // consumer doesn't have the fd until after REG_ACK arrives.
        auto shm = hub::ShmQueue::create_reader_standby(
            opts.shm_name,
            hub::schema_spec_to_zmq_fields(opts.slot_spec),
            opts.slot_spec.packing,
            rx_channel,
            /*verify_slot=*/false, /*verify_fz=*/false,
            pImpl->uid, pImpl->name);
        if (!shm)
        {
            LOGGER_ERROR("[{}] ShmQueue create_reader_standby failed for "
                         "channel='{}' shm_name='{}' (HEP-CORE-0041 "
                         "1i-mig-4 capability path)",
                         pImpl->short_tag, rx_channel, opts.shm_name);
            return false;
        }
        if (opts.shm_capability_fd >= 0)
        {
            if (!shm->set_shm_capability_fd(opts.shm_capability_fd))
            {
                LOGGER_ERROR("[{}] ShmQueue::set_shm_capability_fd refused "
                             "for channel='{}' fd={} (test-pre-populated "
                             "path; see queue WARN for reason)",
                             pImpl->short_tag, rx_channel,
                             opts.shm_capability_fd);
                return false;
            }
            if (!shm->start())
            {
                LOGGER_ERROR("[{}] ShmQueue::start failed for channel='{}' "
                             "on capability path (fd={}; see queue ERROR "
                             "for reason)",
                             pImpl->short_tag, rx_channel,
                             opts.shm_capability_fd);
                return false;
            }
        }
        reader.reset(shm.release());
    }
    else if (opts.data_transport == "zmq")
    {
        // HEP-CORE-0017 §3.3 + HEP-CORE-0036 §6.4 + §6.7: `producer_peers`
        // is the canonical source for the consumer's connect target +
        // CURVE serverkey, populated from `CONSUMER_REG_ACK.producers[]`.
        //
        // Per HEP-0036 §6.7 (the queue state machine), build_rx_queue
        // returns a ZmqQueue in Standby (empty producer_peers) OR
        // Configured (peers populated at construction time — test paths
        // and the legacy pre-AUTH-1 single-step shape).  The
        // Standby → Configured transition happens later via
        // `apply_consumer_reg_ack(ack)` on the RoleAPIBase, which
        // dispatches through the polymorphic
        // `QueueReader::apply_master_approval`.  Configured → Active is
        // driven by the same call, via `start()`.
        //
        // No early validation rejection here: an empty producer_peers
        // is the EXPECTED state for the AUTH-1 uniform role-host
        // pattern, where the broker delivers peers in CONSUMER_REG_ACK
        // AFTER queue construction.  The CURVE-unconditional invariant
        // (HEP-CORE-0035 §2) is enforced inside ZmqQueue::start() via
        // `is_configured()` — start refuses if no artifacts have
        // arrived, so a missing master delivery surfaces there, not
        // at queue construction.
        std::string inst_id = opts.instance_id.empty()
                                  ? (pImpl->short_tag + ":" + pImpl->uid + ":rx")
                                  : opts.instance_id;
        const auto expected_hash = hub::compute_schema_hash(opts.slot_spec,
                                                             opts.fz_spec);
        namespace sec = pylabhub::utils::security;

        // Standby vs Configured at construction: when producer_peers
        // is empty, pull_from accepts an empty endpoint + empty
        // server_pubkey and the queue enters Standby.  When peers are
        // pre-populated (test-only / legacy paths), the queue enters
        // Configured immediately and is eligible for start() now.
        const bool have_peer =
            !opts.producer_peers.empty()
            && !opts.producer_peers.front().endpoint.empty()
            && !opts.producer_peers.front().pubkey_z85.empty();
        const std::string rx_endpoint =
            have_peer ? opts.producer_peers.front().endpoint : std::string{};
        // HEP-CORE-0040 §8.4.1 Z85PublicKey construction contract.
        // Standby path: explicit `Z85PublicKey{}` sentinel — the
        // HEP-CORE-0036 §6.7 unset state until apply_consumer_reg_ack
        // delivers real peers.  Have-peer path: validate the wire
        // string via the throwing factory — a malformed pubkey in the
        // ACK is a fatal startup error (caught by main()'s phase
        // try/catch; clean exit-1 with diagnostic).
        const sec::Z85PublicKey server_pubkey =
            have_peer ? sec::Z85PublicKey::validate(
                            opts.producer_peers.front().pubkey_z85)
                      : sec::Z85PublicKey{};
        reader = hub::ZmqQueue::pull_from(
            rx_endpoint,
            server_pubkey,
            hub::schema_spec_to_zmq_fields(opts.slot_spec),
            opts.slot_spec.packing,
            /*identity_key_name=*/sec::kRoleIdentityName,
            /*bind=*/false,
            /*max_buffer_depth=*/opts.zmq_buffer_depth,
            /*schema_tag=*/make_schema_tag(expected_hash),
            /*instance_id=*/std::move(inst_id));
        if (!reader)
            return false;
        // Pre-populated peers: start immediately (legacy path
        // preservation).  Empty peers: queue stays Standby; the
        // role-host MUST call `apply_consumer_reg_ack(ack)` later —
        // any set_* args meanwhile are BUFFERED in Standby; merged
        // into Standby → Configured → Active by `apply_master_approval`
        // per HEP-CORE-0036 §6.7 Option B.
        if (have_peer && !reader->start())
        {
            LOGGER_ERROR("[{}] ZMQ PULL start() failed for '{}'",
                         pImpl->short_tag, rx_endpoint);
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

bool RoleAPIBase::apply_consumer_reg_ack(const nlohmann::json &ack)
{
    // Defensive wire-deserialization (HEP-CORE-0036 §I8 "soft
    // rejection" + §6.4 contract).  The broker is the contract-honoring
    // party for ACK shape, but a federation peer relaying the ACK, a
    // tampered message, or a future schema mismatch could deliver a
    // malformed payload (non-string channel_name/status, non-object
    // producers entries, etc.).  Catch nlohmann::json access errors at
    // the function boundary so the exception never escapes to the BRC
    // poll handler — log + return false instead of propagating.
    //
    // We deliberately do NOT catch std::bad_alloc or other system
    // errors here — those should propagate to the caller's
    // resource-management policy (apply_consumer_reg_ack runs on a
    // startup path where OOM is fatal at the role-host level).
    try
    {
        // Hoist the "producers" field lookup once: used by the
        // rx_queue-null error log, the success-path info log, and the
        // cache commit below.  The "[]" literal avoids constructing a
        // temporary nlohmann::json::array() when the field is absent
        // (SHM ACK per §5.6).  The .dump() cost is intrinsic to the
        // marker shape — operators see the full producer list in the
        // log for diagnostic value; accepted overhead on this
        // per-role-lifetime path.
        const bool has_producers = ack.contains("producers");
        const std::string producers_dump =
            has_producers ? ack["producers"].dump() : "[]";

        // Pre-parse script_view + channel_name BEFORE queue mutation.
        // This eliminates the half-success window: if any of these
        // parsing operations throws (malformed wire payload), the
        // queue state is still unchanged — apply_master_approval
        // hasn't run yet.  Without this pre-parse, a malformed
        // producers[] entry could cause apply_master_approval to
        // succeed (queue Active) followed by a parse throw here,
        // leaving the queue Active without a valid script cache view
        // and the role-host's failure path having to tear down a
        // live socket + worker thread.  ack["producers"] (when
        // present) is guaranteed to be an array by the post-parse
        // apply_master_approval check at hub_zmq_queue.cpp:948-952,
        // but per-entry parsing here may throw on a malformed entry
        // shape that apply_master_approval would also reject.
        std::vector<AllowedPeer> script_view;
        if (has_producers)
        {
            const auto &producers = ack.at("producers");
            script_view.reserve(producers.size());
            for (const auto &p : producers)
            {
                if (!p.is_object()) continue;
                // Wire shape: {role_uid, pubkey, endpoint}.  Script
                // view exposes only the IDENTITY half ({role_uid,
                // pubkey}) — endpoint is transport-layer detail per
                // §I11 (scripts read membership, not connection
                // mechanics).
                AllowedPeer entry;
                entry.role_uid = p.value("role_uid", std::string{});
                // HEP-CORE-0036 §5b B-4 (#289, 2026-06-25) — single
                // canonical key `pubkey_z85`.  Pre-B-4 the broker
                // emitted `pubkey` and we fell back to `pubkey_z85`
                // here; B-4 unified the broker emit to `pubkey_z85`
                // (matching the §I10 one-pubkey-per-uid invariant
                // and the §5b unified shape), and the dual-name
                // fallback is dropped.
                entry.pubkey = p.value("pubkey_z85", std::string{});
                if (!entry.pubkey.empty())
                    script_view.push_back(std::move(entry));
            }
        }
        // HEP-CORE-0036 §5b B-5 (#290, 2026-06-26) — hard-error on
        // missing canonical wire fields.  Pre-B-5 the function
        // tolerated empty `channel_name` and silently skipped the
        // cache commit + Authorized FSM transition further down
        // (`if (!channel_name.empty() && ...)` patterns), masking
        // broker-contract violations as "ACK applied" returns.
        // Post-B-1..B-4 every emitter route is unified on
        // `channel_name`, so absence is unambiguously a contract
        // violation worth a hard error.
        const auto channel_name =
            ack.value("channel_name", std::string{});
        if (channel_name.empty())
        {
            LOGGER_ERROR(
                "[{}] apply_consumer_reg_ack: ACK missing required "
                "`channel_name` (HEP-CORE-0036 §5b.7 + B-5 #290 — "
                "broker contract violation; pre-B-5 this would have "
                "silently skipped cache commit + Authorized FSM "
                "transition).",
                pImpl->short_tag);
            return false;
        }

        // rx_queue precondition check.  The ack-received LOGGER_INFO
        // below is pinned by Pattern 4 rung 4 as step 5 of the success
        // sequence (followed by the queue's Standby→Configured +
        // Configured→Active markers as steps 6 + 7).  Firing the
        // LOGGER_INFO on the null-rx_queue path would advance the test
        // ladder past a step that didn't actually complete, producing
        // a misleading "step 6 timeout" diagnostic that masks the
        // actual rx_queue wiring bug.  The null-rx_queue path instead
        // emits a LOGGER_ERROR with the full ACK content so operators
        // still see the ACK that was dropped (operational
        // observability via the error log).
        if (!pImpl->rx_queue)
        {
            LOGGER_ERROR("[{}] apply_consumer_reg_ack: rx_queue not "
                         "wired — discarding ACK (channel='{}' "
                         "status='{}' producers={})",
                         pImpl->short_tag,
                         ack.value("channel_name", "?"),
                         ack.value("status", "?"),
                         producers_dump);
            return false;
        }

        // rx_queue is wired; emit the observable ACK reception marker
        // (HEP-CORE-0036 §6.4).  Logged BEFORE apply_master_approval
        // so this marker chronologically precedes the queue's
        // Standby->Configured + Configured->Active markers that
        // apply_master_approval emits internally — matching the rung
        // 4 expected sequence order.
        LOGGER_INFO("[{}] event=ConsumerRegAckReceived channel='{}' "
                    "status={} producers={}",
                    pImpl->short_tag,
                    ack.value("channel_name", "?"),
                    ack.value("status", "?"),
                    producers_dump);

        // HEP-CORE-0036 §5b B-4 (#289, 2026-06-25) — unified
        // CONSUMER_REG_ACK shape across transports.  Pre-B-4 the
        // broker emitted flat `shm_capability_endpoint` +
        // `producer_pubkey_z85` for SHM and `producers[]` for ZMQ,
        // and we discriminated by "which field name is present" — a
        // fragile pattern of the same silent-gate class as the
        // pre-B-1 `channel_id` vs `channel_name` bug.  Post-B-4 both
        // transports emit `producers[]` with `pubkey_z85` keyed
        // consistently, and the broker echoes `data_transport` so
        // dispatch is data-driven instead of shape-driven.
        //
        // D2 (designer decision, unchanged from 1i-mig-4): SHM does
        // NOT route through `apply_master_approval` (the ZMQ-PULL
        // peer-set mutator).  SHM activation runs the §5.5 ZAP-CURVE
        // handshake, recvs the memfd via SCM_RIGHTS, hands the fd to
        // the rx queue, and starts it — a separate code path.
        // HEP-CORE-0036 §5b B-5 (#290, 2026-06-26) — hard-error on
        // missing / unknown `data_transport`.  Pre-B-5 a missing
        // value fell through to the ZMQ branch as a temporary
        // bridge while older tests were migrated; B-4 unified the
        // emit so every CONSUMER_REG_ACK now carries the echo, and
        // the bridge is dead weight.  An unknown value is also a
        // hard error — silent dispatch to the wrong code path
        // (ZMQ when SHM is intended, or vice versa) would lead to
        // a confusing later failure deep in the queue or attach
        // protocol.
        const std::string data_transport_echo =
            ack.value("data_transport", std::string{});
        if (data_transport_echo != "shm" && data_transport_echo != "zmq")
        {
            LOGGER_ERROR(
                "[{}] apply_consumer_reg_ack: channel='{}' ACK has "
                "missing or unknown `data_transport`='{}' "
                "(HEP-CORE-0036 §5b.7 + B-5 #290 — must be one of "
                "{{\"shm\",\"zmq\"}}; broker contract violation).",
                pImpl->short_tag, channel_name, data_transport_echo);
            return false;
        }
        if (data_transport_echo == "shm")
        {
            // SHM is single-producer per HEP-CORE-0023 §2.1.1
            // cardinality; the broker has already rejected a second
            // SHM producer at REG_REQ time.  Take producers[0].
            if (!ack.contains("producers") || !ack["producers"].is_array() ||
                ack["producers"].empty())
            {
                LOGGER_ERROR(
                    "[{}] apply_consumer_reg_ack: SHM channel='{}' "
                    "broker ACK missing producers[] (HEP-CORE-0036 §5b "
                    "unified shape)",
                    pImpl->short_tag, channel_name);
                return false;
            }
            const auto &p = ack["producers"][0];
            const std::string shm_endpoint =
                p.value("endpoint", std::string{});
            const std::string producer_pubkey_z85 =
                p.value("pubkey_z85", std::string{});
            LOGGER_INFO("[{}] event=ShmCapabilityFieldsReceived channel='{}' "
                        "endpoint='{}' producer_pubkey_z85_len={}",
                        pImpl->short_tag, channel_name, shm_endpoint,
                        producer_pubkey_z85.size());
            if (!apply_consumer_reg_ack_shm_(
                    channel_name, shm_endpoint, producer_pubkey_z85))
            {
                return false;
            }
        }
        else
        {
            // ZMQ-PULL path — HEP-CORE-0042 §7.1 pre-attach coordination
            // BEFORE Standby → Active.  Per the §7.1 "Placement decision"
            // (Option A), the consumer_attach_zmq loop runs FIRST and
            // filters `ack.producers[]` down to the admitted subset;
            // only then does `apply_master_approval` see the filtered
            // ACK.  Rationale (see §7.1):
            //   1. Consumer never dials denied producers — no ZAP
            //      handshake ever runs against a broker-rejected peer.
            //   2. `apply_master_approval` sees truth-of-record — the
            //      bytes it processes MATCH the bytes the queue-level
            //      tests observe (HEP-CORE-0036 §6.7 single-mutator
            //      invariant).
            //   3. Failure mode is a no-op, not a rollback — zero
            //      admitted → filtered ACK carries `producers=[]` →
            //      queue stays in Standby → this function returns false.
            //
            // §7.1 partial-success policy: the loop runs to completion.
            // Loop-level failure returns false ONLY when ZERO producers
            // were admitted.  Failed producers are excluded from the
            // dial set; no automatic retry (retry happens on next
            // consumer restart per §7.1 P0/P3 principles).

            // Consumer's pubkey — from KeyStore.  Symmetric with the
            // SHM `apply_consumer_reg_ack_shm_` path (line ~1186);
            // the ACK does NOT carry the consumer's own pubkey (the
            // broker knows it from CONSUMER_REG_REQ), so we source
            // it from the trusted identity store.
            namespace sec = pylabhub::utils::security;
            std::string consumer_pubkey_z85;
            try
            {
                consumer_pubkey_z85 = std::string{
                    sec::key_store().pubkey(sec::kRoleIdentityName)};
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR(
                    "[{}] apply_consumer_reg_ack: ZMQ channel '{}' — "
                    "KeyStore::pubkey('{}') threw: {} (consumer cannot "
                    "issue §7.1 pre-attach REQs)",
                    pImpl->short_tag, channel_name,
                    sec::kRoleIdentityName, e.what());
                return false;
            }

            auto *brc = pImpl->resolve_bc_for_channel(channel_name);
            if (!brc)
            {
                LOGGER_ERROR(
                    "[{}] apply_consumer_reg_ack: ZMQ channel '{}' — "
                    "no BRC resolved; cannot issue §7.1 pre-attach REQs",
                    pImpl->short_tag, channel_name);
                return false;
            }

            // Extract producers[] from ACK.  B-5 upstream guarantees
            // this field is present + array for ZMQ; check anyway to
            // surface a broker regression cleanly.
            if (!ack.contains("producers") || !ack["producers"].is_array())
            {
                LOGGER_ERROR(
                    "[{}] apply_consumer_reg_ack: ZMQ ACK for channel "
                    "'{}' missing/malformed `producers` field — HEP-"
                    "CORE-0036 §5b B-5 broker contract violation.",
                    pImpl->short_tag, channel_name);
                return false;
            }
            const auto &producers_arr = ack["producers"];
            LOGGER_INFO(
                "[{}] attach:begin channel={} producers={}",
                pImpl->short_tag, channel_name, producers_arr.size());

            nlohmann::json admitted_producers = nlohmann::json::array();
            for (const auto &p : producers_arr)
            {
                const auto producer_uid =
                    p.value("role_uid", std::string{});
                if (producer_uid.empty())
                {
                    // Malformed entry (missing role_uid).  HEP-0036
                    // §5b requires role_uid in every producers[] row.
                    // Treat as broker contract violation.
                    LOGGER_ERROR(
                        "[{}] apply_consumer_reg_ack: producers[] entry "
                        "missing `role_uid` (channel '{}')",
                        pImpl->short_tag, channel_name);
                    return false;
                }

                auto reply = brc->consumer_attach_zmq(
                    channel_name, pImpl->uid, consumer_pubkey_z85,
                    producer_uid);

                // §7.1 null-synthesis: on client-side BRC failure /
                // timeout, synthesize the SAME §5.6 reason string
                // used for broker-observed timeouts — the two failure
                // modes are indistinguishable from the script's
                // perspective, and the reason enum is closed to §5.6
                // taxonomy per §8.
                std::string status;
                std::string reason;
                if (!reply.has_value())
                {
                    status = "timeout";
                    reason = "producer_did_not_confirm_within_budget";
                }
                else
                {
                    status = reply->value("status", std::string{});
                    reason = reply->value("reason", std::string{});
                }

                if (status == "success")
                {
                    admitted_producers.push_back(p);
                    LOGGER_INFO(
                        "[{}] attach:success channel={} producer={}",
                        pImpl->short_tag, channel_name, producer_uid);
                }
                else if (status == "denied")
                {
                    LOGGER_WARN(
                        "[{}] attach:denied channel={} producer={} "
                        "reason={}",
                        pImpl->short_tag, channel_name, producer_uid,
                        reason);
                }
                else if (status == "timeout")
                {
                    LOGGER_WARN(
                        "[{}] attach:timeout channel={} producer={} "
                        "reason={}",
                        pImpl->short_tag, channel_name, producer_uid,
                        reason);
                }
                else
                {
                    // Unknown status — broker regression.  Log WARN
                    // (not ERROR — the loop continues per §7.1) but
                    // exclude from admitted since we can't reason
                    // about the outcome.
                    LOGGER_WARN(
                        "[{}] attach:unknown_status channel={} producer={}"
                        " status='{}' reason='{}' (broker regression?)",
                        pImpl->short_tag, channel_name, producer_uid,
                        status, reason);
                }
            }

            LOGGER_INFO(
                "[{}] attach:complete channel={} admitted={}/{}",
                pImpl->short_tag, channel_name,
                admitted_producers.size(), producers_arr.size());

            // §7.1 partial-success policy — zero admitted → return
            // false.  The filtered ACK below would carry
            // `producers=[]` which `apply_master_approval` treats as
            // "no peer-set change" (queue-side no-op for empty peers
            // — see HEP-CORE-0036 §I11 + hub_zmq_queue.cpp:945).
            // Returning false HERE surfaces the failure to the role
            // host at startup instead of leaving the queue in Standby
            // with no diagnostic.
            if (admitted_producers.empty())
            {
                LOGGER_WARN(
                    "[{}] apply_consumer_reg_ack: ZERO producers admitted"
                    " for channel '{}' — queue stays in Standby "
                    "(HEP-CORE-0042 §7.1 partial-success policy: "
                    "return false)",
                    pImpl->short_tag, channel_name);
                return false;
            }

            // Feed the queue the FILTERED ACK (Option A per §7.1
            // Placement decision): only admitted producers reach the
            // Standby → Active transition.
            nlohmann::json filtered_ack = ack;
            filtered_ack["producers"] = std::move(admitted_producers);

            if (!pImpl->rx_queue->apply_master_approval(filtered_ack))
            {
                LOGGER_ERROR(
                    "[{}] apply_consumer_reg_ack: apply_master_approval "
                    "refused post-filter (HEP-CORE-0036 §6.7 'fully "
                    "refused' — malformed broker delivery or the "
                    "filtered ACK produced by §7.1 loop is queue-"
                    "unacceptable)",
                    pImpl->short_tag);
                return false;
            }
        }

        // Queue is Active.  Commit the pre-parsed cache view
        // atomically under the cache mutex.  The map assignment is
        // no-throw apart from std::bad_alloc — the try block catches
        // only nlohmann::json::exception, so bad_alloc propagates to
        // the caller.  Failure-mode invariant: if cache.put throws,
        // the Authorized transition below does NOT fire; the FSM
        // stays at `Registered`, the §8.2 outer guard refuses the
        // data loop, the run_data_loop WARN at data_loop.hpp:244
        // surfaces the half-state.  No silent corruption.  Per
        // HEP-CORE-0036 §I8 OOM at this startup point is treated as
        // fatal at the role-host level (caller tears down via RAII).
        //
        // HEP-CORE-0036 §I11 + §6.4 cache contract: mirror the queue's
        // no-op tolerance at hub_zmq_queue.cpp:945 — when the
        // `producers` field is ABSENT (SHM ACK per §5.6, or a future
        // runtime refresh that carries no peer-set change), the queue
        // preserves its `producer_peers_` and the cache MUST also
        // preserve, otherwise the two views drift.  When the field is
        // PRESENT (even as an empty array), the queue does a
        // snapshot-replace and the cache mirrors that.
        //
        // Eventual-consistency property (HEP-CORE-0036 §I11): the
        // queue's `producer_peers_` (above, under the queue's own
        // mutex) and this cache (below, under PeerCache::put's
        // internal lock) are updated by two separate critical
        // sections, in causal order.  A script poll of api.producers(channel) between
        // them sees the OLD cache view — a microsecond stale window
        // on a per-registration (startup-only) path.  This is
        // acceptable: the cache is ADVISORY observation only.  Actual
        // data-plane authorization runs through the queue's CURVE
        // handshake against `producer_peers_` directly; the cache
        // cannot mis-admit a peer.  Atomic queue+cache coupling would
        // require exposing a queue update callback (HEP §6.5.1 future
        // work).
        // B-5 (#290): channel_name is non-empty by construction
        // here — the early hard-error above returned false on absent.
        if (has_producers)
        {
            pImpl->producer_peer_cache.put(
                channel_name, std::move(script_view));
        }

        // HEP-CORE-0036 §4.3.2 — Registered → Authorized at the END
        // of apply_consumer_reg_ack success.  By this point: REG_ACK
        // accepted (Layers 1+2), apply_master_approval succeeded
        // (Layer 3 — PULL configured, per-producer connect with
        // CURVE serverkey, queue Active), cache committed.  The
        // role's data plane is armed; the §8.2 outer guard
        // (`any_presence_authorized()`) will now admit this presence
        // to the data loop.
        // B-5 (#290): channel_name is non-empty by construction.
        // `handler_` may legitimately be null in some test fixtures
        // (no presence routing); guard that one only.
        if (pImpl->handler_)
        {
            Presence *presence = pImpl->handler_
                ->find_presence_for_channel(channel_name);
            if (presence)
            {
                presence->registration_state.store(
                    RegistrationState::Authorized,
                    std::memory_order_release);
                LOGGER_INFO("[{}] event=PresenceStateTransition "
                            "channel='{}' role_type=consumer "
                            "from=Registered to=Authorized "
                            "trigger=apply_consumer_reg_ack_done",
                            pImpl->short_tag, channel_name);
            }
        }
        return true;
    }
    catch (const nlohmann::json::exception &e)
    {
        // Malformed wire payload from broker / federation peer.
        // HEP-CORE-0036 §I8 soft rejection: log + return false; the
        // caller (consumer_role_host worker_main_) treats this as a
        // fatal registration failure and tears down via RAII.
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: malformed broker "
                     "delivery — rejecting (json error: {})",
                     pImpl->short_tag, e.what());
        return false;
    }
}

#if defined(PYLABHUB_PLATFORM_LINUX)

bool RoleAPIBase::apply_consumer_reg_ack_shm_(
    const std::string &channel_name,
    const std::string &shm_endpoint,
    const std::string &producer_pubkey_z85)
{
    namespace sec = pylabhub::utils::security;

    if (shm_endpoint.empty() || producer_pubkey_z85.empty())
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' "
                     "ack missing required fields "
                     "(shm_capability_endpoint='{}' "
                     "producer_pubkey_z85_len={})",
                     pImpl->short_tag, channel_name, shm_endpoint,
                     producer_pubkey_z85.size());
        return false;
    }

    // Assemble ConsumerAuthMaterial from the local KeyStore.  D1
    // (HEP-CORE-0040 §8.5.1 use-not-export): the seckey accessor closes
    // over the global key_store() lookup; no seckey byte materialises
    // on this side of the §5.5 handshake.  Symmetric with the
    // producer-side acceptor wiring in
    // RoleHostFrame::spawn_shm_auth_listener_.
    sec::ConsumerAuthMaterial auth;
    auth.role_uid    = pImpl->uid;
    try
    {
        auth.pubkey_z85 = std::string{sec::key_store().pubkey(sec::kRoleIdentityName)};
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' — "
                     "KeyStore::pubkey('{}') threw: {} (consumer cannot "
                     "assemble auth material)",
                     pImpl->short_tag, channel_name,
                     sec::kRoleIdentityName, e.what());
        return false;
    }
    auth.seckey_accessor =
        [](std::function<void(std::span<const std::byte>)> use) {
            sec::key_store().with_seckey(
                sec::kRoleIdentityName,
                [&](std::string_view sv) {
                    use(std::span<const std::byte>(
                        reinterpret_cast<const std::byte *>(sv.data()),
                        sv.size()));
                });
        };

    // D3: bounded retry on ECONNREFUSED to absorb the H3a race window
    // where REG_ACK can reach the consumer before the producer's L2
    // listener bind has happened.  ~10×100ms = up to ~1s of tolerance;
    // beyond that we fail the registration.  ENOENT (socket file not
    // yet visible) returns nullopt the same way ECONNREFUSED does, so
    // both cases reuse this loop.
    constexpr int  kMaxDialAttempts   = 10;
    constexpr auto kDialAttemptPeriod = std::chrono::milliseconds{100};
    std::optional<int> connected_fd_opt;
    int                attempts_used = 0;
    for (int attempt = 0; attempt < kMaxDialAttempts; ++attempt)
    {
        ++attempts_used;
        try
        {
            connected_fd_opt = sec::initiate_consumer_handshake(
                shm_endpoint, auth, producer_pubkey_z85,
                kDialAttemptPeriod);
        }
        catch (const std::exception &e)
        {
            // Protocol-level failures (framing / JSON / size / sodium):
            // not the H3a race shape.  Bail immediately.
            LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' "
                         "handshake to '{}' threw on attempt {}/{}: {}",
                         pImpl->short_tag, channel_name, shm_endpoint,
                         attempt + 1, kMaxDialAttempts, e.what());
            return false;
        }
        if (connected_fd_opt.has_value())
            break; // success — fd ready for SCM_RIGHTS recv
        // nullopt = transport-level connect failure
        // (ECONNREFUSED/ENOENT).  Brief sleep before retry — keeps the
        // total wait ~1s which is well under the BRC consumer_attach
        // 2000ms ceiling (#280 EDGE-2) so the producer-side accept
        // thread doesn't time out while we retry.
        if (attempt + 1 < kMaxDialAttempts)
            std::this_thread::sleep_for(kDialAttemptPeriod);
    }
    if (!connected_fd_opt.has_value())
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' "
                     "handshake to '{}' connect-refused after {} attempts "
                     "(~{}ms total) — H3a race exceeded tolerance window",
                     pImpl->short_tag, channel_name, shm_endpoint,
                     attempts_used,
                     attempts_used * kDialAttemptPeriod.count());
        return false;
    }
    const int connected_fd = *connected_fd_opt;

    // Recv the SHM memfd via SCM_RIGHTS on the post-handshake socket.
    // The factory takes ownership of `connected_fd` (closes on every
    // path) and returns an IShmCapabilityConsumer owning the received
    // memfd + the mmap.  Timeout 2000ms gives the producer's L2c
    // orchestrator generous headroom to run the broker pre-confirm
    // (CONSUMER_ATTACH_REQ_SHM → ACK) + cache lookup + send_capability.
    std::unique_ptr<sec::IShmCapabilityConsumer> consumer;
    try
    {
        consumer = sec::attach_shm_capability_consumer_from_socket(
            connected_fd, std::chrono::milliseconds{2000});
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' — "
                     "SCM_RIGHTS recv failed: {}",
                     pImpl->short_tag, channel_name, e.what());
        return false;
    }

    const int memfd = consumer->borrow_fd();
    if (memfd < 0)
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' — "
                     "IShmCapabilityConsumer returned invalid borrow_fd",
                     pImpl->short_tag, channel_name);
        return false;
    }

    // Hand the fd to the rx queue + activate.  Ownership note: the
    // queue dups the fd internally (substep 1f fd-source factory), so
    // `consumer` keeps owning the original memfd + mmap for the role's
    // lifetime; rx_queue closes its own dup at its destruction.
    if (!pImpl->rx_queue->set_shm_capability_fd(memfd))
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' — "
                     "ShmQueue::set_shm_capability_fd refused (fd={}; "
                     "see queue WARN for reason)",
                     pImpl->short_tag, channel_name, memfd);
        return false;
    }
    if (!pImpl->rx_queue->start())
    {
        LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' — "
                     "ShmQueue::start failed (fd={}; see queue ERROR for "
                     "reason)",
                     pImpl->short_tag, channel_name, memfd);
        return false;
    }

    // Commit ownership AFTER successful activation.  D1: lifetime
    // outlives the rx queue's mmap (the queue must release its mapping
    // before the role's stop_handler_threads tears RoleAPIBase down,
    // which destroys this Impl and the consumer with it).
    pImpl->shm_consumer = std::move(consumer);
    LOGGER_INFO("[{}] event=ShmCapabilityActivated channel='{}' "
                "endpoint='{}' attempts={} (HEP-CORE-0041 1i-mig-4)",
                pImpl->short_tag, channel_name, shm_endpoint,
                attempts_used);
    return true;
}

#else // non-Linux platforms

bool RoleAPIBase::apply_consumer_reg_ack_shm_(
    const std::string &channel_name,
    const std::string & /*shm_endpoint*/,
    const std::string & /*producer_pubkey_z85*/)
{
    // HEP-CORE-0041 §6.5 — symmetric "no backend on this platform"
    // surface.  Mirrors the #error shape on the L1/L2b/L2c .cpp files
    // (which fire at compile time on non-Linux).  This runtime branch
    // only matters for builds that selectively exclude the security
    // libs at compile time — production builds on
    // FreeBSD/macOS/Windows fail to link before they get here, per
    // tasks #259/#260/#261.
    LOGGER_ERROR("[{}] apply_consumer_reg_ack: SHM channel '{}' — "
                 "capability transport not implemented on this platform "
                 "(HEP-CORE-0041 §6.5 — Linux only in Phase 1)",
                 pImpl->short_tag, channel_name);
    return false;
}

#endif // PYLABHUB_PLATFORM_LINUX

bool RoleAPIBase::apply_producer_reg_ack(const nlohmann::json &ack)
{
    if (!pImpl->tx_queue)
    {
        LOGGER_ERROR("[{}] apply_producer_reg_ack: tx_queue not wired",
                     pImpl->short_tag);
        return false;
    }
    // Producer-side mirror of apply_consumer_reg_ack.  Drive Standby
    // → Active per HEP-CORE-0036 §6.7 Option B.  For PUSH queues,
    // apply_master_approval extracts ack["initial_allowlist"] (array
    // of Z85 pubkey strings per HEP-0036 §6.2), seeds the ZAP cache,
    // and calls start() internally to bind and spawn the PUSH worker.
    // For SHM tx, it is a no-op today (config-supplied secret).
    if (!pImpl->tx_queue->apply_master_approval(ack))
    {
        LOGGER_ERROR("[{}] apply_producer_reg_ack: apply_master_approval "
                     "refused (HEP-CORE-0036 §6.7 'fully refused' — "
                     "malformed broker delivery)", pImpl->short_tag);
        return false;
    }

    // HEP-CORE-0036 §5b B-5 (#290, 2026-06-26) — hard-error on
    // missing `channel_name`.  Symmetric with the consumer-side
    // check above; same rationale (pre-B-5 silent skip masked
    // broker-contract violations as "ACK applied" returns).
    const auto channel_name = ack.value("channel_name", std::string{});
    if (channel_name.empty())
    {
        LOGGER_ERROR(
            "[{}] apply_producer_reg_ack: ACK missing required "
            "`channel_name` (HEP-CORE-0036 §5b.4 + B-5 #290 — broker "
            "contract violation; pre-B-5 this would have silently "
            "skipped `initial_allowlist` cache seed + the Registered "
            "→ Authorized FSM transition).",
            pImpl->short_tag);
        return false;
    }

    // HEP-CORE-0042 §5.5.3 — capture the broker-echoed `instance_id`
    // for use on subsequent CHANNEL_AUTH_APPLIED_REQ (Phase 3a.3
    // emission, pending).  §5.2 defines the counter as monotonic
    // per-uid starting at 1; a zero or absent field is a broker
    // contract violation.  Hard-error at the boundary — a silent
    // skip here would leave `producer_instance_id_ == 0` and Phase
    // 3a.3's APPLIED_REQ would be rejected by the broker's
    // stale-instance guard (§5.4 step a), which would surface as a
    // hard-to-diagnose "APPLIED_REQ never advances confirmed_version"
    // symptom instead of the actual "REG_ACK contract broken"
    // root cause.
    const auto instance_id = ack.value("instance_id", std::uint64_t{0});
    if (instance_id == 0)
    {
        LOGGER_ERROR(
            "[{}] apply_producer_reg_ack: ACK missing/zero "
            "`instance_id` for channel '{}' — HEP-CORE-0042 §5.5.3 "
            "requires PRODUCER_REG_ACK to echo the broker-assigned "
            "instance counter (§5.2 monotonic uint64, starts at 1).  "
            "A zero value would silently defeat the stale-instance "
            "guard on CHANNEL_AUTH_APPLIED_REQ (§5.4 step a).",
            pImpl->short_tag, channel_name);
        return false;
    }
    pImpl->producer_instance_id_.store(instance_id,
                                        std::memory_order_relaxed);
    LOGGER_INFO(
        "[{}] event=ProducerInstanceIdCaptured channel='{}' "
        "instance_id={} (HEP-CORE-0042 §5.5.3)",
        pImpl->short_tag, channel_name, instance_id);

    // HEP-CORE-0036 §3.6 REG_ACK note + §I11.1 cache architecture:
    // seed the script-side `allowlist_cache` from REG_ACK.initial_allowlist
    // and fire `on_allowlist_changed(reason="initial_seed")` so a script
    // observing the callback (or polling `api.allowed_peers(channel)`)
    // sees the live set immediately on (re)connect — no waiting for the
    // next CHANNEL_AUTH_CHANGED_NOTIFY doorbell.  Same shape as the
    // NOTIFY-driven path in `handle_channel_auth_notifies` (one cache,
    // transport-agnostic, callback fires on every write — §I11.1
    // invariants #1–#3).  For SHM the cache is observability-only
    // (HEP-CORE-0041 §9 D4 broker pre-confirm at attach IS the gate).
    //
    // **Missing `initial_allowlist` MUST NOT clobber the cache** —
    // §I11.1 invariant #5 + HEP-0036 §6.2 broker contract require the
    // field to be present (empty array on a fresh channel, never absent).
    // An absent field is a broker contract violation; log WARN, preserve
    // the prior cache snapshot, do not fire the callback.
    {
        if (ack.contains("initial_allowlist") &&
            ack.at("initial_allowlist").is_array())
        {
            const auto &initial_allowlist = ack.at("initial_allowlist");
            std::vector<AllowedPeer> script_view;
            for (const auto &entry : initial_allowlist)
            {
                if (!entry.is_string()) continue;
                const auto pk = entry.get<std::string>();
                if (pk.empty()) continue;
                script_view.push_back(AllowedPeer{
                    /*role_uid=*/std::string{}, pk});
            }
            pImpl->allowlist_cache.put(channel_name, script_view);
            LOGGER_INFO(
                "[{}] event=InitialAllowlistSeeded channel='{}' size={} "
                "(HEP-CORE-0036 §3.6 + §I11.1)",
                pImpl->short_tag, channel_name, script_view.size());

            // §3.6 sequence-diagram REG_ACK note: fire the script-side
            // callback alongside the cache write.  Same callback contract
            // as the NOTIFY path in handle_channel_auth_notifies — see
            // §I11.1 invariant #2 (cache writes are transport-agnostic
            // and the on_allowlist_changed callback fires on each write).
            if (pImpl->engine)
            {
                try
                {
                    pImpl->engine->invoke_on_allowlist_changed(
                        channel_name, script_view, "initial_seed");
                }
                catch (const std::exception &e)
                {
                    // Symmetric with handle_channel_auth_notifies:
                    // callback exception does NOT roll back the cache
                    // update (HEP-0036 §I11).
                    LOGGER_ERROR(
                        "[{}] on_allowlist_changed callback threw "
                        "for channel '{}' (reason=initial_seed): {}",
                        pImpl->short_tag, channel_name, e.what());
                }
            }
        }
        else
        {
            LOGGER_WARN(
                "[{}] REG_ACK for channel '{}' is missing "
                "`initial_allowlist` (or it is not an array) — broker "
                "contract violation per HEP-CORE-0036 §6.2 + §I11.1 "
                "invariant #5.  Preserving prior allowlist_cache snapshot "
                "(do NOT clobber with empty).",
                pImpl->short_tag, channel_name);
        }
    }

    // HEP-CORE-0042 §5.5.2 — signal the broker that our cache is at
    // `snapshot_version`.  Broker advances `confirmed_version[K][P]`
    // and drains pending wait-path attaches (§5.4 step d).  Sync
    // REQ with `applied_ack_wait_ms=1000` budget per §5.6; on
    // timeout / transport failure / STALE_INSTANCE the broker will
    // re-drive us via the next NOTIFY.
    //
    // Reads `snapshot_version` from the REG_ACK we're processing —
    // that's the channel_version at the time `initial_allowlist`
    // above was extracted (§5.5.4).  For a freshly-opened channel
    // (no consumers yet) this is 0; the broker still accepts and
    // records the confirmation as a no-op advance.
    const auto applied_version = ack.value("snapshot_version",
                                            std::uint64_t{0});
    auto *brc = pImpl->resolve_bc_for_channel(channel_name);
    if (brc && instance_id > 0)
    {
        auto reply = brc->channel_auth_applied(
            channel_name, pImpl->uid, applied_version, instance_id);
        if (!reply.has_value())
        {
            LOGGER_WARN(
                "[{}] CHANNEL_AUTH_APPLIED_REQ('{}') no reply within "
                "budget — cache stays applied, broker may re-drive on "
                "next NOTIFY (HEP-CORE-0042 §5.5.2 + §5.6)",
                pImpl->short_tag, channel_name);
        }
        else if (reply->value("status", std::string{}) != "ok")
        {
            // STALE_INSTANCE (a re-REG races) or transport error.
            // Log WARN + continue — the next NOTIFY drives resync.
            LOGGER_WARN(
                "[{}] CHANNEL_AUTH_APPLIED_REQ('{}') non-ok reply: "
                "status='{}' error_code='{}' — cache stays applied",
                pImpl->short_tag, channel_name,
                reply->value("status", std::string{}),
                reply->value("error_code", std::string{}));
        }
        else
        {
            LOGGER_INFO(
                "[{}] event=ChannelAuthApplied channel='{}' "
                "applied_version={} (HEP-CORE-0042 §5.5.2)",
                pImpl->short_tag, channel_name, applied_version);
        }
    }
    else if (!brc)
    {
        LOGGER_WARN(
            "[{}] no BRC for channel '{}' at apply_producer_reg_ack; "
            "skipping CHANNEL_AUTH_APPLIED_REQ (HEP-CORE-0042 §5.5.2 "
            "silent-drop safe: broker will re-drive on next NOTIFY)",
            pImpl->short_tag, channel_name);
    }

    // HEP-CORE-0036 §4.3.2 — Registered → Authorized at the END of
    // apply_producer_reg_ack success.  By this point: REG_ACK accepted
    // (Layers 1+2), apply_master_approval succeeded (Layer 3 — PUSH
    // bound, CURVE-server armed, ZAP allowlist seeded, worker spawned,
    // queue Active).  Data plane armed; §8.2 outer guard
    // (`any_presence_authorized()`) will now admit this presence to
    // the data loop.
    // B-5 (#290): channel_name is non-empty by construction
    // (hard-errored above on absent).  Guard only handler_.
    if (pImpl->handler_)
    {
        Presence *presence = pImpl->handler_
            ->find_presence_for_channel(channel_name);
        if (presence)
        {
            presence->registration_state.store(
                RegistrationState::Authorized,
                std::memory_order_release);
            LOGGER_INFO("[{}] event=PresenceStateTransition "
                        "channel='{}' role_type=producer "
                        "from=Registered to=Authorized "
                        "trigger=apply_producer_reg_ack_done",
                        pImpl->short_tag, channel_name);
        }
    }
    return true;
}

std::uint64_t RoleAPIBase::producer_instance_id() const noexcept
{
    return pImpl->producer_instance_id_.load(std::memory_order_relaxed);
}

void RoleAPIBase::reset_tx_queue_metrics()
{
    if (pImpl->tx_queue) pImpl->tx_queue->init_metrics();
}

void RoleAPIBase::reset_rx_queue_metrics()
{
    if (pImpl->rx_queue) pImpl->rx_queue->init_metrics();
}

std::vector<AllowedPeer>
RoleAPIBase::allowed_peers(const std::string &channel) const
{
    return pImpl->allowlist_cache.snapshot(channel);
}

std::vector<AllowedPeer>
RoleAPIBase::producers(const std::string &channel) const
{
    // Consumer-side mirror of `allowed_peers`.  Returns the broker's
    // most recent `CONSUMER_REG_ACK.producers[]` snapshot for this
    // channel as a copy.  Empty if the channel was never registered,
    // is SHM-transport (no producers[] field per §5.6), or the broker
    // delivered an empty list.  Cache is updated by
    // `apply_consumer_reg_ack` after the queue's `apply_master_approval`
    // succeeds.  HEP-CORE-0036 §I11 — read-only script surface.
    return pImpl->producer_peer_cache.snapshot(channel);
}

bool RoleAPIBase::is_channel_ready(const std::string &channel) const noexcept
{
    // HEP-CORE-0036 §6.7 (#190) — script-visible state-machine query.
    // Resolves `channel` against the role's known channel names:
    //   - For processor with distinct in/out channels, `out_channel`
    //     is the TX side.
    //   - For all roles, the primary `channel` field is the role's
    //     main channel (TX for producer; RX for consumer; RX-in for
    //     processor).
    // Returns true iff the corresponding queue is in Active state.
    if (!pImpl->out_channel.empty() && channel == pImpl->out_channel)
        return is_tx_active();
    if (channel == pImpl->channel)
    {
        // Single-channel role match.  rx_queue is set for
        // consumer/processor-input; tx_queue is set for producer.
        // The first non-null queue is the correct side because
        // single-channel roles only ever have one of the two.
        if (pImpl->rx_queue) return is_rx_active();
        if (pImpl->tx_queue) return is_tx_active();
    }
    return false;
}

void RoleAPIBase::handle_channel_auth_notifies(
    std::vector<pylabhub::scripting::IncomingMessage> &msgs)
{
    using pylabhub::scripting::NotificationId;
    namespace sec = pylabhub::utils::security;

    auto it = msgs.begin();
    while (it != msgs.end())
    {
        if (it->notification_id != NotificationId::ChannelAuthChanged)
        {
            ++it;
            continue;
        }

        const std::string channel =
            it->details.value("channel_name", std::string{});
        if (channel.empty())
        {
            LOGGER_WARN(
                "[{}/{}] CHANNEL_AUTH_CHANGED_NOTIFY missing channel_name; "
                "dropping (HEP-CORE-0036 §6.5)",
                pImpl->short_tag, pImpl->uid);
            it = msgs.erase(it);
            continue;
        }

        // Channel-bound routing: find the BRC that owns this channel.
        // resolve_bc_for_channel returns nullptr if this role has no
        // presence for the channel (defensive — a notify routed to the
        // wrong role is a broker bug worth logging but not crashing on).
        auto *brc = pImpl->resolve_bc_for_channel(channel);
        if (!brc)
        {
            LOGGER_WARN(
                "[{}/{}] no BRC for channel '{}' on auth notify; dropping",
                pImpl->short_tag, pImpl->uid, channel);
            it = msgs.erase(it);
            continue;
        }

        try
        {
            // §6.5 pull — sync REQ/REP on the worker thread (NOT the
            // BRC poll thread).  No re-entrance hazard: this thread is
            // not the one reading the BRC socket.
            auto reply = brc->get_channel_auth(channel, pImpl->uid, 5000);
            if (!reply.has_value())
            {
                // Timeout / transport failure.  Per §6.5, log + return;
                // the next notify retries, and BRC reconnect re-syncs
                // via REG_ACK.initial_allowlist.
                LOGGER_WARN(
                    "[{}/{}] GET_CHANNEL_AUTH_REQ('{}') no reply within 5000ms; "
                    "allowlist unchanged",
                    pImpl->short_tag, pImpl->uid, channel);
            }
            else if (reply->value("status", std::string{}) != "success")
            {
                // Broker error (CHANNEL_NOT_FOUND, PRODUCER_NOT_AUTHORIZED,
                // etc.) — race with dereg / channel close.  Log + skip;
                // not a fatal condition.
                LOGGER_WARN(
                    "[{}/{}] GET_CHANNEL_AUTH_REQ('{}') broker error: "
                    "code='{}' msg='{}'; allowlist unchanged",
                    pImpl->short_tag, pImpl->uid, channel,
                    reply->value("error_code", std::string{}),
                    reply->value("message", std::string{}));
            }
            else
            {
                // HEP-CORE-0036 §6.5 (locked 2026-06-12): allowlist
                // entries are bare Z85 pubkey strings.  Symmetric with
                // §6.2 `REG_ACK.initial_allowlist` + §7.2 cache shape.
                // The script-side `api.allowed_peers` surface still
                // exposes `AllowedPeer{role_uid, pubkey}`; we leave
                // `role_uid` empty here because the wire no longer
                // carries it.  A role host that wants role_uids
                // resolves them locally via its `known_roles` view
                // (operator-side metadata).

                // HEP-CORE-0036 §6.5 + §I11.1 invariant #5: a status
                // ="success" GET_CHANNEL_AUTH_ACK MUST carry the
                // `allowlist` field (array, possibly empty) per the
                // §6.5 normative shape.  An absent or non-array field
                // on a success reply is a broker contract violation;
                // log WARN + preserve the prior cache snapshot rather
                // than clobbering with `[]`.  Symmetric with the
                // REG_ACK `initial_allowlist` guard in
                // apply_producer_reg_ack.
                if (!reply->contains("allowlist") ||
                    !reply->at("allowlist").is_array())
                {
                    LOGGER_WARN(
                        "[{}/{}] GET_CHANNEL_AUTH_ACK for channel "
                        "'{}' status=success but `allowlist` is "
                        "missing or not an array — broker contract "
                        "violation per HEP-CORE-0036 §6.5 + §I11.1 "
                        "invariant #5.  Preserving prior "
                        "allowlist_cache snapshot (do NOT clobber).",
                        pImpl->short_tag, pImpl->uid, channel);
                }
                else
                {

                sec::PeerAllowlist allowlist;
                std::vector<AllowedPeer> script_view;
                const auto &arr = reply->at("allowlist");
                for (const auto &entry : arr)
                {
                    if (!entry.is_string()) continue;
                    const auto pk = entry.get<std::string>();
                    if (pk.empty()) continue;
                    allowlist.peers.insert(sec::PeerIdentity{"curve", pk});
                    script_view.push_back(AllowedPeer{
                        /*role_uid=*/std::string{}, pk});
                }
                const auto reason =
                    it->details.value("reason", std::string{"unknown"});

                // Queue-side enforcement push (ZMQ tx queues only).
                // Only ZmqQueue (PUSH-side) implements PeerAdmission;
                // ShmQueue does NOT.  For SHM channels the broker
                // pre-confirm at attach time (HEP-CORE-0041 §9 D4) is
                // the authoritative gate — there is no on-role
                // enforcement cache to seed, so the cast is null and
                // we skip the queue push.  The script-side cache update
                // below still fires unconditionally so observability
                // (`api.allowed_peers(channel)` + the
                // `on_allowlist_changed` callback) tracks the broker's
                // current allowlist regardless of transport.
                bool publish_cache = true;
                auto *admission =
                    dynamic_cast<sec::PeerAdmission *>(pImpl->tx_queue.get());
                if (admission)
                {
                    const bool ok = admission->set_peer_allowlist(allowlist);
                    if (ok)
                    {
                        LOGGER_INFO(
                            "[{}/{}] applied channel '{}' allowlist (size={}, "
                            "reason='{}', HEP-CORE-0036 §6.5)",
                            pImpl->short_tag, pImpl->uid, channel,
                            allowlist.peers.size(), reason);
                    }
                    else
                    {
                        // Keep cache consistent with ZAP enforcement:
                        // if the queue refused the push, the next
                        // handshake will NOT admit what's in
                        // `script_view`, so don't publish a misleading
                        // snapshot.
                        LOGGER_WARN(
                            "[{}/{}] set_peer_allowlist returned false for "
                            "channel '{}' (queue inert on this side?); "
                            "skipping cache update to stay in sync with "
                            "ZAP enforcement",
                            pImpl->short_tag, pImpl->uid, channel);
                        publish_cache = false;
                    }
                }
                else
                {
                    // Non-PeerAdmission tx queue (SHM today, or queue
                    // wired post-1i-mig).  No queue-side enforcement to
                    // seed; cache is pure observability.
                    LOGGER_DEBUG(
                        "[{}/{}] auth notify for channel '{}' on non-ZAP "
                        "tx queue (SHM); updating script-side cache only "
                        "(HEP-CORE-0041 §9 D4 broker pre-confirm is the "
                        "authoritative gate)",
                        pImpl->short_tag, pImpl->uid, channel);
                }

                // Script-side cache + I11 callback.  For ZMQ this runs
                // AFTER the ZAP cache is in place (HEP §6.5 step 5 —
                // the snapshot the script sees IS what the next
                // handshake will admit).  For SHM this is pure
                // observability of the broker's current allowlist.
                if (publish_cache)
                {
                    pImpl->allowlist_cache.put(channel, script_view);
                    if (pImpl->engine)
                    {
                        try
                        {
                            pImpl->engine->invoke_on_allowlist_changed(
                                channel, script_view, reason);
                        }
                        catch (const std::exception &e)
                        {
                            // Per §I11: callback exceptions do NOT
                            // roll back the cache update — log +
                            // continue.
                            LOGGER_ERROR(
                                "[{}/{}] on_allowlist_changed callback "
                                "threw for channel '{}': {}",
                                pImpl->short_tag, pImpl->uid, channel,
                                e.what());
                        }
                    }

                    // HEP-CORE-0042 §5.5.2 — cache is now applied at
                    // GET_CHANNEL_AUTH_ACK.snapshot_version.  Signal
                    // the broker so it advances confirmed_version[K][P]
                    // and drains pending wait-path attaches (§5.4 step
                    // d).  Only emitted for ZMQ (publish_cache branch);
                    // SHM channels use broker pre-confirm and don't
                    // participate in the ZMQ confirmed-version machinery.
                    // Skip if REG_ACK hasn't been observed yet (defensive:
                    // a NOTIFY should never precede REG_ACK in practice,
                    // but a zero instance_id would defeat the
                    // stale-instance guard).
                    const auto instance_id =
                        pImpl->producer_instance_id_.load(
                            std::memory_order_relaxed);
                    const auto snapshot_version =
                        reply->value("snapshot_version", std::uint64_t{0});
                    if (admission && instance_id > 0)
                    {
                        auto applied = brc->channel_auth_applied(
                            channel, pImpl->uid, snapshot_version, instance_id);
                        if (!applied.has_value())
                        {
                            LOGGER_WARN(
                                "[{}/{}] CHANNEL_AUTH_APPLIED_REQ('{}') no "
                                "reply within budget — cache stays applied, "
                                "broker may re-drive on next NOTIFY (HEP-"
                                "CORE-0042 §5.5.2 + §5.6)",
                                pImpl->short_tag, pImpl->uid, channel);
                        }
                        else if (applied->value("status",
                                                 std::string{}) != "ok")
                        {
                            LOGGER_WARN(
                                "[{}/{}] CHANNEL_AUTH_APPLIED_REQ('{}') "
                                "non-ok reply: status='{}' error_code='{}'",
                                pImpl->short_tag, pImpl->uid, channel,
                                applied->value("status", std::string{}),
                                applied->value("error_code", std::string{}));
                        }
                        else
                        {
                            LOGGER_INFO(
                                "[{}/{}] event=ChannelAuthApplied channel='{}'"
                                " applied_version={} reason='{}' "
                                "(HEP-CORE-0042 §5.5.2)",
                                pImpl->short_tag, pImpl->uid, channel,
                                snapshot_version, reason);
                        }
                    }
                }
                } // end of else { /* allowlist field present + array */ }
            }
        }
        catch (const std::exception &e)
        {
            // Audit R5: exception safety — a malformed broker reply must
            // not crash the role.  Log + skip; next notify retries.
            LOGGER_ERROR(
                "[{}/{}] exception handling auth notify for '{}': {}",
                pImpl->short_tag, pImpl->uid, channel, e.what());
        }

        it = msgs.erase(it);
    }
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

// ============================================================================
// Thread manager
// ============================================================================

pylabhub::utils::ThreadManager &RoleAPIBase::thread_manager()
{
    // Always valid — the Impl ctor constructs thread_mgr_ from the
    // ctor-required short_tag + uid. No runtime "did you init?" check.
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
                pImpl->short_tag, role_cfg_ms, hub_max);
            effective_interval_ms = hub_max;
        }
        else
        {
            LOGGER_INFO(
                "[{}] heartbeat: aligned with hub — role cadence {} ms, "
                "hub max {} ms",
                pImpl->short_tag, role_cfg_ms, hub_max);
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
                     pImpl->short_tag);
        return;
    }

    auto *core_post = pImpl->core;
    bc_tick->set_periodic_task(
        [this] { on_heartbeat_tick_(); },
        effective_interval_ms,
        [core_post] { return core_post->iteration_count(); });

    LOGGER_INFO("[{}] heartbeat: periodic tick installed at {}ms",
                pImpl->short_tag, effective_interval_ms);
    pImpl->heartbeat_install_at_ = std::chrono::steady_clock::now();
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
        LOGGER_ERROR("[{}] register_producer_channel: broker comm not connected", pImpl->short_tag);
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
    {
        presence->registration_state.store(
            RegistrationState::RegRequestPending,
            std::memory_order_release);
        LOGGER_INFO("[{}] event=PresenceStateTransition channel='{}' role_type=producer from=Unregistered to=RegRequestPending trigger=REG_REQ_sending",
                    pImpl->short_tag, ch);
    }

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
                     pImpl->short_tag, opts.value("channel_name", "?"));
    else if (!registered)
        LOGGER_ERROR("[{}] REG_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->short_tag, opts.value("channel_name", "?"),
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
        LOGGER_INFO("[{}] event=RegAckReceived channel='{}' status=success initial_allowlist={}",
                    pImpl->short_tag, opts.value("channel_name", "?"),
                    result->value("initial_allowlist",
                                  nlohmann::json::array()).dump());

    if (presence)
    {
        const auto new_state = registered ? RegistrationState::Registered
                                          : RegistrationState::Unregistered;
        presence->registration_state.store(new_state, std::memory_order_release);
        LOGGER_INFO("[{}] event=PresenceStateTransition channel='{}' role_type=producer from=RegRequestPending to={}",
                    pImpl->short_tag, ch, to_string(new_state));
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
        LOGGER_ERROR("[{}] discover_channel: broker comm not connected", pImpl->short_tag);
        return std::nullopt;
    }
    auto result = bc->discover_channel(channel, {}, timeout_ms);
    if (!result.has_value())
        LOGGER_ERROR("[{}] DISC_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->short_tag, channel);
    else if (result->value("status", std::string{}) != "success")
        LOGGER_ERROR("[{}] DISC_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->short_tag, channel,
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
        LOGGER_INFO("[{}] Discovered channel '{}' from broker", pImpl->short_tag, channel);
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
        LOGGER_ERROR("[{}] register_consumer: broker comm not connected", pImpl->short_tag);
        return std::nullopt;
    }

    // Audit S1+O4 (2026-05-17): per-presence FSM marker, same pattern
    // as `register_producer_channel`.
    Presence *presence = pImpl->handler_
        ? pImpl->handler_->find_presence_for_channel(ch) : nullptr;
    if (presence)
    {
        presence->registration_state.store(
            RegistrationState::RegRequestPending,
            std::memory_order_release);
        LOGGER_INFO("[{}] event=PresenceStateTransition channel='{}' "
                    "role_type=consumer from=Unregistered "
                    "to=RegRequestPending trigger=CONSUMER_REG_REQ_sending",
                    pImpl->short_tag, ch);
    }

    auto result = bc->register_consumer(opts, timeout_ms);

    // HEP-CORE-0036 §5.2 R6 + §6.6 reason catalog: the broker rejects
    // CONSUMER_REG_REQ with `CHANNEL_NOT_READY` while the channel is
    // not yet admissible.  The §6.6 reason values that are transient
    // and worth retrying are `awaiting_first_heartbeat` (kRegistering
    // — producer registered, no first heartbeat yet) and
    // `heartbeat_stalled` (kStalled — producer in heartbeat-timeout
    // window per HEP-CORE-0023 §2.6).  Terminal failures (channel
    // does not exist, or producer presences all Disconnected) come
    // back as `CHANNEL_NOT_FOUND` and drop out of this loop on the
    // error_code check.  A 100 ms cadence covers a typical 1 s
    // heartbeat tick with ~10 retries; the broker's rejection is a
    // cheap synchronous reply, so the budget covers many retries.
    const auto is_retryable_reason = [](const nlohmann::json &r) -> bool {
        const auto msg = r.value("message", std::string{});
        return msg.find("awaiting_first_heartbeat") != std::string::npos ||
               msg.find("heartbeat_stalled")        != std::string::npos;
    };
    const auto retry_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds{timeout_ms};
    while (result.has_value() &&
           result->value("status", std::string{}) == "error" &&
           result->value("error_code", std::string{}) == "CHANNEL_NOT_READY" &&
           is_retryable_reason(*result))
    {
        if (std::chrono::steady_clock::now() >= retry_deadline)
        {
            LOGGER_WARN("[{}] CONSUMER_REG_REQ for '{}' deadline exceeded while "
                        "waiting for channel to become ready (last broker "
                        "reason: '{}')",
                        pImpl->short_tag, ch,
                        result->value("message", std::string{}));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                retry_deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) break;
        result = bc->register_consumer(opts, static_cast<int>(remaining_ms));
    }

    const bool registered =
        result.has_value() &&
        result->value("status", std::string{}) == "success";

    if (!result.has_value())
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ no response for channel '{}' (timeout/disconnect)",
                     pImpl->short_tag, opts.value("channel_name", "?"));
    else if (!registered)
        LOGGER_ERROR("[{}] CONSUMER_REG_REQ failed for channel '{}': error_code='{}' message='{}'",
                     pImpl->short_tag, opts.value("channel_name", "?"),
                     result->value("error_code", std::string{}),
                     result->value("message", std::string{}));
    else
        LOGGER_INFO("[{}] Registered consumer on channel '{}' with broker",
                    pImpl->short_tag, opts.value("channel_name", "?"));

    if (presence)
    {
        const auto new_state = registered ? RegistrationState::Registered
                                          : RegistrationState::Unregistered;
        presence->registration_state.store(new_state,
                                           std::memory_order_release);
        LOGGER_INFO("[{}] event=PresenceStateTransition channel='{}' "
                    "role_type=consumer from=RegRequestPending to={}",
                    pImpl->short_tag, ch, to_string(new_state));
    }
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
// consumer_attach — HEP-CORE-0041 §9 D4 broker pre-confirm (producer side)
// ============================================================================
//
// Producer-side wrapper for BRC::consumer_attach.  Routes through the
// channel-bound BRC so the per-channel ShmAttachOrchestrator can hand
// this method as its `BrokerQuery` callback without leaking BRC types
// into the orchestrator.  Returns broker's reply body (`{status:
// "success" | "denied", ...}`) or nullopt on transport failure /
// timeout / no BRC for the channel.
//
// Thread context: called from the SHM accept thread (HEP-CORE-0031 §2,
// 1i-mig-2 categorization).  BRC's sync REQ/REP is thread-safe (the
// caller enqueues a request + waits on CV; the BRC poll thread drains
// and signals).

std::optional<nlohmann::json>
RoleAPIBase::consumer_attach(const std::string &channel,
                             const std::string &consumer_pubkey,
                             const std::string &consumer_role_uid,
                             const std::string &producer_role_uid,
                             int                timeout_ms)
{
    auto *bc = pImpl->resolve_bc_for_channel(channel);
    if (!bc || !bc->is_connected())
    {
        LOGGER_WARN(
            "[{}/{}] consumer_attach for channel '{}': no connected BRC "
            "— returning nullopt (HEP-CORE-0041 §9 D4)",
            pImpl->short_tag, pImpl->uid, channel);
        return std::nullopt;
    }
    return bc->consumer_attach(channel, consumer_pubkey,
                                consumer_role_uid, producer_role_uid,
                                timeout_ms);
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
                    pImpl->short_tag, p.channel);
        (void)deregister_producer_channel(p.channel);
    }

    // Pass 2: consumer presences.
    for (const auto &p : pImpl->handler_->presences())
    {
        if (p.role_kind != RoleKind::Consumer) continue;
        if (!needs_dereg(p)) continue;
        LOGGER_INFO("[{}] ctrl: deregistering consumer from channel '{}' from broker",
                    pImpl->short_tag, p.channel);
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
                    pImpl->short_tag);
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
    // pre-existing `short_tag` string branching that bypassed the
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
                     pImpl->short_tag, p.channel, pImpl->uid, role_type);
        bc->send_heartbeat(p.channel, pImpl->uid, role_type, metrics);
        // HEP-CORE-0023 §2.5 telemetry — task #223.  Counter, not log.
        pImpl->heartbeats_sent_.fetch_add(1, std::memory_order_relaxed);
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
                    pImpl->short_tag);
        return false;
    }
    if (handler == nullptr)
    {
        LOGGER_ERROR("[{}] start_handler_threads: handler is null",
                     pImpl->short_tag);
        return false;
    }

    const std::size_t n_conn = handler->connection_count();
    const std::size_t n_pres = handler->presence_count();

    LOGGER_INFO("[{}] start_handler_threads: ENTRY — {} presence(s) on {} "
                "unique hub(s) (uid='{}' name='{}')",
                pImpl->short_tag, n_pres, n_conn,
                pImpl->uid, pImpl->name);

    // ── Phase 1: Allocate + connect each BRC (via handler) ───────────────
    LOGGER_INFO("[{}] start_handler_threads: Phase 1 — connecting {} BRC(s)",
                pImpl->short_tag, n_conn);
    if (!handler->start_connections(*this))
    {
        LOGGER_ERROR("[{}] start_handler_threads: Phase 1 FAILED — "
                     "handler->start_connections returned false",
                     pImpl->short_tag);
        return false;
    }
    LOGGER_INFO("[{}] start_handler_threads: Phase 1 OK — {} BRC(s) connected",
                pImpl->short_tag, n_conn);

    // Take ownership of the handler now — Phase 1 succeeded, so the
    // handler's BRCs are live + we will spawn threads for them.
    pImpl->handler_ = std::move(handler);

    auto *core = pImpl->core;
    const std::string &tag_local = pImpl->short_tag;

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
                     pImpl->short_tag, n_conn);
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
                pImpl->short_tag, n_conn);
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
    LOGGER_INFO("[{}] start_handler_threads: Phase 2 OK", pImpl->short_tag);

    // ── Phase 3: Spawn one ctrl thread per HubConnection ─────────────────
    LOGGER_INFO("[{}] start_handler_threads: Phase 3 — spawning {} ctrl "
                "thread(s) (first = master per HEP-CORE-0031 §4.2.1)",
                pImpl->short_tag, n_conn);

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
                    pImpl->short_tag, slot_name, endpoint,
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
                         pImpl->short_tag, slot_name, opts.is_master);
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
                pImpl->short_tag, n_conn);

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
                pImpl->short_tag, n_conn, n_pres);
    return true;
}

void RoleAPIBase::stop_handler_threads() noexcept
{
    if (pImpl->handler_ == nullptr && !pImpl->ctrl_threads_started_)
    {
        // Never started, or already stopped — idempotent no-op.
        return;
    }

    LOGGER_INFO("[{}] stop_handler_threads: ENTRY", pImpl->short_tag);

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
                    pImpl->short_tag, n_conn);
        for (auto &c : pImpl->handler_->connections())
        {
            if (c.brc) c.brc->stop();
        }
    }

    // ── Phase 3: Drain the ThreadManager (HEP-CORE-0031 §4.1) ────────────
    LOGGER_INFO("[{}] stop_handler_threads: Phase 3 — draining ThreadManager",
                pImpl->short_tag);
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
                     pImpl->short_tag, e.what());
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
                    pImpl->short_tag);
        pImpl->handler_->stop_connections();
        pImpl->handler_.reset();
    }

    pImpl->ctrl_threads_started_ = false;

    // HEP-CORE-0023 §2.5 telemetry — one-shot summary at handler stop
    // (Pattern 4 rung 3; task #223).  Reports the count of
    // HEARTBEAT_REQ frames emitted across all presences since
    // install_heartbeat.  Zero ticks fired = "0 sent" which is a real
    // observation (broker missed first-tick, install failed, etc.) —
    // do NOT skip the line.
    const auto sent  = pImpl->heartbeats_sent_.load(std::memory_order_relaxed);
    const auto since = pImpl->heartbeat_install_at_.time_since_epoch().count() != 0
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - pImpl->heartbeat_install_at_)
              .count()
        : 0;
    LOGGER_INFO("[{}] event=HeartbeatCounterReport sent={} over={}ms (since install)",
                pImpl->short_tag, sent, since);
    LOGGER_INFO("[{}] stop_handler_threads: COMPLETE", pImpl->short_tag);
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

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
void RoleAPIBase::install_handler_for_test_(
    std::unique_ptr<RoleHandler> handler)
{
    // Private; reachable only through the friend
    // `test::RoleAPIBaseTestAccess`.  See the header docstring for the
    // no-bypass contract.  We replace any existing handler — L2 tests
    // build a fresh RoleAPIBase per scenario.  Symbol physically
    // absent in Release / non-test builds.
    pImpl->handler_ = std::move(handler);
}
#endif

bool RoleAPIBase::any_presence_authorized() const noexcept
{
    // Returns true iff ANY Presence on this role is `Authorized`
    // (HEP-CORE-0036 §4.3.2 — Layer 3 data plane armed).  Reads are
    // atomic with acquire ordering; no lock needed.  Null handler
    // (validate-only / test paths that skip handler construction)
    // reports `false` — there are no presences to be authorized, so
    // the data loop's outer guard correctly refuses to enter.
    if (!pImpl->handler_) return false;
    for (const auto &p : pImpl->handler_->presences())
    {
        if (p.registration_state.load(std::memory_order_acquire)
            == RegistrationState::Authorized)
            return true;
    }
    return false;
}

const std::string &RoleAPIBase::short_tag() const   { return pImpl->short_tag; }
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
        LOGGER_DEBUG("[{}/{}] {}", pImpl->short_tag, pImpl->uid, msg);
    else if (level == "warn" || level == "Warn" || level == "warning")
        LOGGER_WARN("[{}/{}] {}", pImpl->short_tag, pImpl->uid, msg);
    else if (level == "error" || level == "Error")
        LOGGER_ERROR("[{}/{}] {}", pImpl->short_tag, pImpl->uid, msg);
    else
        LOGGER_INFO("[{}/{}] {}", pImpl->short_tag, pImpl->uid, msg);
}

void RoleAPIBase::stop()                        { pImpl->core->request_stop(); }
void RoleAPIBase::set_critical_error(std::string_view msg)
{
    // Audit S2 (2026-05-18) — uniform "[short_tag/uid] CRITICAL: <msg>"
    // log line for non-empty messages, BEFORE flipping state so log
    // scrapers see the message adjacent to the stop event.  All three
    // engines (Python / Lua / Native C) route through here so the log
    // format is identical regardless of language.
    if (!msg.empty())
    {
        LOGGER_ERROR("[{}/{}] CRITICAL: {}",
                     pImpl->short_tag, pImpl->uid, msg);
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
                     pImpl->short_tag, channel);
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
                         pImpl->short_tag, channel, code);
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
                    pImpl->short_tag, channel);
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

// ============================================================================
// Output side — flat data-plane verbs
// ============================================================================

bool RoleAPIBase::is_tx_active() const noexcept
{
    // HEP-CORE-0036 §6.7 (#189, Stage 1B).  Post-Stage-1A,
    // `is_running()` on ZmqQueue is true iff the queue is in the
    // Active state (running_ flag is set after start() passes the
    // Configured gate).  Returns false when tx_queue is unset
    // (pre-build) — the cycle ops will simply not dispatch the
    // write side.
    return pImpl->tx_queue && pImpl->tx_queue->is_running();
}

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

bool RoleAPIBase::is_rx_active() const noexcept
{
    // HEP-CORE-0036 §6.7 (#189, Stage 1B) — symmetric to is_tx_active.
    return pImpl->rx_queue && pImpl->rx_queue->is_running();
}

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
    // Reads from the introspection cache populated by RoleHostFrame at
    // setup time.  See FlexzoneInfoCache in role_api_base.hpp for
    // the linear-forward-time contract.
    const auto &fz = pImpl->fz_info_cache;

    if (side.has_value())
    {
        return (*side == ChannelSide::Tx) ? fz.tx_logical_size
                                          : fz.rx_logical_size;
    }

    if (fz.has_tx_fz && fz.has_rx_fz)
        throw std::runtime_error("flexzone_logical_size: side parameter required for processor");

    if (fz.has_tx_fz) return fz.tx_logical_size;
    if (fz.has_rx_fz) return fz.rx_logical_size;
    return 0;
}

bool RoleAPIBase::has_tx_fz() const noexcept
{
    return pImpl->fz_info_cache.has_tx_fz;
}

bool RoleAPIBase::has_rx_fz() const noexcept
{
    return pImpl->fz_info_cache.has_rx_fz;
}

size_t RoleAPIBase::flexzone_physical_size(std::optional<ChannelSide> side) const
{
    const auto &fz = pImpl->fz_info_cache;

    if (side.has_value())
    {
        return (*side == ChannelSide::Tx) ? fz.tx_physical_size
                                          : fz.rx_physical_size;
    }

    if (fz.has_tx_fz && fz.has_rx_fz)
        throw std::runtime_error("flexzone_physical_size: side parameter required for processor");

    if (fz.has_tx_fz) return fz.tx_physical_size;
    if (fz.has_rx_fz) return fz.rx_physical_size;
    return 0;
}

void RoleAPIBase::set_flexzone_info_cache_(
    const FlexzoneInfoCache &cache) noexcept
{
    pImpl->fz_info_cache = cache;
}

const RoleAPIBase::FlexzoneInfoCache &
RoleAPIBase::fz_info_cache() const noexcept
{
    return pImpl->fz_info_cache;
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

    // Queue metrics: key depends on which sides exist.  CURVE
    // engagement state is NOT in this metrics dict — scripts query it
    // via the dedicated `queue_mechanism()` accessor (#186), which
    // they reach via the engine-specific binding
    // (Lua: `api.queue_mechanism(side)`).
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

hub::Mechanism RoleAPIBase::queue_mechanism(ChannelSide side) const noexcept
{
    // Polymorphic dispatch via the QueueReader::mechanism() virtual
    // added in task #279 (2026-06-22).  Pre-#279 this method
    // dynamic_cast<ZmqQueue *>'d and returned `Uninitialized` for SHM
    // (the enum had no SHM-shaped value).  Now ZmqQueue returns
    // `Curve`, ShmQueue returns `ShmCapability`, InboxQueue (and any
    // future queue type) returns the base-class default `Uninitialized`.
    //
    // tx_queue is held as unique_ptr<QueueWriter> (not Reader), so a
    // cross-cast to QueueReader* is needed to reach the mechanism()
    // virtual.  Both ZmqQueue + ShmQueue (the only concrete queue
    // types today) inherit BOTH QueueReader + QueueWriter, so the
    // dynamic_cast succeeds; an unknown queue type that only inherits
    // QueueWriter would gracefully degrade to `Uninitialized`.
    hub::QueueReader *reader = nullptr;
    if (side == ChannelSide::Tx)
    {
        reader = dynamic_cast<hub::QueueReader *>(pImpl->tx_queue.get());
    }
    else
    {
        reader = pImpl->rx_queue.get();
    }
    return reader ? reader->mechanism() : hub::Mechanism::Uninitialized;
}

} // namespace pylabhub::scripting
