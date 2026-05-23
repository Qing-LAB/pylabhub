/**
 * @file role_host_frame.cpp
 * @brief RoleHostFrame definitions.
 *
 * See role_host_frame.hpp for full design context and the M9 staged-
 * migration plan.  This file currently provides only the constructor
 * and destructor for the skeleton; subsequent migration sub-steps
 * (2b/2c/2d) will add setup_infrastructure_, teardown_infrastructure_,
 * and worker_main_ bodies as they're absorbed from the three role
 * hosts.
 */

#include "utils/role_host_frame.hpp"

#include <utility>

namespace pylabhub::scripting
{

RoleHostFrame::RoleHostFrame(config::RoleConfig    config,
                              std::atomic<bool>    *shutdown_flag,
                              RoleHostFrameConfig   frame_cfg)
    : RoleHostBase(frame_cfg.role_tag,
                   std::move(config),
                   shutdown_flag),
      frame_cfg_(std::move(frame_cfg))
{
}

RoleHostFrame::~RoleHostFrame() = default;

} // namespace pylabhub::scripting
