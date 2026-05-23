#pragma once
/**
 * @file role_host_frame.hpp
 * @brief RoleHostFrame — plain base class sitting between RoleHostBase
 *        and the three concrete role hosts (Producer/Consumer/Processor).
 *
 * Per design doc `docs/tech_draft/role_host_template_design.md` §11.1,
 * this is a **plain (non-templated) class**.  See that section for the
 * structural reasoning grounded in concrete code facts — short version:
 * `EngineHost<ApiT>` one layer up must be a template because its
 * `config_` member's TYPE varies by host kind (RoleConfig vs
 * std::monostate, different memory layouts); RoleHostFrame has no
 * such variant member (CycleOps lives as a stack local inside
 * worker_main_, never as a member), so virtual hooks suffice.
 *
 * Migration plan (M9 step 2):
 *   - 2a (this commit): create the skeleton; the three role hosts
 *     inherit from this instead of RoleHostBase directly.  No
 *     behavior consolidation yet — each role host continues to
 *     implement its own worker_main_ / setup_infrastructure_ /
 *     teardown_infrastructure_.
 *   - 2b: absorb teardown_infrastructure_ into the frame (100%
 *     identical across the three roles today).
 *   - 2c: absorb setup_infrastructure_ into the frame (uses the
 *     shared make_tx_opts / make_rx_opts free functions shipped in
 *     commit d05ec247).
 *   - 2d: absorb worker_main_ into the frame; add build_presences_ +
 *     run_loop_ pure-virtual hooks; role hosts shrink to thin
 *     wrappers providing only the role-specific bits.
 *
 * Each sub-step leaves the tree green; each is independently
 * commit-able and revert-able.
 */

#include "pylabhub_utils_export.h"
#include "utils/config/inbox_config.hpp"
#include "utils/engine_host.hpp"

#include <memory>
#include <string>

namespace pylabhub::hub
{
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::scripting
{

/// Per-role runtime configuration carried by RoleHostFrame.
///
/// All three current roles need the same three strings (role tag, label,
/// required script callback name).  Future role kinds (tap/monitor,
/// router, sink) inherit the same shape; if they need additional
/// runtime configuration, extend this struct rather than adding new
/// template parameters.
struct RoleHostFrameConfig
{
    /// Short role tag.  Forwarded to RoleHostBase as its role_tag; used
    /// in log prefixes and as the broker uid prefix component
    /// ("prod" / "cons" / "proc" today).
    std::string role_tag;

    /// Human-readable role label for diagnostics ("producer" /
    /// "consumer" / "processor").
    std::string role_label;

    /// Required script callback name ("on_produce" / "on_consume" /
    /// "on_process").  Engine asserts this exists during init.
    std::string required_callback;
};

/// Base class for all role hosts.  Plain (non-templated) class —
/// virtual hooks vary behavior per role; no template-instantiated
/// member-type variance is needed (see §11.1 of the design doc).
///
/// Still abstract: derived classes (ProducerRoleHost, ConsumerRoleHost,
/// ProcessorRoleHost) must implement RoleHostBase::worker_main_().
/// In a future sub-step (2d) the frame will provide a final
/// worker_main_ and add pure-virtual build_presences_ / run_loop_
/// hooks; for now each role host continues to override worker_main_
/// directly.
class PYLABHUB_UTILS_EXPORT RoleHostFrame : public RoleHostBase
{
  public:
    RoleHostFrame(config::RoleConfig    config,
                  std::atomic<bool>    *shutdown_flag,
                  RoleHostFrameConfig   frame_cfg);

    ~RoleHostFrame() override;

    // Copy/move deleted by RoleHostBase.

  protected:
    /// Read-only accessor for the frame configuration.  Derived classes
    /// (and, after sub-step 2c+, the frame's own setup/teardown bodies)
    /// use this to read role_tag/label/required_callback.
    [[nodiscard]] const RoleHostFrameConfig &frame_cfg() const noexcept
    {
        return frame_cfg_;
    }

    /// Shared teardown body, moved from the three per-role
    /// implementations during M9 sub-step 2b.  Pre-M9 each role host
    /// had its own `teardown_infrastructure_` method with byte-
    /// equivalent bodies (modulo comments).  Now lives here once.
    ///
    /// Sequence:
    ///   1. `core().clear_inbox_cache()`
    ///   2. if `inbox_queue_` exists: stop + reset.
    ///   3. if `has_api()`: `api().stop_handler_threads()`.
    ///   4. if `has_api()`: `api().close_queues()`.
    ///
    /// Called from each role's `worker_main_` at the teardown phase
    /// (and from the lambda passed to `do_role_teardown`).  Protected
    /// (not private) so derived classes can call it from inside
    /// `worker_main_`.
    void teardown_infrastructure_();

    /// Inbox queue + config — owned by the frame so the shared
    /// setup/teardown bodies can populate/release them.  Pre-M9
    /// these lived as private members on each role host.  Made
    /// protected so derived classes' `worker_main_` continues to
    /// access them by name (e.g. `api.set_inbox_queue(inbox_queue_.get())`).
    std::unique_ptr<hub::InboxQueue> inbox_queue_;

    /// Resolved (post-load) inbox config.  Copied from
    /// `config().inbox()` during `setup_infrastructure_`.
    config::InboxConfig              inbox_cfg_;

  private:
    RoleHostFrameConfig frame_cfg_;
};

} // namespace pylabhub::scripting
