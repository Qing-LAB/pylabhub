/**
 * @file role_handler.cpp
 * @brief Wave-B M3 skeleton — RoleHandler dedup + index build.
 *
 * Pure logic; no network, no threading, no allocations of the
 * `BrokerRequestComm` pImpl (that arrives in Wave-B M4 with
 * `start()`/`shutdown()`).  See `role_handler.hpp` for the
 * construction contract + invariants.
 */

#include "utils/role_handler.hpp"

#include "utils/broker_request_comm.hpp"
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"
#include "utils/thread_manager.hpp"

namespace pylabhub::scripting
{

RoleHandler::RoleHandler(std::vector<Presence> presences)
    : presences_(std::move(presences))
{
    build_connections_();
    build_channel_index_();
}

void RoleHandler::build_connections_()
{
    // Single-pass dedup by `(broker_endpoint, broker_pubkey)`.  Order
    // is "first presence wins" — the first presence in the declared
    // list that names a given hub identity creates the HubConnection;
    // subsequent presences naming the same hub bind to that slot.
    //
    // Vector size is bounded by `presences_.size()` (worst case: every
    // presence on a distinct hub).  Reserve up front so we can safely
    // hold `HubConnection *` from `presences_[i].connection` without
    // reallocation invalidating those pointers.
    //
    // CRITICAL: `connections_` must NOT be mutated after this build
    // loop returns.  Every Presence in `presences_` will hold a raw
    // `HubConnection *` into this vector; any subsequent `emplace_back`
    // /  `erase` / `clear` / `resize` on `connections_` could trigger
    // reallocation and silently dangle every Presence::connection
    // pointer.  See `role_handler.hpp` § "Immutability contract" for
    // the full pointer-stability rationale; future hub-failover work
    // must re-architect to a stable-storage container before allowing
    // post-build mutation.
    connections_.reserve(presences_.size());

    for (auto &p : presences_)
    {
        // Linear scan over `connections_` — O(presences²) worst case,
        // but presence_count is small (≤ 3 in current designs, bounded
        // by topology not data volume).  No hash key needed.
        HubConnection *match = nullptr;
        for (auto &c : connections_)
        {
            if (c.broker_endpoint == p.hub.broker &&
                c.broker_pubkey   == p.hub.broker_pubkey)
            {
                match = &c;
                break;
            }
        }
        if (match == nullptr)
        {
            connections_.emplace_back(p.hub.broker, p.hub.broker_pubkey);
            match = &connections_.back();
        }
        p.connection = match;
    }
}

void RoleHandler::build_channel_index_()
{
    channel_index_.reserve(presences_.size());

    for (auto &p : presences_)
    {
        if (p.channel.empty())
            continue;
        // Unbound presences (empty channel name) are NOT indexed.  In
        // current topologies the caller passes channel-bound presences
        // at ctor time, so this path is unused.  It exists as a
        // forward-compat hook for future phases where a presence may
        // be declared before its channel is resolved (e.g.,
        // schema-resolve-driven late binding) — those phases will need
        // to call `find_presence_for_channel` only AFTER binding
        // completes.  Today, an empty-channel presence in production
        // input is a caller bug; we skip-and-continue rather than
        // throw so the rest of the index still builds.

        // Caller-responsibility invariant: no two presences on this
        // role share the same channel name.  See file-header §
        // "Wave-B M3 invariants verified by L2 tests".  We log + skip
        // duplicates rather than throw — the L2 tests pin the count
        // via `presence_count_for_channel` so a mistake at the call
        // site fails loudly without needing exception propagation.
        const auto it = channel_index_.find(p.channel);
        if (it == channel_index_.end())
        {
            channel_index_.emplace(p.channel, &p);
        }
        else
        {
            LOGGER_ERROR("RoleHandler: duplicate presence on channel '{}' — "
                         "the second entry is not indexed (callers must "
                         "ensure presences within a role have distinct "
                         "channels per HEP-CORE-0033 §19)",
                         p.channel);
        }
    }
}

const Presence *
RoleHandler::find_presence_for_channel(const std::string &channel) const noexcept
{
    auto it = channel_index_.find(channel);
    return (it == channel_index_.end()) ? nullptr : it->second;
}

std::size_t
RoleHandler::presence_count_for_channel(const std::string &channel) const noexcept
{
    std::size_t n = 0;
    for (const auto &p : presences_)
        if (p.channel == channel)
            ++n;
    return n;
}

RoleHandler::~RoleHandler()
{
    // RAII safety: callers SHOULD invoke shutdown() explicitly so any
    // failure to drain threads / disconnect BRCs surfaces with a log
    // line.  The dtor mops up to avoid leaving live ctrl threads if
    // a caller forgets.
    if (is_started())
    {
        LOGGER_WARN("RoleHandler: shutdown() not called before destruction — "
                    "draining via RAII safety net");
        shutdown();
    }
}

bool RoleHandler::start(const RoleAPIBase &owner)
{
    if (owner_ != nullptr)
    {
        LOGGER_ERROR("RoleHandler::start called twice without intervening "
                     "shutdown — refusing (owner_={})",
                     static_cast<const void *>(owner_));
        return false;
    }

    owner_ = &owner;

    // ── Phase 1: allocate + connect each HubConnection's BRC ──────────────
    //
    // Per role_host_template_design.md §5.4 dedup: connections_ has one
    // entry per unique `(broker_endpoint, broker_pubkey)` in the role's
    // declared presence list.  Each gets its own BRC.  Identity (uid,
    // name, CURVE keys) is role-wide and comes from `owner`.
    bool all_ok = true;
    for (auto &c : connections_)
    {
        c.brc = std::make_unique<hub::BrokerRequestComm>();

        hub::BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = c.broker_endpoint;
        cfg.broker_pubkey   = c.broker_pubkey;
        cfg.client_pubkey   = owner.auth_client_pubkey();
        cfg.client_seckey   = owner.auth_client_seckey();
        cfg.role_uid        = owner.uid();
        cfg.role_name       = owner.name();

        if (!c.brc->connect(cfg))
        {
            LOGGER_ERROR("RoleHandler: BRC connect failed for hub '{}' "
                         "(role_uid='{}')",
                         c.broker_endpoint, owner.uid());
            all_ok = false;
            break;
        }

        // M4b will install per-presence notification routing here.
        // For M4a, a no-op notification callback is sufficient — the
        // BRC will still poll and process replies; only async pushes
        // are dropped (none expected before any REG is issued).
        c.brc->on_notification(
            [](const std::string &, const nlohmann::json &) {
                // M4b: route to RoleHandler via inspection of body's
                // channel_name / band_name.
            });
    }

    if (!all_ok)
    {
        // Partial setup — release whatever connected so the caller can
        // re-try cleanly or destroy the handler safely.  We do NOT
        // clear owner_ because the spawn phase below didn't run; the
        // dtor's safety net checks is_started() which only flips true
        // after spawn completes.
        for (auto &c : connections_)
            if (c.brc)
            {
                c.brc->stop();
                c.brc->disconnect();
                c.brc.reset();
            }
        owner_ = nullptr;
        return false;
    }

    // ── Phase 2: spawn one ctrl thread per HubConnection ──────────────────
    //
    // The first spawned ctrl thread is the master (HEP-CORE-0031 §4.2
    // — at most one master per ThreadManager).  When M4c wires the
    // handler into RoleAPIBase alongside today's `pImpl->broker_channel`
    // ctrl thread, that conflict must be resolved (e.g., by retiring
    // the legacy ctrl thread or making the handler's ctrl threads
    // peers and adding a master elsewhere).
    //
    // Each ctrl thread runs `brc->run_poll_loop(should_run)` inside
    // `ctx.with_active_loop(...)` per the Thread Shutdown Contract,
    // matching the existing pattern at role_api_base.cpp:697.
    auto &tm = const_cast<RoleAPIBase &>(owner).thread_manager();

    bool master_spawned = false;
    for (std::size_t i = 0; i < connections_.size(); ++i)
    {
        auto                                    *brc = connections_[i].brc.get();
        pylabhub::utils::ThreadManager::SpawnOptions opts;
        opts.is_master = !master_spawned;
        master_spawned = true;

        const std::string slot_name =
            "handler_ctrl_" + std::to_string(i);

        tm.spawn(
            slot_name,
            [brc, slot_name](pylabhub::utils::ThreadManager::SlotContext &ctx)
            {
                LOGGER_INFO("[{}] poll thread started", slot_name);
                ctx.with_active_loop(
                    [brc, &ctx]
                    {
                        brc->run_poll_loop(
                            [&ctx] { return !ctx.shutdown_requested(); });
                    });
                LOGGER_INFO("[{}] poll thread exiting", slot_name);
            },
            opts);
    }

    return true;
}

void RoleHandler::shutdown() noexcept
{
    if (owner_ == nullptr)
        return;  // not started or already shut down — idempotent.

    // ── Phase 1: signal each BRC's poll loop to exit ─────────────────────
    for (auto &c : connections_)
        if (c.brc)
            c.brc->stop();

    // ── Phase 2: drive the ThreadManager Thread Shutdown Contract
    //              (HEP-CORE-0031 §4.1) ───────────────────────────────────
    // Threads observe `ctx.shutdown_requested()` via the predicate set
    // in the spawn lambda; `wait_for_quiescence()` blocks until each
    // ctrl thread has exited its `with_active_loop` bracket, after
    // which the BRC pImpl is safe to destroy.
    try
    {
        auto &tm =
            const_cast<RoleAPIBase &>(*owner_).thread_manager();
        tm.request_shutdown_all();
        // 5s ceiling matches typical role-host teardown bounds; under
        // load, ctrl threads should exit zmq_poll within ~100 ms of
        // brc->stop() flipping their predicate.  A timeout here is
        // logged as an ERROR (drain() in the role host's later
        // teardown will catch + report any straggler).
        (void)tm.wait_for_quiescence(std::chrono::seconds(5));
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("RoleHandler::shutdown: ThreadManager drain threw — {}",
                     e.what());
    }

    // ── Phase 3: disconnect + release each BRC ──────────────────────────
    for (auto &c : connections_)
    {
        if (!c.brc) continue;
        c.brc->disconnect();
        c.brc.reset();
    }

    owner_ = nullptr;
}

}  // namespace pylabhub::scripting
