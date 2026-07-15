#pragma once
/**
 * @file broker_admin_workers.h
 * @brief Workers for BrokerService admin API tests
 *        (list_channels_json_str / query_channel_snapshot /
 *        request_close_channel; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace broker_admin
{

int list_channels_empty();
int list_channels_one_channel();
int list_channels_field_presence();
int snapshot_empty();
int snapshot_one_channel();
int snapshot_after_consumer();
int close_channel_existing();
int close_channel_non_existent();

// #281 (2026-06-23) — REG_REQ wire-contract pins for `data_transport`.
// HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1 — broker MUST reject:
//   * missing  / non-string / empty / non-{shm,zmq}  `data_transport`
//   * `data_transport=shm`  + missing `shm_capability_endpoint` (the
//     pre-#281 §5.1 H3c check; never had its own pin before #281).
// Positive pins anchor that the production-shaped wire shapes still
// succeed end-to-end.
// reg_validation error paths (missing/empty/bogus data_transport +
// shm_missing_endpoint) MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_admin.cpp (task #52 Round 2).  The *_success
// variants stay — they inspect the in-process channel snapshot.
int reg_validation_shm_success();
int reg_validation_zmq_success();

} // namespace broker_admin
} // namespace pylabhub::tests::worker
