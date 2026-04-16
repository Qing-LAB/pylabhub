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
