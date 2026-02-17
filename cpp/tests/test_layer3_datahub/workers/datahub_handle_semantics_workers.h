// tests/test_layer3_datahub/workers/handle_semantics_workers.h
#pragma once

namespace pylabhub::tests::worker::handle_semantics
{

int move_producer_transfers_ownership();
int move_consumer_transfers_ownership();
int default_constructed_handles_are_invalid();

} // namespace pylabhub::tests::worker::handle_semantics
