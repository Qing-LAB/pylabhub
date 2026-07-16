// tests/test_layer3_datahub/workers/datahub_slot_allocation_workers.h
//
// ⚠ MASKED FROM THE BUILD (Rule-6, 2026-06-30) — see the paired .cpp header.
//   Coverage migrated under #275-S2 to the fd-source workers; physical
//   deletion tracked under REVIEW-C / #276.  Do not resurrect.
#pragma once

namespace pylabhub::tests::worker::slot_allocation
{

int varied_schema_sizes_allocation();
int ring_buffer_scaling();
int write_read_roundtrip_varied_sizes();

} // namespace pylabhub::tests::worker::slot_allocation
