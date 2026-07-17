// tests/test_layer3_datahub/workers/datahub_slot_allocation_workers.h
//
// DataBlock slot-allocation workers (fd-source).  Driver:
// test_datahub_slot_allocation.cpp (DatahubSlotAllocationTest).
#pragma once

namespace pylabhub::tests::worker::slot_allocation
{

int varied_schema_sizes_allocation();
int ring_buffer_scaling();
int write_read_roundtrip_varied_sizes();

} // namespace pylabhub::tests::worker::slot_allocation
