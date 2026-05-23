#pragma once
/**
 * @file role_config_translation.hpp
 * @brief Pure config→opts translation functions used by the role hosts'
 *        setup_infrastructure_() path.
 *
 * These functions are extracted from each role host's
 * setup_infrastructure_ body so the translation layer can be exercised
 * in isolation by L2 tests, and so the producer/processor-tx and
 * consumer/processor-rx duplicates collapse into one shared
 * implementation.
 *
 * ── Audit trail ──────────────────────────────────────────────────────
 *
 * Two production bugs lived in this translation layer in the
 * 2026-05-20/21 demo-harness session:
 *   - B5  — consumer's `shm_name` was never copied from config → SHM
 *           connect failed silently.
 *   - B11 — consumer/processor's ZMQ fields (data_transport /
 *           zmq_node_endpoint / clear shm_name) were never copied
 *           when transport was ZMQ → build_rx_queue dispatched the
 *           SHM path on a ZMQ pipeline.
 *
 * Both lived inline in setup_infrastructure_ where the L3 test
 * (role_api_flexzone_workers.cpp) couldn't reach without also
 * triggering broker connection.  Closing the test gap required
 * extracting the translation.  The first extraction (commit
 * eb3eed36, 2026-05-22) created PER-ROLE static methods.  This
 * file consolidates those into TWO free functions shared across all
 * three roles, per the M9 design (docs/tech_draft/
 * role_host_template_design.md §11.6).
 *
 * ── HEP-CORE-0021 + Q1 resolution ────────────────────────────────────
 *
 * `make_rx_opts` adopts the Consumer's placement convention for
 * `zmq_buffer_depth`: assigned only inside the `if (transport ==
 * Zmq)` branch.  Resolves the pre-existing inconsistency where
 * Processor's per-role static method set the field unconditionally
 * (functionally inert today because defaults converge, but a latent
 * foot-gun if defaults diverge).  See the design doc §11.6 for the
 * Q1 discussion.
 */

#include "pylabhub_utils_export.h"
#include "plh_datahub.hpp"
#include "utils/config/role_config.hpp"
#include "utils/role_api_base.hpp"
#include "utils/schema_utils.hpp"

namespace pylabhub::scripting
{

/// Pure config→TxQueueOptions translation, shared by Producer and
/// Processor-output sides.
///
/// Inputs:
///   - `config`        — fully-loaded RoleConfig (post-load_from_directory).
///   - `out_slot_spec` — schema for the output slot (pass-through to opts).
///   - `out_fz_spec`   — flexzone schema for the output side (pass-through).
///   - `has_tx_fz`     — whether the role has a TX flexzone configured;
///                       AND'd with `config.checksum().flexzone` to derive
///                       `opts.flexzone_checksum`.
///
/// Pure: no side effects, no broker access, no queue construction.
/// Safe to call from tests.
[[nodiscard]] PYLABHUB_UTILS_EXPORT
hub::TxQueueOptions
make_tx_opts(const config::RoleConfig &config,
             const hub::SchemaSpec    &out_slot_spec,
             const hub::SchemaSpec    &out_fz_spec,
             bool                      has_tx_fz);

/// Pure config→RxQueueOptions translation, shared by Consumer and
/// Processor-input sides.
///
/// Resolves the pre-existing zmq_buffer_depth placement inconsistency
/// (Q1 in the 2026-05-22 fresh-eye review) by adopting Consumer's
/// convention: set the field only inside the `if (transport == Zmq)`
/// branch.
///
/// Inputs mirror `make_tx_opts` but on the input side: `in_slot_spec`,
/// `in_fz_spec`, `has_rx_fz`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT
hub::RxQueueOptions
make_rx_opts(const config::RoleConfig &config,
             const hub::SchemaSpec    &in_slot_spec,
             const hub::SchemaSpec    &in_fz_spec,
             bool                      has_rx_fz);

} // namespace pylabhub::scripting
