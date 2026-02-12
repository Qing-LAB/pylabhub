// tests/test_layer3_datahub/workers/slot_protocol_workers.h
#pragma once

namespace pylabhub::tests::worker::slot_protocol
{

int write_read_succeeds_in_process();
int checksum_update_verify_succeeds();
int layout_with_checksum_and_flexible_zone_succeeds();
int diagnostic_handle_opens_and_accesses_header();

} // namespace pylabhub::tests::worker::slot_protocol
