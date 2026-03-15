// tests/test_layer3_datahub/workers/exception_safety_workers.h
#pragma once

namespace pylabhub::tests::worker::exception_safety
{

int exception_before_publish_aborts_write_slot();
int exception_in_write_transaction_leaves_producer_usable();
int exception_in_read_transaction_leaves_consumer_usable();

} // namespace pylabhub::tests::worker::exception_safety
