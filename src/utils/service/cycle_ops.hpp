#pragma once
/**
 * @file cycle_ops.hpp
 * @brief Concrete CycleOps classes for producer, consumer, processor.
 *
 * Internal header — not part of the public pylabhub-utils API.
 *
 * Each class implements the role-specific acquire / invoke-and-commit slot
 * of the shared data-loop frame (run_data_loop template in data_loop.hpp).
 * Behavior is specified by docs/tech_draft/role_unification_design.md S4.5a
 * (35 branch IDs with stable per-class tags: P-*, C-*, PR-*).
 *
 * These are plain concrete classes — no virtual base. The run_data_loop
 * template uses duck typing, and the compiler inlines all ops calls.
 */

#include "service/data_loop.hpp"

#include "utils/role_api_base.hpp"
#include "utils/script_engine.hpp"
#include "utils/role_host_core.hpp"

#include <chrono>
#include <cstring>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// Broker-notification dispatch — uniform "user override OR native default"
// ============================================================================
//
// Each cycle drains broker-emitted notifications (CHANNEL_CLOSING_NOTIFY,
// CONSUMER_DIED_NOTIFY, HUB_DEAD, …) into the role's incoming-message
// list.  The BRC-side enqueue tags each message with its `NotificationId`
// (see `role_host_core.hpp` — parsed once at the wire boundary so dispatch
// here is O(1) per message via integer index, not N string compares).
//
// Design (audit D1/D2, 2026-05-18 — user design call):
// Every known notification has TWO functions in its row of the dispatch
// table: an `invoke_user` adapter that fires the script's named
// callback, and a `default_native` C++ function that runs when the
// script has not defined the override.  The dispatch loop is uniform:
//
//   for each msg in msgs:
//     entry = kNotificationTable[msg.notification_id]
//     if entry.invoke_user is nullptr:           // Unknown — never seen
//         leave in msgs (script's generic-scan path for future
//         notification types not yet in the enum)
//     elif engine.has_callback(entry.callback_name):
//         entry.invoke_user(engine, msg)         // user override
//     else:
//         entry.default_native(msg, stop_req)    // framework default
//     erase from msgs                            // always consumed
//
// "this is really just a callback that replaces the default
// straightforward stop()" — 2026-05-15T03:29:14.
//
// The `StopRequestor` arg (defined in role_host_core.hpp) is a narrow
// capability handle exposing ONLY `request(StopReason)` — defaults
// can stop the role with a typed reason but cannot call arbitrary
// RoleHostCore methods.  Defaults that never need to stop
// (`default_consumer_died`) simply ignore the argument; passing it
// to every default keeps the function-pointer signature uniform.
//
// Adding a notification:
//   1. Add the enum value in `role_host_core.hpp` (NotificationId).
//   2. Map the wire string in `parse_notification_id`.
//   3. Add the matching `invoke_on_X` pure virtual on `ScriptEngine`
//      and implement it across Python/Lua/Native engines.
//   4. Write `invoke_user_<X>` and `default_<X>` in this file.
//   5. Add the row to `kNotificationTable[]`.
//   6. If the default stops the role with a distinct reason, add a
//      `StopReason` enum value in `role_host_core.hpp` and a case in
//      `stop_reason_string()`.

// ── User-override adapters ────────────────────────────────────────────
// Pull args out of the message JSON and call the engine's typed
// callback.  No policy here — purely an argument-unpack adapter.

inline void invoke_user_channel_closing(ScriptEngine &engine,
                                        const IncomingMessage &msg)
{
    engine.invoke_on_channel_closing(
        msg.details.value("channel_name", std::string{}),
        msg.details.value("reason",       std::string{}));
}

inline void invoke_user_consumer_died(ScriptEngine &engine,
                                      const IncomingMessage &msg)
{
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): payload field is
    // `role_uid` (was `consumer_uid`).  The script-facing callback
    // parameter keeps its `consumer_uid` name because the callback
    // itself is named `on_consumer_died` — the parameter type is
    // already unambiguously a consumer's uid.
    engine.invoke_on_consumer_died(
        msg.details.value("channel_name", std::string{}),
        msg.details.value("role_uid",     std::string{}),
        msg.details.value("reason",       std::string{}));
}

inline void invoke_user_hub_dead(ScriptEngine &engine,
                                 const IncomingMessage &msg)
{
    engine.invoke_on_hub_dead(msg.source_hub_uid);
}

// ── Native defaults ───────────────────────────────────────────────────
// One per notification type.  Each is the framework's answer to
// "what should we do when the script hasn't defined the override?"

/// Channel closed and the script has no `on_channel_closing` —
/// graceful stop with `StopReason::ChannelClosed` so downstream
/// readers can distinguish channel-close from generic api.stop().
/// Mirrors the master-hub-dead reason-first-then-stop sequencing.
inline void default_channel_closing(const IncomingMessage & /*msg*/,
                                    const StopRequestor &stop)
{
    stop.request(RoleHostCore::StopReason::ChannelClosed);
}

/// Consumer died and the producer's script has no `on_consumer_died`
/// — no-op.  A dead consumer is not by itself reason to stop the
/// producer; the channel stays alive, only per-consumer bookkeeping
/// would need cleanup (which the broker has already done on its
/// side).  Producers that care must define the callback.
inline void default_consumer_died(const IncomingMessage & /*msg*/,
                                  const StopRequestor & /*stop*/)
{
    // Intentionally empty.
}

/// One of the role's broker connections died and the script has no
/// `on_hub_dead`.  Asymmetric default by master/peer (HEP-CORE-0023
/// §2.5):
///   * Master (is_master=true)  → graceful stop with HubDead reason.
///     The master ctrl thread drives the heartbeat timer; without
///     it the broker reaps the role anyway.
///   * Peer   (is_master=false) → no-op.  Role keeps running on
///     master with degraded reach.  Scripts wishing to exit on
///     peer death must define `on_hub_dead` and call `api.stop()`
///     inside it.
inline void default_hub_dead(const IncomingMessage &msg,
                             const StopRequestor &stop)
{
    if (msg.details.value("is_master", false))
        stop.request(RoleHostCore::StopReason::HubDead);
}

// ── Dispatch table ────────────────────────────────────────────────────

using InvokeUserFn     = void (*)(ScriptEngine&,        const IncomingMessage&);
using DefaultNativeFn  = void (*)(const IncomingMessage&, const StopRequestor&);

/// One row per `NotificationId`.  `callback_name` MUST match the
/// `set_standard_callback_present(name, ...)` calls in each engine's
/// `load_script`; otherwise `engine.has_callback(name)` will always
/// return false and the user override will never fire.
struct NotificationEntry
{
    const char *      callback_name;  ///< nullptr ⇒ no row (Unknown)
    InvokeUserFn      invoke_user;    ///< nullptr ⇒ leave in msgs
    DefaultNativeFn   default_native; ///< called when override missing
};

inline constexpr NotificationEntry
kNotificationTable[static_cast<std::size_t>(NotificationId::Count)] = {
    /* Unknown        */ { nullptr,               nullptr,                       nullptr                  },
    /* ChannelClosing */ { "on_channel_closing",  &invoke_user_channel_closing,  &default_channel_closing },
    /* ConsumerDied   */ { "on_consumer_died",    &invoke_user_consumer_died,    &default_consumer_died   },
    /* HubDead        */ { "on_hub_dead",         &invoke_user_hub_dead,         &default_hub_dead        },
};

/// Single-pass dispatcher.  For each known msg: fire the user
/// override if defined, else the native default; ALWAYS consume the
/// msg (erase from msgs).  Unknown / future notification types fall
/// through and stay in `msgs` (no row in the table).
///
/// Order of dispatch = wire arrival order; cascading events
/// (e.g. CHANNEL_CLOSING_NOTIFY followed by CONSUMER_DIED_NOTIFY in
/// the same cycle) are seen by the script in the order the broker
/// emitted them.
inline void dispatch_notifications(ScriptEngine &engine,
                                   std::vector<IncomingMessage> &msgs,
                                   const StopRequestor &stop)
{
    auto it = msgs.begin();
    while (it != msgs.end())
    {
        const auto idx = static_cast<std::size_t>(it->notification_id);
        if (idx >= static_cast<std::size_t>(NotificationId::Count))
        {
            ++it; continue;             // out-of-range — leave in msgs
        }
        const NotificationEntry &entry = kNotificationTable[idx];
        if (entry.invoke_user == nullptr)
        {
            ++it; continue;             // Unknown — leave in msgs
        }
        if (engine.has_callback(entry.callback_name))
            entry.invoke_user(engine, *it);
        else
            entry.default_native(*it, stop);
        it = msgs.erase(it);            // always consumed
    }
}

// ============================================================================
// ProducerCycleOps — single-side output. Branch IDs: P-A-*, P-S-*, P-I-*, P-X
// ============================================================================

class ProducerCycleOps final
{
    RoleAPIBase  &api_;
    ScriptEngine &engine_;
    RoleHostCore &core_;
    bool          stop_on_error_;

    // Cached at construction (stable after queue start).
    size_t buf_sz_;

    // Per-cycle state.
    void *buf_{nullptr};

  public:
    ProducerCycleOps(RoleAPIBase &api, ScriptEngine &e,
                     RoleHostCore &c, bool stop_on_error)
        : api_(api), engine_(e), core_(c),
          stop_on_error_(stop_on_error),
          buf_sz_(api.write_item_size())
    {}

    bool acquire(const AcquireContext &ctx)
    {
        buf_ = retry_acquire(ctx, core_,
            [this](auto t) { return api_.write_acquire(t); });
        return buf_ != nullptr;
    }

    void cleanup_on_shutdown()
    {
        if (buf_) { api_.write_discard(); buf_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs)
    {
        // Per HEP-CORE-0011: route every broker-emitted notification
        // (CHANNEL_CLOSING_NOTIFY, CONSUMER_DIED_NOTIFY, …) to its
        // dedicated script callback if defined, stripping the notify
        // from msgs (single-delivery contract).  Unrecognised notifies
        // and notifies whose callback isn't defined stay in msgs for
        // the script's generic scan inside `on_produce`/`on_consume`.
        dispatch_notifications(engine_, msgs, StopRequestor{core_});

        if (buf_) std::memset(buf_, 0, buf_sz_);

        auto result = engine_.invoke_produce(
            InvokeTx{buf_, buf_sz_}, msgs);

        if (buf_)
        {
            if (result == InvokeResult::Commit)
            {
                api_.write_commit();
                core_.inc_out_slots_written();
            }
            else
            {
                api_.write_discard();
                core_.inc_out_drop_count();
            }
        }
        else
        {
            core_.inc_out_drop_count();
        }
        buf_ = nullptr;

        if (result == InvokeResult::Error && stop_on_error_)
        {
            // Audit S2 (2026-05-18) — set typed reason BEFORE request_stop
            // so downstream observers reading `stop_reason_string()`
            // see "script_error" rather than the default "normal".
            // Distinct from `CriticalError` (set by api.set_critical_error()).
            core_.set_stop_reason(RoleHostCore::StopReason::ScriptError);
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit() {} // nothing held across cycles
};

// ============================================================================
// ConsumerCycleOps — single-side input. Branch IDs: C-A-*, C-S-*, C-I-*, C-X
// ============================================================================

class ConsumerCycleOps final
{
    RoleAPIBase  &api_;
    ScriptEngine &engine_;
    RoleHostCore &core_;
    bool          stop_on_error_;

    size_t      item_sz_;
    const void *data_{nullptr};

  public:
    ConsumerCycleOps(RoleAPIBase &api, ScriptEngine &e,
                     RoleHostCore &core, bool stop_on_error)
        : api_(api), engine_(e), core_(core),
          stop_on_error_(stop_on_error),
          item_sz_(api.read_item_size())
    {}

    bool acquire(const AcquireContext &ctx)
    {
        // const_cast required: retry_acquire returns void* but read_acquire
        // returns const void* (input slot is read-only). The cast is safe here
        // because the pointer is immediately stored in const void* data_ and
        // never written through. DO NOT add write operations on the pointer
        // inside retry_acquire or this lambda — it would silently corrupt
        // shared memory owned by the producer.
        data_ = retry_acquire(ctx, core_,
            [this](auto t) { return const_cast<void *>(api_.read_acquire(t)); });
        return data_ != nullptr;
    }

    void cleanup_on_shutdown()
    {
        if (data_) { api_.read_release(); data_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs)
    {
        // Per HEP-CORE-0011: route every broker-emitted notification
        // (CHANNEL_CLOSING_NOTIFY, CONSUMER_DIED_NOTIFY, …) to its
        // dedicated script callback if defined, stripping the notify
        // from msgs (single-delivery contract).  Unrecognised notifies
        // and notifies whose callback isn't defined stay in msgs for
        // the script's generic scan inside `on_produce`/`on_consume`.
        dispatch_notifications(engine_, msgs, StopRequestor{core_});

        if (data_)
            core_.inc_in_slots_received();

        const uint64_t errors_before = engine_.script_error_count();

        engine_.invoke_consume(
            InvokeRx{data_, item_sz_}, msgs);

        if (data_) { api_.read_release(); data_ = nullptr; }

        if (stop_on_error_ && engine_.script_error_count() > errors_before)
        {
            // Audit S2 (2026-05-18) — see ProducerCycleOps for rationale.
            core_.set_stop_reason(RoleHostCore::StopReason::ScriptError);
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit() {} // nothing held across cycles
};

// ============================================================================
// ProcessorCycleOps — dual-side with input-hold. Branch IDs: PR-A-*, PR-S-*,
// PR-I-*, PR-X-*
// ============================================================================

class ProcessorCycleOps final
{
    RoleAPIBase  &api_;
    ScriptEngine &engine_;
    RoleHostCore &core_;
    bool          stop_on_error_;
    bool          drop_mode_;

    size_t in_sz_, out_sz_;

    const void *held_input_{nullptr};
    void       *out_buf_{nullptr};

  public:
    ProcessorCycleOps(RoleAPIBase &api, ScriptEngine &e, RoleHostCore &c,
                      bool stop_on_error, bool drop_mode)
        : api_(api), engine_(e), core_(c),
          stop_on_error_(stop_on_error), drop_mode_(drop_mode),
          in_sz_(api.read_item_size()), out_sz_(api.write_item_size())
    {}

    /// Processor always returns true — maintains timing cadence on idle cycles.
    bool acquire(const AcquireContext &ctx)
    {
        // Primary: input with retry (skip if held from previous cycle).
        // const_cast: same rationale as ConsumerCycleOps::acquire() — the
        // pointer is stored in const void* held_input_ and never written
        // through. See comment there for the full safety explanation.
        if (!held_input_)
        {
            held_input_ = retry_acquire(ctx, core_,
                [this](auto t) { return const_cast<void *>(api_.read_acquire(t)); });
        }

        // Secondary: output (only if input available, policy-dependent timeout).
        out_buf_ = nullptr;
        if (held_input_)
        {
            if (drop_mode_)
            {
                out_buf_ = api_.write_acquire(std::chrono::milliseconds{0});
            }
            else
            {
                auto output_timeout = ctx.short_timeout;
                if (ctx.deadline != std::chrono::steady_clock::time_point::max())
                {
                    auto remaining = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            ctx.deadline - std::chrono::steady_clock::now());
                    if (remaining > ctx.short_timeout)
                        output_timeout = remaining;
                }
                out_buf_ = api_.write_acquire(output_timeout);
            }
        }

        return true;
    }

    void cleanup_on_shutdown()
    {
        if (held_input_) { api_.read_release();  held_input_ = nullptr; }
        if (out_buf_)    { api_.write_discard(); out_buf_    = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs)
    {
        // Per HEP-CORE-0011: route every broker-emitted notification
        // (CHANNEL_CLOSING_NOTIFY, CONSUMER_DIED_NOTIFY, …) to its
        // dedicated script callback if defined, stripping the notify
        // from msgs (single-delivery contract).  Unrecognised notifies
        // and notifies whose callback isn't defined stay in msgs for
        // the script's generic scan inside `on_produce`/`on_consume`.
        dispatch_notifications(engine_, msgs, StopRequestor{core_});

        if (out_buf_) std::memset(out_buf_, 0, out_sz_);

        auto result = engine_.invoke_process(
            InvokeRx{held_input_, in_sz_},
            InvokeTx{out_buf_,    out_sz_},
            msgs);

        // Output commit/discard.
        if (out_buf_)
        {
            if (result == InvokeResult::Commit)
            { api_.write_commit();  core_.inc_out_slots_written(); }
            else
            { api_.write_discard(); core_.inc_out_drop_count(); }
        }
        else if (held_input_)
        {
            core_.inc_out_drop_count();
        }

        // Input release or hold.
        if (held_input_)
        {
            if (out_buf_ || drop_mode_)
            {
                api_.read_release();
                held_input_ = nullptr;
                core_.inc_in_slots_received();
            }
            // else: Block mode + output failed -> hold for next cycle.
        }
        out_buf_ = nullptr;

        if (result == InvokeResult::Error && stop_on_error_)
        {
            // Audit S2 (2026-05-18) — see ProducerCycleOps for rationale.
            core_.set_stop_reason(RoleHostCore::StopReason::ScriptError);
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit()
    {
        if (held_input_) { api_.read_release(); held_input_ = nullptr; }
    }
};

} // namespace pylabhub::scripting
