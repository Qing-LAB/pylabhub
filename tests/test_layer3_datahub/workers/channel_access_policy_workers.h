#pragma once
/**
 * @file channel_access_policy_workers.h
 * @brief Workers for broker connection-policy enforcement tests
 *        (Pattern 3).  Suite 1 (enum conversion) stays Pattern 1 in
 *        the parent file; only the broker-enforcement Suite 2 is
 *        migrated to subprocess workers.
 */

namespace pylabhub::tests::worker
{
namespace channel_access_policy
{

int open_policy_accepts_anonymous();
int open_policy_accepts_with_identity();
int required_policy_rejects_anonymous();
int required_policy_accepts_with_identity();
int verified_policy_rejects_unknown_role();
int verified_policy_accepts_known_role();
int per_channel_glob_override_restricts_channel();

} // namespace channel_access_policy
} // namespace pylabhub::tests::worker
