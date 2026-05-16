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
// Broker-notification callback dispatch (HEP-CORE-0011 callback table)
// ============================================================================
//
// Each cycle drains broker-emitted notifications (CHANNEL_CLOSING_NOTIFY,
// CONSUMER_DIED_NOTIFY, etc.) into the role's incoming-message list.
// The BRC-side enqueue tags each message with its `NotificationId` (see
// `role_host_core.hpp` — parsed once at the wire boundary so dispatch
// here is O(1) per message via integer index, not N string compares).
//
// Per-notification handlers below are pure functions of (engine, msg):
//   - Return true  → callback fired; msg is "consumed" (erased from
//                    msgs so `on_produce`/`on_consume` doesn't see it).
//   - Return false → script didn't define the callback; msg stays in
//                    msgs for the script's generic scan path.
//
// Single delivery contract: dedicated callback OR generic scan, never
// both.  See HEP-CORE-0011 §"Channel-close handling" for the original
// articulation; the same rule applies to every notify entry here.
//
// Adding a notification:
//   1. Add the enum value in `role_host_core.hpp` (NotificationId).
//   2. Map the wire string in `parse_notification_id`.
//   3. Write the handler below + add it to `kNotificationHandlers[]`.
//   4. Add the matching `invoke_on_X` pure virtual on `ScriptEngine`.

inline bool handle_channel_closing(ScriptEngine &engine,
                                   const IncomingMessage &msg)
{
    if (!engine.has_callback("on_channel_closing")) return false;
    engine.invoke_on_channel_closing(
        msg.details.value("channel_name", std::string{}),
        msg.details.value("reason",       std::string{}));
    return true;
}

inline bool handle_consumer_died(ScriptEngine &engine,
                                 const IncomingMessage &msg)
{
    if (!engine.has_callback("on_consumer_died")) return false;
    engine.invoke_on_consumer_died(
        msg.details.value("channel_name", std::string{}),
        msg.details.value("consumer_uid", std::string{}),
        msg.details.value("reason",       std::string{}));
    return true;
}

/// Handler signature: returns true if the msg was consumed (erase from
/// msgs), false if the script didn't define the callback (keep in msgs).
using NotificationHandlerFn = bool(*)(ScriptEngine&, const IncomingMessage&);

/// Dispatch table indexed by NotificationId.  Slot for `Unknown` is nullptr
/// (unrecognised types fall through to msgs untouched).  Sized by
/// NotificationId::Count so the array stays in lockstep with the enum.
inline constexpr NotificationHandlerFn
kNotificationHandlers[static_cast<std::size_t>(NotificationId::Count)] = {
    /* NotificationId::Unknown        */ nullptr,
    /* NotificationId::ChannelClosing */ &handle_channel_closing,
    /* NotificationId::ConsumerDied   */ &handle_consumer_died,
};

/// Single-pass dispatcher over the drained per-cycle msgs list.
/// Replaces the previous N-helpers-each-scans-msgs design with one
/// integer-indexed loop.  Order of dispatch matches wire arrival
/// order; cascading events (e.g. CHANNEL_CLOSING followed by
/// CONSUMER_DIED in the same cycle) are seen by the script in the
/// order the broker emitted them.
inline void dispatch_notifications(ScriptEngine &engine,
                              std::vector<IncomingMessage> &msgs)
{
    auto it = msgs.begin();
    while (it != msgs.end())
    {
        const auto idx = static_cast<std::size_t>(it->notification_id);
        NotificationHandlerFn fn = (idx < static_cast<std::size_t>(NotificationId::Count))
                             ? kNotificationHandlers[idx]
                             : nullptr;
        if (fn && fn(engine, *it))
        {
            it = msgs.erase(it);    // consumed by callback
        }
        else
        {
            ++it;                    // leave in msgs for generic scan
        }
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
        dispatch_notifications(engine_, msgs);

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
        dispatch_notifications(engine_, msgs);

        if (data_)
            core_.inc_in_slots_received();

        const uint64_t errors_before = engine_.script_error_count();

        engine_.invoke_consume(
            InvokeRx{data_, item_sz_}, msgs);

        if (data_) { api_.read_release(); data_ = nullptr; }

        if (stop_on_error_ && engine_.script_error_count() > errors_before)
        {
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
        dispatch_notifications(engine_, msgs);

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
        { core_.request_stop(); return false; }
        return true;
    }

    void cleanup_on_exit()
    {
        if (held_input_) { api_.read_release(); held_input_ = nullptr; }
    }
};

} // namespace pylabhub::scripting
