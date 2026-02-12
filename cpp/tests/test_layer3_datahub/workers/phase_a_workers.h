// tests/test_layer3_datahub/workers/phase_a_workers.h
#pragma once

namespace pylabhub::tests::worker::phase_a
{

int flexible_zone_span_empty_when_no_zones();
int flexible_zone_span_non_empty_when_zones_defined();
int checksum_flexible_zone_false_when_no_zones();
int checksum_flexible_zone_true_when_valid();
int consumer_without_expected_config_gets_empty_zones();
int consumer_with_expected_config_gets_zones();

// Structured flexible zone: producer writes typed struct, consumer reads and verifies
int structured_flex_zone_data_passes();
// Error modes: API throws or checksum verify fails
int error_flex_zone_type_too_large_throws();
int error_checksum_flex_zone_fails_after_tampering();

} // namespace pylabhub::tests::worker::phase_a
