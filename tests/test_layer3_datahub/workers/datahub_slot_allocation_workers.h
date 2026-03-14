// tests/test_layer3_datahub/workers/datahub_slot_allocation_workers.h
#pragma once

namespace pylabhub::tests::worker::slot_allocation
{

int varied_schema_sizes_allocation();
int ring_buffer_scaling();
int write_read_roundtrip_varied_sizes();

} // namespace pylabhub::tests::worker::slot_allocation
