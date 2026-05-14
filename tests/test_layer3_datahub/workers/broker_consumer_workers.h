#pragma once
/**
 * @file broker_consumer_workers.h
 * @brief Workers for consumer registration protocol integration tests
 *        (CONSUMER_REG_REQ / CONSUMER_DEREG_REQ; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace broker_consumer
{

int consumer_reg_channel_not_found();
int consumer_reg_happy_path();
int consumer_dereg_happy_path();
int consumer_dereg_pid_mismatch();
int disc_shows_consumer_count();

} // namespace broker_consumer
} // namespace pylabhub::tests::worker
