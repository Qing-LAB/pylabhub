#pragma once
/**
 * @file cycle_ops.hpp
 * @brief Concrete RoleCycleOps subclasses for producer, consumer, processor.
 *
 * These three classes implement the role-specific acquire / invoke-and-commit
 * slot of the shared data-loop frame (`RoleAPIBase::run_data_loop`). Behavior is
 * specified by `docs/tech_draft/loop_design_unified.md` §3 (7-step skeleton)
 * and `docs/tech_draft/role_unification_design.md` §4.5a (35 branch IDs with
 * stable per-class tags: P-*, C-*, PR-*).
 *
 * Published as a public header (previously anonymous-namespace classes inside
 * the per-role hosts) so the L3.β baseline test suite can instantiate them
 * directly against in-process queue pairs + a RecordingEngine stub. No behavior
 * change vs. the inline anonymous-namespace definitions; this is a mechanical
 * relocation only.
 *
 * Post-L3.β these three classes collapse into a single unified
 * `scripting::CycleOps`; this header is the oracle the unified version must
 * match branch-for-branch and metric-site-for-metric-site.
 */

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

class ProducerCycleOps final : public RoleCycleOps
{
    RoleAPIBase  &api_;
    ScriptEngine &engine_;
    RoleHostCore &core_;
    bool          stop_on_error_;

    // Cached at construction (stable after queue start).
    size_t buf_sz_;
    void  *fz_ptr_;
    size_t fz_sz_;

    // Per-cycle state.
    void *buf_{nullptr};

  public:
    ProducerCycleOps(RoleAPIBase &api, ScriptEngine &e,
                     RoleHostCore &c, bool stop_on_error)
        : api_(api), engine_(e), core_(c),
          stop_on_error_(stop_on_error),
          buf_sz_(api.write_item_size()),
          fz_ptr_(c.has_out_fz() ? api.flexzone(ChannelSide::Tx) : nullptr),
          fz_sz_(c.has_out_fz() ? api.flexzone_size(ChannelSide::Tx) : 0)
    {}

    bool acquire(const AcquireContext &ctx) override
    {
        buf_ = retry_acquire(ctx, core_,
            [this](auto t) { return api_.write_acquire(t); });
        return buf_ != nullptr;
    }

    void cleanup_on_shutdown() override
    {
        if (buf_) { api_.write_discard(); buf_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs) override
    {
        if (buf_) std::memset(buf_, 0, buf_sz_);

        // Re-read flexzone pointer each cycle (ShmQueue may move it).
        if (core_.has_out_fz())
            fz_ptr_ = api_.flexzone(ChannelSide::Tx);

        auto result = engine_.invoke_produce(
            InvokeTx{buf_, buf_sz_, fz_ptr_, fz_sz_}, msgs);

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

    void cleanup_on_exit() override {} // nothing held across cycles
};

// ============================================================================
// ConsumerCycleOps — single-side input. Branch IDs: C-A-*, C-S-*, C-I-*, C-X
// ============================================================================

class ConsumerCycleOps final : public RoleCycleOps
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

    bool acquire(const AcquireContext &ctx) override
    {
        data_ = retry_acquire(ctx, core_,
            [this](auto t) { return const_cast<void *>(api_.read_acquire(t)); });
        return data_ != nullptr;
    }

    void cleanup_on_shutdown() override
    {
        if (data_) { api_.read_release(); data_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs) override
    {
        if (data_)
            core_.inc_in_slots_received();

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        void *fz_ptr = core_.has_in_fz()
            ? api_.flexzone(ChannelSide::Rx) : nullptr;
        const size_t fz_sz = core_.has_in_fz() ? api_.flexzone_size(ChannelSide::Rx) : 0;

        const uint64_t errors_before = engine_.script_error_count();

        engine_.invoke_consume(
            InvokeRx{data_, item_sz_, fz_ptr, fz_sz}, msgs);

        if (data_) { api_.read_release(); data_ = nullptr; }

        if (stop_on_error_ && engine_.script_error_count() > errors_before)
        {
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit() override {} // nothing held across cycles
};

// ============================================================================
// ProcessorCycleOps — dual-side with input-hold. Branch IDs: PR-A-*, PR-S-*,
// PR-I-*, PR-X-*
// ============================================================================

class ProcessorCycleOps final : public RoleCycleOps
{
    RoleAPIBase  &api_;
    ScriptEngine &engine_;
    RoleHostCore &core_;
    bool          stop_on_error_;
    bool          drop_mode_;

    size_t in_sz_, out_sz_;
    void  *out_fz_ptr_;
    size_t out_fz_sz_;
    void  *in_fz_ptr_;
    size_t in_fz_sz_;

    const void *held_input_{nullptr};
    void       *out_buf_{nullptr};

  public:
    ProcessorCycleOps(RoleAPIBase &api, ScriptEngine &e, RoleHostCore &c,
                      bool stop_on_error, bool drop_mode)
        : api_(api), engine_(e), core_(c),
          stop_on_error_(stop_on_error), drop_mode_(drop_mode),
          in_sz_(api.read_item_size()), out_sz_(api.write_item_size()),
          out_fz_ptr_(c.has_out_fz() ? api.flexzone(ChannelSide::Tx) : nullptr),
          out_fz_sz_(c.has_out_fz() ? api.flexzone_size(ChannelSide::Tx) : 0),
          in_fz_ptr_(c.has_in_fz() ? api.flexzone(ChannelSide::Rx) : nullptr),
          in_fz_sz_(c.has_in_fz() ? api.flexzone_size(ChannelSide::Rx) : 0)
    {}

    /// Processor always returns true — maintains timing cadence on idle cycles.
    bool acquire(const AcquireContext &ctx) override
    {
        // Primary: input with retry (skip if held from previous cycle).
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

    void cleanup_on_shutdown() override
    {
        if (held_input_) { api_.read_release();  held_input_ = nullptr; }
        if (out_buf_)    { api_.write_discard(); out_buf_    = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs) override
    {
        if (out_buf_) std::memset(out_buf_, 0, out_sz_);

        // Re-read flexzone pointers each cycle (ShmQueue may move them).
        if (core_.has_out_fz()) out_fz_ptr_ = api_.flexzone(ChannelSide::Tx);
        if (core_.has_in_fz())
            in_fz_ptr_ = api_.flexzone(ChannelSide::Rx);

        auto result = engine_.invoke_process(
            InvokeRx{held_input_, in_sz_, in_fz_ptr_, in_fz_sz_},
            InvokeTx{out_buf_,    out_sz_, out_fz_ptr_, out_fz_sz_},
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
            // else: Block mode + output failed → hold for next cycle.
        }
        out_buf_ = nullptr;

        if (result == InvokeResult::Error && stop_on_error_)
        { core_.request_stop(); return false; }
        return true;
    }

    void cleanup_on_exit() override
    {
        if (held_input_) { api_.read_release(); held_input_ = nullptr; }
    }
};

} // namespace pylabhub::scripting
