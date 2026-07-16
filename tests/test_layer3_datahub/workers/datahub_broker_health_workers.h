#pragma once
// tests/test_layer3_datahub/workers/datahub_broker_health_workers.h
//
// Broker/Producer/Consumer health and notification tests.

namespace pylabhub::tests::worker::broker_health
{

// producer_gets_closing_notify / producer_auto_deregisters /
// schema_mismatch_notify MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_health.cpp (task #52 Round 2).

/** Consumer::close() sends CONSUMER_DEREG_REQ; broker consumer_count drops to 0. */
int consumer_auto_deregisters(int argc, char **argv);

// dead_consumer_orchestrator / dead_consumer_exiter /
// consumer_heartbeat_timeout_notify MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_health.cpp (task #52 Round 2).

/**
 * HEP-CORE-0035 §4.2 Layer-1 ZAP deny-path pin (PeerAdmission D2).
 * Broker with `enforce_ctrl_admission=true` + empty allowlist denies
 * a CURVE peer; `ZapRouter::denied_count()` increases; REG_REQ times
 * out.  This is the security-gate path-discriminator the D2 commit
 * acknowledged as a follow-on (audit B2).
 */
int ctrl_zap_deny_path(int argc, char **argv);

} // namespace pylabhub::tests::worker::broker_health
