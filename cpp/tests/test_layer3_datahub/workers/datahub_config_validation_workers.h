// tests/test_layer3_datahub/workers/datahub_config_validation_workers.h
#pragma once

namespace pylabhub::tests::worker::config_validation
{

int policy_unset_throws();
int consumer_sync_policy_unset_throws();
int physical_page_size_unset_throws();
int ring_buffer_capacity_zero_throws();
int valid_config_creates_successfully();

} // namespace pylabhub::tests::worker::config_validation
