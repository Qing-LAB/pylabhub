// tests/test_layer3_datahub/workers/c_api_checksum_workers.h
#pragma once

namespace pylabhub::tests::worker::c_api_checksum
{

int enforced_roundtrip_passes();
int enforced_corruption_detected();
int none_skips_verification();
int manual_no_auto_checksum();
int manual_explicit_checksum_roundtrip();
int invalidate_checksum_zero_hash_rejected();

} // namespace pylabhub::tests::worker::c_api_checksum
